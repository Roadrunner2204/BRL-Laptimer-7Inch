package expo.modules.wifibinder

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition

/**
 * Forces ALL socket operations in this process to go through the WiFi
 * interface, bypassing Android's default-network routing.
 *
 * Android marks AP-only networks as "no internet" and routes OkHttp
 * (React Native fetch) through mobile data even when WiFi is connected.
 * bindProcessToNetwork(wifiNetwork) overrides this per-process.
 */
class WifiBinderModule : Module() {
    override fun definition() = ModuleDefinition {
        Name("WifiBinder")

        // Bind every socket in this process to the WiFi network.
        // Call before fetch() to ensure requests reach the local AP.
        AsyncFunction("bindToWifi") {
            val cm = connectivityManager() ?: return@AsyncFunction false
            val wifi = cm.allNetworks.firstOrNull { net ->
                val caps = cm.getNetworkCapabilities(net) ?: return@firstOrNull false
                caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
            } ?: return@AsyncFunction false
            cm.bindProcessToNetwork(wifi)
        }

        // Release the WiFi binding so normal routing resumes
        // (important for map tiles / internet features).
        AsyncFunction("unbind") {
            connectivityManager()?.bindProcessToNetwork(null)
        }
    }

    private fun connectivityManager(): ConnectivityManager? {
        val ctx = appContext.reactContext ?: return null
        return ctx.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
    }
}
