# VyStream 2.6.1 — isolated two-way talkback

## Audio routing fixes

- Programme microphone mute now works during preview, local recording, and livestreaming.
- Muting the programme track does not disable the private talkback microphone path.
- Camera-to-director reply starts from a standby `VOICE_COMMUNICATION` capture, so it works before recording or livestreaming starts.
- Director downlink audio and camera-operator reply audio are replaced with silence on the encoded programme track.
- OBS discovery refreshes every second and applies a discovered return address immediately.

The three audio roles are intentionally independent:

1. **Programme mic** — sent to the livestream and MP4 unless programme mute/intercom isolation is active.
2. **Director downlink** — played only to the camera operator.
3. **Camera reply** — sent only to the director return port.

## Build

```bash
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
export ANDROID_HOME="$HOME/android-sdk"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"
printf 'sdk.dir=%s\n' "$ANDROID_HOME" > local.properties
./gradlew --no-daemon :app:assembleFossDebug
```

APK output:

`app/build/outputs/apk/foss/debug/app-foss-debug.apk`

