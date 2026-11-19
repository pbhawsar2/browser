#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_SRC="${ROOT_DIR}/target/release/explorer"
LIB_SRC="${ROOT_DIR}/target/release/explorer-webext/libexplorer_webext.so"

RUN_TESTS=false
FORCE_REBUILD=false
SET_MIME=true

for arg in "$@"; do
  case "${arg}" in
    --test) RUN_TESTS=true ;;
    --rebuild) FORCE_REBUILD=true ;;
    --no-mime) SET_MIME=false ;;
    -h|--help)
      echo "Usage: ./install.sh [OPTIONS]"
      echo ""
      echo "Industrial-grade standalone installer for Explorer Browser (GTK 4 / Libadwaita / WebKitGTK 6.0)."
      echo "Defaults to complete, turnkey installation without requiring any arguments."
      echo ""
      echo "Options:"
      echo "  --test      Run cargo test before compiling release binaries"
      echo "  --rebuild   Force re-compilation even if release binary already exists"
      echo "  --no-mime   Skip registering Explorer as default web browser"
      echo "  -h, --help  Show this help message and exit"
      exit 0
      ;;
    *)
      echo "Unknown option: ${arg}" >&2
      echo "Run './install.sh --help' for usage." >&2
      exit 1
      ;;
  esac
done

cd "${ROOT_DIR}"

# Step 1: Optional test run
if [[ "${RUN_TESTS}" == "true" ]]; then
  echo "[browser] Running test suite..."
  cargo test
fi

# Step 2: Ensure optimized release binaries are built
if [[ ! -x "${BIN_SRC}" ]] || [[ ! -f "${LIB_SRC}" ]] || [[ "${FORCE_REBUILD}" == "true" ]]; then
  echo "[browser] Building optimized release binary and WebExtension..."
  cargo build --release
fi

# Step 3: Install standalone binary, WebExtension, icons, and desktop integration
"${ROOT_DIR}/install-desktop.sh"

# Step 4: Register as default web browser handler by default
if [[ "${SET_MIME}" == "true" ]] && command -v xdg-mime >/dev/null 2>&1; then
  echo "[browser] Registering Explorer as default web browser..."
  MIME_TYPES=(
    text/html
    text/xml
    application/xhtml+xml
    x-scheme-handler/http
    x-scheme-handler/https
  )
  for mime in "${MIME_TYPES[@]}"; do
    xdg-mime default explorer.desktop "${mime}" 2>/dev/null || true
  done
fi

echo "[browser] Done! Launch 'Explorer' from GNOME or run 'explorer'."
