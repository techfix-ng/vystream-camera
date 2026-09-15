package com.port80.app.service

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.res.Configuration
import android.net.ConnectivityManager
import android.content.Intent
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import com.pedro.common.ConnectChecker
import com.pedro.library.view.OpenGlView
import com.port80.app.camera.DeviceCapabilityQuery
import com.port80.app.crash.CredentialSanitizer
import com.port80.app.data.EndpointProfileRepository
import com.port80.app.data.SettingsRepository
import com.port80.app.data.model.StabilizationMode
import com.port80.app.data.model.StreamProtocol
import com.port80.app.data.model.StreamState
import com.port80.app.data.model.StreamStats
import com.port80.app.data.model.StopReason
import com.port80.app.data.model.VideoCodec
import com.port80.app.util.RedactingLogger
import dagger.hilt.android.AndroidEntryPoint
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicBoolean
import javax.inject.Inject

/**
 * The foreground service that manages the entire streaming session.
 *
 * This is the SINGLE SOURCE OF TRUTH for stream state.
 * The UI (ViewModel) observes state via StateFlow but never modifies it.
 *
 * Lifecycle:
 * 1. Activity calls startForegroundService() with profileId
 * 2. Service starts, shows notification, transitions to Connecting
 * 3. Connects to RTMP server - transitions to Live
 * 4. On stop: disconnects, releases resources, stops self
 *
 * State machine:
 *   Idle -> Previewing -> Connecting -> Live -> Stopping -> Stopped
 *                                    \-> Reconnecting -/
 */
@AndroidEntryPoint
class StreamingService : Service(), StreamingServiceControl, ConnectChecker {

    companion object {
        private const val TAG = "StreamingService"
        /** Intent extra key for the endpoint profile ID (a String, not credentials!) */
        const val EXTRA_PROFILE_ID = "profileId"
    }

    // -- Injected dependencies --
    @Inject lateinit var profileRepository: EndpointProfileRepository
    @Inject lateinit var settingsRepository: SettingsRepository
    @Inject lateinit var encoderBridgeFactory: EncoderBridge.Factory
    @Inject lateinit var deviceCapabilityQuery: DeviceCapabilityQuery
    @Inject lateinit var activeStreamStateProvider: MutableActiveStreamStateProvider
    @Inject lateinit var talkbackManager: com.port80.app.audio.TalkbackManager

    // -- State (owned exclusively by this service) --
    private val _streamState = MutableStateFlow<StreamState>(StreamState.Idle)
    override val streamState: StateFlow<StreamState> = _streamState.asStateFlow()

    private val _streamStats = MutableStateFlow(StreamStats())
    override val streamStats: StateFlow<StreamStats> = _streamStats.asStateFlow()

    private val _lastFailureDetail = MutableStateFlow<String?>(null)
    override val lastFailureDetail: StateFlow<String?> = _lastFailureDetail.asStateFlow()

    private val _isRecording = MutableStateFlow(false)
    override val isRecording: StateFlow<Boolean> = _isRecording.asStateFlow()
    private val _recordingDurationMs = MutableStateFlow(0L)
    override val recordingDurationMs: StateFlow<Long> = _recordingDurationMs.asStateFlow()
    private val _recordingStatus = MutableStateFlow<String?>(null)
    override val recordingStatus: StateFlow<String?> = _recordingStatus.asStateFlow()
    private val _microphoneLevel = MutableStateFlow(0f)
    override val microphoneLevel: StateFlow<Float> = _microphoneLevel.asStateFlow()
    private val _programMicMuted = MutableStateFlow(false)
    override val programMicMuted: StateFlow<Boolean> = _programMicMuted.asStateFlow()
    private val _outputStatuses = MutableStateFlow<List<OutputStatus>>(emptyList())
    override val outputStatuses: StateFlow<List<OutputStatus>> = _outputStatuses.asStateFlow()
    private var recordingTickerJob: Job? = null
    private var recordingStartedAtMs = 0L
    private var resumeRecordingAfterBridgeChange = false
    private var multiSessionInitialized = false
    private val localRecordingManager by lazy { LocalRecordingManager(this) }

    // -- Encoder: created per-session based on profile protocol --
    private var encoderBridge: EncoderBridge? = null

    // -- Coroutine scope tied to service lifecycle --
    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    // -- Surface management --
    private var currentSurface: OpenGlView? = null
    private var currentPreviewWidth: Int = 0
    private var currentPreviewHeight: Int = 0
    private var previewResizeJob: Job? = null

    // -- Camera switcher: created per-session with available cameras --
    private var cameraSwitcher: CameraSwitcher? = null

    // -- Stabilization: tracks the active mode for carry-forward on bridge swap --
    private var activeStabilizationMode: StabilizationMode = StabilizationMode.OFF

    // -- CPU wake lock: keeps the CPU running while streaming in background --
    private var wakeLock: PowerManager.WakeLock? = null

    // -- Termination guard: prevents duplicate cleanup from overlapping callbacks --
    private val isTerminating = AtomicBoolean(false)

    // -- Stats ticker: updates durationMs at ~1 Hz while streaming --
    private var streamStartTimeMs: Long = 0L
    private var accumulatedDurationMs: Long = 0L
    private var statsTickerJob: Job? = null

    // -- Reconnect: stores active session params for reconnect attempts --
    private var activeConnectionParams: ConnectionParams? = null
    private var activeEncoderConfig: EncoderConfig? = null
    private var activeAutoReconnect: Boolean = false
    private var activeMaxReconnectAttempts: Int = 10
    private var connectionManager: ConnectionManager? = null

    // -- Binder for Activity/ViewModel to communicate with this service --
    inner class LocalBinder : Binder() {
        /** Get the service instance as StreamingServiceControl. */
        fun getService(): StreamingServiceControl = this@StreamingService
    }
    private val binder = LocalBinder()

    // ==========================================================
    // Service Lifecycle
    // ==========================================================

    override fun onCreate() {
        super.onCreate()
        RedactingLogger.d(TAG, "Service created")
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        RedactingLogger.d(TAG, "onStartCommand received")

        // Show a foreground notification immediately (required within 10s on API 31+)
        startForeground(
            NotificationController.NOTIFICATION_ID,
            createBasicNotification()
        )

        // Extract the profile ID from the intent (NEVER credentials!)
        val profileId = intent?.getStringExtra(EXTRA_PROFILE_ID)
        if (profileId != null) {
            startStream(profileId)
        }

        // Redeliver the profile intent if Android reclaims the foreground service.
        return START_REDELIVER_INTENT
    }

    override fun onBind(intent: Intent?): IBinder {
        RedactingLogger.d(TAG, "Client bound to service")
        return binder
    }

    override fun onDestroy() {
        RedactingLogger.d(TAG, "Service destroying - cleaning up")
        connectionManager?.stop()
        cleanupAndStop()
        serviceScope.cancel()
        super.onDestroy()
    }

    // ==========================================================
    // StreamingServiceControl Implementation
    // ==========================================================

    override fun startStream(profileId: String) {
        // Idempotent: only start if we're idle, previewing, or stopped
        val currentState = _streamState.value
        if (currentState != StreamState.Idle &&
            currentState !is StreamState.Previewing &&
            currentState !is StreamState.Stopped
        ) {
            RedactingLogger.d(TAG, "startStream ignored - already in state: $currentState")
            return
        }

        updateStreamState(StreamState.Connecting)
        _lastFailureDetail.value = null
        _streamStats.value = StreamStats()
        _outputStatuses.value = emptyList()
        multiSessionInitialized = false
        isTerminating.set(false)
        previewResizeJob?.cancel()
        previewResizeJob = null
        RedactingLogger.i(TAG, "Starting stream with profile: $profileId")

        serviceScope.launch { startStreamInternal(profileId) }
    }

    override fun stopStream() {
        // Idempotent: only stop if we're actually streaming, connecting, reconnecting, or previewing
        val currentState = _streamState.value
        if (currentState == StreamState.Idle || currentState is StreamState.Stopped) {
            RedactingLogger.d(TAG, "stopStream ignored - already in state: $currentState")
            return
        }

        // If only previewing, just stop preview
        if (currentState is StreamState.Previewing) {
            stopPreviewOnly()
            return
        }

        RedactingLogger.i(TAG, "Stopping stream (user request)")
        connectionManager?.stop()
        updateStreamState(StreamState.Stopping)
        terminateService(StopReason.USER_REQUEST)
    }

    override fun toggleMute() {
        val newMuteState = !_programMicMuted.value
        _programMicMuted.value = newMuteState
        talkbackManager.setProgramMuted(newMuteState)
        val currentState = _streamState.value
        if (currentState is StreamState.Live) updateStreamState(currentState.copy(isMuted = newMuteState))
        RedactingLogger.d(TAG, "Programme microphone mute toggled: $newMuteState")
    }

    override fun startPreviewOnly() {
        val currentState = _streamState.value
        if (currentState != StreamState.Idle && currentState !is StreamState.Stopped) {
            RedactingLogger.d(TAG, "startPreviewOnly ignored - already in state: $currentState")
            return
        }

        val surface = currentSurface
        if (surface == null) {
            RedactingLogger.w(TAG, "startPreviewOnly: no surface attached yet, deferring")
            return
        }

        serviceScope.launch {
            try {
                val cameraId = resolveDefaultCameraId()
                val stabMode = settingsRepository.getStabilizationMode().first()

                // Match preview to the selected endpoint protocol so starting a stream
                // does not replace the bridge and terminate an active recording.
                val previewProfile = profileRepository.getDefault()
                    ?: profileRepository.getAll().first().firstOrNull()
                val previewProtocol = previewProfile?.url?.let { StreamProtocol.fromUrl(it) }
                    ?: StreamProtocol.RTMP
                encoderBridge?.release()
                encoderBridge = encoderBridgeFactory.create(this@StreamingService, previewProtocol)

                val bridge = encoderBridge ?: return@launch
                val availableCameras = deviceCapabilityQuery.getAvailableCameras()
                cameraSwitcher = CameraSwitcher(bridge, availableCameras).apply {
                    setInitialCamera(cameraId)
                }

                bridge.startPreview(surface, cameraId)
                applyStabilizationMode(stabMode, cameraId)
                applySavedPreviewControls(bridge)

                updateStreamState(StreamState.Previewing(cameraId))
                RedactingLogger.i(TAG, "Preview started with camera $cameraId, stabilization=$stabMode")
            } catch (e: Exception) {
                RedactingLogger.e(TAG, "Failed to start preview", e)
            }
        }
    }

    override fun stopPreviewOnly() {
        val currentState = _streamState.value
        if (currentState !is StreamState.Previewing) {
            RedactingLogger.d(TAG, "stopPreviewOnly ignored - not previewing: $currentState")
            return
        }

        stopLocalRecording()
        previewResizeJob?.cancel()
        previewResizeJob = null
        RedactingLogger.d(TAG, "Stopping preview-only session")
        encoderBridge?.stopPreview()
        encoderBridge?.release()
        encoderBridge = null
        cameraSwitcher = null
        activeStabilizationMode = StabilizationMode.OFF
        updateStreamState(StreamState.Idle)
    }

    override fun switchCamera() {
        val state = _streamState.value
        if (state is StreamState.Live || state is StreamState.Previewing ||
            state == StreamState.Connecting || state is StreamState.Reconnecting
        ) {
            val switcher = cameraSwitcher
            val bridge = encoderBridge
            if (switcher != null) {
                switcher.switchCamera()
            } else if (bridge != null) {
                bridge.switchCamera()
            } else {
                RedactingLogger.w(TAG, "switchCamera: no switcher or bridge ready yet")
                return
            }
            updatePreviewingCameraId()
            revalidateStabilizationAfterSwitch()
            RedactingLogger.d(TAG, "Camera switched")
        }
    }

    override fun switchCamera(cameraId: String) {
        val state = _streamState.value
        if (state is StreamState.Live || state is StreamState.Previewing ||
            state == StreamState.Connecting || state is StreamState.Reconnecting
        ) {
            val switcher = cameraSwitcher
            val bridge = encoderBridge
            if (switcher != null) {
                switcher.switchToCamera(cameraId)
            } else if (bridge != null) {
                bridge.switchCamera(cameraId)
            } else {
                RedactingLogger.w(TAG, "switchCamera($cameraId): no switcher or bridge ready yet")
                return
            }
            updatePreviewingCameraId()
            revalidateStabilizationAfterSwitch()
            RedactingLogger.d(TAG, "Camera switched to $cameraId")
        }
    }

    override fun setZoom(level: Float) {
        encoderBridge?.setZoom(level.coerceAtLeast(1f))
    }

    override fun setExposure(value: Int) {
        encoderBridge?.setExposure(value)
    }

    override fun toggleTorch(): Boolean = encoderBridge?.toggleTorch() ?: false

    override fun setWhiteBalanceKelvin(kelvin: Int) {
        encoderBridge?.setWhiteBalanceKelvin(kelvin.coerceIn(2500, 7500))
    }

    override fun toggleRecording() {
        _recordingStatus.value = null
        if (_isRecording.value) {
            stopLocalRecording()
            return
        }
        val state = _streamState.value
        if (state !is StreamState.Live && state !is StreamState.Previewing) return
        serviceScope.launch {
            val bridge = encoderBridge ?: return@launch
            bridge.setAudioLevelListener { level -> _microphoneLevel.value = level }
            bridge.setIntercomAudioListener(talkbackManager::sendReturnPcm)
            if (state is StreamState.Previewing) {
                val profile = profileRepository.getDefault()
                    ?: profileRepository.getAll().first().firstOrNull()
                val codec = profile?.videoCodec ?: VideoCodec.H264
                if (!bridge.prepareRecording(buildEncoderConfig(codec))) {
                    RedactingLogger.e(TAG, "Standalone recording encoder preparation failed")
                    _recordingStatus.value = "Recording could not start: encoder preparation failed"
                    return@launch
                }
            }
            val target = localRecordingManager.createRecordingTarget() ?: run {
                _recordingStatus.value = "Recording could not start: storage is unavailable"
                return@launch
            }
            if (bridge.startRecording(target.file.absolutePath)) {
                _isRecording.value = true
                _recordingStatus.value = "Recording started"
                recordingStartedAtMs = android.os.SystemClock.elapsedRealtime()
                acquireWakeLock()
                recordingTickerJob?.cancel()
                recordingTickerJob = serviceScope.launch {
                    while (_isRecording.value) {
                        _recordingDurationMs.value = android.os.SystemClock.elapsedRealtime() - recordingStartedAtMs
                        delay(250)
                    }
                }
            } else {
                localRecordingManager.finishRecording(success = false)
                _recordingStatus.value = "Recording could not start"
            }
        }
    }

    private fun stopLocalRecording() {
        if (!_isRecording.value) return
        val finalized = encoderBridge?.stopRecording() == true
        val savedName = localRecordingManager.finishRecording(success = finalized)
        _recordingStatus.value = if (savedName != null) {
            "Saved $savedName to Movies/VyStream"
        } else {
            "Recording was not saved. Check free storage and try again."
        }
        _isRecording.value = false
        recordingTickerJob?.cancel()
        recordingTickerJob = null
        _recordingDurationMs.value = 0L
        if (_streamState.value is StreamState.Previewing) releaseWakeLock()
    }

    override fun setStabilizationMode(mode: StabilizationMode) {
        val state = _streamState.value
        if (state is StreamState.Previewing || state is StreamState.Live) {
            val currentCameraId = cameraSwitcher?.currentCameraId ?: return
            applyStabilizationMode(mode, currentCameraId)
        }
    }

    override fun attachPreviewSurface(openGlView: OpenGlView) {
        currentSurface = openGlView
        val state = _streamState.value
        if (state is StreamState.Live || state == StreamState.Connecting ||
            state is StreamState.Reconnecting
        ) {
            // Hot-swap the surface back while keeping the stream alive
            encoderBridge?.replaceView(openGlView)
            RedactingLogger.d(TAG, "Preview surface re-attached via replaceView (stream active)")
        } else if (state is StreamState.Previewing) {
            // Re-attach during preview-only mode
            encoderBridge?.replaceView(openGlView)
            RedactingLogger.d(TAG, "Preview surface re-attached via replaceView (preview active)")
        } else {
            RedactingLogger.d(TAG, "Preview surface attached (stream not active)")
        }
    }

    override fun detachPreviewSurface() {
        currentSurface = null
        previewResizeJob?.cancel()
        previewResizeJob = null
        val state = _streamState.value
        if (state is StreamState.Live || state == StreamState.Connecting ||
            state is StreamState.Reconnecting
        ) {
            // Switch to headless background mode — camera keeps capturing
            encoderBridge?.replaceViewWithBackground(this)
            RedactingLogger.d(TAG, "Preview surface detached — switched to background mode (stream active)")
        } else if (state is StreamState.Previewing) {
            // In preview-only mode, stop the preview entirely
            stopPreviewOnly()
            RedactingLogger.d(TAG, "Preview surface detached — stopped preview (preview-only mode)")
        } else {
            encoderBridge?.stopPreview()
            RedactingLogger.d(TAG, "Preview surface detached — stopped preview (stream not active)")
        }
    }

    override fun onPreviewDimensionsChanged(width: Int, height: Int) {
        currentPreviewWidth = width
        currentPreviewHeight = height

        val state = _streamState.value
        // During active streaming, orientation is locked — ignore dimension changes
        if (state is StreamState.Live || state == StreamState.Connecting ||
            state is StreamState.Reconnecting
        ) {
            return
        }

        // Only restart preview when in Previewing state and aspect ratio flipped
        if (state !is StreamState.Previewing) return

        val surface = currentSurface ?: return
        val cameraId = state.cameraId

        // Debounce: wait for rotation animation to settle
        previewResizeJob?.cancel()
        previewResizeJob = serviceScope.launch {
            delay(300L)

            RedactingLogger.i(TAG, "Restarting preview for new dimensions ${width}x${height}")
            try {
                val stabMode = settingsRepository.getStabilizationMode().first()

                // Release old bridge to avoid leaking Camera2 state
                encoderBridge?.stopPreview()
                encoderBridge?.release()

                // Recreate bridge and restart preview
                encoderBridge = RtmpCamera2Bridge(this@StreamingService)
                val bridge = encoderBridge ?: return@launch
                val availableCameras = deviceCapabilityQuery.getAvailableCameras()
                cameraSwitcher = CameraSwitcher(bridge, availableCameras).apply {
                    setInitialCamera(cameraId)
                }

                bridge.startPreview(surface, cameraId)
                applyStabilizationMode(stabMode, cameraId)

                updateStreamState(StreamState.Previewing(cameraId))
                RedactingLogger.i(TAG, "Preview restarted for orientation change")
            } catch (e: Exception) {
                RedactingLogger.e(TAG, "Failed to restart preview after rotation", e)
            }
        }
    }

    // ==========================================================
    // Private Helpers
    // ==========================================================

    /** Clean up all streaming resources. */
    private fun cleanupAndStop() {
        stopLocalRecording()
        _microphoneLevel.value = 0f
        connectionManager?.stop()
        connectionManager = null
        activeConnectionParams = null
        activeEncoderConfig = null
        _outputStatuses.value = emptyList()
        multiSessionInitialized = false
        previewResizeJob?.cancel()
        previewResizeJob = null
        stopStatsTicker()
        accumulatedDurationMs = 0L
        releaseWakeLock()
        cameraSwitcher = null
        activeStabilizationMode = StabilizationMode.OFF
        try {
            encoderBridge?.disconnect()
            encoderBridge?.release()
            encoderBridge = null
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "Error during cleanup", e)
        }
    }

    /**
     * Full service termination: clean up resources, update state, and stop the FGS.
     * Idempotent via AtomicBoolean CAS — safe to call from overlapping callbacks.
     * The guard stays set until a new stream session starts (see [startStream]).
     */
    private fun terminateService(reason: StopReason) {
        if (!isTerminating.compareAndSet(false, true)) {
            RedactingLogger.d(TAG, "terminateService($reason) skipped — already terminating")
            return
        }
        RedactingLogger.i(TAG, "Terminating service: $reason")

        cleanupAndStop()
        updateStreamState(StreamState.Stopped(reason))

        @Suppress("DEPRECATION")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } else {
            stopForeground(true)
        }
        stopSelf()

        // Restore camera preview if a surface is still attached (user is on the stream screen).
        // The service stays alive due to BIND_AUTO_CREATE binding even after stopSelf().
        // startPreviewOnly() runs async in serviceScope, so the UI sees the Stopped state
        // briefly (long enough for error snackbars) before transitioning to Previewing.
        if (currentSurface != null) {
            startPreviewOnly()
        }
    }

    private fun acquireWakeLock() {
        if (wakeLock == null) {
            val pm = getSystemService(POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "StreamCaster::Streaming")
        }
        wakeLock?.let {
            if (!it.isHeld) {
                it.acquire(4 * 60 * 60 * 1000L) // 4-hour safety timeout
                RedactingLogger.d(TAG, "Wake lock acquired")
            }
        }
    }

    private fun releaseWakeLock() {
        wakeLock?.let {
            if (it.isHeld) {
                it.release()
                RedactingLogger.d(TAG, "Wake lock released")
            }
        }
        wakeLock = null
    }

    private fun startStatsTicker() {
        statsTickerJob?.cancel()
        streamStartTimeMs = System.currentTimeMillis()
        accumulatedDurationMs = 0L
        statsTickerJob = serviceScope.launch {
            while (true) {
                delay(1_000L)
                val elapsed = accumulatedDurationMs + (System.currentTimeMillis() - streamStartTimeMs)
                _streamStats.value = _streamStats.value.copy(durationMs = elapsed)
            }
        }
    }

    private fun pauseStatsTicker() {
        statsTickerJob?.cancel()
        statsTickerJob = null
        accumulatedDurationMs += System.currentTimeMillis() - streamStartTimeMs
    }

    private fun resumeStatsTicker() {
        statsTickerJob?.cancel()
        streamStartTimeMs = System.currentTimeMillis()
        statsTickerJob = serviceScope.launch {
            while (true) {
                delay(1_000L)
                val elapsed = accumulatedDurationMs + (System.currentTimeMillis() - streamStartTimeMs)
                _streamStats.value = _streamStats.value.copy(durationMs = elapsed)
            }
        }
    }

    private fun stopStatsTicker() {
        statsTickerJob?.cancel()
        statsTickerJob = null
    }

    private suspend fun applySavedPreviewControls(bridge: EncoderBridge) {
        bridge.setZoom(settingsRepository.getPreviewZoom().first())
        bridge.setExposure(settingsRepository.getPreviewExposure().first())
        bridge.setWhiteBalanceKelvin(settingsRepository.getPreviewWhiteBalance().first())
    }

    private suspend fun startStreamInternal(profileId: String) {
        try {
            val profiles = profileRepository.getAll().first()
            val profile = profiles.firstOrNull { it.id == profileId }
                ?: profiles.firstOrNull { it.isDefault }
                ?: profiles.firstOrNull()
            if (profile == null) {
                RedactingLogger.e(TAG, "Profile not found: $profileId")
                terminateService(StopReason.ERROR_PROFILE)
                return
            }

            // One shared encoder must use a codec understood by every destination.
            // H.264 is the universal safe choice whenever SRT participates.
            val hasSrt = profiles.any { StreamProtocol.fromUrl(it.url) == StreamProtocol.SRT }
            val sharedCodec = if (hasSrt) VideoCodec.H264 else profile.videoCodec

            val hasPreviewBridge = encoderBridge != null
            val bridgeMatchesProtocol = encoderBridge is RtmpCamera2Bridge

            if (hasPreviewBridge && bridgeMatchesProtocol) {
                RedactingLogger.d(TAG, "Reusing matching preview bridge for stream")
            } else if (hasPreviewBridge) {
                // A protocol change requires a new RootEncoder bridge. Finalize the
                // current MP4 safely and automatically start a continuation when live.
                if (_isRecording.value) {
                    stopLocalRecording()
                    resumeRecordingAfterBridgeChange = true
                    _recordingStatus.value = "Recording segment saved; continuation will start when live"
                }
                // Carry forward the current camera ID and stabilization mode.
                val currentCameraId = cameraSwitcher?.currentCameraId
                val currentStab = activeStabilizationMode
                encoderBridge?.release()
                encoderBridge = encoderBridgeFactory.create(this, StreamProtocol.RTMP)
                ensurePreview(overrideCameraId = currentCameraId)
                applyStabilizationMode(currentStab, currentCameraId ?: resolveDefaultCameraId())
                RedactingLogger.d(TAG, "Recreated shared multi-output bridge (camera=$currentCameraId, stab=$currentStab)")
            } else {
                // No preview was running — create bridge from scratch
                encoderBridge?.release()
                encoderBridge = encoderBridgeFactory.create(this, StreamProtocol.RTMP)
                ensurePreview()
                val stabMode = settingsRepository.getStabilizationMode().first()
                applyStabilizationMode(stabMode, cameraSwitcher?.currentCameraId ?: resolveDefaultCameraId())
            }

            val targets = profiles.mapNotNull { saved ->
                runCatching {
                    val protocol = StreamProtocol.fromUrl(saved.url)
                    MultiOutputTarget(saved.id, saved.name, buildConnectionParams(saved, protocol))
                }.onFailure {
                    RedactingLogger.e(TAG, "Skipping malformed output ${saved.id}", it)
                }.getOrNull()
            }
            if (targets.isEmpty()) {
                _lastFailureDetail.value = "No valid saved streaming destinations were found."
                terminateService(StopReason.ERROR_PROFILE)
                return
            }
            val encoderConfig = buildEncoderConfig(sharedCodec)

            // Individual clients own reconnect state in multi-output mode.
            activeConnectionParams = targets.first().params
            activeEncoderConfig = encoderConfig
            activeAutoReconnect = false
            activeMaxReconnectAttempts = -1

            RedactingLogger.d(TAG, "startStream(): connecting ${targets.size} saved outputs")
            acquireWakeLock()
            encoderBridge?.setAudioLevelListener { level -> _microphoneLevel.value = level }
            encoderBridge?.setIntercomAudioListener(talkbackManager::sendReturnPcm)
            encoderBridge?.setOutputStatusListener { states ->
                _outputStatuses.value = states
                val live = states.count { it.state == OutputConnectionState.LIVE }
                if (live > 0 && _streamState.value !is StreamState.Live) {
                    updateStreamState(StreamState.Live(isMuted = _programMicMuted.value))
                    if (!multiSessionInitialized) {
                        multiSessionInitialized = true
                        startStatsTicker()
                        encoderBridge?.setFpsListener { fps ->
                            _streamStats.value = _streamStats.value.copy(fps = fps.toFloat())
                        }
                        if (resumeRecordingAfterBridgeChange) {
                            resumeRecordingAfterBridgeChange = false
                            toggleRecording()
                        }
                    } else {
                        resumeStatsTicker()
                    }
                } else if (live == 0 && states.any { it.state == OutputConnectionState.RETRYING }) {
                    pauseStatsTicker()
                    updateStreamState(StreamState.Reconnecting(0, 0, -1))
                }
            }
            encoderBridge?.connectAll(targets, encoderConfig)
        } catch (e: Exception) {
            RedactingLogger.e(TAG, "Failed to start stream", e)
            _lastFailureDetail.value =
                "Could not start streaming: ${e.javaClass.simpleName}. Check camera/audio permissions and try again."
            terminateService(StopReason.ERROR_ENCODER)
        }
    }

    private fun buildConnectionParams(
        profile: com.port80.app.data.model.EndpointProfile,
        protocol: StreamProtocol
    ): ConnectionParams = when (protocol) {
        StreamProtocol.RTMP, StreamProtocol.RTMPS -> ConnectionParams.Rtmp(
            baseUrl = profile.url,
            streamKey = profile.streamKey,
            username = profile.username,
            password = profile.password,
            videoCodec = profile.videoCodec,
        )
        StreamProtocol.SRT -> ConnectionParams.Srt(
            host = parseSrtHost(profile.url).also {
                require(it != "0.0.0.0" && it != "::") {
                    "0.0.0.0 is an OBS listener address, not the PC address"
                }
            },
            port = parseSrtPort(profile.url),
            passphrase = profile.srtPassphrase,
            srtKeyLength = profile.srtKeyLength,
            latencyMs = profile.srtLatencyMs,
            mode = profile.srtMode,
            streamId = profile.srtStreamId,
            videoCodec = profile.videoCodec,
        )
    }

    private fun parseSrtHost(url: String): String {
        val withoutProtocol = url.substringAfter("://")
        return withoutProtocol.substringBefore(":").substringBefore("/").substringBefore("?")
    }

    private fun parseSrtPort(url: String): Int {
        val withoutProtocol = url.substringAfter("://")
        val hostPort = withoutProtocol.substringBefore("?").substringBefore("/")
        return if (hostPort.contains(":")) {
            hostPort.substringAfter(":").toIntOrNull() ?: 8888
        } else {
            8888
        }
    }

    private suspend fun buildEncoderConfig(
        profile: com.port80.app.data.model.EndpointProfile
    ): EncoderConfig = buildEncoderConfig(profile.videoCodec)

    private suspend fun buildEncoderConfig(videoCodec: VideoCodec): EncoderConfig {
        val resolution = settingsRepository.getResolution().first()

        // Determine display orientation: portrait if height > width on the surface.
        // Fallback to system configuration if surface dimensions haven't been reported yet.
        val isPortrait = if (currentPreviewWidth > 0 || currentPreviewHeight > 0) {
            currentPreviewHeight > currentPreviewWidth
        } else {
            resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT
        }
        val orientedWidth: Int
        val orientedHeight: Int
        val rotation: Int
        if (isPortrait) {
            // Portrait: swap so the narrower side is width
            orientedWidth = minOf(resolution.width, resolution.height)
            orientedHeight = maxOf(resolution.width, resolution.height)
            rotation = 90
        } else {
            orientedWidth = maxOf(resolution.width, resolution.height)
            orientedHeight = minOf(resolution.width, resolution.height)
            rotation = 0
        }

        return EncoderConfig(
            videoCodec = videoCodec,
            width = resolution.width,
            height = resolution.height,
            fps = settingsRepository.getFps().first(),
            videoBitrateKbps = settingsRepository.getVideoBitrateKbps().first(),
            audioBitrateKbps = settingsRepository.getAudioBitrateKbps().first(),
            audioSampleRate = settingsRepository.getAudioSampleRate().first(),
            stereo = settingsRepository.getStereo().first(),
            keyframeIntervalSec = settingsRepository.getKeyframeIntervalSec().first(),
            orientedWidth = orientedWidth,
            orientedHeight = orientedHeight,
            rotation = rotation,
        )
    }

    private suspend fun ensurePreview(overrideCameraId: String? = null) {
        val surface = currentSurface
        val cameraId = overrideCameraId ?: resolveDefaultCameraId()

        // Create camera switcher with the available cameras list
        val bridge = encoderBridge
        if (bridge != null) {
            val availableCameras = deviceCapabilityQuery.getAvailableCameras()
            cameraSwitcher = CameraSwitcher(bridge, availableCameras).apply {
                setInitialCamera(cameraId)
            }
        }

        if (surface == null) {
            RedactingLogger.w(TAG, "startStream(): no preview surface attached; stream will attempt headless camera start")
        } else {
            RedactingLogger.d(TAG, "startStream(): preview surface present, starting preview with camera $cameraId")
            encoderBridge?.startPreview(surface, cameraId)
        }
        encoderBridge?.let { applySavedPreviewControls(it) }
    }

    /**
     * Apply the given stabilization mode if the current camera supports it.
     * Falls back to OFF if the mode isn't supported.
     */
    private fun applyStabilizationMode(mode: StabilizationMode, cameraId: String) {
        val supportedModes = deviceCapabilityQuery.getSupportedStabilizationModes(cameraId)
        val effectiveMode = if (mode in supportedModes) mode else StabilizationMode.OFF
        activeStabilizationMode = effectiveMode
        encoderBridge?.setStabilizationMode(effectiveMode)
        if (effectiveMode != mode) {
            RedactingLogger.w(TAG, "Stabilization $mode not supported on camera $cameraId, using $effectiveMode")
        }
    }

    /** Update the Previewing state's cameraId after a camera switch. */
    private fun updatePreviewingCameraId() {
        val state = _streamState.value
        if (state is StreamState.Previewing) {
            val newId = cameraSwitcher?.currentCameraId ?: return
            updateStreamState(StreamState.Previewing(newId))
        }
    }

    /** After camera switch, re-validate that the active stabilization mode is still supported. */
    private fun revalidateStabilizationAfterSwitch() {
        val currentCameraId = cameraSwitcher?.currentCameraId ?: return
        applyStabilizationMode(activeStabilizationMode, currentCameraId)
    }

    /**
     * Resolve the persisted default camera ID, validating it against
     * currently available cameras. Falls back to the primary back camera.
     */
    private suspend fun resolveDefaultCameraId(): String {
        val savedId = settingsRepository.getDefaultCameraId().first()
        val availableIds = deviceCapabilityQuery.getCameraIds()

        return if (savedId in availableIds) {
            savedId
        } else {
            val fallback = availableIds.firstOrNull() ?: "0"
            RedactingLogger.w(TAG, "Saved camera ID '$savedId' not available, falling back to '$fallback'")
            fallback
        }
    }

    // ==========================================================
    // ConnectChecker — driven by RtmpCamera2Bridge callbacks
    // ==========================================================

    override fun onConnectionStarted(url: String) {
        RedactingLogger.d(TAG, "RTMP connection started: $url")
    }

    override fun onConnectionSuccess() {
        RedactingLogger.i(TAG, "Connection succeeded — stream is live")
        _lastFailureDetail.value = null

        val cm = connectionManager
        if (cm != null && cm.isReconnecting) {
            // Returning to Live after a reconnect attempt
            cm.notifyReconnectResult(true)
            updateStreamState(StreamState.Live())
            resumeStatsTicker()
        } else {
            // Initial connection
            updateStreamState(StreamState.Live())
            startStatsTicker()
            if (activeAutoReconnect && connectionManager == null) createAndStartConnectionManager()
        }

        encoderBridge?.setFpsListener { fps ->
            _streamStats.value = _streamStats.value.copy(fps = fps.toFloat())
        }
        encoderBridge?.setAudioLevelListener { level -> _microphoneLevel.value = level }
        encoderBridge?.setIntercomAudioListener(talkbackManager::sendReturnPcm)
        if (resumeRecordingAfterBridgeChange) {
            resumeRecordingAfterBridgeChange = false
            toggleRecording()
        }
    }

    override fun onConnectionFailed(reason: String) {
        RedactingLogger.e(TAG, "Connection failed: $reason")
        _lastFailureDetail.value = StreamFailureMapper.buildFailureDetail(reason)
        val stopReason = StreamFailureMapper.mapFailureReason(reason)

        val cm = connectionManager
        val currentState = _streamState.value

        if (cm != null && StreamFailureMapper.isRetryable(reason) &&
            (currentState is StreamState.Reconnecting)
        ) {
            // A reconnect attempt failed — notify CM to schedule next retry
            cm.notifyReconnectResult(false)
        } else if (cm != null && activeAutoReconnect && StreamFailureMapper.isRetryable(reason)) {
            pauseStatsTicker()
            cm.onInitialConnectionFailed()
        } else {
            // Non-retryable, or initial connection, or no CM — terminate
            cm?.stop()
            terminateService(stopReason)
        }
    }

    override fun onNewBitrate(bitrate: Long) {
        // Update stats when RootEncoder reports actual measured bitrate.
        _streamStats.value = _streamStats.value.copy(videoBitrateKbps = (bitrate / 1000).toInt())
    }

    override fun onDisconnect() {
        val previousState = _streamState.value
        RedactingLogger.w(
            TAG,
            "Disconnected (previousState=$previousState, encoderStreaming=${encoderBridge?.isStreaming() == true})"
        )

        val cm = connectionManager
        if (previousState is StreamState.Live && activeAutoReconnect && cm != null) {
            // Mid-stream drop with auto-reconnect enabled — delegate to ConnectionManager
            pauseStatsTicker()
            cm.onConnectionLost()
        } else if (previousState is StreamState.Reconnecting && cm != null) {
            // A reconnect attempt itself disconnected — notify CM of failure
            cm.notifyReconnectResult(false)
        } else if (previousState is StreamState.Live || previousState is StreamState.Reconnecting) {
            _lastFailureDetail.value =
                "Connection to server was lost. Check network stability and server availability, then retry."
            terminateService(StopReason.ERROR_ENCODER)
        }
    }

    override fun onAuthError() {
        RedactingLogger.e(TAG, "RTMP auth error — wrong stream key or credentials")
        _lastFailureDetail.value =
            "Authentication rejected by the server. Verify stream key/username/password in endpoint settings."
        connectionManager?.stop()
        terminateService(StopReason.ERROR_AUTH)
    }

    override fun onAuthSuccess() {
        RedactingLogger.d(TAG, "RTMP auth succeeded")
    }

    // ==========================================================
    // Auto-Reconnect
    // ==========================================================

    private fun createAndStartConnectionManager() {
        connectionManager?.stop()
        val cm = getSystemService(CONNECTIVITY_SERVICE) as ConnectivityManager
        val pm = getSystemService(POWER_SERVICE) as PowerManager

        val policy = ExponentialBackoffReconnectPolicy(maxAttempts = activeMaxReconnectAttempts)

        connectionManager = ConnectionManager(
            connectivityManager = cm,
            powerManager = pm,
            reconnectPolicy = policy,
            scope = serviceScope
        ).apply {
            requestReconnect = { attemptReconnect() }
            onReconnectExhausted = { reason -> terminateService(reason) }
            onStateChanged = { state -> updateStreamState(state) }
        }
        connectionManager?.start()
    }

    private fun attemptReconnect() {
        val params = activeConnectionParams ?: run {
            RedactingLogger.e(TAG, "attemptReconnect: no stored connection params")
            terminateService(StopReason.ERROR_ENCODER)
            return
        }
        val config = activeEncoderConfig ?: run {
            RedactingLogger.e(TAG, "attemptReconnect: no stored encoder config")
            terminateService(StopReason.ERROR_ENCODER)
            return
        }
        serviceScope.launch {
            try {
                RedactingLogger.i(TAG, "Attempting reconnect...")
                encoderBridge?.disconnect()
                encoderBridge?.connect(params, config)
                // Success/failure comes via ConnectChecker callbacks
            } catch (e: Exception) {
                RedactingLogger.e(TAG, "Reconnect attempt threw exception", e)
                connectionManager?.notifyReconnectResult(false)
            }
        }
    }

    /**
     * Single helper for state changes. It updates the service-owned StateFlow
     * and then publishes the simplified active-camera flag used by Settings.
     */
    private fun updateStreamState(state: StreamState) {
        _streamState.value = state
        activeStreamStateProvider.publish(state)
    }

    /** Create the notification channel (required on Android 8.0+). */
    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                NotificationController.CHANNEL_ID,
                "Streaming Service",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Shows streaming status while VyStream is live"
            }
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(channel)
        }
    }

    /**
     * Create a basic notification for startForeground().
     * This will be replaced with a richer notification by NotificationController (T-008).
     */
    private fun createBasicNotification() =
        NotificationCompat.Builder(this, NotificationController.CHANNEL_ID)
            .setContentTitle("VyStream")
            .setContentText("Preparing to stream...")
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setOngoing(true)
            .setContentIntent(NotificationController.openActivityIntent(this))
            .build()
}
