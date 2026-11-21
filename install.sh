#!/usr/bin/env bash
# Turnkey Explorer Browser clean build (Brave adblock-rust + C++20 SOLID modules),
# CTest verification, legacy clutter cleanup, official Explorer icons, and standalone install.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
CEF_DIR="${EXPLORER_CEF_DIR:-$HOME/.local/share/explorer-cef}"
BUILD_DIR="${SCRIPT_DIR}/build"

# 1. Stop any running Explorer / legacy explorer-v2 instances and clear stale clutter
pkill -f "explorer-cef/Release/explorer" 2>/dev/null || true
pkill -f "explorer-cef/Release/explorer-v2" 2>/dev/null || true
sleep 0.4
rm -rf /tmp/explorer_v2_* /tmp/explorer_test_* /tmp/explorer_e2e_* 2>/dev/null || true
rm -f "$PREFIX/bin/explorer-v2" \
      "$CEF_DIR/Release/explorer-v2" \
      "$PREFIX/share/applications/explorer-v2.desktop" \
      "$PREFIX/share/applications/io.explorer.BrowserV2.desktop" \
      "$PREFIX/share/icons/hicolor/scalable/apps/io.explorer.BrowserV2.svg" 2>/dev/null || true
rm -rf "$PREFIX/lib/explorer" 2>/dev/null || true

if [[ ! -f "$CEF_DIR/Release/libcef.so" ]]; then
    bash "$SCRIPT_DIR/install-cef.sh"
fi

# 2. Build Brave adblock-rust static library (shield-brave)
echo "Building Brave adblock-rust engine..."
cargo build --release --manifest-path "$SCRIPT_DIR/shield-brave/Cargo.toml"

# 3. Configure, compile (Clang -O3 -march=native), and run all SOLID unit test suites
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release \
    -DCEF_ROOT="$CEF_DIR"

cmake --build "$BUILD_DIR" -j"$(nproc)"
ctest --test-dir "$BUILD_DIR" --output-on-failure

# 4. Install standalone binary alongside libcef.so + clean launcher (zero symlinks)
mkdir -p "$PREFIX/bin" "$PREFIX/share/applications" "$PREFIX/share/icons/hicolor/scalable/apps"
install -p -m 755 "$BUILD_DIR/explorer" "$CEF_DIR/Release/explorer"

cat > "$PREFIX/bin/explorer" <<EOF
#!/usr/bin/env bash
exec "${CEF_DIR}/Release/explorer" "\$@"
EOF
chmod 755 "$PREFIX/bin/explorer"

# 5. Install official Explorer SVG + rasterized PNG icons (48/64/128/256px)
ICON_SRC="${SCRIPT_DIR}/assets/icons/io.explorer.Browser.svg"
if [[ -f "$ICON_SRC" ]]; then
    install -p -m 644 "$ICON_SRC" "$PREFIX/share/icons/hicolor/scalable/apps/io.explorer.Browser.svg"
    if command -v magick >/dev/null 2>&1; then
        for sz in 48 64 128 256; do
            mkdir -p "$PREFIX/share/icons/hicolor/${sz}x${sz}/apps"
            magick -background none -density 384 "$ICON_SRC" -resize "${sz}x${sz}" \
                "$PREFIX/share/icons/hicolor/${sz}x${sz}/apps/io.explorer.Browser.png"
        done
        install -p -m 644 "$PREFIX/share/icons/hicolor/256x256/apps/io.explorer.Browser.png" \
            "$CEF_DIR/Release/explorer-icon.png"
    fi
fi

# 6. Install validated FreeDesktop launcher matching Wayland app_id=explorer
DESKTOP_FILE="$PREFIX/share/applications/explorer.desktop"
cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=Explorer
GenericName=Web Browser
Comment=Hardware-tuned Blink + V8 browser with Brave Shield for AMD Ryzen / Wayland
Exec=${PREFIX}/bin/explorer %U
Icon=io.explorer.Browser
Terminal=false
Categories=Network;WebBrowser;
MimeType=text/html;x-scheme-handler/http;x-scheme-handler/https;
StartupNotify=true
StartupWMClass=explorer
EOF

if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$DESKTOP_FILE"
fi
gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true
update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true

echo "Explorer cleanly installed -> ${PREFIX}/bin/explorer (Blink+V8 + Brave Shield + AES-256-GCM Vault + WebApps)"
