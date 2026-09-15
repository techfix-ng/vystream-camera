package com.port80.app.ui.stream

import android.app.Application
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Build
import android.os.IBinder
import android.view.SurfaceHolder
import com.pedro.library.view.OpenGlView
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.port80.app.data.EndpointProfileRepository
import com.port80.app.data.SettingsRepository
import com.port80.app.camera.DeviceCapabilityQuery
import com.port80.app.data.model.CameraInfo
import com.port80.app.data.model.EndpointProfile
import com.port80.app.data.model.StabilizationMode
import com.port80.app.data.model.StreamState
import com.port80.app.data.model.StreamStats
import com.port80.app.data.model.StopReason
import com.port80.app.service.StreamingService
import com.port80.app.service.StreamingServiceControl
import com.port80.app.service.OutputStatus
import com.port80.app.discovery.ObsDiscoveryClient
import com.port80.app.discovery.CameraHealthReporter
import com.port80.app.util.RedactingLogger
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.map
import java.lang.ref.WeakReference
import java.net.URI
import android.app.Activity
import android.content.pm.ActivityInfo
import com.port80.app.util.OrientationHelper
import javax.inject.Inject
import com.port80.app.audio.TalkbackManager

/**
 * ViewModel for the streaming screen.
 *
 * This ViewModel acts as a bridge between the UI (Compose) and the
 * StreamingService (foreground service). It:
 *
 * 1. Binds to StreamingService via Android's ServiceConnection
 * 2. Observes StreamState and StreamStats as StateFlows
 * 3. Forwards user actions (start, stop, mute, switch camera) to the service
 * 4. Manages the camera preview surface lifecycle
 *
 * IMPORTANT: The ViewModel NEVER modifies stream state directly.
 * All state changes go through the service.
 */
@HiltViewModel
class StreamViewModel @Inject constructor(
    application: Application,
    private val profileRepository: EndpointProfileRepository,
    private val settingsRepository: SettingsRepository,
    private val deviceCapabilityQuery: DeviceCapabilityQuery,
    private val obsDiscoveryClient: ObsDiscoveryClient,
    private val cameraHealthReporter: CameraHealthReporter,
    private val talkbackManager: TalkbackManager
) : AndroidViewModel(application) {

    companion object {
        private const val TAG = "StreamViewModel"
    }

    // ── Service binding ──────────────────────────

    // Reference to the service's control interface — null when not bound.
    private var serviceControl: StreamingServiceControl? = null

    // Tracks whether we currently hold a binding to the service.
    private var isBound = false

    // Replaced on each bind so repeated service connections do not duplicate collectors.
    private var serviceCollectorsJob: Job? = null

    // ── State exposed to the UI ──────────────────

    // These MutableStateFlows mirror the service's StateFlows.
    // The UI observes the read-only versions below.
    private val _streamState = MutableStateFlow<StreamState>(StreamState.Idle)

    /** Current streaming state (Idle, Connecting, Live, etc.). */
    val streamState: StateFlow<StreamState> = _streamState.asStateFlow()

    private val _streamStats = MutableStateFlow(StreamStats())

    /** Live streaming statistics (bitrate, FPS, dropped frames, etc.). */
    val streamStats: StateFlow<StreamStats> = _streamStats.asStateFlow()

    private val _lastFailureDetail = MutableStateFlow<String?>(null)

    /** Last user-facing diagnostic detail for stream startup/connection failures. */
    val lastFailureDetail: StateFlow<String?> = _lastFailureDetail.asStateFlow()
    private val _isRecording = MutableStateFlow(false)
    val isRecording: StateFlow<Boolean> = _isRecording.asStateFlow()
    private val _recordingDurationMs = MutableStateFlow(0L)
    val recordingDurationMs: StateFlow<Long> = _recordingDurationMs.asStateFlow()
    private val _recordingStatus = MutableStateFlow<String?>(null)
    val recordingStatus: StateFlow<String?> = _recordingStatus.asStateFlow()
    private val _microphoneLevel = MutableStateFlow(0f)
    val microphoneLevel: StateFlow<Float> = _microphoneLevel.asStateFlow()
    private val _programMicMuted = MutableStateFlow(false)
    val programMicMuted: StateFlow<Boolean> = _programMicMuted.asStateFlow()
    private val _outputStatuses = MutableStateFlow<List<OutputStatus>>(emptyList())
    val outputStatuses: StateFlow<List<OutputStatus>> = _outputStatuses.asStateFlow()
    val talkbackReceiving: StateFlow<Boolean> = talkbackManager.receiving
    val talkbackSpeakerName: StateFlow<String> = talkbackManager.speakerName
    val talkbackMuted: StateFlow<Boolean> = talkbackManager.muted
    val talkbackVolume: StateFlow<Float> = talkbackManager.volume
    val talkbackReplying: StateFlow<Boolean> = talkbackManager.replying
    val obsTallyState: StateFlow<String> = talkbackManager.tallyState

    // One-shot UI events (e.g. "service died") — SharedFlow so they're
    // not replayed on recomposition / re-collection.
    private val _uiEvents = MutableSharedFlow<UiEvent>(extraBufferCapacity = 1)

    /** One-shot events the UI should show (snackbar, toast, etc.). */
    val uiEvents: SharedFlow<UiEvent> = _uiEvents.asSharedFlow()

    // ── Settings ─────────────────────────────────

    /** Whether to keep the screen on during streaming (user preference). */
    val keepScreenOnSetting: StateFlow<Boolean> = settingsRepository.getKeepScreenOn()
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), true)

    /** Resolution selected for the next preview/stream session. Defaults to HD 720p. */
    val resolution: StateFlow<com.port80.app.data.model.Resolution> = settingsRepository.getResolution()
        .stateIn(
            viewModelScope,
            SharingStarted.Eagerly,
            com.port80.app.data.model.Resolution(1280, 720)
        )

    /** Camera/encoder resolutions offered by the Ratio control. */
    val supportedResolutions: List<com.port80.app.data.model.Resolution> by lazy {
        val cameraId = deviceCapabilityQuery.getCameraIds().firstOrNull()
        val detected = cameraId?.let(deviceCapabilityQuery::getSupportedResolutions).orEmpty()
        val preferred = listOf(
            com.port80.app.data.model.Resolution(1280, 720),
            com.port80.app.data.model.Resolution(1920, 1080),
            com.port80.app.data.model.Resolution(854, 480)
        )
        preferred.filter { it in detected }.ifEmpty {
            detected.ifEmpty { preferred }
        }.sortedBy { it.width }
    }

    val previewZoom: StateFlow<Float> = settingsRepository.getPreviewZoom()
        .stateIn(viewModelScope, SharingStarted.Eagerly, 1f)
    val previewExposure: StateFlow<Int> = settingsRepository.getPreviewExposure()
        .stateIn(viewModelScope, SharingStarted.Eagerly, 0)
    val previewWhiteBalance: StateFlow<Int> = settingsRepository.getPreviewWhiteBalance()
        .stateIn(viewModelScope, SharingStarted.Eagerly, 5000)

    // ── Endpoint profiles ────────────────────────

    /** All configured streaming endpoint profiles. */
    val endpointProfiles: StateFlow<List<EndpointProfile>> = profileRepository.getAll()
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())

    /**
     * Endpoint selected on the camera screen. This is intentionally held in
     * memory immediately instead of waiting for encrypted preference writes;
     * otherwise a quick Select -> Go Live sequence can start the old profile.
     */
    private val _selectedProfileId = MutableStateFlow<String?>(null)
    val selectedProfileId: StateFlow<String?> = _selectedProfileId.asStateFlow()

    /** Set a profile as the active endpoint for streaming. */
    fun selectEndpoint(profileId: String) {
        if (endpointProfiles.value.none { it.id == profileId }) return
        _selectedProfileId.value = profileId
        viewModelScope.launch {
            profileRepository.setDefault(profileId)
        }
    }

    // ── Camera info ──────────────────────────────

    /** All cameras available on this device. */
    val availableCameras: List<CameraInfo> by lazy { deviceCapabilityQuery.getAvailableCameras() }

    /** Whether the device has more than one rear camera (multi-camera). */
    val hasMultipleRearCameras: Boolean by lazy { deviceCapabilityQuery.getRearCameras().size > 1 }

    // ── Stabilization ────────────────────────────

    /** Supported stabilization modes for the current default camera. */
    val supportedStabilizationModes: Set<StabilizationMode> by lazy {
        val defaultId = availableCameras.firstOrNull()?.id ?: "0"
        deviceCapabilityQuery.getSupportedStabilizationModes(defaultId)
    }

    // ── Minimal mode ─────────────────────────────

    private val _isMinimalMode = MutableStateFlow(false)

    /** Whether the UI is in minimal (power-saving) mode. */
    val isMinimalMode: StateFlow<Boolean> = _isMinimalMode.asStateFlow()

    /** Toggle minimal streaming mode on/off. */
    fun toggleMinimalMode() {
        _isMinimalMode.value = !_isMinimalMode.value
    }

    /** Exit minimal mode (e.g., when the user taps "Restore Preview"). */
    fun exitMinimalMode() {
        _isMinimalMode.value = false
    }

    // ── Surface management ───────────────────────

    // CompletableDeferred acts as a one-shot gate:
    // preview attach waits until the surface is actually created.
    private var surfaceDeferred = CompletableDeferred<SurfaceHolder>()

    // WeakReference prevents memory leaks — if the Activity/Fragment is
    // destroyed, the OpenGlView can be garbage-collected.
    private var surfaceRef: WeakReference<OpenGlView>? = null

    // ── Service connection callback ──────────────
    // Android calls these methods when the service binding succeeds or drops.
    private val serviceConnection = object : ServiceConnection {

        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            // Cast the generic IBinder to our LocalBinder to get
            // the StreamingServiceControl interface.
            val localBinder = binder as? StreamingService.LocalBinder
            serviceControl = localBinder?.getService()
            isBound = true

            serviceControl?.let { service ->
                // Detect process-death recovery: the ViewModel was recreated
                // but the FGS was still alive and streaming.
                val serviceState = service.streamState.value
                if (serviceState is StreamState.Live || serviceState is StreamState.Reconnecting) {
                    RedactingLogger.i(TAG, "Recovering — service already in $serviceState")
                } else {
                    RedactingLogger.d(TAG, "Bound to StreamingService (state: $serviceState)")
                }

                // Start collecting the service's state flows into our local
                // MutableStateFlows. Each runs in its own coroutine so they
                // don't block each other.
                startServiceCollectors(service)

                // Re-attach preview surface. This covers two cases:
                // 1. Surface was created before the service connected (normal flow)
                // 2. Process-death recovery: ViewModel + surface are new,
                //    service is already Live and needs the new surface
                surfaceRef?.get()?.let { openGlView ->
                    service.attachPreviewSurface(openGlView)
                    // Auto-start preview if service is idle
                    val serviceState = service.streamState.value
                    if (serviceState == StreamState.Idle || serviceState is StreamState.Stopped) {
                        service.startPreviewOnly()
                    }
                }
            }
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            // Called when the service process crashes or is killed by the OS.
            // This does NOT fire on a normal unbind — only on unexpected death.
            val wasPreviouslyActive = serviceControl != null && (
                _streamState.value is StreamState.Live ||
                _streamState.value is StreamState.Connecting ||
                _streamState.value is StreamState.Reconnecting)
            serviceControl = null
            isBound = false
            stopServiceCollectors()
            _streamState.value = StreamState.Stopped(StopReason.USER_REQUEST)
            RedactingLogger.w(TAG, "Service disconnected unexpectedly")

            if (wasPreviouslyActive) {
                _uiEvents.tryEmit(UiEvent.ServiceDied)
            }
        }
    }

    // Bind to the service as soon as this ViewModel is created.
    // On first launch this creates the service (BIND_AUTO_CREATE).
    // After process-death recreation the FGS may still be running —
    // binding succeeds and onServiceConnected picks up the live state.
    init {
        bindToService()
        startObsAddressSync()
        viewModelScope.launch {
            profileRepository.getAll().collect { profiles ->
                val selectedStillExists = profiles.any { it.id == _selectedProfileId.value }
                if (!selectedStillExists) {
                    _selectedProfileId.value = profiles.firstOrNull { it.isDefault }?.id
                        ?: profiles.firstOrNull()?.id
                }
            }
        }
        viewModelScope.launch {
            profileRepository.getAll().collect { profiles ->
                talkbackManager.setAllowedTokens(profiles.mapNotNull { it.srtStreamId }.toSet())
                val active = profiles.firstOrNull { it.isDefault } ?: profiles.firstOrNull()
                val host = runCatching { URI(active?.url.orEmpty()).host }.getOrNull()
                talkbackManager.configureReturn(host, active?.srtStreamId)
            }
        }
    }

    fun toggleTalkbackMute() = talkbackManager.toggleMuted()
    fun setTalkbackVolume(value: Float) = talkbackManager.setVolume(value)
    fun setTalkbackReplying(value: Boolean) = talkbackManager.setReplying(value)

    /** Keeps QR/discovery-paired OBS endpoints current when DHCP changes the desktop IP. */
    private fun startObsAddressSync() {
        viewModelScope.launch {
            while (true) {
                val offers = runCatching { obsDiscoveryClient.discover(timeoutMs = 800) }
                    .getOrDefault(emptyList())
                offers.forEach { offer ->
                        talkbackManager.configureReturn(offer.address, offer.pairingToken)
                        val saved = profileRepository.getAll().first()
                            .firstOrNull { it.isSrt && !it.srtStreamId.isNullOrBlank() &&
                                it.srtStreamId == offer.pairingToken }
                        if (saved != null) {
                            val refreshedUrl = "srt://${offer.address}:${offer.srtPort}"
                            if (saved.url != refreshedUrl || saved.name != offer.assignedCameraName) {
                                profileRepository.save(saved.copy(
                                    name = offer.assignedCameraName,
                                    url = refreshedUrl
                                ))
                            }
                        }
                    }
                runCatching {
                    cameraHealthReporter.report(
                        destinations = offers,
                        streamState = _streamState.value,
                        stats = _streamStats.value,
                        microphoneMuted = _programMicMuted.value
                    )
                }
                delay(1_000)
            }
        }
    }

    // ══════════════════════════════════════════════
    //  User Actions — forwarded to the service
    // ══════════════════════════════════════════════

    /**
     * Start streaming with the given endpoint profile.
     *
     * This launches the foreground service (if not running) and triggers
     * the RTMP connection. The Intent carries only the profile ID — the
     * service reads the actual credentials from the repository at runtime.
     *
     * Uses [startForegroundService] (required on API 26+) to ensure the
     * service can call startForeground() within the 10-second window.
     */
    fun startStream(profileId: String) {
        val context = getApplication<Application>()

        // Build the intent with ONLY the profile ID.
        // Never put stream keys or URLs in Intent extras.
        val intent = Intent(context, StreamingService::class.java).apply {
            putExtra(StreamingService.EXTRA_PROFILE_ID, profileId)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(intent)
        } else {
            context.startService(intent)
        }

        // Bind so we can observe state and forward commands.
        if (!isBound) {
            bindToService()
        }
    }

    /** Start the endpoint visibly selected in the UI; fall back only if needed. */
    fun startStreamWithDefaultProfile() {
        viewModelScope.launch {
            val profiles = profileRepository.getAll().first()
            val resolvedProfileId = _selectedProfileId.value
                ?.takeIf { selected -> profiles.any { it.id == selected } }
                ?: profileRepository.getDefault()?.id
                ?: profileRepository.getAll().first().firstOrNull()?.id

            if (resolvedProfileId == null) {
                _uiEvents.tryEmit(UiEvent.NoProfilesConfigured)
                return@launch
            }

            startStream(resolvedProfileId)
        }
    }

    /**
     * Stop the current stream gracefully.
     * Idempotent — safe to call when already stopped or idle.
     */
    fun stopStream() {
        serviceControl?.stopStream()
    }

    /**
     * Toggle audio mute on/off.
     * Idempotent — the service tracks the current mute state internally.
     */
    fun toggleMute() {
        serviceControl?.toggleMute()
    }

    /**
     * Switch between front and back cameras.
     * Idempotent — safe to call during any stream state.
     */
    fun switchCamera() {
        serviceControl?.switchCamera()
    }

    /**
     * Switch to a specific camera by Camera2 camera ID.
     * Used by the camera picker UI on multi-camera devices.
     */
    fun switchCamera(cameraId: String) {
        serviceControl?.switchCamera(cameraId)
    }

    fun setResolution(resolution: com.port80.app.data.model.Resolution) {
        if (resolution !in supportedResolutions) return
        viewModelScope.launch { settingsRepository.setResolution(resolution) }
    }

    fun setZoom(level: Float) {
        serviceControl?.setZoom(level)
        viewModelScope.launch { settingsRepository.setPreviewZoom(level) }
    }

    fun setExposure(value: Int) {
        serviceControl?.setExposure(value)
        viewModelScope.launch { settingsRepository.setPreviewExposure(value) }
    }

    fun toggleTorch(): Boolean = serviceControl?.toggleTorch() ?: false

    fun setWhiteBalanceKelvin(kelvin: Int) {
        serviceControl?.setWhiteBalanceKelvin(kelvin)
        viewModelScope.launch { settingsRepository.setPreviewWhiteBalance(kelvin) }
    }

    fun setPreviewOrientation(activity: Activity, orientation: Int) {
        viewModelScope.launch {
            settingsRepository.setPreferredOrientation(orientation)
            settingsRepository.setOrientationLocked(true)
            OrientationHelper.lock(activity, orientation)
        }
    }

    fun toggleRecording() {
        serviceControl?.toggleRecording()
    }

    // ══════════════════════════════════════════════
    //  Surface Lifecycle
    // ══════════════════════════════════════════════

    /**
     * Called when the SurfaceView's surface is created and ready for drawing.
     *
     * This does two things:
     * 1. Completes the [surfaceDeferred] gate (signals "surface is available")
     * 2. Attaches the surface to the service so RtmpCamera2 can render the
     *    camera preview onto it
     *
     * The SurfaceHolder is stored as a [WeakReference] — if the Activity is
     * garbage-collected, we won't hold it in memory.
     */
    fun onSurfaceReady(openGlView: OpenGlView) {
        surfaceRef = WeakReference(openGlView)

        // Complete the gate only once; subsequent calls are no-ops.
        if (!surfaceDeferred.isCompleted) {
            surfaceDeferred.complete(openGlView.holder)
        }

        // If already bound to the service, attach immediately.
        serviceControl?.attachPreviewSurface(openGlView)

        // Auto-start preview if camera permission is granted and we're idle
        val state = _streamState.value
        if (state == StreamState.Idle || state is StreamState.Stopped) {
            serviceControl?.startPreviewOnly()
        }

        RedactingLogger.d(TAG, "Surface ready — preview attached")
    }

    /**
     * Called when the SurfaceView's surface is destroyed (e.g., the user
     * navigates away or the Activity goes to the background).
     *
     * This detaches the preview but does NOT stop the stream — the service
     * continues streaming without a preview surface.
     */
    fun onSurfaceDestroyed() {
        surfaceRef = null
        val state = _streamState.value

        if (state is StreamState.Previewing) {
            // In preview-only mode, stop the entire preview
            serviceControl?.stopPreviewOnly()
        } else {
            // During streaming, just detach the surface (stream continues)
            serviceControl?.detachPreviewSurface()
        }

        // Reset the deferred so the next surfaceCreated() can complete it
        // again. CompletableDeferred is single-use — once completed, a new
        // instance is needed for the next surface lifecycle.
        surfaceDeferred = CompletableDeferred()
        RedactingLogger.d(TAG, "Surface destroyed — preview detached")
    }

    /**
     * Called from [CameraPreview] whenever the surface dimensions change
     * (e.g., device rotation). Forwards to the service so it can restart
     * the preview at the correct resolution.
     */
    fun onSurfaceSizeChanged(width: Int, height: Int) {
        serviceControl?.onPreviewDimensionsChanged(width, height)
    }

    /**
     * Restore the user's saved orientation preference on the given Activity.
     * Called when streaming ends so the Activity returns to the user's choice.
     */
    fun restoreOrientationPreference(activity: Activity) {
        viewModelScope.launch {
            val locked = settingsRepository.getOrientationLocked().first()
            val preferred = settingsRepository.getPreferredOrientation().first()
            if (locked && preferred != ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED) {
                OrientationHelper.lock(activity, preferred)
            } else {
                OrientationHelper.unlock(activity)
            }
        }
    }

    // ══════════════════════════════════════════════
    //  Private Helpers
    // ══════════════════════════════════════════════

    /**
     * Bind to the StreamingService using the application context.
     *
     * BIND_AUTO_CREATE will create the service if it isn't running yet.
     * If the service hasn't been started via startForegroundService(),
     * the bind may fail — we catch and log silently.
     */
    private fun bindToService() {
        val context = getApplication<Application>()
        val intent = Intent(context, StreamingService::class.java)
        try {
            context.bindService(intent, serviceConnection, Context.BIND_AUTO_CREATE)
        } catch (e: Exception) {
            RedactingLogger.w(TAG, "Could not bind to service (not running yet)")
        }
    }

    private fun startServiceCollectors(service: StreamingServiceControl) {
        stopServiceCollectors()
        serviceCollectorsJob = viewModelScope.launch {
            launch {
                service.streamState.collect { state ->
                    _streamState.value = state
                }
            }
            launch {
                service.streamStats.collect { stats ->
                    _streamStats.value = stats
                }
            }
            launch {
                service.lastFailureDetail.collect { detail ->
                    _lastFailureDetail.value = detail
                }
            }
            launch { service.isRecording.collect { _isRecording.value = it } }
            launch { service.recordingDurationMs.collect { _recordingDurationMs.value = it } }
            launch { service.recordingStatus.collect { _recordingStatus.value = it } }
            launch { service.microphoneLevel.collect { _microphoneLevel.value = it } }
            launch { service.programMicMuted.collect { _programMicMuted.value = it } }
            launch { service.outputStatuses.collect { _outputStatuses.value = it } }
            // Auto-reset minimal mode when stream stops
            launch {
                service.streamState.collect { state ->
                    if (state is StreamState.Idle || state is StreamState.Stopped ||
                        state is StreamState.Previewing
                    ) {
                        _isMinimalMode.value = false
                    }
                }
            }
        }
    }

    private fun stopServiceCollectors() {
        serviceCollectorsJob?.cancel()
        serviceCollectorsJob = null
    }

    /**
     * Clean up when the ViewModel is being destroyed (e.g., the user
     * leaves the streaming screen for good).
     *
     * Unbinds from the service to prevent leaked ServiceConnection.
     * This does NOT stop the stream — the foreground service continues
     * independently until explicitly stopped.
     */
    override fun onCleared() {
        super.onCleared()
        stopServiceCollectors()

        // Stop preview if we were in preview-only mode
        if (_streamState.value is StreamState.Previewing) {
            serviceControl?.stopPreviewOnly()
        }

        if (isBound) {
            try {
                getApplication<Application>().unbindService(serviceConnection)
            } catch (e: Exception) {
                // Already unbound — harmless.
            }
            isBound = false
        }
    }

    // ══════════════════════════════════════════════
    //  UI Events
    // ══════════════════════════════════════════════

    /** One-shot events that the UI layer should display once. */
    sealed class UiEvent {
        /** The streaming service died unexpectedly (OS killed the process). */
        data object ServiceDied : UiEvent()

        /** No endpoint profile is configured, so stream start cannot proceed. */
        data object NoProfilesConfigured : UiEvent()
    }
}
