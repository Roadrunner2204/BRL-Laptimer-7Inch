package com.brl.telemetry.network

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.util.Base64
import android.util.Log
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.bridge.ReadableMap
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors
import javax.net.ssl.HttpsURLConnection

/**
 * BrlNetwork — dual-network support for parallel WiFi (laptimer AP) + cellular.
 *
 * The problem: Android routes all fetch() through the "validated" default
 * network. Our laptimer AP returns HTTP 204 to the captive-portal probe so
 * Android keeps WiFi connected — but that means map tiles / Nominatim also
 * go through WiFi and fail (no real internet).
 *
 * This module acquires BOTH transports simultaneously and exposes explicit
 * per-request binding + a process-default toggle:
 *
 *   preferCellularDefault()  — process default = cellular (for the WebView
 *                              map tiles to load via mobile data while the
 *                              laptimer WiFi is still connected).
 *   preferWifiDefault()      — explicit (rarely needed; default after unbind).
 *   unbindDefault()          — restore system routing.
 *   fetchViaWifi(url, opts)  — explicit WiFi request (display API calls).
 *   fetchViaCellular(url, …) — explicit cellular request.
 *
 * Networks are held via requestNetwork() callbacks for the module lifetime,
 * so both stay active regardless of the default routing choice.
 */
class BrlNetworkModule(private val ctx: ReactApplicationContext) :
    ReactContextBaseJavaModule(ctx) {

    private val TAG = "BrlNetwork"
    private val cm: ConnectivityManager by lazy {
        ctx.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    }
    private val executor = Executors.newCachedThreadPool()

    private var wifiNetwork: Network? = null
    private var cellularNetwork: Network? = null
    private var wifiCallback: ConnectivityManager.NetworkCallback? = null
    private var cellularCallback: ConnectivityManager.NetworkCallback? = null

    override fun getName(): String = "BrlNetwork"

    override fun initialize() {
        super.initialize()
        requestBoth()
    }

    override fun invalidate() {
        try {
            wifiCallback?.let { cm.unregisterNetworkCallback(it) }
            cellularCallback?.let { cm.unregisterNetworkCallback(it) }
        } catch (_: Throwable) { /* best effort */ }
        wifiCallback = null
        cellularCallback = null
        wifiNetwork = null
        cellularNetwork = null
        try { cm.bindProcessToNetwork(null) } catch (_: Throwable) {}
        super.invalidate()
    }

    // ── Network acquisition ────────────────────────────────────────────────
    private fun requestBoth() {
        // WiFi
        val wifiReq = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .build()
        val wcb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                wifiNetwork = network
                Log.i(TAG, "WiFi network available")
            }
            override fun onLost(network: Network) {
                if (wifiNetwork == network) wifiNetwork = null
                Log.i(TAG, "WiFi network lost")
            }
        }
        wifiCallback = wcb
        try { cm.requestNetwork(wifiReq, wcb) } catch (e: Throwable) {
            Log.w(TAG, "requestNetwork(WIFI) failed: ${e.message}")
        }

        // Cellular (must ALSO request INTERNET capability, else the system
        // won't keep it active when WiFi is also connected).
        val cellReq = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()
        val ccb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                cellularNetwork = network
                Log.i(TAG, "Cellular network available")
            }
            override fun onLost(network: Network) {
                if (cellularNetwork == network) cellularNetwork = null
                Log.i(TAG, "Cellular network lost")
            }
        }
        cellularCallback = ccb
        try { cm.requestNetwork(cellReq, ccb) } catch (e: Throwable) {
            Log.w(TAG, "requestNetwork(CELLULAR) failed: ${e.message}")
        }
    }

    // ── Default-routing toggles ───────────────────────────────────────────
    @ReactMethod
    fun preferCellularDefault(promise: Promise) {
        val n = cellularNetwork
        if (n == null) { promise.reject("NO_CELLULAR", "cellular network not available"); return }
        try { cm.bindProcessToNetwork(n); promise.resolve(true) }
        catch (e: Throwable) { promise.reject("BIND_FAIL", e) }
    }

    @ReactMethod
    fun preferWifiDefault(promise: Promise) {
        val n = wifiNetwork
        if (n == null) { promise.reject("NO_WIFI", "wifi network not available"); return }
        try { cm.bindProcessToNetwork(n); promise.resolve(true) }
        catch (e: Throwable) { promise.reject("BIND_FAIL", e) }
    }

    @ReactMethod
    fun unbindDefault(promise: Promise) {
        try { cm.bindProcessToNetwork(null); promise.resolve(true) }
        catch (e: Throwable) { promise.reject("UNBIND_FAIL", e) }
    }

    @ReactMethod
    fun getState(promise: Promise) {
        val map = Arguments.createMap()
        map.putBoolean("wifiAvailable", wifiNetwork != null)
        map.putBoolean("cellularAvailable", cellularNetwork != null)
        promise.resolve(map)
    }

    // ── Explicit per-request fetches ──────────────────────────────────────
    @ReactMethod
    fun fetchViaWifi(url: String, opts: ReadableMap?, promise: Promise) {
        fetchOn(wifiNetwork, "WIFI", url, opts, promise)
    }

    @ReactMethod
    fun fetchViaCellular(url: String, opts: ReadableMap?, promise: Promise) {
        fetchOn(cellularNetwork, "CELLULAR", url, opts, promise)
    }

    private fun fetchOn(network: Network?, tag: String, url: String,
                        opts: ReadableMap?, promise: Promise) {
        if (network == null) {
            promise.reject("NO_NETWORK", "$tag network not available")
            return
        }
        executor.execute {
            try {
                val u = URL(url)
                val conn = network.openConnection(u) as HttpURLConnection
                conn.connectTimeout = opts?.takeIfInt("connectTimeoutMs") ?: 15_000
                conn.readTimeout    = opts?.takeIfInt("readTimeoutMs") ?: 30_000
                conn.requestMethod  = opts?.takeIfString("method") ?: "GET"
                conn.doInput        = true

                val headers = opts?.getMap("headers")
                if (headers != null) {
                    val it = headers.keySetIterator()
                    while (it.hasNextKey()) {
                        val k = it.nextKey()
                        val v = headers.getString(k)
                        if (v != null) conn.setRequestProperty(k, v)
                    }
                }

                val body = opts?.takeIfString("body")
                if (body != null) {
                    conn.doOutput = true
                    conn.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
                }

                val status = conn.responseCode
                val stream = if (status in 200..299) conn.inputStream else conn.errorStream
                val bytes  = stream?.readBytes() ?: ByteArray(0)

                val result = Arguments.createMap()
                result.putInt("status", status)
                // Base64 so binary tile PNGs round-trip through the JS bridge.
                result.putString("bodyBase64",
                    Base64.encodeToString(bytes, Base64.NO_WRAP))
                val hdrs = Arguments.createMap()
                for ((k, vs) in conn.headerFields) {
                    if (k != null && vs != null && vs.isNotEmpty())
                        hdrs.putString(k, vs.joinToString(", "))
                }
                result.putMap("headers", hdrs)
                conn.disconnect()
                promise.resolve(result)
            } catch (e: Throwable) {
                Log.w(TAG, "fetchOn($tag) $url failed: ${e.message}")
                promise.reject("FETCH_FAIL", e)
            }
        }
    }

    // ── ReadableMap helpers ───────────────────────────────────────────────
    private fun ReadableMap.takeIfString(k: String): String? =
        if (hasKey(k) && !isNull(k)) getString(k) else null
    private fun ReadableMap.takeIfInt(k: String): Int? =
        if (hasKey(k) && !isNull(k)) getInt(k) else null
}
