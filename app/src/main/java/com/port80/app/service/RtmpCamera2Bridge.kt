package com.port80.app.service

import android.content.Context
import android.view.MotionEvent
import com.pedro.common.ConnectChecker
import com.pedro.common.VideoCodec as RootEncoderVideoCodec
import com.pedro.library.view.OpenGlView
import com.pedro.encoder.input.audio.CustomAudioEffect
import com.pedro.encoder.input.gl.render.filters.TemperatureFilterRender
import com.port80.app.data.model.StabilizationMode
import com.port80.app.data.model.VideoCodec
import com.port80.app.util.RedactingLogger

/**
 * Shared Camera2 encoder bridge. Despite the legacy class name, version 2.7.1
 * fans one encoded camera feed to all configured RTMP(S) and SRT destinations.
 *
 * Handles RTMP and RTMPS connections. For Enhanced RTMP, sets the codec to
 * H.265 or AV1 via RootEncoder's [RootEncoderVideoCodec] enum before preparing
 * the video encoder.
 *
 * Lifecycle (matches StreamingService flow):
 * 1. [startPreview] — opens the camera and begins rendering frames
 * 2. [connect]      — configures encoders with codec/resolution/bitrate and starts streaming
 * 3. [disconnect]   — stops the stream (camera stays open)
 * 4. [stopPreview]  — stops camera capture
 * 5. [release]      — frees all resources
 */
class RtmpCamera2Bridge(
    private val connectChecker: ConnectChecker
) : EncoderBridge {

    companion object {
        private const val TAG = "RtmpCamera2Bridge"
    }

    private var rtmpCamera2: MultiCamera2? = null
    private var previewView: OpenGlView? = null
    private var whiteBalanceKelvin: Int = 5000
    private var whiteBalanceFilter: TemperatureFilterRender? = null
    private var audioLevelCallback: (Float) -> Unit = {}
    private var intercomAudioCallback: (ByteArray, Int, Boolean) -> Boolean = { _, _, _ -> false }
    private var outputStatusCallback: (List<OutputStatus>) -> Unit = {}

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
            previewView = openGlView
            rtmpCamera2 = MultiCamera2(openGlView)
            RedactingLogger.d(TAG, "MultiCamera2 instance created with OpenGlView")
            if (cameraId != null) {
                rtmpCamera2?.startPreview(cameraId)
            } else {
                rtmpCamera2?.startPreview()
            }
            installWhiteBalanceFilter(openGlView)
            RedactingLogger.d(TAG, "startPreview() completed (isOnPreview=${rtmpCamera2?.isOnPreview == true})")
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "startPreview() failed", e)
            connectChecker.onConnectionFailed(
                "PREVIEW_START_FAILED: ${e.javaClass.simpleName}: ${e.message}"
            )
        }
    }

    override fun stopPreview() {
        RedactingLogger.d(TAG, "stopPreview()")
        rtmpCamera2?.stopPreview()
    }

    override fun replaceViewWithBackground(context: Context) {
        RedactingLogger.d(TAG, "replaceViewWithBackground() — switching to headless mode")
        try {
            rtmpCamera2?.replaceView(context)
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "replaceViewWithBackground() failed, falling back to stopPreview", e)
            rtmpCamera2?.stopPreview()
        }
    }

    override fun replaceView(openGlView: OpenGlView) {
        RedactingLogger.d(TAG, "replaceView() — hot-swapping to new surface")
        try {
            previewView = openGlView
            rtmpCamera2?.replaceView(openGlView)
            installWhiteBalanceFilter(openGlView)
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "replaceView() failed", e)
        }
    }

    // ── Streaming ────────────────────────────────────────────────────────

    override fun connect(params: ConnectionParams, config: EncoderConfig) {
        connectAll(listOf(MultiOutputTarget("legacy", "Primary output", params)), config)
    }

    override fun connectAll(targets: List<MultiOutputTarget>, config: EncoderConfig) {
        require(targets.isNotEmpty()) { "At least one output is required" }

        val camera = rtmpCamera2
        if (camera == null) {
            RedactingLogger.e(TAG, "connect() called before startPreview() — ignoring")
            connectChecker.onConnectionFailed("CAMERA_NOT_INITIALIZED: connect called before preview")
            return
        }

        RedactingLogger.d(TAG, "connectAll() begin (${targets.size} outputs, codec=${config.videoCodec})")
        camera.configure(targets, outputStatusCallback)

        // startRecord() and startStream() can share already-prepared encoders.
        // Re-preparing while recording stops RootEncoder's active MP4 muxer.
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
                camera.setCustomAudioEffect(
                    AudioMeterEffect(
                        config.audioSampleRate,
                        config.stereo,
                        { audioLevelCallback(it) },
                        { pcm, rate, stereo -> intercomAudioCallback(pcm, rate, stereo) }
                    )
                )
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

        try {
            camera.startStream("vystream://multi-output")
            RedactingLogger.d(TAG, "shared encoder started for ${targets.size} outputs")
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "startStream() failed", e)
            connectChecker.onConnectionFailed(
                "STREAM_START_FAILED: ${e.javaClass.simpleName}: ${e.message}"
            )
        }
    }

    override fun disconnect() {
        val camera = rtmpCamera2
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
            rtmpCamera2?.switchCamera()
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "Failed to switch camera", e)
        }
    }

    override fun switchCamera(cameraId: String) {
        RedactingLogger.d(TAG, "switchCamera(cameraId=$cameraId)")
        try {
            rtmpCamera2?.switchCamera(cameraId)
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "Failed to switch to camera $cameraId", e)
        }
    }

    override fun setZoom(level: Float) {
        runCatching { rtmpCamera2?.setZoom(level.coerceAtLeast(1f)) }
            .onFailure { RedactingLogger.e(TAG, "Failed to set zoom", it) }
    }

    override fun setExposure(value: Int) {
        runCatching { rtmpCamera2?.setExposure(value) }
            .onFailure { RedactingLogger.e(TAG, "Failed to set exposure", it) }
    }

    override fun toggleTorch(): Boolean {
        val camera = rtmpCamera2 ?: return false
        return runCatching {
            if (!camera.isLanternSupported) return@runCatching false
            if (camera.isLanternEnabled) camera.disableLantern() else camera.enableLantern()
            camera.isLanternEnabled
        }.onFailure { RedactingLogger.e(TAG, "Failed to toggle flashlight", it) }
            .getOrDefault(false)
    }

    override fun setWhiteBalanceKelvin(kelvin: Int) {
        whiteBalanceKelvin = kelvin.coerceIn(2500, 7500)
        val normalized = (whiteBalanceKelvin - 2500f) / 5000f
        whiteBalanceFilter?.setTemperature(normalized)
            ?: previewView?.let(::installWhiteBalanceFilter)
        RedactingLogger.d(TAG, "White balance set to ${whiteBalanceKelvin}K")
    }

    private fun installWhiteBalanceFilter(view: OpenGlView) {
        val normalized = (whiteBalanceKelvin - 2500f) / 5000f
        val filter = TemperatureFilterRender().apply { setTemperature(normalized) }
        whiteBalanceFilter = filter
        view.setFilter(filter)
    }

    override fun tapToFocus(event: MotionEvent): Boolean =
        runCatching { rtmpCamera2?.tapToFocus(event) == true }.getOrDefault(false)

    // ── Encoder tuning ───────────────────────────────────────────────────

    override fun setVideoBitrateOnFly(bitrateKbps: Int) {
        val bitrateBps = bitrateKbps * 1000
        RedactingLogger.d(TAG, "setVideoBitrateOnFly(${bitrateKbps} kbps → $bitrateBps bps)")
        rtmpCamera2?.setVideoBitrateOnFly(bitrateBps)
    }

    override fun setStabilizationMode(mode: StabilizationMode) {
        RedactingLogger.d(TAG, "setStabilizationMode($mode)")
        val camera = rtmpCamera2 ?: return
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
        rtmpCamera2?.let { camera ->
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
        rtmpCamera2 = null
        previewView = null
        whiteBalanceFilter = null
        RedactingLogger.d(TAG, "release() completed")
    }

    override fun isStreaming(): Boolean {
        return rtmpCamera2?.isStreaming == true
    }

    override fun setFpsListener(callback: (Int) -> Unit) {
        rtmpCamera2?.setFpsListener { fps -> callback(fps) }
    }

    override fun prepareRecording(config: EncoderConfig): Boolean {
        val camera = rtmpCamera2 ?: return false
        return runCatching {
            camera.setVideoCodec(config.videoCodec.toRootEncoder())
            val video = camera.prepareVideo(config.orientedWidth, config.orientedHeight, config.fps,
                config.videoBitrateKbps * 1000, config.keyframeIntervalSec, config.rotation)
            val audio = camera.prepareAudio(config.audioBitrateKbps * 1000, config.audioSampleRate, config.stereo)
            camera.setCustomAudioEffect(AudioMeterEffect(config.audioSampleRate, config.stereo, { audioLevelCallback(it) }, { pcm, rate, stereo -> intercomAudioCallback(pcm, rate, stereo) }))
            video && audio
        }.onFailure { RedactingLogger.e(TAG, "Failed to prepare standalone recording", it) }.getOrDefault(false)
    }

    override fun startRecording(filePath: String): Boolean = runCatching {
        rtmpCamera2?.startRecord(filePath)
        rtmpCamera2?.isRecording == true
    }.onFailure { RedactingLogger.e(TAG, "Failed to start recording", it) }.getOrDefault(false)

    override fun stopRecording(): Boolean = runCatching {
        if (rtmpCamera2?.isRecording == true) rtmpCamera2?.stopRecord()
        rtmpCamera2?.isRecording != true
    }.onFailure { RedactingLogger.e(TAG, "Failed to stop recording", it) }.getOrDefault(false)

    override fun isRecording(): Boolean = rtmpCamera2?.isRecording == true

    override fun setAudioLevelListener(callback: (Float) -> Unit) {
        audioLevelCallback = callback
    }

    override fun setIntercomAudioListener(callback: (ByteArray, Int, Boolean) -> Boolean) {
        intercomAudioCallback = callback
    }

    override fun setOutputStatusListener(callback: (List<OutputStatus>) -> Unit) {
        outputStatusCallback = callback
    }

    override fun setAudioMuted(muted: Boolean) {
        if (muted) rtmpCamera2?.disableAudio() else rtmpCamera2?.enableAudio()
    }
}

private class AudioMeterEffect(
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
