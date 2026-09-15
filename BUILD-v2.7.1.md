# VyStream Android 2.7.1

Focused camera-control update built on the working 2.7.0 automatic
multi-output release.

## Added

- Flashlight/torch toggle in the left camera tool rail.
- Unsupported front cameras and devices without flash fail safely.
- White-balance slider from 2500K to 7500K.
- White-balance correction is applied through RootEncoder's OpenGL camera
  pipeline, so the preview, SRT/RTMP outputs and local recording match.
- Both controls are available in portrait and landscape layouts.

## Preserved

- Concurrent SRT, RTMP and RTMPS output.
- Independent destination retries.
- Local recording and two-way talkback isolation.
- 60-second control timeout and always-visible talkback indicator.

RootEncoder 2.5.5 APIs verified: `enableLantern`, `disableLantern`,
`isLanternSupported`, `isLanternEnabled` and `TemperatureFilterRender`.
