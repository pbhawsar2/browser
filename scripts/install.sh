#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="${HOME}/.local/bin"
LIB_DIR="${HOME}/.local/lib/explorer"
APP_DIR="${HOME}/.local/share/applications"
ICON_BASE="${HOME}/.local/share/icons/hicolor"

cd "${ROOT_DIR}"

echo "==> Running unit tests..."
cargo test

echo "==> Building release binary and WebProcess extension..."
cargo build --release

echo "==> Installing binary and WebExtension..."
install -Dm755 "target/release/explorer" "${BIN_DIR}/explorer"
install -Dm755 "target/release/explorer-webext/libexplorer_webext.so" "${LIB_DIR}/libexplorer_webext.so"

echo "==> Installing desktop entry..."
mkdir -p "${APP_DIR}"
sed "s|^Exec=.*|Exec=${BIN_DIR}/explorer %u|" "assets/explorer.desktop" > "${APP_DIR}/explorer.desktop"
if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "${APP_DIR}/explorer.desktop"
fi

echo "==> Installing application icons..."
install -Dm644 "assets/icons/io.explorer.Browser.svg" \
    "${ICON_BASE}/scalable/apps/io.explorer.Browser.svg"

if command -v rsvg-convert >/dev/null 2>&1; then
    for size in 48 64 128 256 512; do
        mkdir -p "${ICON_BASE}/${size}x${size}/apps"
        rsvg-convert -w "${size}" -h "${size}" \
            "assets/icons/io.explorer.Browser.svg" \
            -o "${ICON_BASE}/${size}x${size}/apps/io.explorer.Browser.png"
    done
else
    echo "    (rsvg-convert not found; installed scalable SVG icon only)"
fi

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "${ICON_BASE}" >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${APP_DIR}" >/dev/null 2>&1 || true
fi

echo "==> Installed Explorer successfully:"
echo "    Binary:       ${BIN_DIR}/explorer"
echo "    WebExtension: ${LIB_DIR}/libexplorer_webext.so"
echo "    Desktop file: ${APP_DIR}/explorer.desktop"
