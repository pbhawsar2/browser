#!/usr/bin/env bash
# Clean uninstaller for Explorer Browser (preserves user vault/profile unless --purge-data is passed).
set -euo pipefail

PREFIX="${PREFIX:-$HOME/.local}"
CEF_DIR="${EXPLORER_CEF_DIR:-$HOME/.local/share/explorer-cef}"
PURGE_DATA=0

for arg in "$@"; do
    [[ "$arg" == "--purge-data" ]] && PURGE_DATA=1
done

pkill -f "explorer-cef/Release/explorer" 2>/dev/null || true
sleep 0.3

rm -f "$PREFIX/bin/explorer" \
      "$CEF_DIR/Release/explorer" \
      "$CEF_DIR/Release/explorer-icon.png" \
      "$PREFIX/share/applications/explorer.desktop" \
      "$PREFIX/share/icons/hicolor/scalable/apps/io.explorer.Browser.svg" \
      "$PREFIX/share/icons/hicolor/48x48/apps/io.explorer.Browser.png" \
      "$PREFIX/share/icons/hicolor/64x64/apps/io.explorer.Browser.png" \
      "$PREFIX/share/icons/hicolor/128x128/apps/io.explorer.Browser.png" \
      "$PREFIX/share/icons/hicolor/256x256/apps/io.explorer.Browser.png"

if [[ "$PURGE_DATA" -eq 1 ]]; then
    rm -rf "${XDG_DATA_HOME:-$HOME/.local/share}/explorer" "$CEF_DIR" "${CEF_DIR}.prev"
fi

gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true
update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
echo "Explorer uninstalled cleanly."
