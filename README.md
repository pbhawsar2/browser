# Explorer Browser

Fast, minimalist Linux Wayland browser built in Rust with **GTK4**, **Libadwaita**, **WebKitGTK 6.0**, and **mimalloc** (`target-cpu=native`, ThinLTO). Designed for clean separation between upstream browser core logic and Fedora/AMD/Optimus hardware optimizations.

## Key Features

- **Clean Core / Tuning Split:** Pure GTK4/WebKitGTK UI in `src/core/` isolated from hardware/engine optimizations in `src/tuning/` for seamless future Fedora upgrades (`EXPLORER_DISABLE_TUNING=1` runs pure upstream behavior).
- **Unified `36x34px` Top Bezel:** Crisp dark-mode UI (`#080C14` / `#131C2C`) with Cairo window controls, `1.05x` default 1080p DPI zoom, **Google** default search, and an auto-hiding `38px` horizontal tab bar (hidden when 1 tab is open so pages start at `x = 0`).
- **Pre-warmed New Tab (`Ctrl+T`):** Lazily keeps one spare Start Page `WebView` ready for instant tab creation.
- **16-Thread `AC Turbo` Scaling:** Scales Skia & JavaScriptCore threads to CPU parallelism on AC power (`+29.1%` Speedometer 3.1 vs stock WebKitGTK).
- **AMD Cezanne VA-API & Optimus Isolation:** Probes `/dev/dri/renderD*` via `O_RDWR` `libva` (`VP9 · H264 · HEVC`), prefers `vavp9dec`/`vah264dec`, and keeps the discrete NVIDIA GPU suspended for `0 RPM` quiet browsing.
- **Brave-Style Native DFA Adblocker:** Curated third-party tracker/ad rules (`explorer-adblock-v3`) compiled once to disk via `UserContentFilterStore`.
- **Smooth Scroll Scaler:** `3.0x` discrete wheel / `1.85x` smooth trackpad scaling on WebKit's `EventControllerScroll` (keeping `Ctrl+Wheel` zoom `1.0x`).
- **WebApp (PWA) Installer:** Install any website as a standalone desktop app (`--app=<url>`) with generated squircle icons.

## Quick Start & Installation

### Build Prerequisites (Fedora)

```bash
sudo dnf install -y --setopt=install_weak_deps=False \
  rust cargo gcc pkgconf-pkg-config \
  gtk4-devel libadwaita-devel webkitgtk6.0-devel \
  libva-devel libdrm-devel gstreamer1-devel gstreamer1-plugins-base-devel
```

### Build & Install to `~/.local`

```bash
./scripts/install.sh
```

Or manually:

```bash
cargo test
cargo build --release
```

`build.rs` automatically discovers an optional custom PGO/ThinLTO WebKitGTK prefix (`$EXPLORER_WEBKIT_DIR`, `/opt/explorer-webkit`, or `/opt/velox-webkit`) and falls back transparently to system `webkitgtk-6.0`.

## Documentation

- [**Technical Reference**](docs/TECHNICAL.md) — Complete folder map, CLI flags (`--app`, `--app-id`), D-Bus actions, keyboard shortcuts, all `EXPLORER_*` environment variables & hidden configurations, and UI specs.
- [**Engineering Findings & Benchmarks**](docs/FINDINGS.md) — Hardware/DRM, WebKit Content Blocker, GTK4 layout/CSS postmortems, and Chrome vs. Explorer memory/performance measurements.
- [**Custom WebKitGTK & Fedora Upgrade Guide**](patches/README.md) — Notes on optional Clang 2-stage PGO + ThinLTO WebKitGTK builds and future Fedora upgrades.
- [**AI Agent Rules**](AGENTS.md) — Repository rules for AI coding assistants (never auto-commit or auto-push without explicit user permission).
