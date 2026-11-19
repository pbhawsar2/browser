#!/usr/bin/env bash
set -euo pipefail

PREFIX="${PREFIX:-${HOME}/.local}"
BIN_DST="${PREFIX}/bin/explorer"
LIB_DST="${PREFIX}/lib/explorer"
APP_DST="${PREFIX}/share/applications/explorer.desktop"
ICON_BASE="${PREFIX}/share/icons/hicolor"

PURGE_DATA=false
for arg in "$@"; do
  case "${arg}" in
    --purge|--all-data)
      PURGE_DATA=true
      ;;
    -h|--help)
      echo "Usage: ./uninstall.sh [--purge]"
      echo "Uninstalls binary, WebExtension, icons, and desktop entry."
      echo "  --purge, --all-data  Also delete user profile, WebKit cache, and application data"
      exit 0
      ;;
    *)
      echo "Unknown option: ${arg}" >&2
      exit 1
      ;;
  esac
done

# Remove binaries, library directory, and desktop integration
rm -f "${BIN_DST}"
rm -rf "${LIB_DST}"
rm -f "${APP_DST}"
rm -f "${ICON_BASE}/scalable/apps/io.explorer.Browser.svg"
for size in 48 64 128 256 512; do
  rm -f "${ICON_BASE}/${size}x${size}/apps/io.explorer.Browser.png"
done

# Clean up application data, profile, and cache if requested
if [[ "${PURGE_DATA}" == "true" ]]; then
  rm -rf "${HOME}/.cache/explorer"
  rm -rf "${HOME}/.local/share/explorer"
  rm -rf "${HOME}/.config/explorer"
  rm -rf "${PREFIX}/share/explorer"
  echo "[browser] Removed user profile, cache, and application data."
fi

if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "${PREFIX}/share/applications" >/dev/null 2>&1 || true
fi
if command -v gtk4-update-icon-cache >/dev/null 2>&1; then
  gtk4-update-icon-cache -f -t "${ICON_BASE}" >/dev/null 2>&1 || true
elif command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -f -t "${ICON_BASE}" >/dev/null 2>&1 || true
fi

echo "[browser] Uninstalled binary, WebExtension, icons, and desktop entry from ${PREFIX}."
