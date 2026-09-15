package com.port80.app.discovery

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import android.os.BatteryManager
import com.port80.app.data.model.StreamState
import com.port80.app.data.model.StreamStats
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import javax.inject.Inject
import javax.inject.Singleton

/** Sends lightweight, non-audio camera health telemetry to a paired OBS dock. */
@Singleton
class CameraHealthReporter @Inject constructor(
    @ApplicationContext private val context: Context
) {
    companion object {
        private const val PREFIX = "VYSHEALTH1"
    }

    suspend fun report(
        destinations: List<DiscoveredObs>,
        streamState: StreamState,
        stats: StreamStats,
        microphoneMuted: Boolean
    ) = withContext(Dispatchers.IO) {
        if (destinations.isEmpty()) return@withContext
        val battery = context.getSystemService(BatteryManager::class.java)
            ?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY)
            ?.coerceIn(0, 100) ?: -1
        val (network, signal) = networkHealth()
        val state = when (streamState) {
            StreamState.Idle -> "READY"
            is StreamState.Previewing -> "PREVIEW"
            StreamState.Connecting -> "CONNECTING"
            is StreamState.Live -> "LIVE"
            is StreamState.Reconnecting -> "RECONNECTING"
            StreamState.Stopping -> "STOPPING"
            is StreamState.Stopped -> "STOPPED"
        }
        DatagramSocket().use { socket ->
            destinations.distinctBy { "${it.address}:${it.pairingToken}" }.forEach { obs ->
                val fields = listOf(
                    PREFIX,
                    obs.pairingToken,
                    state,
                    network,
                    signal.toString(),
                    stats.videoBitrateKbps.toString(),
                    // SRT configured latency; -1 means unavailable for a non-SRT profile.
                    if (stats.protocol?.name == "SRT") "200" else "-1",
                    "-1", // Packet-loss telemetry is not exposed by the current encoder library.
                    stats.fps.toInt().toString(),
                    stats.droppedFrames.toString(),
                    battery.toString(),
                    stats.thermalLevel.name,
                    stats.resolution.replace('|', '-'),
                    stats.videoCodec?.name.orEmpty(),
                    if (stats.isRecording) "1" else "0",
                    if (microphoneMuted) "1" else "0",
                    System.currentTimeMillis().toString()
                )
                val bytes = fields.joinToString("|").toByteArray(Charsets.UTF_8)
                runCatching {
                    socket.send(
                        DatagramPacket(
                            bytes,
                            bytes.size,
                            InetAddress.getByName(obs.address),
                            ObsDiscoveryClient.DISCOVERY_PORT
                        )
                    )
                }
            }
        }
    }

    @Suppress("DEPRECATION")
    private fun networkHealth(): Pair<String, Int> {
        val connectivity = context.getSystemService(ConnectivityManager::class.java)
        val capabilities = connectivity?.getNetworkCapabilities(connectivity.activeNetwork)
            ?: return "OFFLINE" to -1
        return when {
            capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> {
                val wifi = context.applicationContext
                    .getSystemService(Context.WIFI_SERVICE) as? WifiManager
                val rssi = wifi?.connectionInfo?.rssi ?: -127
                "WIFI" to rssi
            }
            capabilities.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) -> "CELLULAR" to -1
            capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) -> "ETHERNET" to 100
            else -> "NETWORK" to -1
        }
    }
}
