# VyStream 2.6.3

- Preview controls remain visible for 60 seconds after the last interaction.
- Portrait preview uses a dedicated three-column Zoom, Brightness, and Volume tray with single-line labels, end buttons, dividers, and full-width sliders.
- Starting a livestream reuses recording-prepared encoders, preventing the active MP4 muxer from being stopped.
- Preview now uses the selected endpoint protocol. If the user changes protocol while recording, VyStream safely finalizes the first segment and automatically begins a continuation after the stream connects.
- Director talkback no longer inserts silence into programme audio. It is routed through Android's communication/earpiece path to reduce acoustic bleed.
- Camera-operator reply remains isolated because reply and programme audio share the same physical microphone.

Build with JDK 17:

```bash
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
export ANDROID_HOME="$HOME/android-sdk"
printf 'sdk.dir=%s\n' "$ANDROID_HOME" > local.properties
./gradlew --no-daemon :app:assembleFossDebug
```

APK: `app/build/outputs/apk/foss/debug/app-foss-debug.apk`
