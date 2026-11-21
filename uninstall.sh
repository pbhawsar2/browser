#!/usr/bin/env bash
# ==============================================================================
# Explorer Browser — Complete Uninstaller & Data Purge Utility
# ==============================================================================
#
# Removes:
#   - Running Explorer browser and WebApp instances
#   - Installed browser binaries ($PREFIX/bin/explorer, $CEF_DIR/Release/explorer)
#   - Main desktop entry ($PREFIX/share/applications/explorer.desktop)
#   - All installed WebApp desktop entries (explorer-app-*.desktop, io.explorer.WebApp.*.desktop)
#   - All browser and WebApp icons across hicolor sizes (scalable, 48x48, 64x64, 128x128, 256x256)
#   - User profile, session, preferences, filters, WebApp profiles, and encrypted vault data
#     (${XDG_DATA_HOME:-$HOME/.local/share}/explorer) unless --keep-data is specified
#   - Optional: pass --purge-cef to also remove the cached CEF 158 SDK runtime ($CEF_DIR)
# ==============================================================================
set -euo pipefail

PREFIX="${PREFIX:-$HOME/.local}"
CEF_DIR="${EXPLORER_CEF_DIR:-$HOME/.local/share/explorer-cef}"
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/explorer"
PURGE_DATA=1
PURGE_CEF=0

for arg in "$@"; do
    case "$arg" in
        --keep-data) PURGE_DATA=0 ;;
        --purge-data) PURGE_DATA=1 ;;
        --purge-cef|--purge-all) PURGE_CEF=1; PURGE_DATA=1 ;;
    esac
done

# 1. Stop any running Explorer processes
pkill -x "explorer" 2>/dev/null || true
pkill -x "explorer-v2" 2>/dev/null || true
sleep 0.4

# 2. Remove binaries, desktop launchers, and installed WebApp launchers/icons
rm -f "$PREFIX/bin/explorer" \
      "$PREFIX/bin/explorer-v2" \
      "$CEF_DIR/Release/explorer" \
      "$CEF_DIR/Release/explorer-v2" \
      "$CEF_DIR/Release/explorer-icon.png" \
      "$PREFIX/share/applications/explorer.desktop" \
      "$PREFIX/share/applications/explorer-v2.desktop" \
      "$PREFIX/share/applications/io.explorer.BrowserV2.desktop" \
      "$PREFIX/share/applications/"explorer-app-*.desktop \
      "$PREFIX/share/applications/"io.explorer.WebApp.*.desktop \
      "$PREFIX/share/icons/hicolor/scalable/apps/io.explorer.Browser.svg" \
      "$PREFIX/share/icons/hicolor/scalable/apps/io.explorer.BrowserV2.svg"

for sz in 48x48 64x64 128x128 256x256; do
    rm -f "$PREFIX/share/icons/hicolor/${sz}/apps/io.explorer.Browser.png" \
          "$PREFIX/share/icons/hicolor/${sz}/apps/"explorer-app-*.png \
          "$PREFIX/share/icons/hicolor/${sz}/apps/"io.explorer.WebApp.*.png
done

# 3. Purge user profile, session, preferences, filters, WebApp subprofiles, and temporary files
if [[ "$PURGE_DATA" -eq 1 ]]; then
    rm -rf "$DATA_DIR" \
           "${DATA_DIR}-v2" \
           /tmp/explorer_v2_* \
           /tmp/explorer_test_* \
           /tmp/explorer_e2e_* \
           /tmp/explorer_pgo_* 2>/dev/null || true
fi

if [[ "$PURGE_CEF" -eq 1 ]]; then
    rm -rf "$CEF_DIR" "${CEF_DIR}.prev"
fi

gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true
update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
echo "Explorer uninstalled and user data purged cleanly."
