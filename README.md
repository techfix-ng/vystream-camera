# VyStream OBS Dock 2.9.0 — GitHub Actions build

This repository builds the native VyStream OBS Dock for Windows x64 and macOS Universal using the official OBS plugin-template automation. GitHub downloads the pinned OBS and Qt build dependencies; no local Visual Studio, Xcode, CMake, Qt or OBS SDK installation is required.

The pinned OBS 31.1.1 SDK is intended to remain ABI-compatible with OBS 32.x. Test both generated packages on OBS 32.2.2 before public release.

## Upload from Codespaces

```bash
cd /workspaces/obs-cam
unzip -o VyStream-OBS-Dock-v2.9.0-GitHub-Actions-source.zip
git add .
git commit -m "Release VyStream OBS Dock 2.9.0 native builds"
git push
```

## Build and download both platforms

```bash
cd /workspaces/obs-cam
git commit --allow-empty -m "Trigger VyStream 2.9.0 native installer builds"
git push
RUN_ID="$(gh run list --workflow push.yaml --branch main --limit 1 --json databaseId --jq '.[0].databaseId')"
gh run watch "$RUN_ID" --exit-status
mkdir -p /workspaces/obs-cam/2.9.0-builds
gh run download "$RUN_ID" --dir /workspaces/obs-cam/2.9.0-builds
find /workspaces/obs-cam/2.9.0-builds -maxdepth 3 -type f -ls
```

Or use the included launcher after committing and pushing:

```bash
chmod +x build-github.sh
./build-github.sh
```

## User-facing installers

The workflow publishes these additional artifacts:

- `VyStream-OBS-Dock-v2.9.0-Windows-Setup`: contains `VyStream-OBS-Dock-v2.9.0-Setup.exe`.
- `VyStream-OBS-Dock-v2.9.0-macOS-Installer`: contains `install.command` and the compiled Universal plugin bundle.

The Windows EXE requests administrator access, closes OBS, removes known legacy VyStream/OBS-SRT plugin locations, installs the current dock for the signed-in user, replaces its firewall rules for UDP `45990`, `46010`, and `46011`, writes an uninstaller, and restarts OBS.

The macOS installer closes OBS, removes old per-user and system-wide plugin copies, installs the new bundle under the signed-in user's OBS plugin directory, clears quarantine from that bundle, and reopens a supported OBS application location.

After installation the UI is available at **OBS → Docks → VyStream Camera**.

Unsigned artifacts are suitable for testing. Public macOS distribution requires Apple Developer signing/notarization; Windows reputation requires Authenticode signing.
