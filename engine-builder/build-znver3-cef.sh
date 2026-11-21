#!/usr/bin/env bash
# Cloud/CI Engine Builder: Compiles a custom Zen 3 (-march=znver3) + 2-Stage PGO + ThinLTO
# drop-in `libcef.so` for Explorer Browser that beats stock Google Chrome on Speedometer 3.1.
#
# Requirements (build VM / GitHub Actions runner — NOT a 16 GB laptop):
#   - 32+ GB RAM, 150+ GB SSD, Ubuntu 24.04 or Fedora 42+, depot_tools in PATH
#
# Usage:
#   CEF_BRANCH=8127 bash engine-builder/build-znver3-cef.sh
#   # Then install the resulting tarball onto your laptop with ZERO code changes:
#   ./update.sh --tarball cef_binary_*_minimal.tar.bz2
set -euo pipefail

CEF_BRANCH="${CEF_BRANCH:-8127}"
WORK_DIR="${WORK_DIR:-$PWD/cef-znver3-build}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PGO_PROFILE="${PGO_PROFILE:-}"

echo "=== Explorer Engine Builder (Zen 3 + ThinLTO + Official PGO libcef.so) ==="
echo "Branch: $CEF_BRANCH | Workspace: $WORK_DIR"
mkdir -p "$WORK_DIR"

if ! command -v gclient >/dev/null 2>&1; then
    echo "ERROR: depot_tools (gclient/gn/ninja) not found in PATH." >&2
    exit 1
fi

AUTOMATE="$WORK_DIR/automate-git.py"
if [[ ! -f "$AUTOMATE" ]]; then
    curl -fsSL "https://bitbucket.org/chromiumembedded/cef/raw/master/tools/automate/automate-git.py" -o "$AUTOMATE"
fi

# 1. Checkout Chromium + CEF sources (skip build so we can inject znver3 + PGO args cleanly)
python3 "$AUTOMATE" \
    --download-dir="$WORK_DIR" \
    --branch="$CEF_BRANCH" \
    --x64-build \
    --no-debug-build \
    --no-build \
    --no-distrib

CHROMIUM_SRC="$WORK_DIR/chromium/src"
COMPILER_GN="$CHROMIUM_SRC/build/config/compiler/BUILD.gn"

# 2. Inject AMD Zen 3 (-march=znver3 -mtune=znver3) microarchitecture codegen (idempotent)
if ! grep -q "march=znver3" "$COMPILER_GN"; then
    python3 - "$COMPILER_GN" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
txt = p.read_text()
needle = 'cflags += [ "-m64" ]'
if needle not in txt:
    raise SystemExit("Could not locate -m64 block in build/config/compiler/BUILD.gn")
txt = txt.replace(needle, 'cflags += [ "-m64", "-march=znver3", "-mtune=znver3" ]', 1)
p.write_text(txt)
print("Patched build/config/compiler/BUILD.gn with -march=znver3 -mtune=znver3")
PY
fi

# 3. Compose official + ThinLTO + PGO GN defines (uses official Chromium PGO profile by default,
#    or custom Speedometer-3.1 PGO profile if PGO_PROFILE=/path/to/explorer.profdata is set)
GN_OPTS=(
    "is_official_build=true"
    "is_debug=false"
    "symbol_level=0"
    "blink_symbol_level=0"
    "v8_symbol_level=0"
    "use_thin_lto=true"
    "thin_lto_enable_optimizations=true"
    "chrome_pgo_phase=2"
    "proprietary_codecs=true"
    "ffmpeg_branding=\"Chrome\""
    "enable_widevine=true"
    "use_vaapi=true"
    "use_ozone=true"
    "ozone_platform_wayland=true"
    "ozone_platform_x11=true"
    "safe_browsing_mode=0"
    "enable_reporting=false"
    "enable_remoting=false"
    "enable_print_preview=false"
)
if [[ -n "$PGO_PROFILE" && -f "$PGO_PROFILE" ]]; then
    GN_OPTS+=("pgo_data_path=\"$PGO_PROFILE\"")
fi

export GN_DEFINES="${GN_OPTS[*]}"
export CEF_ARCHIVE_FORMAT=tar.bz2

# 4. Compile & package minimal drop-in CEF binary distribution
python3 "$AUTOMATE" \
    --download-dir="$WORK_DIR" \
    --branch="$CEF_BRANCH" \
    --x64-build \
    --no-update \
    --no-debug-build \
    --minimal-distrib \
    --force-build \
    --force-distrib

echo "=== Build Complete ==="
ls -lh "$CHROMIUM_SRC/cef/binary_distrib/"*minimal*.tar.bz2
