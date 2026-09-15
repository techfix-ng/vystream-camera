package com.port80.app.discovery

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.NetworkInterface
import javax.inject.Inject
import javax.inject.Singleton
import com.port80.app.audio.TalkbackManager

data class DiscoveredObs(
    val name: String,
    val assignedCameraName: String,
    val address: String,
    val srtPort: Int,
    val pairingToken: String,
    /** Stable desktop identity. Unlike IP/port, this survives DHCP changes. */
    val receiverId: String = "$address:$srtPort",
    val platform: String = "unknown",
    val application: String = "obs",
    val pluginVersion: String = ""
)

/**
 * Discovers the companion OBS plugin on the local network.
 * Protocol v2 uses a small UDP broadcast handshake; stream credentials are
 * returned only as a short-lived pairing token and never written to logs.
 */
@Singleton
class ObsDiscoveryClient @Inject constructor(
    @ApplicationContext private val context: Context
) {
    companion object {
        const val DISCOVERY_PORT = 45990
        private const val REQUEST_PREFIX = "OBS_SRT_DISCOVER_V4|"
        private const val RESPONSE_PREFIX_V4 = "OBS_SRT_OFFER_V4|"
        private const val RESPONSE_PREFIX_V3 = "OBS_SRT_OFFER_V3|"

        internal fun parseOffer(
            message: String,
            sourceAddress: String,
            suggestedName: String
        ): DiscoveredObs? {
            val fields = message.split('|')
            return when {
                message.startsWith(RESPONSE_PREFIX_V4) && fields.size >= 10 -> {
                    val receiverId = fields[1].takeIf { it.isNotBlank() } ?: return null
                    val port = fields[4].toIntOrNull()?.takeIf { it in 1..65535 } ?: return null
                    DiscoveredObs(
                        receiverId = receiverId,
                        name = fields[2].ifBlank { "OBS Studio" },
                        assignedCameraName = fields[6].ifBlank { suggestedName },
                        address = sourceAddress,
                        srtPort = port,
                        pairingToken = fields[5],
                        platform = fields[7].ifBlank { "unknown" },
                        application = fields[8].ifBlank { "obs" },
                        pluginVersion = fields[9]
                    )
                }
                message.startsWith(RESPONSE_PREFIX_V3) && fields.size >= 6 -> {
                    val port = fields[3].toIntOrNull()?.takeIf { it in 1..65535 } ?: return null
                    DiscoveredObs(
                        name = fields[1].ifBlank { "OBS Studio" },
                        assignedCameraName = fields[5].ifBlank { suggestedName },
                        address = sourceAddress,
                        srtPort = port,
                        pairingToken = fields[4],
                        receiverId = "$sourceAddress:$port"
                    )
                }
                else -> null
            }
        }
    }

    private val identityPreferences by lazy {
        context.getSharedPreferences("obs_device_identity", Context.MODE_PRIVATE)
    }

    private fun persistentDeviceId(): String {
        val existing = identityPreferences.getString("device_id", null)
        if (!existing.isNullOrBlank()) return existing
        val created = java.util.UUID.randomUUID().toString()
        identityPreferences.edit().putString("device_id", created).apply()
        return created
    }

    suspend fun discover(timeoutMs: Int = 5_000): List<DiscoveredObs> = withContext(Dispatchers.IO) {
        val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
        val lock = wifi?.createMulticastLock("obs-srt-discovery")?.apply {
            setReferenceCounted(false)
            acquire()
        }
        try {
            DatagramSocket().use { socket ->
                socket.broadcast = true
                socket.soTimeout = 100
                val suggestedName = Build.MODEL.replace('|', '-').ifBlank { "Android Camera" }
                val request = "$REQUEST_PREFIX${persistentDeviceId()}|$suggestedName|${TalkbackManager.PORT}"
                    .toByteArray(Charsets.UTF_8)
                val deadline = System.currentTimeMillis() + timeoutMs
                val results = linkedMapOf<String, DiscoveredObs>()
                val startedAt = System.currentTimeMillis()
                val rapidProbeScheduleMs = longArrayOf(0L, 120L, 300L, 650L, 1_200L, 2_000L, 3_200L)
                var probeIndex = 0
                var lastResultAt = 0L
                while (System.currentTimeMillis() < deadline) {
                    val now = System.currentTimeMillis()
                    if (probeIndex < rapidProbeScheduleMs.size &&
                        now - startedAt >= rapidProbeScheduleMs[probeIndex]
                    ) {
                        discoveryTargets().forEach { address ->
                            runCatching {
                                socket.send(DatagramPacket(request, request.size, address, DISCOVERY_PORT))
                            }
                        }
                        probeIndex++
                    }
                    try {
                        val buffer = ByteArray(1024)
                        val packet = DatagramPacket(buffer, buffer.size)
                        socket.receive(packet)
                        val message = String(packet.data, 0, packet.length, Charsets.UTF_8)
                        val address = packet.address.hostAddress ?: continue
                        val offer = parseOffer(message, address, suggestedName) ?: continue
                        // V4 is keyed by the persistent plugin installation UUID. V3
                        // remains keyed by IP:port so existing plugins keep working.
                        results[offer.receiverId] = offer
                        lastResultAt = System.currentTimeMillis()
                    } catch (_: java.net.SocketTimeoutException) {
                        // Rapid retries cover normal Wi-Fi and phone-hosted hotspots.
                    }
                    // Do not make the UI wait for the whole timeout after OBS replies.
                    // A short settle window still allows multiple OBS computers to answer.
                    if (results.isNotEmpty() && System.currentTimeMillis() - lastResultAt >= 350L) break
                }
                results.values.toList()
            }
        } finally {
            if (lock?.isHeld == true) lock.release()
        }
    }

    /** Global + directed broadcasts work on normal Wi-Fi and phone-hosted hotspots. */
    private fun discoveryTargets(): Set<InetAddress> {
        val targets = linkedSetOf(InetAddress.getByName("255.255.255.255"))
        runCatching {
            NetworkInterface.getNetworkInterfaces().toList()
                .filter { it.isUp && !it.isLoopback }
                .flatMap { it.interfaceAddresses }
                .mapNotNullTo(targets) { it.broadcast }
        }
        return targets
    }
}
