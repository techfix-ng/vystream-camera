# VYSTRM source bundle v3.0.5

This branch is the consolidated offline source snapshot for VYSTRM Camera.

## Included

- `src/` — Windows/macOS OBS plugin, authentication, Bible assistant, dock UI, pairing, and camera integration.
- `server/` — PHP authentication API and database schema.
- `streamcaster/` — Streamcaster/mobile source included in this repository.
- `cmake/`, `CMakeLists.txt`, `CMakePresets.json`, `buildspec.json` — native build configuration.
- `installer/` — Windows NSIS and macOS installer sources.
- `data/` — locale and Bible source/manifest documentation.
- `.github/actions/` and build scripts — CI/build helpers.

## Windows build

Build on a Windows 2022 runner with OBS/Qt dependencies available:

```powershell
git clone --branch source-bundle-v3.0.5 https://github.com/techfix-ng/vystream-camera.git
cd vystream-camera
./.github/Build-Windows.ps1
./.github/Package-Windows.ps1
```

The Windows GitHub Actions runner was unavailable for the previous hosted builds, so this branch is intended for offline/local or a new account's own runner.

## macOS build

```bash
git clone --branch source-bundle-v3.0.5 https://github.com/techfix-ng/vystream-camera.git
cd vystream-camera
./.github/scripts/build-macos
./.github/scripts/package-macos
```

## Bible behavior

Bible references remain staged until the operator selects Preview or Push to Program. Clear removes VYSTRM Bible Preview/Program scene items. The dock removes stale Bible scene items when it opens.

## Transfer

The branch archive can be downloaded from GitHub using:

`https://github.com/techfix-ng/vystream-camera/archive/refs/heads/source-bundle-v3.0.5.zip`

To move this bundle into another GitHub account, connect that account and provide the destination repository name; then copy this branch or upload the archive.
