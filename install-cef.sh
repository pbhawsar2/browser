#!/usr/bin/env bash
# Installs the stripped CEF 158 (Chromium/Blink/V8) Linux x86_64 minimal runtime
# cleanly into ~/.local/share/explorer-cef (or $EXPLORER_CEF_DIR).
set -euo pipefail

DEST="${EXPLORER_CEF_DIR:-$HOME/.local/share/explorer-cef}"
CEF_VER="158.0.3+g8c29be2+chromium-158.0.8127.18"
TARBALL_URL="https://cef-builds.spotifycdn.com/cef_binary_158.0.3%2Bg8c29be2%2Bchromium-158.0.8127.18_linux64_minimal.tar.bz2"

if [[ -f "$DEST/Release/libcef.so" && -f "$DEST/cmake/FindCEF.cmake" ]]; then
    echo "CEF runtime already installed at $DEST"
    exit 0
fi

mkdir -p "$DEST"

if [[ -d "/tmp/explorer-v2-poc/cef/Release" && -f "/tmp/explorer-v2-poc/cef/cmake/FindCEF.cmake" ]]; then
    echo "Reusing verified stripped CEF 158 runtime from /tmp/explorer-v2-poc/cef -> $DEST"
    cp -a /tmp/explorer-v2-poc/cef/cmake \
          /tmp/explorer-v2-poc/cef/include \
          /tmp/explorer-v2-poc/cef/libcef_dll \
          /tmp/explorer-v2-poc/cef/Release \
          /tmp/explorer-v2-poc/cef/Resources \
          "$DEST/"
    rm -f "$DEST/Release/explorer_cef_poc"
else
    TMPDIR="$(mktemp -d /tmp/explorer-cef-dl.XXXXXX)"
    trap 'rm -rf "$TMPDIR"' EXIT
    echo "Downloading CEF $CEF_VER minimal..."
    curl -fL "$TARBALL_URL" -o "$TMPDIR/cef.tar.bz2"
    tar -xjf "$TMPDIR/cef.tar.bz2" -C "$TMPDIR"
    SRC="$(find "$TMPDIR" -maxdepth 1 -type d -name 'cef_binary_*' | head -n 1)"
    strip --strip-unneeded "$SRC/Release/libcef.so"
    cp -a "$SRC/Resources/"* "$SRC/Release/"
    cp -a "$SRC/cmake" "$SRC/include" "$SRC/libcef_dll" "$SRC/Release" "$SRC/Resources" "$DEST/"
fi

# Ensure resources (icudtl.dat, *.pak, locales/) sit next to libcef.so for reliable lookup
cp -an "$DEST/Resources/"* "$DEST/Release/" 2>/dev/null || true

echo "CEF 158 runtime ready at $DEST ($(du -sh "$DEST" | cut -f1))"
