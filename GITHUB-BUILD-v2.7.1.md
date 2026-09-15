# GitHub Codespaces build — VyStream Android 2.7.1

```bash
cd /workspaces/obs-cam/streamcaster

export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
export ANDROID_HOME=/workspaces/obs-cam/android-sdk
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"

printf 'sdk.dir=%s\n' "$ANDROID_HOME" > local.properties
chmod +x gradlew

./gradlew --no-daemon clean :app:assembleFossDebug

mkdir -p /workspaces/obs-cam/2.7.1
cp app/build/outputs/apk/foss/debug/app-foss-debug.apk \
  /workspaces/obs-cam/2.7.1/VyStream-v2.7.1.apk

sha256sum /workspaces/obs-cam/2.7.1/VyStream-v2.7.1.apk
```

```bash
cd /workspaces/obs-cam
git add streamcaster
git commit -m "Release VyStream Android 2.7.1 flashlight and white balance"
git push origin main
```
