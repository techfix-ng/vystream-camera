# VyStream Android 2.6.5 — portrait talkback refinement

- Keeps the application name as **VyStream**.
- Moves the portrait Talkback Ready control to the upper-right.
- Uses a smaller portrait talkback control without changing the landscape layout.
- Includes the crew intercom and talkback isolation introduced in 2.6.4.
- Compatible with VyStream OBS Dock 2.7.1 on Windows and macOS.

## Build

```bash
./gradlew --no-daemon :app:assembleFossDebug
```

The APK is generated at:

```text
app/build/outputs/apk/foss/debug/app-foss-debug.apk
```
