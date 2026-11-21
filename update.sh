#!/usr/bin/env bash
# Explorer Zero-Issue Regular Engine Updater (with automatic rollback protection).
# Usage:
#   ./update.sh                              # Check & install latest stable CEF/Chromium
#   ./update.sh --tarball /path/to/cef.tbz2  # Install custom znver3 PGO+ThinLTO+BOLT libcef
#   ./update.sh --rollback                   # Instant rollback to previous CEF runtime
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CEF_DIR="${EXPLORER_CEF_DIR:-$HOME/.local/share/explorer-cef}"
BACKUP_DIR="${CEF_DIR}.prev"
TARBALL=""
ROLLBACK=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rollback) ROLLBACK=1; shift ;;
        --tarball=*) TARBALL="${1#*=}"; shift ;;
        --tarball) TARBALL="${2:-}"; shift 2 ;;
        *) shift ;;
    esac
done

if [[ "$ROLLBACK" -eq 1 ]]; then
    if [[ ! -d "$BACKUP_DIR" ]]; then
        echo "No previous CEF backup found at $BACKUP_DIR" >&2
        exit 1
    fi
    pkill -x "explorer" 2>/dev/null || true
    sleep 0.3
    rm -rf "$CEF_DIR"
    mv "$BACKUP_DIR" "$CEF_DIR"
    bash "$SCRIPT_DIR/install.sh"
    echo "Rolled back cleanly to previous CEF runtime."
    exit 0
fi

STAGE="$(mktemp -d /tmp/explorer-cef-update.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

if [[ -n "$TARBALL" ]]; then
    echo "Staging custom CEF tarball: $TARBALL"
    tar -xf "$TARBALL" -C "$STAGE"
    NEW_VER="custom-$(basename "$TARBALL")"
else
    echo "Checking latest stable Chromium/CEF Linux x64 release..."
    read -r NEW_VER DL_NAME < <(python3 -c '
import json, urllib.request
data = json.loads(urllib.request.urlopen("https://cef-builds.spotifycdn.com/index.json", timeout=15).read())
for v in data["linux64"]["versions"]:
    if v.get("channel") == "stable":
        for f in v["files"]:
            if f.get("type") == "minimal":
                print(v["cef_version"], f["name"])
                raise SystemExit(0)
raise SystemExit(1)
')
    CUR_VER="$(cat "$CEF_DIR/CEF_VERSION" 2>/dev/null || echo "none")"
    echo "Installed: $CUR_VER"
    echo "Latest:    $NEW_VER"
    if [[ "$CUR_VER" == "$NEW_VER" ]]; then
        echo "Already up to date!"
        exit 0
    fi
    ENC_NAME="$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$DL_NAME")"
    echo "Downloading $DL_NAME..."
    curl -fL "https://cef-builds.spotifycdn.com/${ENC_NAME}" -o "$STAGE/cef.tar.bz2"
    tar -xjf "$STAGE/cef.tar.bz2" -C "$STAGE"
fi

SRC="$(find "$STAGE" -maxdepth 1 -type d -name 'cef_binary_*' | head -n 1)"
if [[ -z "$SRC" || ! -f "$SRC/Release/libcef.so" ]]; then
    echo "Invalid CEF archive (libcef.so missing)" >&2
    exit 1
fi

strip --strip-unneeded "$SRC/Release/libcef.so" 2>/dev/null || true
cp -a "$SRC/Resources/"* "$SRC/Release/"
mkdir -p "$STAGE/final"
cp -a "$SRC/cmake" "$SRC/include" "$SRC/libcef_dll" "$SRC/Release" "$SRC/Resources" "$STAGE/final/"
echo "$NEW_VER" > "$STAGE/final/CEF_VERSION"

# Atomic swap preserving user profile, encrypted vault, cookies, and installed WebApps
pkill -x "explorer" 2>/dev/null || true
sleep 0.4
rm -rf "$BACKUP_DIR"
if [[ -d "$CEF_DIR" ]]; then
    mv "$CEF_DIR" "$BACKUP_DIR"
fi
mv "$STAGE/final" "$CEF_DIR"

echo "Rebuilding & testing Explorer against $NEW_VER..."
if ! bash "$SCRIPT_DIR/install.sh"; then
    echo "Build/test failed — automatically rolling back to previous CEF runtime!" >&2
    rm -rf "$CEF_DIR"
    [[ -d "$BACKUP_DIR" ]] && mv "$BACKUP_DIR" "$CEF_DIR"
    exit 1
fi

echo "Updated Explorer cleanly to CEF $NEW_VER (rollback available via ./update.sh --rollback)."
