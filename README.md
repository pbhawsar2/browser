# Explorer Browser

**Explorer** is a hardware-tuned, modular C++20 + Rust desktop web browser powered by **Blink + V8 + Skia (CEF 158 Alloy Mode)** — engineered for Linux Wayland / AMD Ryzen + Radeon iGPU + NVIDIA Optimus laptops with zero Chrome bloat, **2-Stage Clang Profile-Guided Optimization (PGO) + ThinLTO**, built-in **Brave `adblock-rust`**, **AES-256-GCM** encrypted password vault (with Linux PAM login/root verification for password reveal), standalone chromeless **Web Apps**, and rock-solid multi-tab stability.

## SOLID Architecture

| Module | Directory | Responsibility |
| :--- | :--- | :--- |
| **HAL** | `hal/` | AMD/Intel iGPU discovery (`0x1002`), NVIDIA Optimus dGPU D3cold isolation (`0x10de`), RADV Vulkan ICD pinning, direct `O_RDWR` `libva` VLD probe, persistent `sd-bus` PPD performance hold, UPower/Mutter 60↔144 Hz refresh control, reversible per-TID `SCHED_BATCH` + `IOPRIO_IDLE` throttling, and live sysfs/hwmon telemetry (APU `PPT` package watts, dual cooling fans, battery `%` & wattage — zero UI/CEF coupling) |
| **Policy** | `policy/` | Pure deterministic state machines: `PowerPolicyEngine` (`Zero-Fan` ↔ `AC Turbo`), `MediaCodecPolicy` (hardware VA-API VP9/H.264/HEVC vs software AV1 gatekeeper), `PidThrottlePlanner` (active & media-playing tab exemption), and `navigation_policy` (Google default search, strict secure-origin checks, `1.00x` / `100%` default page zoom) |
| **Shield** | `shield/` + `shield-brave/` | Native **Brave `adblock-rust`** (`v0.13.6`) network-layer ad & tracker blocker with instant atomic On/Off toggle, full resource-type mapping (`$script`, `$xhr`, `$ping`, `$media`, `$font`, `$subdocument`), OTT/payment `@@` exceptions, and async `posix_spawnp` EasyList/EasyPrivacy/Peter Lowe filter list updater |
| **Vault** | `vault/` | OpenSSL `libcrypto` **AES-256-GCM** encrypted local password vault (`0600` key + random 96-bit IV + 128-bit auth tag) with strict origin validation, save, update, delete, form-scoped autofill, and PAM (`unix_chkpwd`/`sudo`) authentication required before revealing any saved password |
| **WebApp** | `webapp/` | Turnkey Linux FreeDesktop installer launching sites as pure chromeless standalone desktop apps (`--app=<url> --app-id=<id>`) with magic-byte validated multi-size (`256/128/64/48px`) hicolor icons, Wayland `app_id` dock binding, and isolated profiles |
| **Engine** | `engine/` | Dependency-inverted `ITabObserver`, hardware-driven Blink/V8/VA-API/Wayland flag generator, and `ExplorerCefClient` (DoClose multi-tab isolation, Shield IO hook, GPC/DNT headers on all navigations, Downloads manager, per-tab zoom persistence, keyboard shortcuts, renderer PID & media-state IPC) |
| **App UI** | `app/` | Frameless Obsidian WebUI chrome (`44px` top bezel with `36x34px` controls & segmented Quick Zoom Pill `[ − | 100% | + ]`, auto-hiding `38px` tab bar for `tabs > 1`, `38px` incremental Find bar, 5-tab `Power / Shield / Vault / Downloads / Apps` floating control overlay, ultra-sharp antialiased typography), zero browsing history, persistent cookies, and atomic session restore |
| **PGO Trainer** | `pgo/` | Automated 2-Stage Clang PGO + ThinLTO workload trainer & benchmark suite (`pgo/train_pgo.py`, `pgo/explorer.profdata`) profiling heavy sites (`robu.in`, `amazon.in`, `youtube.com`), scrolling, zooming, HUD, Vault, and hardware video/audio decode |

## Prerequisites (Fresh Linux Installation)

- **Fedora / RHEL**:
  ```bash
  sudo dnf install -y clang llvm cmake ninja-build rust cargo pkgconf-pkg-config \
       systemd-devel openssl-devel ImageMagick desktop-file-utils curl tar bzip2 python3
  ```
- **Ubuntu / Debian**:
  ```bash
  sudo apt install -y clang llvm cmake ninja-build rustc cargo pkg-config \
       libsystemd-dev libssl-dev imagemagick desktop-file-utils curl tar bzip2 python3
  ```
- **Arch Linux**:
  ```bash
  sudo pacman -S --needed clang llvm cmake ninja rust pkgconf systemd openssl \
       imagemagick desktop-file-utils curl tar bzip2 python
  ```

## Build, PGO Train, Test & Install

```bash
chmod +x install-cef.sh install.sh update.sh uninstall.sh
./install.sh               # Build with Clang PGO + ThinLTO, run 6/6 CTest suites, and install
./install.sh --train-pgo   # Re-run full Stage-1 live PGO workload training + Stage-2 PGO build
```

## Uninstalling & Purging Data

```bash
./uninstall.sh             # Stop Explorer and purge binaries, WebApps, icons, and user profile/vault data
./uninstall.sh --keep-data # Remove binaries/launchers while preserving user profile & encrypted vault
./uninstall.sh --purge-all # Also remove the cached ~/.local/share/explorer-cef SDK runtime
```

## Updating Chromium / CEF Engine Safely

```bash
./update.sh                              # Check & upgrade to latest stable CEF/Chromium
./update.sh --tarball /path/to/cef.tbz2  # Install custom znver3 PGO+ThinLTO+BOLT libcef.so
./update.sh --rollback                   # Instant rollback if needed
```

## Runtime Escape Hatches

- `EXPLORER_DISABLE_TUNING=1 explorer` — bypass all hardware, power, VA-API, and V8 optimizations.
- `EXPLORER_ALLOW_DGPU=1 explorer` — allow discrete GPU access instead of Optimus D3cold isolation.
- `EXPLORER_GPU_BACKEND=angle|vulkan|graphite explorer` — select graphics backend (default `angle`).
- `EXPLORER_CEF_DIR=/custom/cef` — override the CEF runtime directory.
