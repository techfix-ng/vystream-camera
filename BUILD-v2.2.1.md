# OBS SRT Camera v2.2.1

Build with Java 17 and Android SDK 35:

```bash
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
export PATH="$JAVA_HOME/bin:$PATH"
./gradlew --stop
./gradlew --no-daemon :app:assembleFossDebug
```

APK: `app/build/outputs/apk/foss/debug/app-foss-debug.apk`

## Changes

- Field-monitor preview styling with translucent status, telemetry and audio overlays.
- Recording start/save/failure confirmation; finalized staging files are retained if publishing fails.
- Recordings publish to `Movies/OBS SRT Camera` through MediaStore.
- Stable OBS pairing identity on Windows and macOS plugin protocol.
- Saved SRT endpoint IP silently refreshes from OBS discovery every 20 seconds.
- `0.0.0.0` remains only the correct OBS listener bind address and is never saved as the phone destination.
