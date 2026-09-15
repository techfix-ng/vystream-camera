package com.port80.app.audio

import android.content.Context
import android.Manifest
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.media.AudioRecord
import android.media.MediaRecorder
import android.media.AudioDeviceInfo
import android.os.Build
import androidx.core.content.ContextCompat
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.SocketException
import java.net.InetAddress
import java.util.concurrent.atomic.AtomicBoolean
import javax.inject.Inject
import javax.inject.Singleton

/** Independent director-to-camera intercom receiver. It never touches encoder audio. */
@Singleton
class TalkbackManager @Inject constructor(
    @ApplicationContext private val context: Context
) {
    companion object {
        const val PORT = 46010
        const val RETURN_PORT = 46011
        private const val DIRECTOR_PREFIX = "VYSTB1|"
        private const val CREW_PREFIX = "VYSTB2|"
        private const val TALLY_PREFIX = "VYSTALLY1|"
        private const val SAMPLE_RATE = 16_000
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val preferences = context.getSharedPreferences("talkback_preferences", Context.MODE_PRIVATE)
    private val allowedTokens = MutableStateFlow<Set<String>>(emptySet())
    private val _receiving = MutableStateFlow(false)
    val receiving: StateFlow<Boolean> = _receiving.asStateFlow()
    private val _speakerName = MutableStateFlow("Director")
    val speakerName: StateFlow<String> = _speakerName.asStateFlow()
    private val _muted = MutableStateFlow(false)
    val muted: StateFlow<Boolean> = _muted.asStateFlow()
    private val _volume = MutableStateFlow(preferences.getFloat("volume", 0.8f))
    val volume: StateFlow<Float> = _volume.asStateFlow()
    @Volatile private var lastPacketAt = 0L
    @Volatile private var lastTallyAt = 0L
    private val _replying = MutableStateFlow(false)
    val replying: StateFlow<Boolean> = _replying.asStateFlow()
    @Volatile private var returnHost: String? = null
    @Volatile private var returnToken: String? = null
    @Volatile private var lastEncoderPcmAt = 0L
    private val standbyCaptureRunning = AtomicBoolean(false)
    private val _programMuted = MutableStateFlow(false)
    private val _tallyState = MutableStateFlow("STANDBY")
    /** OBS programme tally: LIVE when this camera is on programme, otherwise STANDBY. */
    val tallyState: StateFlow<String> = _tallyState.asStateFlow()
    private val returnSocket by lazy { DatagramSocket() }

    init {
        scope.launch { receiveLoop() }
        scope.launch {
            while (isActive) {
                _receiving.value = System.currentTimeMillis() - lastPacketAt < 350
                if (System.currentTimeMillis() - lastTallyAt > 3_000) _tallyState.value = "STANDBY"
                delay(100)
            }
        }
    }

    fun setAllowedTokens(tokens: Set<String>) { allowedTokens.value = tokens.filter { it.isNotBlank() }.toSet() }
    fun setMuted(value: Boolean) { _muted.value = value }
    fun toggleMuted() { _muted.value = !_muted.value }
    fun setVolume(value: Float) {
        val normalized = value.coerceIn(0f, 1f)
        _volume.value = normalized
        preferences.edit().putFloat("volume", normalized).apply()
    }
    fun setReplying(value: Boolean) {
        _replying.value = value
        if (value && System.currentTimeMillis() - lastEncoderPcmAt > 350) {
            scope.launch { standbyReplyLoop() }
        }
    }
    fun setProgramMuted(value: Boolean) { _programMuted.value = value }
    fun configureReturn(host: String?, token: String?) {
        returnHost = host?.takeIf { it.isNotBlank() }
        returnToken = token?.takeIf { it.isNotBlank() }
    }

    /** Sends a 16 kHz mono copy to OBS while preserving the original encoder buffer. */
    fun sendReturnPcm(pcm: ByteArray, sampleRate: Int, stereo: Boolean): Boolean {
        lastEncoderPcmAt = System.currentTimeMillis()
        if (_replying.value) sendResampledPcm(pcm, sampleRate, stereo)
        // Director downlink stays out of the encoder by acoustic routing, not by
        // punching silence into programme audio. Reply still uses the same physical
        // microphone, so it must remain isolated from the programme track.
        return _programMuted.value || _replying.value
    }

    private fun sendResampledPcm(pcm: ByteArray, sampleRate: Int, stereo: Boolean) {
        if (!_replying.value) return
        val host = returnHost ?: return
        val token = returnToken ?: return
        if (sampleRate <= 0 || pcm.size < 2) return
        val channels = if (stereo) 2 else 1
        val sourceFrames = pcm.size / (2 * channels)
        if (sourceFrames <= 0) return
        val ratio = sampleRate.toDouble() / SAMPLE_RATE
        val outputFrames = (sourceFrames / ratio).toInt().coerceAtLeast(1)
        val mono = ByteArray(outputFrames * 2)
        for (outFrame in 0 until outputFrames) {
            val sourceFrame = (outFrame * ratio).toInt().coerceAtMost(sourceFrames - 1)
            val byteIndex = sourceFrame * channels * 2
            var sample = ((pcm[byteIndex + 1].toInt() shl 8) or (pcm[byteIndex].toInt() and 0xff)).toShort().toInt()
            if (channels == 2 && byteIndex + 3 < pcm.size) {
                val right = ((pcm[byteIndex + 3].toInt() shl 8) or (pcm[byteIndex + 2].toInt() and 0xff)).toShort().toInt()
                sample = (sample + right) / 2
            }
            mono[outFrame * 2] = (sample and 0xff).toByte()
            mono[outFrame * 2 + 1] = ((sample shr 8) and 0xff).toByte()
        }
        runCatching {
            val address = InetAddress.getByName(host)
            val header = "VYSUP1|$token|".toByteArray(Charsets.UTF_8)
            var offset = 0
            while (offset < mono.size) {
                val count = minOf(960, mono.size - offset)
                val payload = ByteArray(header.size + count)
                header.copyInto(payload)
                mono.copyInto(payload, header.size, offset, offset + count)
                returnSocket.send(DatagramPacket(payload, payload.size, address, RETURN_PORT))
                offset += count
            }
        }
    }

    /** Mic path used only before the stream/recorder encoder owns microphone capture. */
    private suspend fun standbyReplyLoop() {
        if (!standbyCaptureRunning.compareAndSet(false, true)) return
        var recorder: AudioRecord? = null
        try {
            if (ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) !=
                PackageManager.PERMISSION_GRANTED) return
            val minimum = AudioRecord.getMinBufferSize(
                SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT
            )
            recorder = AudioRecord(
                MediaRecorder.AudioSource.VOICE_COMMUNICATION,
                SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                maxOf(minimum, 3_200)
            )
            if (recorder.state != AudioRecord.STATE_INITIALIZED) return
            recorder.startRecording()
            val buffer = ByteArray(640)
            while (scope.isActive && _replying.value &&
                System.currentTimeMillis() - lastEncoderPcmAt > 350) {
                val count = recorder.read(buffer, 0, buffer.size, AudioRecord.READ_BLOCKING)
                if (count > 0) sendMono16k(buffer, count)
            }
        } catch (_: Throwable) {
            // Encoder capture can take ownership while standby is closing; retry on next press.
        } finally {
            runCatching { recorder?.stop() }
            recorder?.release()
            standbyCaptureRunning.set(false)
        }
    }

    private fun sendMono16k(pcm: ByteArray, length: Int) {
        val host = returnHost ?: return
        val token = returnToken ?: return
        runCatching {
            val address = InetAddress.getByName(host)
            val header = "VYSUP1|$token|".toByteArray(Charsets.UTF_8)
            var offset = 0
            while (offset < length) {
                val count = minOf(960, length - offset)
                val payload = ByteArray(header.size + count)
                header.copyInto(payload)
                pcm.copyInto(payload, header.size, offset, offset + count)
                returnSocket.send(DatagramPacket(payload, payload.size, address, RETURN_PORT))
                offset += count
            }
        }
    }

    private fun newTrack(): AudioTrack {
        val minimum = AudioTrack.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        )
        val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
        runCatching {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                audioManager.availableCommunicationDevices
                    .firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE }
                    ?.let { audioManager.setCommunicationDevice(it) }
            } else {
                @Suppress("DEPRECATION")
                audioManager.isSpeakerphoneOn = false
            }
        }
        return AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(SAMPLE_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .build()
            )
            .setBufferSizeInBytes(maxOf(minimum, 6_400))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
    }

    private suspend fun receiveLoop() {
        while (scope.isActive) {
            var track: AudioTrack? = null
            try {
                DatagramSocket(PORT).use { socket ->
                    socket.receiveBufferSize = 65_536
                    track = newTrack().also { it.play() }
                    val buffer = ByteArray(1_500)
                    while (scope.isActive) {
                        val packet = DatagramPacket(buffer, buffer.size)
                        socket.receive(packet)
                        val text = String(buffer, 0, packet.length, Charsets.UTF_8)
                        if (text.startsWith(TALLY_PREFIX)) {
                            val fields = text.split('|')
                            if (fields.size >= 3 && fields[1] in allowedTokens.value) {
                                _tallyState.value = if (fields[2] == "LIVE") "LIVE" else "STANDBY"
                                lastTallyAt = System.currentTimeMillis()
                            }
                            continue
                        }
                        val crewPacket = packet.length > CREW_PREFIX.length &&
                            String(buffer, 0, CREW_PREFIX.length, Charsets.UTF_8) == CREW_PREFIX
                        val prefix = if (crewPacket) CREW_PREFIX else DIRECTOR_PREFIX
                        if (packet.length <= prefix.length ||
                            String(buffer, 0, prefix.length, Charsets.UTF_8) != prefix) continue
                        var tokenEnd = -1
                        for (index in prefix.length until packet.length) {
                            if (buffer[index] == '|'.code.toByte()) { tokenEnd = index; break }
                        }
                        if (tokenEnd < 0) continue
                        val token = String(buffer, prefix.length, tokenEnd - prefix.length, Charsets.UTF_8)
                        if (token !in allowedTokens.value) continue
                        var audioOffset = tokenEnd + 1
                        if (crewPacket) {
                            var senderEnd = -1
                            for (index in audioOffset until packet.length) {
                                if (buffer[index] == '|'.code.toByte()) { senderEnd = index; break }
                            }
                            if (senderEnd < 0) continue
                            _speakerName.value = String(buffer, audioOffset, senderEnd - audioOffset, Charsets.UTF_8)
                                .take(48).ifBlank { "Crew camera" }
                            audioOffset = senderEnd + 1
                        } else {
                            _speakerName.value = "Director"
                        }
                        lastPacketAt = System.currentTimeMillis()
                        if (!_muted.value) {
                            track?.setVolume(_volume.value)
                            track?.write(buffer, audioOffset, packet.length - audioOffset, AudioTrack.WRITE_BLOCKING)
                        }
                    }
                }
            } catch (_: SocketException) {
                delay(500)
            } catch (_: Throwable) {
                delay(1_000)
            } finally {
                runCatching { track?.stop() }
                track?.release()
            }
        }
    }
}
