# OBS SRT Camera v2.2.0 build

In the project directory, run:

```bash
chmod +x gradlew
./gradlew --no-daemon :app:assembleFossDebug
```

The APK will be created at:

`app/build/outputs/apk/foss/debug/app-foss-debug.apk`

## v2.2.0 fixes

- Repeated OBS discovery on normal Wi-Fi and phone-hosted hotspot networks.
- Uses the OBS response packet's real PC/Mac address; `0.0.0.0` is rejected as a phone destination.
- SRT sessions wait and reconnect indefinitely when OBS closes, crashes, or restarts.
- Foreground stream intent is redelivered if Android reclaims the service.
- Portrait and landscape selectors plus zoom and exposure/brightness sliders on preview.
- MP4 is finalized to a seekable temporary file, checked, and then saved to `Movies/OBS SRT Camera`.
