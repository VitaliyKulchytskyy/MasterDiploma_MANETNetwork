package com.erl.node

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.hardware.GeomagneticField
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.location.GnssStatus
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Surface
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

/**
 * The phone's GPS and compass for an emergency unit built without them (GPS: or without a fix
 * yet): while the app is open and the option is on (Info page, [enabled]), send them to the node
 * (cgi-bin/phone); merld uses them and hands them on to the unit, which broadcasts them to its
 * peers. Every [FEATURE_CHECK_S] merld is asked which it wants, so only those sensors are turned
 * on (GPS costs battery).
 *
 * [nodeBase] is the node's address, e.g. "http://192.168.7.1/", null without a link; requests go
 * over the network the process is bound to (the USB link), like the WebView's.
 */
class PhoneSensors(private val activity: Activity, private val nodeBase: () -> String?) {

    companion object {
        private const val TAG = "ErlPhoneSensors"
        const val PREF_ENABLED = "phone_sensors"
        private const val FEATURE_GPS = 0
        private const val FEATURE_COMPASS = 1
        private const val FEATURE_CHECK_S = 10L
        const val REQUEST_LOCATION = 2                  // MainActivity restarts the sensors on the answer
        private const val POSITION_EVERY_MS = 10_000L      // or sooner after moving MOVE_M
        private const val MOVE_M = 5f
        private const val HEADING_EVERY_MS = 5_000L        // or sooner after turning TURN_DEG
        private const val TURN_DEG = 2.0
        private const val HEADING_MIN_GAP_MS = 500L
    }

    private val prefs = activity.getSharedPreferences(MainActivity.PREFS, Activity.MODE_PRIVATE)
    private val locations = activity.getSystemService(LocationManager::class.java)
    private val sensors = activity.getSystemService(SensorManager::class.java)
    private val net = Executors.newSingleThreadScheduledExecutor()
    private var check: ScheduledFuture<*>? = null
    private var running = false

    // what the unit lacks, from its feature list (main thread)
    private var gpsOn = false
    private var gpsProviders = emptyList<String>()     // the providers the listener is on
    private var askedPermission = false                 // once per start(): no dialog every 10 s
    private var compassOn = false

    @Volatile private var lastLocation: Location? = null
    // the phone's own GNSS receiver: satellites used in the fix / in view, -1 until it reports
    @Volatile private var satellitesUsed = -1
    @Volatile private var satellitesInView = -1
    private var sentLocation: Location? = null
    private var sentLocationAt = 0L
    private var sentHeading = Double.NaN
    private var sentHeadingAt = 0L

    var enabled: Boolean
        get() = prefs.getBoolean(PREF_ENABLED, false)
        set(value) {
            prefs.edit().putBoolean(PREF_ENABLED, value).apply()
            if (running) {
                stop()
                start()
            }
        }

    fun hasLocationPermission() =
        activity.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED ||
            activity.checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED

    /** From onResume: read the unit's features now and every FEATURE_CHECK_S while enabled. */
    fun start() {
        running = true
        askedPermission = false
        if (!enabled) return
        check = net.scheduleWithFixedDelay({ checkFeatures() }, 0, FEATURE_CHECK_S, TimeUnit.SECONDS)
    }

    /** From onPause: all sensors off. */
    fun stop() {
        running = false
        check?.cancel(false)
        check = null
        activity.runOnUiThread {
            setGps(false)
            setCompass(false)
        }
    }

    // ---- which sensors: the ones merld wants (missing feature, or GPS without a fix) ------

    private fun checkFeatures() {
        val base = nodeBase() ?: return
        val state = try {
            JSONObject(get("${base}cgi-bin/merl")).optJSONObject("state") ?: return
        } catch (e: Exception) {
            return
        }
        // merld says which sensors it wants: GPS also for a unit with GPS but no fix yet
        state.optJSONObject("phone")?.let { want ->
            activity.runOnUiThread {
                if (!running) return@runOnUiThread
                val wantGps = want.optBoolean("gps")
                // Location switched on after the listener was set up with no provider: again
                if (gpsOn && gpsProviders.isEmpty() && enabledProviders().isNotEmpty()) setGps(false)
                setGps(wantGps)
                setCompass(want.optBoolean("compass"))
                if (wantGps) {
                    if (!hasLocationPermission() && !askedPermission) {
                        askedPermission = true
                        activity.requestPermissions(
                            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION),
                            REQUEST_LOCATION
                        )
                    }
                    // why the node has no position from us, for its Info page
                    post("action=gps&state=${gpsState()}")
                    // no new fix (indoors): keep the node (and a restarted merld) on the last one
                    if (gpsOn) lastLocation?.let { sendLocation(it) }
                }
            }
            return
        }
        // an older merld: by the firmware's feature list
        val features = state.optJSONObject("features")
        // only a feature list that was read, and says the feature is missing
        val list = features?.takeIf { it.optBoolean("supported") }?.optJSONArray("list") ?: run {
            activity.runOnUiThread { setGps(false); setCompass(false) }
            return
        }
        val ids = (0 until list.length()).map { list.getJSONObject(it).getInt("id") }.toSet()
        activity.runOnUiThread {
            if (!running) return@runOnUiThread
            setGps(FEATURE_GPS !in ids)
            setCompass(FEATURE_COMPASS !in ids)
        }
    }

    // ---- GPS ------------------------------------------------------------------------------

    private val locationListener = LocationListener { location ->
        // a GPS fix wins over a network one that arrives later but is less accurate
        val last = lastLocation
        if (last != null && location.provider != last.provider && location.accuracy > last.accuracy &&
            location.time - last.time < POSITION_EVERY_MS
        ) return@LocationListener
        lastLocation = location
        sendLocation(location)
    }

    // At most every POSITION_EVERY_MS, or sooner after moving MOVE_M
    private fun sendLocation(location: Location) {
        val now = System.currentTimeMillis()
        val sent = sentLocation
        if (sent == null || now - sentLocationAt >= POSITION_EVERY_MS || location.distanceTo(sent) >= MOVE_M) {
            sentLocation = location
            sentLocationAt = now
            post(
                "action=position&lon=${fmt(location.longitude, 7)}&lat=${fmt(location.latitude, 7)}" +
                    "&acc=${fmt(location.accuracy.toDouble(), 0)}" +
                    (if (satellitesInView >= 0) "&used=$satellitesUsed&view=$satellitesInView" else "") +
                    // how old the fix itself is: a last known one may be hours old
                    "&age=${((System.currentTimeMillis() - location.time) / 1000).coerceAtLeast(0)}"
            )
        }
    }

    // Satellites of every constellation the phone receives (GPS, GLONASS, Galileo, BeiDou...)
    private val gnssCallback = object : GnssStatus.Callback() {
        override fun onSatelliteStatusChanged(status: GnssStatus) {
            satellitesInView = status.satelliteCount
            satellitesUsed = (0 until status.satelliteCount).count { status.usedInFix(it) }
        }

        override fun onStopped() {
            satellitesUsed = -1
            satellitesInView = -1
        }
    }

    private fun enabledProviders(): List<String> =
        (listOf(LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER) +
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) listOf(LocationManager.FUSED_PROVIDER) else emptyList())
            .filter { runCatching { locations.isProviderEnabled(it) }.getOrDefault(false) }

    // no-permission: location not allowed for the app; location-off: the phone's Location switch;
    // waiting: no position from any provider yet (indoors, no last known one); ok
    private fun gpsState(): String = when {
        !hasLocationPermission() -> "no-permission"
        enabledProviders().isEmpty() -> "location-off"
        lastLocation == null -> "waiting"
        else -> "ok"
    }

    // satellite status needs the precise location permission, not only the approximate one
    private fun hasFineLocation() =
        activity.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED

    @Suppress("MissingPermission")  // checked in hasLocationPermission()
    private fun setGps(on: Boolean) {
        val want = on && hasLocationPermission()
        if (want == gpsOn) return
        gpsOn = want
        if (want) {
            // the fused provider (Android 12+) also answers indoors, where GPS alone may never fix
            val used = enabledProviders()
            gpsProviders = used
            for (provider in used)
                locations.requestLocationUpdates(provider, 2000L, 0f, locationListener, Looper.getMainLooper())
            if (hasFineLocation())
                runCatching { locations.registerGnssStatusCallback(gnssCallback, Handler(Looper.getMainLooper())) }
            // send the last known position at once: a new fix can take long, or never come indoors
            if (lastLocation == null) lastKnown()
            lastLocation?.let { sendLocation(it) }
            Log.i(TAG, "unit wants the phone's position: providers $used, last known ${lastLocation != null}")
        } else {
            locations.removeUpdates(locationListener)
            locations.unregisterGnssStatusCallback(gnssCallback)
            gpsProviders = emptyList()
            satellitesUsed = -1
            satellitesInView = -1
        }
    }

    // ---- compass --------------------------------------------------------------------------

    private val rotation = FloatArray(9)
    private val remapped = FloatArray(9)
    private val orientation = FloatArray(3)

    private val compassListener = object : SensorEventListener {
        override fun onSensorChanged(event: SensorEvent) {
            SensorManager.getRotationMatrixFromVector(rotation, event.values)
            // the heading of the screen's top edge, whichever way the phone is turned
            val (x, y) = when (displayRotation()) {
                Surface.ROTATION_90 -> SensorManager.AXIS_Y to SensorManager.AXIS_MINUS_X
                Surface.ROTATION_180 -> SensorManager.AXIS_MINUS_X to SensorManager.AXIS_MINUS_Y
                Surface.ROTATION_270 -> SensorManager.AXIS_MINUS_Y to SensorManager.AXIS_X
                else -> SensorManager.AXIS_X to SensorManager.AXIS_Y
            }
            SensorManager.remapCoordinateSystem(rotation, x, y, remapped)
            SensorManager.getOrientation(remapped, orientation)
            var heading = Math.toDegrees(orientation[0].toDouble())
            // magnetic -> true north, with the phone's own position
            lastLocation?.let {
                heading += GeomagneticField(
                    it.latitude.toFloat(), it.longitude.toFloat(), it.altitude.toFloat(), System.currentTimeMillis()
                ).declination
            }
            heading = ((heading % 360) + 360) % 360

            val now = System.currentTimeMillis()
            val turned = sentHeading.isNaN() || angleDiff(heading, sentHeading) >= TURN_DEG
            if (now - sentHeadingAt >= HEADING_MIN_GAP_MS && (turned || now - sentHeadingAt >= HEADING_EVERY_MS)) {
                sentHeading = heading
                sentHeadingAt = now
                post("action=compass&heading=${fmt(heading, 1)}")
            }
        }

        override fun onAccuracyChanged(sensor: Sensor, accuracy: Int) {}
    }

    private fun setCompass(on: Boolean) {
        val sensor = sensors.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)
        val want = on && sensor != null
        if (want == compassOn) return
        compassOn = want
        if (want) {
            sensors.registerListener(compassListener, sensor, SensorManager.SENSOR_DELAY_UI)
            // true north needs a position: the last known one is enough
            if (hasLocationPermission() && lastLocation == null) lastKnown()
            Log.i(TAG, "unit has no compass: sending the phone's heading")
        } else {
            sensors.unregisterListener(compassListener)
        }
    }

    @Suppress("MissingPermission")
    private fun lastKnown() {
        lastLocation = locations.allProviders
            .mapNotNull { runCatching { locations.getLastKnownLocation(it) }.getOrNull() }
            .maxByOrNull { it.time }
    }

    @Suppress("DEPRECATION")
    private fun displayRotation(): Int =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) activity.display?.rotation ?: Surface.ROTATION_0
        else activity.windowManager.defaultDisplay.rotation

    // ---- to the node ----------------------------------------------------------------------

    private fun post(form: String) {
        val base = nodeBase() ?: return
        net.execute {
            try {
                val conn = URL("${base}cgi-bin/phone").openConnection() as HttpURLConnection
                conn.connectTimeout = 2000
                conn.readTimeout = 3000
                conn.requestMethod = "POST"
                conn.doOutput = true
                conn.outputStream.use { it.write(form.toByteArray()) }
                conn.inputStream.use { it.readBytes() }
                conn.disconnect()
            } catch (e: Exception) {
                Log.w(TAG, "sending to the node failed: ${e.message}")
            }
        }
    }

    private fun get(url: String): String {
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.connectTimeout = 2000
        conn.readTimeout = 3000
        return try {
            conn.inputStream.bufferedReader().use { it.readText() }
        } finally {
            conn.disconnect()
        }
    }

    private fun fmt(v: Double, decimals: Int): String =
        URLEncoder.encode(String.format(java.util.Locale.US, "%.${decimals}f", v), "UTF-8")

    private fun angleDiff(a: Double, b: Double): Double {
        val d = Math.abs(a - b) % 360
        return if (d > 180) 360 - d else d
    }
}
