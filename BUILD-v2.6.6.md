# VyStream Android 2.6.6 — camera health telemetry

- Reports connection state, network type, Wi-Fi RSSI, measured video bitrate,
  FPS, dropped frames, battery, thermal state, resolution, codec, recording,
  and program-microphone mute state to paired VyStream OBS docks.
- Reporting starts after pairing and does not require streaming or recording.
- Health packets contain no audio, video, account credentials, or user content.
- Compatible with VyStream OBS Dock 2.8.0; older docks safely ignore telemetry.

```bash
./gradlew --no-daemon :app:assembleFossDebug
```
