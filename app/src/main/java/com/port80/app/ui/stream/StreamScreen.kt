package com.port80.app.ui.stream

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Cameraswitch
import androidx.compose.material.icons.filled.CropFree
import androidx.compose.material.icons.filled.Exposure
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.DarkMode
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material.icons.filled.RecordVoiceOver
import androidx.compose.material.icons.filled.GridOn
import androidx.compose.material.icons.filled.ScreenRotation
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Podcasts
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.FiberManualRecord
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Remove
import androidx.compose.material.icons.filled.BrightnessLow
import androidx.compose.material.icons.filled.BrightnessHigh
import androidx.compose.material.icons.filled.FlashlightOff
import androidx.compose.material.icons.filled.FlashlightOn
import androidx.compose.material.icons.filled.AcUnit
import androidx.compose.material.icons.filled.WbSunny
import androidx.compose.material.icons.filled.VolumeDown
import androidx.compose.material.icons.filled.VolumeUp
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.SmallFloatingActionButton
import androidx.compose.material3.Surface
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.foundation.layout.offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalConfiguration
import android.content.res.Configuration
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import android.app.Activity
import android.content.pm.ActivityInfo
import android.view.WindowManager
import androidx.hilt.navigation.compose.hiltViewModel
import com.port80.app.data.model.CameraInfo
import com.port80.app.data.model.EndpointProfile
import com.port80.app.data.model.StopReason
import com.port80.app.data.model.StreamState
import com.port80.app.ui.components.CameraPreview
import com.port80.app.ui.components.CameraPreviewPlaceholder
import com.port80.app.ui.components.MinimalStreamingOverlay
import com.port80.app.ui.components.PermissionHandler
import com.port80.app.ui.components.PreviewPermissionGate
import com.port80.app.ui.components.StreamHud
import com.port80.app.service.OutputConnectionState
import com.port80.app.service.OutputStatus
import com.port80.app.util.OrientationHelper
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.delay
import kotlin.math.roundToInt

private enum class Adjustment { ZOOM, EXPOSURE, BRIGHTNESS, WHITE_BALANCE, VOLUME }

/**
 * Main streaming screen — the primary UI the user interacts with.
 *
 * Layout (landscape-first design):
 * ┌──────────────────────────────────┐
 * │ [HUD - bitrate, fps, duration]  │
 * │                                  │
 * │       Camera Preview             │ [Start/Stop]
 * │                                  │ [Mute]
 * │                                  │ [Switch Camera]
 * │ [Connection state]              │ [Settings]
 * └──────────────────────────────────┘
 *
 * Controls are at the right edge for easy thumb reach in landscape.
 */
@Composable
fun StreamScreen(
    viewModel: StreamViewModel = hiltViewModel(),
    onNavigateToSettings: () -> Unit = {},
    onNavigateToEndpoints: () -> Unit = {}
) {
    val isPortrait = LocalConfiguration.current.orientation == Configuration.ORIENTATION_PORTRAIT
    val streamState by viewModel.streamState.collectAsState()
    val streamStats by viewModel.streamStats.collectAsState()
    val lastFailureDetail by viewModel.lastFailureDetail.collectAsState()
    val microphoneLevel by viewModel.microphoneLevel.collectAsState()
    val programMicMuted by viewModel.programMicMuted.collectAsState()
    val talkbackReceiving by viewModel.talkbackReceiving.collectAsState()
    val talkbackSpeakerName by viewModel.talkbackSpeakerName.collectAsState()
    val talkbackMuted by viewModel.talkbackMuted.collectAsState()
    val talkbackVolume by viewModel.talkbackVolume.collectAsState()
    val talkbackReplying by viewModel.talkbackReplying.collectAsState()
    val obsTallyState by viewModel.obsTallyState.collectAsState()
    val recordingStatus by viewModel.recordingStatus.collectAsState()
    val outputStatuses by viewModel.outputStatuses.collectAsState()
    val savedZoom by viewModel.previewZoom.collectAsState()
    val savedExposure by viewModel.previewExposure.collectAsState()
    val savedWhiteBalance by viewModel.previewWhiteBalance.collectAsState()

    // Keep screen on while streaming (when enabled in settings)
    val keepScreenOn by viewModel.keepScreenOnSetting.collectAsState()
    val activity = LocalContext.current as? Activity

    val isActiveStream = streamState is StreamState.Connecting ||
        streamState is StreamState.Live ||
        streamState is StreamState.Reconnecting

    val isMinimalMode by viewModel.isMinimalMode.collectAsState()
    val endpointProfiles by viewModel.endpointProfiles.collectAsState()
    val selectedProfileId by viewModel.selectedProfileId.collectAsState()
    val activeEndpointName = endpointProfiles.firstOrNull { it.id == selectedProfileId }?.name
        ?: endpointProfiles.firstOrNull()?.name

    // Use Activity window flag so it survives in-app navigation
    DisposableEffect(isActiveStream, keepScreenOn) {
        if (isActiveStream && keepScreenOn) {
            activity?.window?.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            activity?.window?.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
        onDispose {
            activity?.window?.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }

    // Lock orientation while streaming to prevent rotation-induced preview disruption.
    // Restore user preference when stream ends.
    DisposableEffect(isActiveStream) {
        if (activity != null) {
            OrientationHelper.lock(activity, ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE)
        }
        onDispose {}
    }

    val snackbarHostState = remember { SnackbarHostState() }

    // Show error snackbar when stream stops with an error
    LaunchedEffect(streamState) {
        val state = streamState
        if (state is StreamState.Stopped && state.reason != StopReason.USER_REQUEST) {
            val message = stoppedMessage(state.reason, lastFailureDetail)
            snackbarHostState.showSnackbar(message)
        }
    }

    LaunchedEffect(viewModel) {
        viewModel.uiEvents.collectLatest { event ->
            when (event) {
                StreamViewModel.UiEvent.ServiceDied -> {
                    snackbarHostState.showSnackbar("Streaming service stopped unexpectedly")
                }

                StreamViewModel.UiEvent.NoProfilesConfigured -> {
                    snackbarHostState.showSnackbar("No streaming endpoint configured")
                }
            }
        }
    }

    LaunchedEffect(recordingStatus) {
        recordingStatus?.let { snackbarHostState.showSnackbar(it) }
    }

    // Track whether to show the plain-RTMP warning dialog
    var showRtmpWarning by remember { mutableStateOf(false) }
    var pendingProfileId by remember { mutableStateOf<String?>(null) }
    var showGrid by remember { mutableStateOf(false) }
    var torchEnabled by remember { mutableStateOf(false) }
    var landscapePreview by remember { mutableStateOf(true) }
    var controlsVisible by remember { mutableStateOf(true) }
    var activeAdjustment by remember { mutableStateOf<Adjustment?>(null) }
    var interactionVersion by remember { mutableStateOf(0) }

    LaunchedEffect(controlsVisible, interactionVersion) {
        if (controlsVisible) {
            delay(20_000)
            controlsVisible = false
        }
    }

    Scaffold(
        snackbarHost = { SnackbarHost(snackbarHostState) },
        containerColor = Color.Black
    ) { contentPadding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(contentPadding)
        ) {
            // Camera preview is ALWAYS in the composition tree to keep the
            // SurfaceHolder alive. Removing it would trigger surfaceDestroyed()
            // which stops the RootEncoder camera capture and kills the stream.
            val showMinimal = isMinimalMode && isActiveStream
            PreviewPermissionGate(
                onGranted = {
                    CameraPreview(
                        modifier = if (showMinimal) Modifier.size(1.dp) else Modifier.fillMaxSize(),
                        onSurfaceReady = { openGlView -> viewModel.onSurfaceReady(openGlView) },
                        onSurfaceDestroyed = { viewModel.onSurfaceDestroyed() },
                        onSurfaceSizeChanged = { w, h -> viewModel.onSurfaceSizeChanged(w, h) }
                    )
                },
                onDenied = {
                    if (!showMinimal) {
                        CameraPreviewPlaceholder(
                            message = "Camera permission required for preview.\nTap Start to request permission."
                        )
                    }
                }
            )

            if (showMinimal) {
                MinimalStreamingOverlay(
                    stats = streamStats,
                    isMuted = programMicMuted,
                    onStopStream = { viewModel.stopStream() },
                    onToggleMute = { viewModel.toggleMute() },
                    onExitMinimalMode = { viewModel.exitMinimalMode() }
                )
            } else {
                PreviewStatusEdge(
                    state = streamState,
                    talkbackReceiving = talkbackReceiving,
                    tallyState = obsTallyState,
                    modifier = Modifier.fillMaxSize()
                )
                if (showGrid) RuleOfThirdsGrid()

                Box(
                    Modifier.fillMaxSize().pointerInput(Unit) {
                        detectTapGestures {
                            controlsVisible = !controlsVisible
                            interactionVersion++
                        }
                    }
                )

                AnimatedVisibility(
                    visible = controlsVisible,
                    enter = fadeIn(),
                    exit = fadeOut(),
                    modifier = Modifier.fillMaxSize()
                ) {
                    Box(Modifier.fillMaxSize()) {
                        PreviewStatusHeader(
                            state = streamState,
                            fps = streamStats.fps,
                            tallyState = obsTallyState,
                            modifier = Modifier.align(Alignment.TopStart).padding(18.dp)
                        )

                        ConnectionStateLabel(
                            state = streamState,
                            modifier = Modifier.align(Alignment.TopCenter).padding(16.dp)
                        )

                        if (outputStatuses.isNotEmpty()) {
                            MultiOutputStatusBar(
                                statuses = outputStatuses,
                                modifier = Modifier.align(Alignment.TopCenter).padding(top = 58.dp)
                            )
                        }

                        BottomControlBar(
                            streamState = streamState,
                            viewModel = viewModel,
                            showGrid = showGrid,
                            torchEnabled = torchEnabled,
                            activeAdjustment = activeAdjustment,
                            onGridChanged = { showGrid = it },
                            onTorchChanged = { torchEnabled = it },
                            onAdjustmentSelected = { selected ->
                                activeAdjustment = if (activeAdjustment == selected) null else selected
                            },
                            onSettingsClick = onNavigateToSettings,
                            onMinimalModeClick = { viewModel.toggleMinimalMode() },
                            zoom = savedZoom,
                            brightness = savedExposure.toFloat(),
                            whiteBalance = savedWhiteBalance.toFloat(),
                            microphoneLevel = microphoneLevel,
                            isMuted = programMicMuted,
                            talkbackVolume = talkbackVolume,
                            talkbackMuted = talkbackMuted,
                            onInteraction = { interactionVersion++ },
                            modifier = Modifier.align(Alignment.BottomCenter)
                        )
                    }
                }

                // Intercom status is intentionally outside AnimatedVisibility so the
                // camera operator can always see the director and hold it to reply.
                TalkbackIndicator(
                    receiving = talkbackReceiving,
                    speakerName = talkbackSpeakerName,
                    muted = talkbackMuted,
                    replying = talkbackReplying,
                    compact = isPortrait,
                    onReplyingChanged = viewModel::setTalkbackReplying,
                    modifier = Modifier.align(Alignment.TopEnd).padding(
                        top = if (isPortrait) 68.dp else 18.dp,
                        end = if (isPortrait) 14.dp else 92.dp
                    )
                )
            }
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun AdjustmentToggle(
    expanded: Boolean,
    onExpandedChanged: (Boolean) -> Unit,
    modifier: Modifier = Modifier
) {
    Surface(
        modifier = modifier.combinedClickable(
            onClick = { onExpandedChanged(!expanded) },
            onLongClick = { onExpandedChanged(true) }
        ),
        color = Color.Black.copy(alpha = .30f),
        shape = CircleShape
    ) {
        Icon(
            imageVector = Icons.Filled.Settings,
            contentDescription = if (expanded) "Hide camera controls" else "Hold for camera controls",
            tint = Color.White.copy(alpha = .9f),
            modifier = Modifier.padding(10.dp).size(20.dp)
        )
    }
}

@Composable
private fun MultiOutputStatusBar(
    statuses: List<OutputStatus>,
    modifier: Modifier = Modifier
) {
    Surface(
        color = Color.Black.copy(alpha = 0.58f),
        shape = CircleShape,
        modifier = modifier
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 7.dp),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            statuses.take(4).forEach { output ->
                val color = when (output.state) {
                    OutputConnectionState.LIVE -> Color(0xFF00E676)
                    OutputConnectionState.CONNECTING,
                    OutputConnectionState.RETRYING -> Color(0xFFFFC107)
                    OutputConnectionState.AUTHENTICATION_FAILED,
                    OutputConnectionState.UNAVAILABLE -> Color(0xFFFF5252)
                    OutputConnectionState.STOPPED -> Color.LightGray
                }
                Row(
                    horizontalArrangement = Arrangement.spacedBy(5.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Box(Modifier.size(7.dp).background(color, CircleShape))
                    Text(
                        text = output.name,
                        color = Color.White,
                        fontSize = 10.sp,
                        maxLines = 1
                    )
                }
            }
            if (statuses.size > 4) {
                Text("+${statuses.size - 4}", color = Color.White, fontSize = 10.sp)
            }
        }
    }
}

@Composable
private fun PreviewStatusEdge(
    state: StreamState,
    talkbackReceiving: Boolean,
    tallyState: String,
    modifier: Modifier = Modifier
) {
    val edgeColor = when {
        talkbackReceiving -> Color(0xFFFF304F)
        tallyState == "LIVE" -> Color(0xFF20D889)
        tallyState != "LIVE" && (state is StreamState.Live || state is StreamState.Previewing) -> Color(0xFFFF9800)
        state is StreamState.Connecting || state is StreamState.Reconnecting -> Color(0xFFFFB020)
        else -> Color.White.copy(alpha = .28f)
    }
    Box(
        modifier = modifier
            .border(width = 3.dp, color = edgeColor, shape = RoundedCornerShape(2.dp))
    )
}

@Composable
private fun PreviewStatusHeader(
    state: StreamState,
    fps: Float,
    tallyState: String,
    modifier: Modifier = Modifier
) {
    val (label, color) = when {
        tallyState == "LIVE" -> "LIVE • ON PROGRAM" to Color(0xFF00E676)
        tallyState != "LIVE" && (state is StreamState.Live || state is StreamState.Previewing) -> "STANDBY • PREVIEW" to Color(0xFFFF9800)
        state is StreamState.Reconnecting -> "WAITING FOR OBS" to Color(0xFFFFC107)
        state is StreamState.Connecting -> "CONNECTING" to Color(0xFFFFC107)
        state is StreamState.Previewing -> "READY" to Color(0xFF00E676)
        else -> "STANDBY" to Color.LightGray
    }
    Surface(color = Color.Transparent, modifier = modifier) {
        Row(
            modifier = Modifier.padding(horizontal = 4.dp, vertical = 2.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(9.dp)
        ) {
            Box(Modifier.size(7.dp).background(color, CircleShape))
            Column {
                Text(label, color = Color.White, fontSize = 11.sp)
                Text("1080p  •  ${fps.toInt().coerceAtLeast(0)}fps  •  H.264", color = Color.White.copy(alpha = .82f), fontSize = 9.sp)
            }
        }
    }
}

@Composable
private fun PreviewAdjustmentPanel(
    zoom: Float,
    brightness: Float,
    whiteBalance: Float,
    microphoneLevel: Float,
    isMuted: Boolean,
    talkbackVolume: Float,
    talkbackMuted: Boolean,
    onZoomChanged: (Float) -> Unit,
    onBrightnessChanged: (Int) -> Unit,
    onWhiteBalanceChanged: (Int) -> Unit,
    onTalkbackVolumeChanged: (Float) -> Unit,
    onToggleTalkbackMute: () -> Unit,
    activeAdjustment: Adjustment,
    onInteraction: () -> Unit,
    modifier: Modifier = Modifier
) {
    val isPortrait = LocalConfiguration.current.orientation == Configuration.ORIENTATION_PORTRAIT
    val sliderColors = SliderDefaults.colors(
        thumbColor = Color.White,
        activeTrackColor = Color(0xFF28C76F),
        inactiveTrackColor = Color.White.copy(alpha = .22f)
    )
    if (isPortrait) {
        PortraitAdjustmentTray(
            zoom = zoom,
            brightness = brightness,
            whiteBalance = whiteBalance,
            volume = if (talkbackMuted) 0f else talkbackVolume,
            colors = sliderColors,
            onZoomChanged = {
                onZoomChanged(it.coerceIn(1f, 8f))
                onInteraction()
            },
            onBrightnessChanged = {
                onBrightnessChanged(it.coerceIn(-6f, 6f).roundToInt())
                onInteraction()
            },
            onWhiteBalanceChanged = {
                onWhiteBalanceChanged(it.coerceIn(2500f, 7500f).roundToInt())
                onInteraction()
            },
            onVolumeChanged = {
                if (talkbackMuted && it > 0f) onToggleTalkbackMute()
                onTalkbackVolumeChanged(it.coerceIn(0f, 1f))
                onInteraction()
            },
            modifier = modifier
        )
        return
    }
    Surface(
        modifier = modifier.fillMaxWidth(if (isPortrait) .96f else .86f),
        color = Color.Transparent,
        shape = RoundedCornerShape(28.dp)
    ) {
        Row(
            modifier = Modifier.padding(
                horizontal = if (isPortrait) 12.dp else 24.dp,
                vertical = if (isPortrait) 12.dp else 10.dp
            ),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(if (isPortrait) 8.dp else 22.dp)
        ) {
            if (activeAdjustment == Adjustment.ZOOM) AdjustmentSlider(
                label = "ZOOM",
                valueText = String.format("%.1fx", zoom),
                value = zoom,
                range = 1f..8f,
                colors = sliderColors,
                modifier = Modifier.weight(if (isPortrait) .9f else 1f),
                onValueChange = {
                    onZoomChanged(it)
                    onInteraction()
                }
            )
            if (activeAdjustment == Adjustment.BRIGHTNESS || activeAdjustment == Adjustment.EXPOSURE) AdjustmentSlider(
                label = if (activeAdjustment == Adjustment.EXPOSURE) "EXPOSURE" else "BRIGHTNESS",
                valueText = if (brightness == 0f) "0 EV" else String.format("%+.1f EV", brightness),
                value = brightness,
                range = -6f..6f,
                colors = sliderColors,
                modifier = Modifier.weight(if (isPortrait) .9f else 1f),
                onValueChange = {
                    onBrightnessChanged(it.roundToInt())
                    onInteraction()
                }
            )
            if (activeAdjustment == Adjustment.WHITE_BALANCE) AdjustmentSlider(
                label = "WHITE BALANCE",
                valueText = "${whiteBalance.roundToInt()}K",
                value = whiteBalance,
                range = 2500f..7500f,
                colors = sliderColors,
                modifier = Modifier.weight(1f),
                onValueChange = {
                    onWhiteBalanceChanged(it.roundToInt())
                    onInteraction()
                }
            )
            if (activeAdjustment == Adjustment.VOLUME) Column(Modifier.weight(if (isPortrait) 1f else .8f)) {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("AUDIO", color = Color.White.copy(alpha = .78f), fontSize = 11.sp)
                    Text(
                        if (isMuted) "MUTED" else "${(microphoneLevel.coerceIn(0f, 1f) * 100).roundToInt()}%",
                        color = Color.White,
                        fontSize = 11.sp
                    )
                }
                Box(
                    Modifier.fillMaxWidth().padding(top = 14.dp).height(5.dp)
                        .background(Color.White.copy(alpha=.22f), CircleShape)
                ) {
                    Box(
                        Modifier.fillMaxWidth(if (isMuted) 0f else microphoneLevel.coerceIn(0f, 1f))
                            .height(5.dp)
                            .background(
                                Brush.horizontalGradient(listOf(Color(0xFF28C76F), Color.Yellow, Color.Red)),
                                CircleShape
                            )
                    )
                }
                Row(
                    Modifier.fillMaxWidth().padding(top = 6.dp),
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text("TALKBACK", color = Color.White.copy(alpha = .78f), fontSize = 9.sp)
                    Text(if (talkbackMuted) "MUTED" else "${(talkbackVolume * 100).roundToInt()}%", color = Color.White, fontSize = 9.sp)
                }
                Slider(
                    value = if (talkbackMuted) 0f else talkbackVolume,
                    onValueChange = {
                        if (talkbackMuted && it > 0f) onToggleTalkbackMute()
                        onTalkbackVolumeChanged(it)
                        onInteraction()
                    },
                    valueRange = 0f..1f,
                    colors = sliderColors,
                    modifier = Modifier.fillMaxWidth().height(24.dp)
                )
            }
        }
    }
}

@Composable
private fun PortraitAdjustmentTray(
    zoom: Float,
    brightness: Float,
    whiteBalance: Float,
    volume: Float,
    colors: androidx.compose.material3.SliderColors,
    onZoomChanged: (Float) -> Unit,
    onBrightnessChanged: (Float) -> Unit,
    onWhiteBalanceChanged: (Float) -> Unit,
    onVolumeChanged: (Float) -> Unit,
    modifier: Modifier = Modifier
) {
    Surface(
        modifier = modifier.fillMaxWidth(.96f),
        color = Color.Transparent,
        shape = RoundedCornerShape(28.dp)
    ) {
        Row(
            Modifier.padding(horizontal = 12.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            PortraitTrayControl(
                label = "ZOOM",
                valueText = String.format("%.1fx", zoom),
                value = zoom,
                range = 1f..8f,
                startIcon = Icons.Filled.Remove,
                endIcon = Icons.Filled.Add,
                colors = colors,
                onStart = { onZoomChanged((zoom - .1f).coerceAtLeast(1f)) },
                onEnd = { onZoomChanged((zoom + .1f).coerceAtMost(8f)) },
                onValueChange = onZoomChanged,
                modifier = Modifier.weight(1f)
            )
            PortraitTrayDivider()
            PortraitTrayControl(
                label = "BRIGHTNESS",
                valueText = "${(((brightness + 6f) / 12f) * 100f).roundToInt()}%",
                value = brightness,
                range = -6f..6f,
                startIcon = Icons.Filled.BrightnessLow,
                endIcon = Icons.Filled.BrightnessHigh,
                colors = colors,
                onStart = { onBrightnessChanged((brightness - 1f).coerceAtLeast(-6f)) },
                onEnd = { onBrightnessChanged((brightness + 1f).coerceAtMost(6f)) },
                onValueChange = onBrightnessChanged,
                modifier = Modifier.weight(1f)
            )
            PortraitTrayDivider()
            PortraitTrayControl(
                label = "WB",
                valueText = "${whiteBalance.roundToInt()}K",
                value = whiteBalance,
                range = 2500f..7500f,
                startIcon = Icons.Filled.AcUnit,
                endIcon = Icons.Filled.WbSunny,
                colors = colors,
                onStart = { onWhiteBalanceChanged((whiteBalance - 250f).coerceAtLeast(2500f)) },
                onEnd = { onWhiteBalanceChanged((whiteBalance + 250f).coerceAtMost(7500f)) },
                onValueChange = onWhiteBalanceChanged,
                modifier = Modifier.weight(1f)
            )
            PortraitTrayDivider()
            PortraitTrayControl(
                label = "VOLUME",
                valueText = "${(volume * 100f).roundToInt()}%",
                value = volume,
                range = 0f..1f,
                startIcon = Icons.Filled.VolumeDown,
                endIcon = Icons.Filled.VolumeUp,
                colors = colors,
                onStart = { onVolumeChanged((volume - .1f).coerceAtLeast(0f)) },
                onEnd = { onVolumeChanged((volume + .1f).coerceAtMost(1f)) },
                onValueChange = onVolumeChanged,
                modifier = Modifier.weight(1f)
            )
        }
    }
}

@Composable
private fun PortraitTrayDivider() {
    Box(Modifier.padding(horizontal = 7.dp).width(1.dp).height(82.dp)
        .background(Color.White.copy(alpha = .22f)))
}

@Composable
private fun PortraitTrayControl(
    label: String,
    valueText: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    startIcon: ImageVector,
    endIcon: ImageVector,
    colors: androidx.compose.material3.SliderColors,
    onStart: () -> Unit,
    onEnd: () -> Unit,
    onValueChange: (Float) -> Unit,
    modifier: Modifier = Modifier
) {
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(label, color = Color.White.copy(alpha = .86f), fontSize = 9.sp, maxLines = 1)
            Text(valueText, color = Color.White, fontSize = 9.sp, maxLines = 1)
        }
        Row(
            Modifier.fillMaxWidth().padding(top = 7.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            TrayStepButton(startIcon, onStart)
            TrayStepButton(endIcon, onEnd)
        }
        Slider(
            value = value,
            onValueChange = onValueChange,
            valueRange = range,
            colors = colors,
            modifier = Modifier.fillMaxWidth().height(28.dp)
        )
    }
}

@Composable
private fun TrayStepButton(icon: ImageVector, onClick: () -> Unit) {
    Box(
        Modifier.size(30.dp).clip(CircleShape)
            .background(Color.Black.copy(alpha = .55f)).clickable(onClick = onClick),
        contentAlignment = Alignment.Center
    ) {
        Icon(icon, null, tint = Color.White, modifier = Modifier.size(17.dp))
    }
}

@Composable
private fun TalkbackIndicator(
    receiving: Boolean,
    speakerName: String,
    muted: Boolean,
    replying: Boolean,
    compact: Boolean = false,
    onReplyingChanged: (Boolean) -> Unit,
    modifier: Modifier = Modifier
) {
    val label = when {
        replying -> "TALKBACK ON · TAP TO DISABLE"
        receiving && muted -> "${speakerName.uppercase()} MUTED"
        receiving -> "${speakerName.uppercase()} TALKBACK"
        else -> "TALKBACK READY · TAP TO TALK"
    }
    Surface(
        modifier = modifier.clickable { onReplyingChanged(!replying) },
        color = when {
            replying -> Color(0xFF00A86B).copy(alpha = .55f)
            receiving -> Color.Red.copy(alpha = .62f)
            muted -> Color(0xFFB3261E).copy(alpha = .52f)
            else -> Color.Black.copy(alpha = .28f)
        },
        shape = CircleShape
    ) {
        Row(
            Modifier.padding(if (receiving || replying || muted) 7.dp else 5.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(if (compact) 5.dp else 7.dp)
        ) {
            Icon(
                Icons.Filled.RecordVoiceOver,
                "Director talkback",
                tint = Color.White,
                modifier = Modifier.size(if (compact) 12.dp else 15.dp)
            )
            if (receiving || replying || muted) {
                Text(label, color = Color.White, fontSize = if (compact) 8.sp else 10.sp)
            }
        }
    }
}

@Composable
private fun AdjustmentSlider(
    label: String,
    valueText: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    colors: androidx.compose.material3.SliderColors,
    onValueChange: (Float) -> Unit,
    modifier: Modifier = Modifier
) {
    Column(modifier) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(label, color = Color.White.copy(alpha = .78f), fontSize = 11.sp)
            Text(valueText, color = Color.White, fontSize = 11.sp)
        }
        Slider(
            value = value,
            onValueChange = onValueChange,
            valueRange = range,
            colors = colors,
            modifier = Modifier.fillMaxWidth().height(30.dp)
        )
    }
}

@Composable
private fun CompactRoundButton(icon:ImageVector,description:String,onClick:()->Unit,compact:Boolean=false){
    val buttonSize = if (compact) 32.dp else 40.dp
    val iconSize = if (compact) 17.dp else 22.dp
    Box(contentAlignment=Alignment.Center,modifier=Modifier.size(buttonSize).clip(CircleShape)
        .background(Color.Transparent).clickable(onClick=onClick)){
        Icon(icon,description,tint=Color.White,modifier=Modifier.size(iconSize))
    }
}

@Composable
private fun PreviewToolRail(
    isMuted:Boolean,showGrid:Boolean,torchEnabled:Boolean,onSwitchCamera:()->Unit,onToggleGrid:()->Unit,
    onToggleTorch:()->Unit,onRotate:()->Unit,onToggleMute:()->Unit,
    activeAdjustment: Adjustment?, onAdjustmentSelected: (Adjustment) -> Unit,
    modifier:Modifier=Modifier
){
    val compact = activeAdjustment != null
    Row(modifier=modifier, horizontalArrangement=Arrangement.spacedBy(14.dp), verticalAlignment=Alignment.CenterVertically){
        CompactRoundButton(Icons.Filled.Cameraswitch,"Switch camera",onSwitchCamera,compact)
        CompactRoundButton(Icons.Filled.GridOn,if(showGrid)"Hide grid" else "Show grid",onToggleGrid,compact)
        CompactRoundButton(
            if (torchEnabled) Icons.Filled.FlashlightOn else Icons.Filled.FlashlightOff,
            if (torchEnabled) "Turn flashlight off" else "Turn flashlight on",
            onToggleTorch,
            compact
        )
        CompactRoundButton(if(isMuted)Icons.Filled.MicOff else Icons.Filled.Mic,if(isMuted)"Unmute" else "Mute",onToggleMute,compact)
        CompactRoundButton(Icons.Filled.Search, "Zoom controls", { onAdjustmentSelected(Adjustment.ZOOM) },compact)
        CompactRoundButton(Icons.Filled.BrightnessHigh, "Brightness controls", { onAdjustmentSelected(Adjustment.BRIGHTNESS) },compact)
        CompactRoundButton(Icons.Filled.WbSunny, "White balance controls", { onAdjustmentSelected(Adjustment.WHITE_BALANCE) },compact)
        CompactRoundButton(Icons.Filled.VolumeUp, "Volume controls", { onAdjustmentSelected(Adjustment.VOLUME) },compact)
    }
}

@Composable
private fun RuleOfThirdsGrid(){
    Canvas(Modifier.fillMaxSize()){
        val c=Color.White.copy(alpha=.42f);val stroke=1.dp.toPx()
        drawLine(c,androidx.compose.ui.geometry.Offset(size.width/3f,0f),androidx.compose.ui.geometry.Offset(size.width/3f,size.height),stroke)
        drawLine(c,androidx.compose.ui.geometry.Offset(size.width*2f/3f,0f),androidx.compose.ui.geometry.Offset(size.width*2f/3f,size.height),stroke)
        drawLine(c,androidx.compose.ui.geometry.Offset(0f,size.height/3f),androidx.compose.ui.geometry.Offset(size.width,size.height/3f),stroke)
        drawLine(c,androidx.compose.ui.geometry.Offset(0f,size.height*2f/3f),androidx.compose.ui.geometry.Offset(size.width,size.height*2f/3f),stroke)
    }
}

@Composable
private fun BottomControlBar(
    streamState: StreamState,
    viewModel: StreamViewModel,
    showGrid: Boolean,
    torchEnabled: Boolean,
    activeAdjustment: Adjustment?,
    onGridChanged: (Boolean) -> Unit,
    onTorchChanged: (Boolean) -> Unit,
    onAdjustmentSelected: (Adjustment) -> Unit,
    onSettingsClick: () -> Unit,
    onMinimalModeClick: () -> Unit,
    zoom: Float,
    brightness: Float,
    whiteBalance: Float,
    microphoneLevel: Float,
    isMuted: Boolean,
    talkbackVolume: Float,
    talkbackMuted: Boolean,
    onInteraction: () -> Unit,
    modifier: Modifier = Modifier
) {
    val isStreaming = streamState is StreamState.Live ||
        streamState is StreamState.Connecting || streamState is StreamState.Reconnecting
    val isPreviewing = streamState is StreamState.Previewing
    val isRecording by viewModel.isRecording.collectAsState()
    val selectedResolution by viewModel.resolution.collectAsState()
    val dockColor = Color.Black.copy(alpha = .42f)

    Box(modifier.fillMaxWidth().height(92.dp).padding(horizontal = 2.dp, vertical = 4.dp)) {
      if (activeAdjustment != null) {
        PreviewAdjustmentPanel(
          zoom, brightness, whiteBalance, microphoneLevel, isMuted, talkbackVolume, talkbackMuted,
          viewModel::setZoom, viewModel::setExposure, viewModel::setWhiteBalanceKelvin,
          viewModel::setTalkbackVolume, viewModel::toggleTalkbackMute, activeAdjustment,
          onInteraction,
          // Overlay the panel without contributing to the dock's measured height.
          modifier = Modifier.align(Alignment.BottomCenter).offset(y = (-78).dp).width(280.dp)
        )
      }
      Surface(
        modifier = Modifier.fillMaxWidth(),
        color = dockColor,
        shape = RoundedCornerShape(topStart = 22.dp, topEnd = 22.dp, bottomStart = 22.dp, bottomEnd = 22.dp)
      ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
            horizontalArrangement = Arrangement.SpaceEvenly,
            verticalAlignment = Alignment.Bottom
        ) {
            DockButton(Icons.Filled.Cameraswitch, "FLIP", false, { viewModel.switchCamera() })
            DockButton(Icons.Filled.GridOn, "GRID", false, { onGridChanged(!showGrid) })
            DockButton(
                Icons.Filled.CropFree,
                selectedResolution.label.uppercase(),
                false,
                {
                    val options = viewModel.supportedResolutions
                    if (options.isNotEmpty()) {
                        val currentIndex = options.indexOf(selectedResolution)
                        val nextIndex = if (currentIndex < 0) 0 else (currentIndex + 1) % options.size
                        viewModel.setResolution(options[nextIndex])
                        onInteraction()
                    }
                }
            )
            DockButton(Icons.Filled.Exposure, "EXPOSURE", activeAdjustment == Adjustment.EXPOSURE, { onAdjustmentSelected(Adjustment.EXPOSURE) })
            DockButton(Icons.Filled.Search, "ZOOM", activeAdjustment == Adjustment.ZOOM, { onAdjustmentSelected(Adjustment.ZOOM) })
            DockButton(Icons.Filled.BrightnessHigh, "BRIGHTNESS", activeAdjustment == Adjustment.BRIGHTNESS, { onAdjustmentSelected(Adjustment.BRIGHTNESS) })
            DockButton(Icons.Filled.WbSunny, "WHITE BALANCE", activeAdjustment == Adjustment.WHITE_BALANCE, { onAdjustmentSelected(Adjustment.WHITE_BALANCE) })
            DockButton(
                if (isMuted) Icons.Filled.MicOff else Icons.Filled.Mic,
                "MIC",
                activeAdjustment == Adjustment.VOLUME,
                { viewModel.toggleMute() },
                onLongClick = { onAdjustmentSelected(Adjustment.VOLUME) }
            )
            if (isPreviewing || isStreaming) {
                PermissionHandler(onResult = { result -> if (result.canStreamVideoAndAudio || isRecording) viewModel.toggleRecording() }) { request ->
                    SmallFloatingActionButton(
                        onClick = { if (isRecording) viewModel.toggleRecording() else request() },
                        modifier = Modifier.size(58.dp),
                        containerColor = if (isRecording) Color.Red else Color.White,
                        contentColor = if (isRecording) Color.White else Color.Red
                    ) { Icon(if (isRecording) Icons.Filled.Stop else Icons.Filled.FiberManualRecord, "Record") }
                }
            }
            PermissionHandler(onResult = { result -> if (result.canStreamVideoAndAudio) { if (isStreaming) viewModel.stopStream() else viewModel.startStreamWithDefaultProfile() } }) { request ->
                FloatingActionButton(
                    onClick = { if (isStreaming) viewModel.stopStream() else request() },
                    modifier = Modifier.size(64.dp),
                    containerColor = if (isStreaming) Color.Red else Color(0xFF20C968),
                    contentColor = Color.White
                ) { Icon(if (isStreaming) Icons.Filled.Stop else Icons.Filled.PlayArrow, if (isStreaming) "Stop stream" else "Go live") }
            }
            DockButton(Icons.Filled.Settings, "SETTINGS", false, onSettingsClick)
        }
      }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun DockButton(
    icon: ImageVector,
    label: String,
    selected: Boolean,
    onClick: () -> Unit,
    onLongClick: (() -> Unit)? = null
) {
    val actionModifier = if (onLongClick != null) {
        Modifier.combinedClickable(onClick = onClick, onLongClick = onLongClick)
    } else {
        Modifier.clickable(onClick = onClick)
    }
    // Every utility control owns the same fixed geometry. Selected/pressed state
    // only changes the background; it never changes size, padding, or position.
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        modifier = actionModifier.padding(horizontal = 3.dp)
    ) {
        Box(Modifier.size(48.dp), contentAlignment = Alignment.Center) {
            if (selected) {
                Box(Modifier.fillMaxSize().background(Color.White.copy(alpha = .22f), CircleShape))
            }
            Icon(icon, label, tint = Color.White, modifier = Modifier.size(24.dp))
        }
        Text(label, color = Color.White, fontSize = 9.sp, maxLines = 1)
        Text(
            text = "⌃",
            color = if (selected) Color.White else Color.Transparent,
            fontSize = 12.sp,
            modifier = Modifier.height(12.dp)
        )
    }
}

/**
 * Vertical column of control buttons on the right edge of the screen.
 */
@Composable
private fun ControlPanel(
    streamState: StreamState,
    viewModel: StreamViewModel,
    onSettingsClick: () -> Unit,
    onMinimalModeClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    val isStreaming = streamState is StreamState.Live ||
        streamState is StreamState.Connecting ||
        streamState is StreamState.Reconnecting

    val isPreviewing = streamState is StreamState.Previewing
    val showCameraControls = isStreaming || isPreviewing

    val isMuted = (streamState as? StreamState.Live)?.isMuted == true

    val endpointProfiles by viewModel.endpointProfiles.collectAsState()
    val selectedProfileId by viewModel.selectedProfileId.collectAsState()
    val isRecording by viewModel.isRecording.collectAsState()
    val recordingDurationMs by viewModel.recordingDurationMs.collectAsState()

    Row(
        modifier = modifier,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        // Endpoint quick-switch (shown when not streaming and profiles exist)
        if (!isStreaming && endpointProfiles.isNotEmpty()) {
            EndpointSwitchButton(
                profiles = endpointProfiles,
                selectedProfileId = selectedProfileId,
                onSelect = { profileId -> viewModel.selectEndpoint(profileId) }
            )
        }

        // Recording is deliberately separate from the Go Live control.
        if (showCameraControls) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                PermissionHandler(
                    onResult = { result ->
                        if (result.canStreamVideoAndAudio || isRecording) viewModel.toggleRecording()
                    }
                ) { requestPermissions ->
                    SmallFloatingActionButton(
                        onClick = {
                            if (isRecording) viewModel.toggleRecording() else requestPermissions()
                        },
                        containerColor = when {
                            isRecording -> Color.Red
                            streamState is StreamState.Live || streamState is StreamState.Previewing -> Color.White
                            else -> Color.DarkGray.copy(alpha = 0.72f)
                        },
                        contentColor = if (isRecording) Color.White else Color.Red,
                        shape = CircleShape
                    ) {
                        Icon(
                            imageVector = if (isRecording) Icons.Filled.Stop else Icons.Filled.FiberManualRecord,
                            contentDescription = if (isRecording) "Stop recording" else "Record video"
                        )
                    }
                }
                Text(
                    text = if (isRecording) formatRecordingDuration(recordingDurationMs) else "REC",
                    color = if (isRecording) Color.Red else Color.White,
                    fontSize = 10.sp,
                    modifier = Modifier.padding(top = 2.dp)
                )
            }
        }

        // Start/Stop button — large FAB
        // When previewing: requests RECORD_AUDIO + POST_NOTIFICATIONS to go live
        // When idle: requests all permissions
        PermissionHandler(
            onResult = { result ->
                if (result.canStreamVideoAndAudio) {
                    if (isStreaming) {
                        viewModel.stopStream()
                    } else {
                        viewModel.startStreamWithDefaultProfile()
                    }
                }
            }
        ) { requestPermissions ->
            FloatingActionButton(
                onClick = {
                    if (isStreaming) {
                        viewModel.stopStream()
                    } else {
                        requestPermissions()
                    }
                },
                containerColor = if (isStreaming) {
                    MaterialTheme.colorScheme.error
                } else if (isPreviewing) {
                    Color(0xFF4CAF50) // Green for "Go Live"
                } else {
                    MaterialTheme.colorScheme.primary
                },
                modifier = Modifier.size(64.dp),
                shape = CircleShape
            ) {
                Icon(
                    imageVector = if (isStreaming) Icons.Filled.Stop else Icons.Filled.PlayArrow,
                    contentDescription = when {
                        isStreaming -> "Stop stream"
                        isPreviewing -> "Go live"
                        else -> "Start stream"
                    },
                    modifier = Modifier.size(28.dp)
                )
            }
        }

        // Minimal mode button (only shown when streaming)
        if (isStreaming) {
            ControlButton(
                icon = Icons.Filled.DarkMode,
                contentDescription = "Minimal mode",
                onClick = onMinimalModeClick
            )
        }

        // Settings button (shown when idle, stopped, or previewing)
        if (!isStreaming) {
            ControlButton(
                icon = Icons.Filled.Settings,
                contentDescription = "Settings",
                onClick = onSettingsClick
            )
        }
    }
}

private fun formatRecordingDuration(durationMs: Long): String {
    val totalSeconds = durationMs / 1000
    return String.format("%02d:%02d", totalSeconds / 60, totalSeconds % 60)
}

/**
 * Small FAB used for secondary controls (mute, switch camera, settings).
 */
@Composable
private fun ControlButton(
    icon: ImageVector,
    contentDescription: String,
    onClick: () -> Unit
) {
    SmallFloatingActionButton(
        onClick = onClick,
        containerColor = Color.Black.copy(alpha = 0.6f),
        contentColor = Color.White,
        shape = CircleShape
    ) {
        Icon(
            imageVector = icon,
            contentDescription = contentDescription
        )
    }
}

/**
 * Camera switch button with multi-camera support.
 *
 * - **Short tap:** cycles to the next camera (existing front/back behavior,
 *   extended to cycle through multiple rear cameras).
 * - **Long press:** opens a dropdown picker showing all available cameras
 *   (only on devices with >1 rear camera).
 * - **Single rear camera devices:** behaves identically to the original button.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun CameraSwitchButton(
    hasMultipleRearCameras: Boolean,
    availableCameras: List<CameraInfo>,
    onCycle: () -> Unit,
    onSelectCamera: (String) -> Unit
) {
    var showPicker by remember { mutableStateOf(false) }

    Box {
        Box(
            contentAlignment = Alignment.Center,
            modifier = Modifier
                .size(40.dp)
                .background(
                    color = Color.Black.copy(alpha = 0.6f),
                    shape = CircleShape
                )
                .clip(CircleShape)
                .combinedClickable(
                    onClick = onCycle,
                    onLongClick = if (hasMultipleRearCameras) {
                        { showPicker = true }
                    } else {
                        null
                    }
                )
        ) {
            Icon(
                imageVector = Icons.Filled.Cameraswitch,
                contentDescription = "Switch camera",
                tint = Color.White,
                modifier = Modifier.size(24.dp)
            )
        }

        // Camera picker dropdown — shown on long press for multi-camera devices
        if (showPicker) {
            DropdownMenu(
                expanded = true,
                onDismissRequest = { showPicker = false }
            ) {
                availableCameras.forEach { cam ->
                    DropdownMenuItem(
                        text = { Text(cam.label) },
                        onClick = {
                            onSelectCamera(cam.id)
                            showPicker = false
                        }
                    )
                }
            }
        }
    }
}

/**
 * Endpoint quick-switch button.
 *
 * - **Tap:** opens a dropdown showing all configured streaming endpoints.
 * - Selected profile is indicated with a checkmark.
 * - Shows the active profile name below the button.
 */
@Composable
private fun EndpointSwitchButton(
    profiles: List<EndpointProfile>,
    selectedProfileId: String?,
    onSelect: (String) -> Unit
) {
    var showPicker by remember { mutableStateOf(false) }
    val selectedName = if (profiles.isEmpty()) "" else "ALL • ${profiles.size}"

    Column(
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Box {
            Box(
                contentAlignment = Alignment.Center,
                modifier = Modifier
                    .size(40.dp)
                    .background(
                        color = Color.Black.copy(alpha = 0.6f),
                        shape = CircleShape
                    )
                    .clip(CircleShape)
                    .clickable { showPicker = true }
            ) {
                Icon(
                    imageVector = Icons.Filled.Podcasts,
                    contentDescription = "All saved outputs",
                    tint = Color.White,
                    modifier = Modifier.size(24.dp)
                )
            }

            if (showPicker) {
                DropdownMenu(
                    expanded = true,
                    onDismissRequest = { showPicker = false }
                ) {
                    profiles.forEach { profile ->
                        DropdownMenuItem(
                            text = { Text("${profile.name} • Auto") },
                            onClick = {
                                onSelect(profile.id)
                                showPicker = false
                            },
                            leadingIcon = {
                                Icon(
                                    imageVector = Icons.Filled.Check,
                                    contentDescription = "Automatically enabled",
                                    modifier = Modifier.size(18.dp)
                                )
                            }
                        )
                    }
                }
            }
        }

        // Active profile name label
        if (selectedName.isNotEmpty()) {
            Text(
                text = selectedName,
                color = Color.White.copy(alpha = 0.8f),
                fontSize = 10.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
                modifier = Modifier
                    .padding(top = 2.dp)
                    .width(64.dp)
            )
        }
    }
}

/**
 * Connection state label shown at the bottom-left of the screen.
 */
@Composable
private fun ConnectionStateLabel(
    state: StreamState,
    modifier: Modifier = Modifier
) {
    val (text, color) = when (state) {
        is StreamState.Idle -> "" to Color.Transparent
        is StreamState.Previewing -> "Preview" to Color.White
        is StreamState.Connecting -> "Connecting…" to Color.Yellow
        is StreamState.Live -> "● LIVE" to Color.Red
        is StreamState.Reconnecting -> (if (state.maxAttempts < 0) "Waiting for OBS — retry ${state.attempt + 1}"
            else "Reconnecting ${state.attempt + 1}/${state.maxAttempts}") to Color(0xFFFF8800)
        is StreamState.Stopping -> "Stopping…" to Color.Yellow
        is StreamState.Stopped -> stoppedLabel(state.reason)
    }

    if (text.isNotEmpty()) {
        Text(
            text = text,
            color = color,
            fontSize = 14.sp,
            modifier = modifier
                .background(Color.Black.copy(alpha = 0.6f), MaterialTheme.shapes.small)
                .padding(horizontal = 12.dp, vertical = 6.dp)
        )
    }
}

private fun stoppedLabel(reason: StopReason): Pair<String, Color> = when (reason) {
    StopReason.USER_REQUEST -> "Stopped" to Color.White
    StopReason.ERROR_PROFILE -> "Profile Missing" to Color.Red
    StopReason.ERROR_AUTH -> "Auth Failed" to Color.Red
    StopReason.ERROR_ENCODER -> "Encoder Error" to Color.Red
    StopReason.ERROR_CAMERA -> "Camera Error" to Color.Red
    StopReason.ERROR_AUDIO -> "Audio Error" to Color.Red
    StopReason.THERMAL_CRITICAL -> "Overheated" to Color.Red
    StopReason.BATTERY_CRITICAL -> "Low Battery" to Color.Red
}

private fun stoppedMessage(reason: StopReason, detail: String?): String {
    val base = when (reason) {
        StopReason.ERROR_PROFILE ->
            "No endpoint profile found. Configure an endpoint in Settings > Endpoints."

        StopReason.ERROR_AUTH ->
            "Server rejected authentication. Verify stream key/username/password."

        StopReason.ERROR_CAMERA ->
            "Camera error while preparing stream. Check camera permission and close other camera apps."

        StopReason.ERROR_AUDIO ->
            "Microphone/audio error while preparing stream. Check mic permission and close apps using audio input."

        StopReason.ERROR_ENCODER ->
            "Could not connect to the streaming endpoint. Verify URL, network, and ingest server status."

        StopReason.THERMAL_CRITICAL ->
            "Streaming stopped: device overheated. Let the device cool down before retrying."

        StopReason.BATTERY_CRITICAL ->
            "Streaming stopped: battery critically low. Charge device and try again."

        StopReason.USER_REQUEST -> "Stream stopped"
    }

    return if (detail.isNullOrBlank()) base else "$base\n$detail"
}
