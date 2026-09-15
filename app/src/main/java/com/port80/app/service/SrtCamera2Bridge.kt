package com.port80.app.service

import android.content.Context
import android.view.MotionEvent
import com.pedro.common.ConnectChecker
import com.pedro.common.VideoCodec as RootEncoderVideoCodec
import com.pedro.library.srt.SrtCamera2
import com.pedro.library.view.OpenGlView
import com.pedro.encoder.input.audio.CustomAudioEffect
import com.port80.app.data.model.SrtMode
import com.port80.app.data.model.StabilizationMode
import com.port80.app.data.model.VideoCodec
import com.port80.app.util.RedactingLogger

/**
 * [EncoderBridge] implementation backed by RootEncoder's [SrtCamera2].
 *
 * Handles SRT (Secure Reliable Transport) connections with support for:
 * - Caller, Listener, and Rendezvous modes
 * - Encryption via passphrase (AES)
 * - Configurable latency and stream ID
 * - H.264 and H.265 codecs (AV1 is NOT supported over SRT by RootEncoder)
 *
 * The SRT URL is built internally from [ConnectionParams.Srt] fields,
 * keeping protocol details encapsulated in the bridge.
 */
class SrtCamera2Bridge(
    private val connectChecker: ConnectChecker
) : EncoderBridge {

    companion object {
        private const val TAG = "SrtCamera2Bridge"
        /** Minimal SRT Access Control stream_id: publish mode, no resource filter. */
        private const val DEFAULT_SRT_STREAM_ID = "#!::m=publish"
    }

    private var srtCamera2: SrtCamera2? = null
    private var audioLevelCallback: (Float) -> Unit = {}
    private var intercomAudioCallback: (ByteArray, Int, Boolean) -> Boolean = { _, _, _ -> false }

    // ── Preview ──────────────────────────────────────────────────────────

    override fun startPreview(openGlView: OpenGlView) {
        doStartPreview(openGlView, cameraId = null)
    }

    override fun startPreview(openGlView: OpenGlView, cameraId: String) {
        doStartPreview(openGlView, cameraId = cameraId)
    }

    override fun startPreview(openGlView: OpenGlView, cameraId: String, width: Int, height: Int) {
        // width/height are surface dimensions used for orientation tracking only.
        // RootEncoder picks camera-native resolution internally.
        doStartPreview(openGlView, cameraId = cameraId)
    }

    private fun doStartPreview(openGlView: OpenGlView, cameraId: String?) {
        RedactingLogger.d(TAG, "startPreview(cameraId=${cameraId ?: "default"})")
        try {
            srtCamera2 = SrtCamera2(openGlView, connectChecker)
            RedactingLogger.d(TAG, "SrtCamera2 instance created with OpenGlView")
            if (cameraId != null) {
                srtCamera2?.startPreview(cameraId)
            } else {
                srtCamera2?.startPreview()
            }
            RedactingLogger.d(TAG, "startPreview() completed (isOnPreview=${srtCamera2?.isOnPreview == true})")
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "startPreview() failed", e)
            connectChecker.onConnectionFailed(
                "PREVIEW_START_FAILED: ${e.javaClass.simpleName}: ${e.message}"
            )
        }
    }

    override fun stopPreview() {
        RedactingLogger.d(TAG, "stopPreview()")
        srtCamera2?.stopPreview()
    }

    override fun replaceViewWithBackground(context: Context) {
        RedactingLogger.d(TAG, "replaceViewWithBackground() — switching to headless mode")
        try {
            srtCamera2?.replaceView(context)
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "replaceViewWithBackground() failed, falling back to stopPreview", e)
            srtCamera2?.stopPreview()
        }
    }

    override fun replaceView(openGlView: OpenGlView) {
        RedactingLogger.d(TAG, "replaceView() — hot-swapping to new surface")
        try {
            srtCamera2?.replaceView(openGlView)
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "replaceView() failed", e)
        }
    }

    // ── Streaming ────────────────────────────────────────────────────────

    override fun connect(params: ConnectionParams, config: EncoderConfig) {
        require(params is ConnectionParams.Srt) { "SrtCamera2Bridge requires ConnectionParams.Srt" }
        require(config.videoCodec != VideoCodec.AV1) { "AV1 is not supported over SRT" }

        val camera = srtCamera2
        if (camera == null) {
            RedactingLogger.e(TAG, "connect() called before startPreview() — ignoring")
            connectChecker.onConnectionFailed("CAMERA_NOT_INITIALIZED: connect called before preview")
            return
        }

        RedactingLogger.d(
            TAG,
            "connect() begin (codec=${config.videoCodec}, mode=${params.mode}, isOnPreview=${camera.isOnPreview})"
        )

        val alreadyPrepared = camera.isRecording
        var videoReady = true
        var audioReady = true
        try {
            if (!alreadyPrepared) {
                camera.setVideoCodec(config.videoCodec.toRootEncoder())
                videoReady = camera.prepareVideo(
                config.orientedWidth,
                config.orientedHeight,
                config.fps,
                config.videoBitrateKbps * 1000,
                config.keyframeIntervalSec,
                config.rotation
            )
                audioReady = camera.prepareAudio(
                config.audioBitrateKbps * 1000,
                config.audioSampleRate,
                config.stereo
            )
                camera.setCustomAudioEffect(SrtAudioMeterEffect(config.audioSampleRate, config.stereo, { audioLevelCallback(it) }, { pcm, rate, stereo -> intercomAudioCallback(pcm, rate, stereo) }))
            }
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "Encoder prepare threw exception", e)
            connectChecker.onConnectionFailed(
                "ENCODER_PREP_EXCEPTION: ${e.javaClass.simpleName}: ${e.message}"
            )
            return
        }

        if (!videoReady || !audioReady) {
            RedactingLogger.e(TAG, "Encoder preparation failed — video=$videoReady, audio=$audioReady")
            connectChecker.onConnectionFailed("ENCODER_PREP_FAILED(video=$videoReady,audio=$audioReady)")
            return
        }

        val srtUrl = buildSrtUrl(params)
        // Log without secrets — CredentialSanitizer handles passphrase redaction
        RedactingLogger.i(TAG, "Connecting to $srtUrl")

        // Configure SRT encryption via RootEncoder's setPassphrase() API.
        // This MUST happen before startStream() — RootEncoder does NOT parse
        // passphrase from the URL; it requires explicit API configuration to
        // include the KMREQ extension in the SRT conclusion handshake.
        if (!params.passphrase.isNullOrBlank()) {
            camera.getStreamClient().setPassphrase(
                params.passphrase,
                params.srtKeyLength.toRootEncoder()
            )
            RedactingLogger.d(TAG, "SRT encryption configured (${params.srtKeyLength.displayName()})")
        }

        try {
            camera.startStream(srtUrl)
            RedactingLogger.d(TAG, "startStream() invoked on SRT encoder")
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "startStream() failed", e)
            connectChecker.onConnectionFailed(
                "STREAM_START_FAILED: ${e.javaClass.simpleName}: ${e.message}"
            )
        }
    }

    override fun disconnect() {
        val camera = srtCamera2
        RedactingLogger.d(
            TAG,
            "disconnect() (hasCamera=${camera != null}, isStreaming=${camera?.isStreaming == true})"
        )
        camera?.stopStream()
    }

    // ── Camera controls ──────────────────────────────────────────────────

    override fun switchCamera() {
        RedactingLogger.d(TAG, "switchCamera()")
        try {
            srtCamera2?.switchCamera()
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "Failed to switch camera", e)
        }
    }

    override fun switchCamera(cameraId: String) {
        RedactingLogger.d(TAG, "switchCamera(cameraId=$cameraId)")
        try {
            srtCamera2?.switchCamera(cameraId)
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "Failed to switch to camera $cameraId", e)
        }
    }

    override fun setZoom(level: Float) {
        runCatching { srtCamera2?.setZoom(level.coerceAtLeast(1f)) }
            .onFailure { RedactingLogger.e(TAG, "Failed to set zoom", it) }
    }

    override fun setExposure(value: Int) {
        runCatching { srtCamera2?.setExposure(value) }
            .onFailure { RedactingLogger.e(TAG, "Failed to set exposure", it) }
    }

    override fun tapToFocus(event: MotionEvent): Boolean =
        runCatching { srtCamera2?.tapToFocus(event) == true }.getOrDefault(false)

    // ── Encoder tuning ───────────────────────────────────────────────────

    override fun setVideoBitrateOnFly(bitrateKbps: Int) {
        val bitrateBps = bitrateKbps * 1000
        RedactingLogger.d(TAG, "setVideoBitrateOnFly(${bitrateKbps} kbps → $bitrateBps bps)")
        srtCamera2?.setVideoBitrateOnFly(bitrateBps)
    }

    override fun setStabilizationMode(mode: StabilizationMode) {
        RedactingLogger.d(TAG, "setStabilizationMode($mode)")
        val camera = srtCamera2 ?: return
        try {
            when (mode) {
                StabilizationMode.OFF -> {
                    camera.disableVideoStabilization()
                    camera.disableOpticalVideoStabilization()
                }
                StabilizationMode.EIS -> {
                    camera.disableOpticalVideoStabilization()
                    camera.enableVideoStabilization()
                }
                StabilizationMode.OIS -> {
                    camera.disableVideoStabilization()
                    camera.enableOpticalVideoStabilization()
                }
            }
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "Failed to set stabilization mode $mode", e)
        }
    }

    // ── Lifecycle ────────────────────────────────────────────────────────

    override fun release() {
        RedactingLogger.d(TAG, "release()")
        srtCamera2?.let { camera ->
            RedactingLogger.d(
                TAG,
                "release() begin (isStreaming=${camera.isStreaming}, isOnPreview=${camera.isOnPreview})"
            )
            if (camera.isRecording) camera.stopRecord()
            if (camera.isStreaming) {
                camera.stopStream()
            }
            if (camera.isOnPreview) {
                camera.stopPreview()
            }
        }
        srtCamera2 = null
        RedactingLogger.d(TAG, "release() completed")
    }

    override fun isStreaming(): Boolean {
        return srtCamera2?.isStreaming == true
    }

    override fun setFpsListener(callback: (Int) -> Unit) {
        srtCamera2?.setFpsListener { fps -> callback(fps) }
    }

    override fun prepareRecording(config: EncoderConfig): Boolean {
        val camera = srtCamera2 ?: return false
        return runCatching {
            camera.setVideoCodec(config.videoCodec.toRootEncoder())
            val video = camera.prepareVideo(config.orientedWidth, config.orientedHeight, config.fps,
                config.videoBitrateKbps * 1000, config.keyframeIntervalSec, config.rotation)
            val audio = camera.prepareAudio(config.audioBitrateKbps * 1000, config.audioSampleRate, config.stereo)
            camera.setCustomAudioEffect(SrtAudioMeterEffect(config.audioSampleRate, config.stereo, { audioLevelCallback(it) }, { pcm, rate, stereo -> intercomAudioCallback(pcm, rate, stereo) }))
            video && audio
        }.onFailure { RedactingLogger.e(TAG, "Failed to prepare standalone recording", it) }.getOrDefault(false)
    }

    override fun startRecording(filePath: String): Boolean = runCatching {
        srtCamera2?.startRecord(filePath)
        srtCamera2?.isRecording == true
    }.onFailure { RedactingLogger.e(TAG, "Failed to start recording", it) }.getOrDefault(false)

    override fun stopRecording(): Boolean = runCatching {
        if (srtCamera2?.isRecording == true) srtCamera2?.stopRecord()
        srtCamera2?.isRecording != true
    }.onFailure { RedactingLogger.e(TAG, "Failed to stop recording", it) }.getOrDefault(false)

    override fun isRecording(): Boolean = srtCamera2?.isRecording == true

    override fun setAudioLevelListener(callback: (Float) -> Unit) {
        audioLevelCallback = callback
    }

    override fun setIntercomAudioListener(callback: (ByteArray, Int, Boolean) -> Boolean) {
        intercomAudioCallback = callback
    }

    override fun setAudioMuted(muted: Boolean) {
        if (muted) srtCamera2?.disableAudio() else srtCamera2?.enableAudio()
    }

    // ── SRT URL builder ──────────────────────────────────────────────────

    /**
     * Build the SRT URL from typed parameters.
     * Format: srt://host:port?mode=caller&latency=120&streamid=Y
     *
     * NOTE: Passphrase is NOT included in the URL. RootEncoder does not parse
     * encryption params from the URL — encryption is configured separately via
     * SrtStreamClient.setPassphrase() before startStream().
     *
     * Always includes `streamid=` to prevent RootEncoder from falling back to
     * using the full query string as the SRT handshake stream_id. If the user
     * hasn't specified a stream ID, defaults to SRT Access Control publish mode.
     */
    private fun buildSrtUrl(params: ConnectionParams.Srt): String {
        val sb = StringBuilder("srt://${params.host}:${params.port}")
        val queryParams = mutableListOf<String>()

        queryParams.add("mode=${params.mode.toUrlParam()}")
        queryParams.add("latency=${params.latencyMs}")

        // Always include streamid so RootEncoder sends it in the SRT handshake.
        // Without this, RootEncoder falls back to getFullPath() which returns the
        // entire query string (e.g. "mode=caller&latency=120") as the stream_id,
        // causing servers to reject the connection as "not a publish request".
        val streamId = if (!params.streamId.isNullOrBlank()) {
            params.streamId
        } else {
            DEFAULT_SRT_STREAM_ID
        }
        queryParams.add("streamid=$streamId")

        if (queryParams.isNotEmpty()) {
            sb.append("?")
            sb.append(queryParams.joinToString("&"))
        }
        return sb.toString()
    }
}

private class SrtAudioMeterEffect(
    private val sampleRate: Int,
    private val stereo: Boolean,
    private val onLevel: (Float) -> Unit,
    private val onPcm: (ByteArray, Int, Boolean) -> Boolean
) : CustomAudioEffect() {
    private var lastUpdate = 0L
    override fun process(pcmBuffer: ByteArray): ByteArray {
        val suppressProgramme = onPcm(pcmBuffer, sampleRate, stereo)
        val now = android.os.SystemClock.elapsedRealtime()
        if (now - lastUpdate >= 75 && pcmBuffer.size >= 2) {
            var sum = 0.0
            var samples = 0
            var index = 0
            while (index + 1 < pcmBuffer.size) {
                val sample = ((pcmBuffer[index + 1].toInt() shl 8) or
                    (pcmBuffer[index].toInt() and 0xff)).toShort().toInt()
                val normalized = sample / 32768.0
                sum += normalized * normalized
                samples++
                index += 2
            }
            val rms = kotlin.math.sqrt(sum / samples.coerceAtLeast(1))
            val db = 20.0 * kotlin.math.log10(rms.coerceAtLeast(0.00001))
            onLevel(((db + 60.0) / 60.0).coerceIn(0.0, 1.0).toFloat())
            lastUpdate = now
        }
        if (suppressProgramme) pcmBuffer.fill(0)
        return pcmBuffer
    }
}

/** Map our VideoCodec enum to RootEncoder's VideoCodec enum. */
private fun VideoCodec.toRootEncoder(): RootEncoderVideoCodec = when (this) {
    VideoCodec.H264 -> RootEncoderVideoCodec.H264
    VideoCodec.H265 -> RootEncoderVideoCodec.H265
    VideoCodec.AV1 -> RootEncoderVideoCodec.AV1
}
