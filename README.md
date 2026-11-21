# Explorer Browser

**Explorer** is a hardware-tuned, modular C++20 + Rust desktop web browser powered by **Blink + V8 + Skia (CEF 158 Alloy Mode)** — engineered for Linux Wayland / AMD Ryzen + Radeon iGPU + NVIDIA Optimus laptops with zero Chrome bloat, built-in **Brave `adblock-rust`**, **AES-256-GCM** encrypted password vault, standalone chromeless **Web Apps**, and rock-solid multi-tab stability.

## SOLID Architecture

| Module | Directory | Responsibility |
| :--- | :--- | :--- |
| **HAL** | `hal/` | AMD/Intel iGPU discovery (`0x1002`), NVIDIA Optimus dGPU D3cold isolation (`0x10de`), RADV Vulkan ICD pinning, direct `O_RDWR` `libva` VLD probe, async `sd-bus` PPD/UPower/Mutter 60↔144 Hz refresh control, reversible per-TID `SCHED_BATCH` + `IOPRIO_IDLE` throttling, and live sysfs/hwmon telemetry (zero UI/CEF coupling) |
| **Policy** | `policy/` | Pure deterministic state machines: `PowerPolicyEngine` (`Zero-Fan` ↔ `AC Turbo`), `MediaCodecPolicy` (hardware VA-API VP9 vs software AV1 gatekeeper), `PidThrottlePlanner`, and `navigation_policy` (Google default search + `1.05x` default page zoom) |
| **Shield** | `shield/` + `shield-brave/` | Native **Brave `adblock-rust`** (`v0.13.6`) network-layer ad & tracker blocker with instant atomic On/Off toggle, OTT/payment `@@` exceptions, and async EasyList/EasyPrivacy/Peter Lowe filter list updater |
| **Vault** | `vault/` | OpenSSL `libcrypto` **AES-256-GCM** encrypted local password vault (`0600` key + random 96-bit IV + 128-bit auth tag) with save, update, delete, origin lookup, and one-click autofill |
| **WebApp** | `webapp/` | Turnkey Linux FreeDesktop installer launching sites as pure chromeless standalone desktop apps (`--app=<url> --app-id=<id>`) with fetched 256px favicon icons and isolated profiles |
| **Engine** | `engine/` | Dependency-inverted `ITabObserver`, hardware-driven Blink/V8/VA-API/Wayland flag generator, and `ExplorerCefClient` (DoClose multi-tab isolation, Shield IO hook, Downloads manager, keyboard shortcuts, renderer PID IPC) |
| **App UI** | `app/` | Frameless Obsidian WebUI chrome (`44px` top bezel with `36x34px` controls, auto-hiding `38px` tab bar for `tabs > 1`, `38px` Find bar, 4-tab `Power / Vault / Downloads / Apps` floating control overlay), zero browsing history, persistent cookies, and atomic session restore |
| **Engine Builder** | `engine-builder/` | Cloud/CI build configuration (`args.gn`, `build-znver3-cef.sh`, GitHub Actions workflow) for drop-in custom `-march=znver3` + 2-Stage PGO + ThinLTO + BOLT `libcef.so` |

## Build, Test & Install

```bash
chmod +x install-cef.sh install.sh update.sh uninstall.sh
./install.sh
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
