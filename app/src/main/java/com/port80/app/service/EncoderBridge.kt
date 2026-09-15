package com.port80.app.service

import android.content.Context
import android.view.MotionEvent
import com.pedro.library.view.OpenGlView
import com.port80.app.data.model.StabilizationMode
import com.port80.app.data.model.StreamProtocol

/**
 * Abstraction layer over RootEncoder's camera-encoder classes.
 * This interface lets us test the service without a real camera/encoder
 * and allows protocol-specific implementations (RTMP vs SRT).
 */
interface EncoderBridge {
    /** Start showing camera preview on the given OpenGlView surface. */
    fun startPreview(openGlView: OpenGlView)

    /**
     * Start showing camera preview using a specific camera by Camera2 camera ID.
     * Falls back to the default camera if [cameraId] is invalid.
     */
    fun startPreview(openGlView: OpenGlView, cameraId: String)

    /**
     * Start showing camera preview with explicit surface dimensions.
     * This ensures the preview renders at the correct resolution for the
     * current display orientation, avoiding black bars on rotation.
     */
    fun startPreview(openGlView: OpenGlView, cameraId: String, width: Int, height: Int)

    /** Stop the camera preview (streaming continues without display). */
    fun stopPreview()

    /**
     * Switch to headless background mode while keeping camera capture and
     * encoder alive. Uses RootEncoder's [Camera2Base.replaceView(Context)]
     * which swaps the GL surface for an off-screen GlStreamInterface.
     */
    fun replaceViewWithBackground(context: Context)

    /**
     * Hot-swap back to a visible preview surface during an active stream.
     * Uses RootEncoder's [Camera2Base.replaceView(OpenGlView)] which
     * re-opens the camera on the new surface without interrupting the stream.
     */
    fun replaceView(openGlView: OpenGlView)

    /** Configure encoders and connect to the streaming server. */
    fun connect(params: ConnectionParams, config: EncoderConfig)

    /**
     * Configure one shared encoder and connect every saved destination.
     * Implementations that do not support fan-out retain the legacy single-output behavior.
     */
    fun connectAll(targets: List<MultiOutputTarget>, config: EncoderConfig) {
        require(targets.size == 1) { "This encoder bridge does not support multiple outputs" }
        connect(targets.first().params, config)
    }

    /** Receive independent state changes for all active/pending destinations. */
    fun setOutputStatusListener(callback: (List<OutputStatus>) -> Unit) = Unit

    /** Disconnect from the streaming server. */
    fun disconnect()

    /** Switch between front and back camera. */
    fun switchCamera()

    /**
     * Switch to a specific camera by Camera2 camera ID.
     * Returns silently if the ID is invalid or the camera cannot be opened.
     */
    fun switchCamera(cameraId: String)

    /** Set Camera2 digital/optical zoom level. 1f means no zoom. */
    fun setZoom(level: Float)

    /** Set camera exposure compensation in device-supported EV steps. */
    fun setExposure(value: Int)

    /** Toggle the active camera's torch. Returns the resulting enabled state. */
    fun toggleTorch(): Boolean = false

    /** Apply a 2500K–7500K white-balance correction to preview, stream and recording. */
    fun setWhiteBalanceKelvin(kelvin: Int) = Unit

    /** Focus the active camera at a point on the preview surface. */
    fun tapToFocus(event: MotionEvent): Boolean

    /** Change video bitrate on the fly without restarting the encoder. */
    fun setVideoBitrateOnFly(bitrateKbps: Int)

    /**
     * Set the image stabilization mode.
     * Only one mode (EIS or OIS) should be active at a time.
     * Safe to call during preview or streaming.
     */
    fun setStabilizationMode(mode: StabilizationMode)

    /** Release all encoder and camera resources. Call this on service destroy. */
    fun release()

    /** Check if we're currently streaming. */
    fun isStreaming(): Boolean

    /**
     * Register a callback that receives the measured FPS once per second.
     * Must be called after the camera/encoder is created (i.e., after startPreview).
     */
    fun setFpsListener(callback: (Int) -> Unit)

    /** Start/stop MP4 recording from the already prepared stream encoders. */
    fun prepareRecording(config: EncoderConfig): Boolean
    fun startRecording(filePath: String): Boolean
    fun stopRecording(): Boolean
    fun isRecording(): Boolean

    /** Normalized microphone level (0 silent, 1 clipping), throttled by the bridge. */
    fun setAudioLevelListener(callback: (Float) -> Unit)
    /** Copy microphone PCM to the private intercom return path without altering programme audio. */
    /** Callback returns true when this PCM buffer must be silenced on the programme track. */
    fun setIntercomAudioListener(callback: (ByteArray, Int, Boolean) -> Boolean)
    fun setAudioMuted(muted: Boolean)

    /** Factory for creating the correct EncoderBridge per protocol. */
    fun interface Factory {
        fun create(
            connectChecker: com.pedro.common.ConnectChecker,
            protocol: StreamProtocol
        ): EncoderBridge
    }
}
