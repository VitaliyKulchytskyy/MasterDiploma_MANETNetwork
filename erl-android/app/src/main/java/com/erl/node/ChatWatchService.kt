package com.erl.node

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Build
import android.os.IBinder
import android.util.Log
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit

/**
 * Keeps an eye on the node's chat while the app is in the background: every few seconds it
 * reads the chat history (cgi-bin/chat) and posts a notification for each new incoming
 * message; a message starting with "SOS" (the firmware's rule) goes to a high-priority channel.
 *
 * A foreground service (Android shows its "watching" notification, with a Stop action), so it
 * keeps running with the screen off. It reaches the node like the app does: over the USB
 * Ethernet link (its own network request, requests bound to that network), or at the address
 * TetherScan found while the phone's Ethernet tethering is on.
 */
class ChatWatchService : Service() {

    companion object {
        private const val TAG = "ErlChatWatch"
        private const val POLL_S = 5L
        private const val NODE_BASE = "http://192.168.7.1"
        private const val CHANNEL_WATCH = "watch"
        private const val CHANNEL_CHAT = "chat"
        private const val CHANNEL_SOS = "sos"
        private const val ID_WATCH = 1
        private const val PREF_LAST_SEEN = "chat_last_seen_id"
        const val ACTION_STOP = "com.erl.node.STOP_WATCH"
        const val EXTRA_PAGE = "page"

        /** The chat page is on screen: no notifications for ordinary messages (SOS still). */
        @Volatile
        var chatVisible = false

        fun start(context: Context) {
            context.startForegroundService(Intent(context, ChatWatchService::class.java))
        }

        fun isSos(text: String) = text.trimStart().startsWith("sos", ignoreCase = true)

        /** Link-test messages from the Test page (erl-exp): "EXP <run> <seq>/<n>" and their echoes. */
        private val TEST_MESSAGE = Regex("""^EXP(-ACK)? \S+ \d+""")
        fun isTest(text: String) = TEST_MESSAGE.containsMatchIn(text)
    }

    private lateinit var connectivity: ConnectivityManager
    private lateinit var notifications: NotificationManager
    private lateinit var poller: ScheduledExecutorService

    @Volatile
    private var usbNetwork: Network? = null
    private var lastSeen = -1L          // newest chat id already handled, -1: not read yet

    private val networkCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) {
            usbNetwork = network
        }

        override fun onLost(network: Network) {
            if (usbNetwork == network) usbNetwork = null
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        connectivity = getSystemService(ConnectivityManager::class.java)
        notifications = getSystemService(NotificationManager::class.java)
        createChannels()

        val watching = Notification.Builder(this, CHANNEL_WATCH)
            .setSmallIcon(android.R.drawable.stat_notify_chat)
            .setContentTitle(getString(R.string.watch_title))
            .setContentText(getString(R.string.watch_text))
            .setContentIntent(openApp(null, 0))
            .addAction(
                Notification.Action.Builder(
                    null, getString(R.string.watch_stop),
                    PendingIntent.getService(
                        this, 0, Intent(this, ChatWatchService::class.java).setAction(ACTION_STOP),
                        PendingIntent.FLAG_IMMUTABLE
                    )
                ).build()
            )
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(ID_WATCH, watching, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(ID_WATCH, watching)
        }

        lastSeen = getSharedPreferences(MainActivity.PREFS, MODE_PRIVATE).getLong(PREF_LAST_SEEN, -1L)
        connectivity.requestNetwork(
            NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build(),
            networkCallback
        )
        poller = Executors.newSingleThreadScheduledExecutor()
        poller.scheduleWithFixedDelay({ poll() }, 1, POLL_S, TimeUnit.SECONDS)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        return START_STICKY
    }

    override fun onDestroy() {
        poller.shutdownNow()
        connectivity.unregisterNetworkCallback(networkCallback)
        super.onDestroy()
    }

    private fun createChannels() {
        notifications.createNotificationChannel(
            NotificationChannel(CHANNEL_WATCH, getString(R.string.channel_watch), NotificationManager.IMPORTANCE_MIN)
        )
        notifications.createNotificationChannel(
            NotificationChannel(CHANNEL_CHAT, getString(R.string.channel_chat), NotificationManager.IMPORTANCE_DEFAULT)
        )
        notifications.createNotificationChannel(
            NotificationChannel(CHANNEL_SOS, getString(R.string.channel_sos), NotificationManager.IMPORTANCE_HIGH).apply {
                enableVibration(true)
                vibrationPattern = longArrayOf(0, 600, 300, 600, 300, 600)
            }
        )
    }

    // ---- polling --------------------------------------------------------------------------

    /** The chat as JSON from the node, over the USB link or the tethering address; null if unreachable. */
    private fun fetchChat(): JSONObject? {
        val network = usbNetwork
        val tetherHost = getSharedPreferences(MainActivity.PREFS, MODE_PRIVATE)
            .getString(MainActivity.PREF_TETHER_HOST, null)
        val url = when {
            network != null -> URL("$NODE_BASE/cgi-bin/chat?limit=50")
            tetherHost != null -> URL("http://$tetherHost/cgi-bin/chat?limit=50")
            else -> return null
        }
        val conn = (network?.openConnection(url) ?: url.openConnection()) as HttpURLConnection
        conn.connectTimeout = 2000
        conn.readTimeout = 3000
        return try {
            JSONObject(conn.inputStream.bufferedReader().use { it.readText() }).takeIf { it.optBoolean("ok") }
        } catch (e: Exception) {
            null
        } finally {
            conn.disconnect()
        }
    }

    private fun poll() {
        try {
            val chat = fetchChat() ?: return
            val lastId = chat.optLong("last_id", 0)
            // first contact, or another node / a fresh history (ids went back): start from here,
            // without replaying the history as notifications
            if (lastSeen < 0 || lastId < lastSeen) {
                saveLastSeen(lastId)
                return
            }
            val messages = chat.getJSONArray("messages")
            for (i in 0 until messages.length()) {
                val m = messages.getJSONObject(i)
                val id = m.getLong("id")
                if (id <= lastSeen || m.getString("dir") != "in") continue
                notifyMessage(id, m)
            }
            if (lastId > lastSeen) saveLastSeen(lastId)
        } catch (e: Exception) {
            Log.w(TAG, "poll failed", e)
        }
    }

    private fun saveLastSeen(id: Long) {
        lastSeen = id
        getSharedPreferences(MainActivity.PREFS, MODE_PRIVATE).edit().putLong(PREF_LAST_SEEN, id).apply()
    }

    private fun notifyMessage(id: Long, m: JSONObject) {
        val text = m.getString("text")
        val from = m.getString("from")
        val sos = isSos(text)
        if (!sos && chatVisible) return
        if (isTest(text)) return            // dozens per test point: no notification for each

        val title = when {
            sos -> getString(R.string.sos_from, from)
            m.optBoolean("broadcast") -> getString(R.string.message_broadcast, from)
            else -> from
        }
        // an SOS opens the peers map (the sender is marked there), a message the chat
        val page = if (sos) "/peers.html" else "/chat.html"
        val notification = Notification.Builder(this, if (sos) CHANNEL_SOS else CHANNEL_CHAT)
            .setSmallIcon(if (sos) android.R.drawable.stat_sys_warning else android.R.drawable.stat_notify_chat)
            .setContentTitle(title)
            .setContentText(text)
            .setStyle(Notification.BigTextStyle().bigText(text))
            .setCategory(if (sos) Notification.CATEGORY_ALARM else Notification.CATEGORY_MESSAGE)
            .setWhen(m.optLong("time") * 1000)
            .setShowWhen(true)
            .setAutoCancel(true)
            .setContentIntent(openApp(page, id.toInt()))
            .build()
        try {
            notifications.notify(1000 + (id % 100000).toInt(), notification)
        } catch (e: SecurityException) {
            Log.w(TAG, "notifications not allowed", e)   // POST_NOTIFICATIONS denied
        }
    }

    private fun openApp(page: String?, requestCode: Int): PendingIntent = PendingIntent.getActivity(
        this, requestCode,
        Intent(this, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP)
            .apply { if (page != null) putExtra(EXTRA_PAGE, page) },
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
    )
}
