#!/usr/bin/env bash
# ==============================================================================
# Explorer Browser — Industrial-Grade Turnkey Installer (with 2-Stage Clang PGO)
# ==============================================================================
#
# OVERVIEW:
#   Builds, tests, PGO-optimizes, and installs Explorer Browser:
#     1. Brave `adblock-rust` (`shield-brave`) static library via Cargo (`--release`)
#     2. Stripped CEF 158 (Chromium/Blink/V8/Skia) Linux x86_64 minimal runtime
#     3. 2-Stage Clang Profile-Guided Optimization (PGO) + ThinLTO C++20 build:
#          - Stage 1 (when training or if pgo/explorer.profdata is missing):
#            Compiles with `-fprofile-generate`, runs `pgo/train_pgo.py` across
#            all 6 unit test suites + live heavy websites (robu.in, amazon.in,
#            youtube.com) with scrolling, zooming, HUD, Vault, and video decode,
#            and merges `.profraw` counters into `pgo/explorer.profdata` via
#            `llvm-profdata`.
#          - Stage 2 (Production Binary):
#            Compiles with `-O3 -march=native -flto=thin -fprofile-use=pgo/explorer.profdata`
#            and runs all 6 SOLID CTest verification suites.
#     4. Installs standalone binary (`install -p -m 755`, zero fragile symlinks),
#        official SVG + 48/64/128/256px PNG icons, and validated `explorer.desktop`.
#
# SYSTEM PREREQUISITES (for fresh OS installation):
#   - Fedora / RHEL:
#       sudo dnf install -y clang llvm cmake ninja-build rust cargo pkgconf-pkg-config \
#            systemd-devel openssl-devel ImageMagick desktop-file-utils curl tar bzip2 python3
#   - Ubuntu / Debian:
#       sudo apt install -y clang llvm cmake ninja-build rustc cargo pkg-config \
#            libsystemd-dev libssl-dev imagemagick desktop-file-utils curl tar bzip2 python3
#   - Arch Linux:
#       sudo pacman -S --needed clang llvm cmake ninja rust pkgconf systemd openssl \
#            imagemagick desktop-file-utils curl tar bzip2 python
#
# USAGE:
#   ./install.sh               # Install using existing or auto-trained PGO profile
#   ./install.sh --train-pgo   # Force fresh Stage-1 PGO training run + Stage-2 PGO build
#   ./install.sh --no-pgo      # Standard Release build (-O3 -march=native without PGO)
#
# ENVIRONMENT OVERRIDES:
#   PREFIX="$HOME/.local"                       # Installation prefix
#   EXPLORER_CEF_DIR="$HOME/.local/share/explorer-cef" # CEF runtime directory
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
CEF_DIR="${EXPLORER_CEF_DIR:-$HOME/.local/share/explorer-cef}"
BUILD_DIR="${SCRIPT_DIR}/build"
PGO_DIR="${SCRIPT_DIR}/pgo"
PGO_PROFDATA="${PGO_DIR}/explorer.profdata"
PGO_RAW_DIR="/tmp/explorer_pgo_raw_$$"

TRAIN_PGO=0
USE_PGO=1
for arg in "$@"; do
    case "$arg" in
        --train-pgo) TRAIN_PGO=1 ;;
        --no-pgo) USE_PGO=0 ;;
    esac
done

# 1. Stop any running Explorer instances and clear stale temporary clutter
pkill -x "explorer" 2>/dev/null || true
pkill -x "explorer-v2" 2>/dev/null || true
sleep 0.4
rm -rf /tmp/explorer_v2_* /tmp/explorer_test_* /tmp/explorer_e2e_* /tmp/explorer_pgo_raw_* 2>/dev/null || true
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

# 3. Profile-Guided Optimization (PGO) Stage 1 training (if requested or profile missing)
if [[ "$USE_PGO" -eq 1 ]] && command -v llvm-profdata >/dev/null 2>&1; then
    if [[ "$TRAIN_PGO" -eq 1 || ! -f "$PGO_PROFDATA" ]]; then
        echo "=== Explorer PGO Stage 1: Building Instrumented Binary (-fprofile-generate) ==="
        mkdir -p "$PGO_DIR" "$PGO_RAW_DIR"
        cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja \
            -DCMAKE_C_COMPILER=clang \
            -DCMAKE_CXX_COMPILER=clang++ \
            -DCMAKE_BUILD_TYPE=Release \
            -DCEF_ROOT="$CEF_DIR" \
            -DEXPLORER_PGO_GENERATE=ON \
            -DEXPLORER_PGO_DIR="$PGO_RAW_DIR"

        cmake --build "$BUILD_DIR" -j"$(nproc)"
        # Stage instrumented binary next to libcef.so so CEF resources resolve during live training
        install -p -m 755 "$BUILD_DIR/explorer" "$CEF_DIR/Release/explorer"

        echo "=== Explorer PGO Stage 1b: Running Heavy Workload Trainer (robu.in / amazon.in / youtube.com / HUD / Zoom / Video) ==="
        LLVM_PROFILE_FILE="${PGO_RAW_DIR}/explorer-%p-%m.profraw" \
            python3 "$SCRIPT_DIR/pgo/train_pgo.py" \
                --binary "$CEF_DIR/Release/explorer" \
                --build-dir "$BUILD_DIR" \
                --pgo-dir "$PGO_RAW_DIR" \
                --label "PGO-Train"

        echo "=== Explorer PGO Stage 1c: Merging Raw Profiles -> ${PGO_PROFDATA} ==="
        llvm-profdata merge -output="$PGO_PROFDATA" "$PGO_RAW_DIR"/*.profraw
        rm -rf "$PGO_RAW_DIR"
        echo "Merged PGO profile ready: $(ls -lh "$PGO_PROFDATA" | awk '{print $5}')"
    fi
fi

# 4. Configure, compile final Release binary (with PGO + ThinLTO if available), and run CTest
CMAKE_PGO_ARGS=()
if [[ "$USE_PGO" -eq 1 && -f "$PGO_PROFDATA" ]]; then
    echo "=== Explorer PGO Stage 2: Building PGO + ThinLTO Optimized Release Binary ==="
    CMAKE_PGO_ARGS=(-DEXPLORER_PGO_USE="$PGO_PROFDATA" -DEXPLORER_PGO_GENERATE=OFF)
else
    echo "=== Building Standard Optimized Release Binary (-O3 -march=native) ==="
    CMAKE_PGO_ARGS=(-DEXPLORER_PGO_USE="" -DEXPLORER_PGO_GENERATE=OFF)
fi

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release \
    -DCEF_ROOT="$CEF_DIR" \
    "${CMAKE_PGO_ARGS[@]}"

cmake --build "$BUILD_DIR" -j"$(nproc)"
ctest --test-dir "$BUILD_DIR" --output-on-failure

# 5. Install standalone binary alongside libcef.so + clean launcher (zero symlinks)
mkdir -p "$PREFIX/bin" "$PREFIX/share/applications" "$PREFIX/share/icons/hicolor/scalable/apps"
install -p -m 755 "$BUILD_DIR/explorer" "$CEF_DIR/Release/explorer"

cat > "$PREFIX/bin/explorer" <<EOF
#!/usr/bin/env bash
exec "${CEF_DIR}/Release/explorer" "\$@"
EOF
chmod 755 "$PREFIX/bin/explorer"

# 6. Install official Explorer SVG + rasterized PNG icons (48/64/128/256px)
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

# 7. Install validated FreeDesktop launcher matching Wayland app_id=explorer
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

echo "Explorer cleanly installed -> ${PREFIX}/bin/explorer (Blink+V8 + Clang PGO + Brave Shield + AES-256-GCM Vault + WebApps)"
