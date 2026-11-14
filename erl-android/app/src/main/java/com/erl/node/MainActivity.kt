package com.erl.node

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.Typeface
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.webkit.JavascriptInterface
import android.webkit.WebResourceError
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors

/**
 * Shows the ERL node's web UI (http://192.168.7.1/) over the USB link.
 *
 * With mobile data or Wi-Fi on, Android sends a browser's traffic over that default network,
 * so a page on the USB Ethernet link (no internet) is unreachable in Chrome. This app instead
 * asks for the Ethernet network explicitly and binds its whole process to it, WebView included,
 * so the node page loads while the rest of the phone keeps using mobile data.
 *
 * With the phone's Ethernet tethering on (sharing mobile data with the node and its mesh), the
 * link is a tethering downstream instead: no network to bind, and the node has an address in
 * the phone's tethering subnet, found by [TetherScan].
 */
class MainActivity : Activity() {

    companion object {
        private const val NODE_URL = "http://192.168.7.1/"
        private const val RETRY_MS = 3000L
        private const val SCAN_MS = 5000L
        // while a page is shown: the node is asked for a tiny CGI every HEALTH_MS; after
        // HEALTH_FAILS misses in a row the link is treated as gone and the node is searched again
        // (the page itself can keep showing, with "–" everywhere, while its data requests fail)
        private const val HEALTH_MS = 5000L
        private const val HEALTH_FAILS = 3
        private const val HEALTH_TIMEOUT_MS = 3000
        // the node's side of the USB link (usb-ncm: 192.168.7.1/24, phones get .10-.50)
        private const val LINK_PREFIX = "192.168.7."
        // this many failed loads in a row over the USB network (RETRY_MS apart): ask Android for it
        // afresh, the network we hold may be one the board dropped while it was booting
        private const val RETRIES_BEFORE_RENEW = 10
        private const val REQUEST_LOCATION = PhoneSensors.REQUEST_LOCATION
        // shared with ChatWatchService
        const val PREFS = "node"
        const val PREF_TETHER_HOST = "tether_host"
    }

    private var startPage: String? = null       // page to open (from a notification), e.g. "/chat.html"
    private var resumed = false

    private lateinit var connectivity: ConnectivityManager
    private lateinit var webView: WebView
    private lateinit var panel: LinearLayout
    private lateinit var status: TextView
    private lateinit var progress: ProgressBar
    private lateinit var diag: TextView         // what the app sees of the phone's networks

    private val handler = Handler(Looper.getMainLooper())
    private val scanner = Executors.newSingleThreadExecutor()
    private val checker = Executors.newSingleThreadExecutor()
    private var healthFails = 0
    @Volatile private var usbNetwork: Network? = null
    @Volatile private var tetherHost: String? = null    // the node's address while Ethernet tethering is on
    @Volatile private var ownNodePage = false           // the page shown is the connected node's own
    private lateinit var phoneSensors: PhoneSensors
    private var scanning = false
    private var pageLoaded = false
    private var failedLoads = 0

    // Our own timers. Never handler.removeCallbacksAndMessages(null) outside onDestroy: that also
    // drops the network callback's queued onAvailable / onLost, and a board that is still booting
    // (its USB link going down and up again) would then never be seen.
    private val retryTask = Runnable { if (usbNetwork != null) loadNode() }
    private val scanTask = Runnable { scan() }
    private val healthTask = Runnable { checkHealth() }
    private fun cancelTimers() {
        handler.removeCallbacks(retryTask)
        handler.removeCallbacks(scanTask)
        handler.removeCallbacks(healthTask)
    }

    private val networkRequest: NetworkRequest = NetworkRequest.Builder()
        .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET)
        .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
        .build()

    // The USB link of the node shows up as an Ethernet network without internet. Only a network
    // that carries the node's subnet is taken: another Ethernet network (a dock, an adapter) or a
    // leftover of the link would bind the app to a network where the node is not.
    // onLinkPropertiesChanged follows onAvailable at once (API 26+), with the addresses.
    private val networkCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onLinkPropertiesChanged(network: Network, lp: LinkProperties) {
            val ours = isNodeLink(lp)
            handler.post {
                if (ours && usbNetwork != network) {
                    connectivity.bindProcessToNetwork(network)
                    usbNetwork = network
                    tetherHost = null
                    failedLoads = 0
                    loadNode()
                } else if (!ours && usbNetwork == network) {
                    // the link lost the node's subnet, e.g. it turned into a tethering downstream
                    usbNetwork = null
                    if (connectivity.boundNetworkForProcess == network) connectivity.bindProcessToNetwork(null)
                    waitForNode()
                }
            }
        }

        override fun onLost(network: Network) {
            handler.post {
                if (usbNetwork != network) return@post
                usbNetwork = null
                // not ours to undo if a newer network was bound in the meantime (its onAvailable
                // binds at once, before its queued part runs)
                if (connectivity.boundNetworkForProcess == network) connectivity.bindProcessToNetwork(null)
                // the link is gone, or it turned into a tethering downstream
                waitForNode()
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        connectivity = getSystemService(ConnectivityManager::class.java)
        setContentView(buildLayout().also { fitSystemBars(it) })

        connectivity.requestNetwork(networkRequest, networkCallback)
        startPage = intent.getStringExtra(ChatWatchService.EXTRA_PAGE)
        waitForNode()

        // chat notifications, also while the app is in the background
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
        }
        ChatWatchService.start(this)
        phoneSensors = PhoneSensors(this) { nodeUrl() }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_LOCATION && resumed) {
            phoneSensors.stop()
            phoneSensors.start()
        }
    }

    // A notification tapped while the app is open: show its page
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        startPage = intent.getStringExtra(ChatWatchService.EXTRA_PAGE) ?: return
        if (nodeUrl() != null) loadNode()
    }

    override fun onResume() {
        super.onResume()
        resumed = true
        ChatWatchService.chatVisible = isChatPage(webView.url)
        phoneSensors.start()
    }

    override fun onPause() {
        resumed = false
        ChatWatchService.chatVisible = false
        phoneSensors.stop()
        super.onPause()
    }

    private fun isChatPage(url: String?) = url != null && Uri.parse(url).path == "/chat.html"

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        scanner.shutdownNow()
        checker.shutdownNow()
        connectivity.unregisterNetworkCallback(networkCallback)
        connectivity.bindProcessToNetwork(null)
        webView.destroy()
        super.onDestroy()
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK && webView.visibility == View.VISIBLE && webView.canGoBack()) {
            webView.goBack()
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    private fun nodeUrl(): String? = when {
        usbNetwork != null -> NODE_URL
        tetherHost != null -> "http://$tetherHost/"
        else -> null
    }

    private fun loadNode() {
        val url = (nodeUrl() ?: return waitForNode()) + (startPage?.removePrefix("/") ?: "")
        cancelTimers()
        pageLoaded = false
        showWaiting(getString(R.string.loading), busy = true)
        webView.loadUrl(url)
    }

    // The link can be up a moment before the node's web server answers (it is still booting)
    private fun retryLater() {
        if (usbNetwork == null) {
            // over tethering: the node may have a new address, or tethering was turned off
            tetherHost = null
            return waitForNode()
        }
        if (++failedLoads >= RETRIES_BEFORE_RENEW) return renewNetwork()
        showWaiting(getString(R.string.unreachable), busy = true)
        cancelTimers()
        handler.postDelayed(retryTask, RETRY_MS)
    }

    // Drop the USB network we hold and ask for one again: Android then hands over the one that
    // is up now (or the next one), not a network the board already took down
    private fun renewNetwork() {
        failedLoads = 0
        runCatching { connectivity.unregisterNetworkCallback(networkCallback) }
        usbNetwork = null
        connectivity.bindProcessToNetwork(null)
        connectivity.requestNetwork(networkRequest, networkCallback)
        waitForNode()
    }

    // A USB Ethernet network Android has now, in case its onAvailable never reached us
    @Suppress("DEPRECATION")    // allNetworks: fine for a one-off check, the callback is the main path
    private fun findUsbNetwork(): Network? = connectivity.allNetworks.firstOrNull { n ->
        connectivity.getNetworkCapabilities(n)?.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) == true &&
            isNodeLink(connectivity.getLinkProperties(n))
    }

    // Ethernet networks Android has (the USB link as a client network) with their addresses
    @Suppress("DEPRECATION")
    private fun ethernetNetworks(): String = connectivity.allNetworks
        .filter { connectivity.getNetworkCapabilities(it)?.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) == true }
        .joinToString(", ") { n ->
            val lp = connectivity.getLinkProperties(n)
            "${lp?.interfaceName ?: "?"} " + (lp?.linkAddresses?.joinToString(" ") ?: "no addresses") +
                (if (isNodeLink(lp)) " (node link)" else "")
        }
        .ifEmpty { "none" }

    private fun showDiag() {
        diag.text = "ethernet networks: ${ethernetNetworks()}\n" +
            TetherScan.report.ifEmpty { "interfaces: ${TetherScan.interfaces()}" }
    }

    private fun isNodeLink(lp: LinkProperties?): Boolean =
        lp?.linkAddresses?.any { it.address.hostAddress?.startsWith(LINK_PREFIX) == true } == true

    // Is the node still answering? Static files can come from the WebView cache or an open
    // connection, so only a CGI request proves the link works.
    private fun checkHealth() {
        val base = nodeUrl() ?: return
        if (!pageLoaded || !resumed) {
            handler.postDelayed(healthTask, HEALTH_MS)
            return
        }
        checker.execute {
            val ok = try {
                val conn = URL(base + "cgi-bin/internet").openConnection() as HttpURLConnection
                conn.connectTimeout = HEALTH_TIMEOUT_MS
                conn.readTimeout = HEALTH_TIMEOUT_MS
                conn.useCaches = false
                try {
                    conn.responseCode == 200 && conn.inputStream.use { it.readBytes() }.isNotEmpty()
                } finally {
                    conn.disconnect()
                }
            } catch (e: Exception) {
                false
            }
            handler.post {
                if (isFinishing || nodeUrl() != base) return@post
                healthFails = if (ok) 0 else healthFails + 1
                if (healthFails < HEALTH_FAILS) {
                    handler.postDelayed(healthTask, HEALTH_MS)
                    return@post
                }
                healthFails = 0
                pageLoaded = false
                if (usbNetwork != null) renewNetwork() else {
                    tetherHost = null       // the node may have a new address in the tethering subnet
                    waitForNode()
                }
            }
        }
    }

    // No USB network: look for the node in a tethering subnet now and every SCAN_MS
    private fun waitForNode() {
        cancelTimers()
        showWaiting(getString(R.string.waiting_text), busy = false)
        showDiag()
        scan()
    }

    private fun scan() {
        if (scanning || usbNetwork != null || tetherHost != null) return
        findUsbNetwork()?.let { network ->
            connectivity.bindProcessToNetwork(network)
            usbNetwork = network
            tetherHost = null
            return loadNode()
        }
        scanning = true
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        val last = prefs.getString(PREF_TETHER_HOST, null)
        scanner.execute {
            val found = TetherScan.find(last)
            handler.post {
                scanning = false
                showDiag()
                if (usbNetwork != null || isFinishing) return@post
                if (found != null) {
                    tetherHost = found
                    prefs.edit().putString(PREF_TETHER_HOST, found).apply()
                    loadNode()
                } else {
                    handler.postDelayed(scanTask, SCAN_MS)
                }
            }
        }
    }

    private fun showWaiting(text: String, busy: Boolean) {
        status.text = text
        progress.visibility = if (busy) View.VISIBLE else View.GONE
        panel.visibility = View.VISIBLE
        webView.visibility = View.INVISIBLE
    }

    private fun showPage() {
        panel.visibility = View.GONE
        webView.visibility = View.VISIBLE
    }

    /**
     * Keep the page clear of the status bar, the navigation bar / gesture area and the
     * keyboard. From Android 15 (targetSdk 35) apps are always drawn edge-to-edge under the
     * bars, so from Android 11 on the app goes edge-to-edge itself and pads its root view by
     * the insets; the padding shows the root's background, the color of the page header.
     * Older versions keep the classic layout, where the system fits the window between the bars.
     */
    @Suppress("DEPRECATION")
    private fun fitSystemBars(root: View) {
        val light = resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK != Configuration.UI_MODE_NIGHT_YES
        val bar = getColor(R.color.bar)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false)
            window.statusBarColor = Color.TRANSPARENT
            window.navigationBarColor = Color.TRANSPARENT
            val lightBars = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS or
                WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS
            window.insetsController?.setSystemBarsAppearance(if (light) lightBars else 0, lightBars)
            root.setOnApplyWindowInsetsListener { v, insets ->
                val i = insets.getInsets(
                    WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout() or WindowInsets.Type.ime()
                )
                v.setPadding(i.left, i.top, i.right, i.bottom)
                WindowInsets.CONSUMED
            }
        } else {
            window.statusBarColor = bar
            window.navigationBarColor = bar
            if (light) {
                window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR or
                    View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR
            }
        }
    }

    private fun versionLine(): String = getString(
        R.string.version_line, BuildConfig.VERSION_MAJOR, BuildConfig.VERSION_MINOR, BuildConfig.BUILD_NUMBER, BuildConfig.BUILT
    )

    /**
     * This app's version for the node's Info page (web/info.html reads window.ErlApp.info()).
     * Read-only; the page of any node in the mesh can see it.
     */
    private inner class AppInfo {
        @JavascriptInterface
        fun info(): String = JSONObject()
            .put("name", getString(R.string.app_name))
            .put("version", "${BuildConfig.VERSION_MAJOR}.${BuildConfig.VERSION_MINOR}")
            .put("major", BuildConfig.VERSION_MAJOR)
            .put("minor", BuildConfig.VERSION_MINOR)
            .put("build", BuildConfig.BUILD_NUMBER)
            .put("build_date", BuildConfig.BUILD_DATE)
            .put("build_time", BuildConfig.BUILD_TIME)
            .put("built", BuildConfig.BUILT)
            .put("android", "${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
            .toString()

        /** The Info page's "use the phone's GPS and compass" option */
        @JavascriptInterface
        fun phoneSensors(): Boolean = phoneSensors.enabled

        // Only the connected node's own page may switch it: pages of other mesh nodes can call
        // this interface too, and must not turn on sending the phone's location
        @JavascriptInterface
        fun setPhoneSensors(on: Boolean) {
            if (!ownNodePage) return
            runOnUiThread {
                phoneSensors.enabled = on
                if (on && !phoneSensors.hasLocationPermission()) {
                    requestPermissions(
                        arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION),
                        REQUEST_LOCATION
                    )
                }
            }
        }
    }

    private fun isNodeUrl(uri: Uri): Boolean {
        val host = uri.host ?: return false
        // the node itself, other nodes of the mesh (their pages link to 10.10.0.x)
        return host == "192.168.7.1" || host == "erl.lan" || host == tetherHost || host.startsWith("10.10.")
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun buildLayout(): FrameLayout {
        val dp = { v: Int -> TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), resources.displayMetrics).toInt() }

        webView = WebView(this).apply {
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            // the node is on a local link: never show a cached page that the node may not serve any
            // more (it would sit there with "–" everywhere while its data requests fail)
            settings.cacheMode = WebSettings.LOAD_NO_CACHE
            addJavascriptInterface(AppInfo(), "ErlApp")
            // without a WebChromeClient the WebView silently drops alert() / confirm(): confirm() returns
            // false, so e.g. "Delete run?" on the Test page never deletes. The default one shows the dialogs.
            webChromeClient = WebChromeClient()
            webViewClient = object : WebViewClient() {
                override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
                    if (isNodeUrl(request.url)) return false
                    // anything else (e.g. OpenStreetMap) opens outside, over the phone's normal network
                    startActivity(Intent(Intent.ACTION_VIEW, request.url))
                    return true
                }

                override fun onPageFinished(view: WebView, url: String) {
                    if (!pageLoaded && isNodeUrl(Uri.parse(url))) {
                        pageLoaded = true
                        failedLoads = 0
                        startPage = null
                        showPage()
                        healthFails = 0
                        handler.removeCallbacks(healthTask)
                        handler.postDelayed(healthTask, HEALTH_MS)
                    }
                    ChatWatchService.chatVisible = resumed && isChatPage(url)
                    val host = Uri.parse(url).host
                    ownNodePage = host == "192.168.7.1" || host == "erl.lan" || (host != null && host == tetherHost)
                }

                override fun onReceivedError(view: WebView, request: WebResourceRequest, error: WebResourceError) {
                    // only a failed page load matters; failed map tiles etc. are fine offline
                    if (request.isForMainFrame) {
                        pageLoaded = true   // do not let onPageFinished show the error page
                        retryLater()
                    }
                }
            }
        }

        status = TextView(this).apply {
            setTextColor(getColor(R.color.muted))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
            gravity = Gravity.CENTER
        }
        progress = ProgressBar(this).apply { isIndeterminate = true }
        val title = TextView(this).apply {
            text = getString(R.string.waiting_title)
            setTextColor(getColor(R.color.text))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
            setTypeface(typeface, Typeface.BOLD)
            gravity = Gravity.CENTER
        }
        val retry = Button(this).apply {
            text = getString(R.string.retry)
            // reload the page, or without a link search the tethering subnet again right now
            setOnClickListener { loadNode() }
        }

        diag = TextView(this).apply {
            setTextColor(getColor(R.color.muted))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
            gravity = Gravity.CENTER
            setTextIsSelectable(true)
        }

        val version = TextView(this).apply {
            text = versionLine()
            setTextColor(getColor(R.color.muted))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            gravity = Gravity.CENTER
        }

        panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(dp(32), dp(32), dp(32), dp(32))
            setBackgroundColor(getColor(R.color.bg))
            addView(title, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { bottomMargin = dp(12) })
            addView(status, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { bottomMargin = dp(20) })
            addView(progress, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply { bottomMargin = dp(20) })
            addView(retry, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
            addView(version, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { topMargin = dp(32) })
            addView(diag, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { topMargin = dp(12) })
        }

        return FrameLayout(this).apply {
            setBackgroundColor(getColor(R.color.bar))
            addView(webView, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
            addView(panel, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        }
    }
}
