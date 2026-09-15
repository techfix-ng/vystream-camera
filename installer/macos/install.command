#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_SOURCE="$SCRIPT_DIR/obs-srt-camera.plugin"
PLUGIN_ROOT="$HOME/Library/Application Support/obs-studio/plugins"
PLUGIN_TARGET="$PLUGIN_ROOT/obs-srt-camera.plugin"

if [[ ! -d "$PLUGIN_SOURCE" ]]; then
  echo "The obs-srt-camera.plugin bundle is missing from this installer."
  read -r -p "Press Return to close..."
  exit 1
fi

echo "Closing OBS Studio..."
osascript -e 'tell application "OBS" to quit' 2>/dev/null || true
pkill -x obs 2>/dev/null || true
pkill -x OBS 2>/dev/null || true
sleep 1

echo "Removing previous VyStream and OBS-SRT Camera plugins..."
for old in \
  "$HOME/Library/Application Support/obs-studio/plugins/obs-srt-camera.plugin" \
  "$HOME/Library/Application Support/obs-studio/plugins/obs-srt-camera" \
  "$HOME/Library/Application Support/obs-studio/plugins/vystrm-camera.plugin" \
  "$HOME/Library/Application Support/obs-studio/plugins/vystream-camera.plugin"; do
  [[ -e "$old" ]] && /bin/rm -rf "$old"
done

for old in \
  "/Library/Application Support/obs-studio/plugins/obs-srt-camera.plugin" \
  "/Library/Application Support/obs-studio/plugins/obs-srt-camera" \
  "/Library/Application Support/obs-studio/plugins/vystrm-camera.plugin"; do
  [[ -e "$old" ]] && sudo /bin/rm -rf "$old"
done

echo "Installing VyStream OBS Dock 2.9.0..."
mkdir -p "$PLUGIN_ROOT"
/usr/bin/ditto "$PLUGIN_SOURCE" "$PLUGIN_TARGET"
/usr/bin/xattr -dr com.apple.quarantine "$PLUGIN_TARGET" 2>/dev/null || true
/bin/chmod -R u+rwX,go+rX "$PLUGIN_TARGET"

echo "Installation complete. The dock is available under OBS > Docks > VyStream Camera."

if [[ -d "/Applications/OBS.app" ]]; then
  open -a "/Applications/OBS.app"
elif [[ -d "/Applications/obs-studio/OBS.app" ]]; then
  open -a "/Applications/obs-studio/OBS.app"
elif [[ -x "/Applications/obs-studio/obs" ]]; then
  open "/Applications/obs-studio/obs"
fi

read -r -p "Press Return to close..."
