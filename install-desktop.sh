#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_SRC="${ROOT_DIR}/target/release/explorer"
LIB_SRC="${ROOT_DIR}/target/release/explorer-webext/libexplorer_webext.so"

PREFIX="${PREFIX:-${HOME}/.local}"
BIN_DIR="${PREFIX}/bin"
BIN_DST="${BIN_DIR}/explorer"
LIB_DIR="${PREFIX}/lib/explorer"
LIB_DST="${LIB_DIR}/libexplorer_webext.so"
ICON_BASE="${PREFIX}/share/icons/hicolor"
APP_DIR="${PREFIX}/share/applications"

for arg in "$@"; do
  case "${arg}" in
    -h|--help)
      echo "Usage: ./install-desktop.sh"
      echo "Installs standalone release binary, WebProcess extension, icons, and desktop integration."
      exit 0
      ;;
    *) ;;
  esac
done

if [[ ! -x "${BIN_SRC}" ]] || [[ ! -f "${LIB_SRC}" ]]; then
  echo "[browser] Error: Release binary or WebExtension not found." >&2
  echo "Expected:" >&2
  echo "  ${BIN_SRC}" >&2
  echo "  ${LIB_SRC}" >&2
  echo "Run ./install.sh or 'cargo build --release' first." >&2
  exit 1
fi

mkdir -p "${BIN_DIR}" "${LIB_DIR}" "${APP_DIR}" "${ICON_BASE}/scalable/apps"

# Industrial-grade installation: atomic standalone binary copy (no fragile symlinks)
install -p -m 755 "${BIN_SRC}" "${BIN_DST}"
install -p -m 755 "${LIB_SRC}" "${LIB_DST}"
install -p -m 644 "${ROOT_DIR}/assets/icons/io.explorer.Browser.svg" "${ICON_BASE}/scalable/apps/io.explorer.Browser.svg"

# Generate rasterized icon sizes if rsvg-convert is installed
if command -v rsvg-convert >/dev/null 2>&1; then
  for size in 48 64 128 256 512; do
    mkdir -p "${ICON_BASE}/${size}x${size}/apps"
    rsvg-convert -w "${size}" -h "${size}" \
      "${ROOT_DIR}/assets/icons/io.explorer.Browser.svg" \
      -o "${ICON_BASE}/${size}x${size}/apps/io.explorer.Browser.png"
  done
fi

# Substitute portable Exec path into the installed .desktop file so GNOME finds it even if ~/.local/bin is not in PATH
sed "s|^Exec=.*|Exec=${BIN_DST} %u|" "${ROOT_DIR}/assets/explorer.desktop" > "${APP_DIR}/explorer.desktop"
chmod 644 "${APP_DIR}/explorer.desktop"

if command -v desktop-file-validate >/dev/null 2>&1; then
  desktop-file-validate "${APP_DIR}/explorer.desktop"
fi
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "${APP_DIR}" >/dev/null 2>&1 || true
fi
if command -v gtk4-update-icon-cache >/dev/null 2>&1; then
  gtk4-update-icon-cache -f -t "${ICON_BASE}" >/dev/null 2>&1 || true
elif command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -f -t "${ICON_BASE}" >/dev/null 2>&1 || true
fi

echo "[browser] Installed launcher, WebExtension & icons to ${PREFIX}:"
echo "  Binary      : ${BIN_DST}"
echo "  WebExtension: ${LIB_DST}"
echo "  Icon (SVG)  : ${ICON_BASE}/scalable/apps/io.explorer.Browser.svg"
echo "  Desktop     : ${APP_DIR}/explorer.desktop"
