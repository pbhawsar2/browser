# Explorer Technical Reference

Complete reference for Explorer's folder structure, CLI parameters, D-Bus actions, keyboard shortcuts, environment variables, hidden configurations, and UI specifications.

---

## 1. Repository Structure

```text
Browser/
├── AGENTS.md                       # AI agent safety & architecture rules
├── Cargo.toml                      # Minimal 6-crate dependency manifest (ThinLTO, panic=abort)
├── Cargo.lock                      # Reproducible dependency lockfile
├── build.rs                        # Custom/system WebKitGTK discovery, VA-API link, webext C build
├── .cargo/config.toml              # `-C target-cpu=native` release optimization
├── assets/
│   ├── explorer.desktop            # FreeDesktop launcher entry (`io.explorer.Browser`)
│   └── icons/
│       └── io.explorer.Browser.svg # Vector application icon
├── docs/
│   ├── TECHNICAL.md                # This reference (CLI, env vars, shortcuts, UI specs)
│   └── FINDINGS.md                 # Engineering findings, benchmarks, and bug postmortems
├── patches/
│   └── README.md                   # Optional custom PGO/ThinLTO WebKitGTK build & Fedora upgrade guide
├── scripts/
│   └── install.sh                  # User-space build + install script (`~/.local`)
├── webext/
│   └── explorer-webext.c           # WebProcess C extension (GStreamer decoder rank tuning & PID reporting)
└── src/
    ├── main.rs                     # mimalloc global allocator, CLI parsing, GTK/Adw app, D-Bus actions
    ├── core/                       # Upstream GTK4 / Libadwaita / WebKitGTK UI & lifecycle
    │   ├── mod.rs
    │   ├── window.rs               # Window, horizontal auto-hiding tabs, URL bar, session, lifecycle
    │   ├── navigation.rs           # URL normalization & Google search query routing
    │   ├── pages.rs                # Built-in Start Page & error page HTML
    │   ├── apps_popover.rs         # Web Apps menu (Install Current Page + launch/uninstall list)
    │   ├── downloads.rs            # Native download manager (`$XDG_DOWNLOAD_DIR`)
    │   ├── find_bar.rs             # In-page Ctrl+F search bar
    │   ├── power_popover.rs        # Live hardware/power HUD popover
    │   ├── webapp.rs               # PWA / WebApp `.desktop` + squircle icon generator
    │   ├── webapp_dialog.rs        # "Install Web App" modal dialog
    │   ├── win_controls.rs         # Cairo-drawn `36x34px` minimize/maximize/close buttons
    │   └── style.css               # Dark theme CSS (`#080C14` / `#131C2C` / `#38BDF8` / `#F87171`)
    └── tuning/                     # Isolated hardware, power, adblock & engine optimizations
        ├── mod.rs                  # `EngineTuning` facade + `EXPLORER_DISABLE_TUNING=1` switch
        ├── hardware.rs             # DRM `renderD*` detection, O_RDWR libva probe, sysfs, PPD, Mutter DBus
        ├── power.rs                # `PowerSupervisor` (`Zero-Fan · <W>` / `AC Turbo`)
        ├── content_blocker.rs      # Brave-style DFA ad/tracker filter (`explorer-adblock-v3`) & AV1 gatekeeper
        └── webkit_engine.rs        # Env defaults, Safari UA, DNS prefetch, `EventControllerScroll` scaler
```

---

## 2. CLI Parameters

```bash
explorer [OPTIONS] [URL...]
```

| Argument | Description |
| :--- | :--- |
| `URL...` | One or more URLs or search terms to open. In single-instance mode, forwards URLs to the already-running Explorer window. |
| `--app=<URL>` / `--app <URL>` | Launch in standalone WebApp (chromeless PWA) mode for `<URL>`, hiding the tab bar and URL chrome. |
| `--app-id=<ID>` / `--app-id <ID>` | Override the GTK application ID / Wayland `StartupWMClass` (defaults to `io.explorer.Browser`). |

---

## 3. D-Bus Actions (`io.explorer.Browser`)

Registered on `adw::Application` in `src/main.rs` for single-instance integration and automated testing:

| Action | Parameter | Behavior |
| :--- | :--- | :--- |
| `app.open-url` | `string` | Opens a URL in the active running browser window. |
| `app.install-app` | none | Opens the "Install Web App" dialog for the active tab. |
| `app.snapshot` | `string` (path) | Captures a PNG snapshot of the active window to the given file path. |
| `app.quit` | none | Closes all open windows cleanly via `w.close()` (preserving session/PPD cleanup). |

---

## 4. Keyboard Shortcuts

| Shortcut | Action |
| :--- | :--- |
| `Ctrl+T` | Open a new tab (instant pre-warmed Start Page) and focus the URL bar |
| `Ctrl+W` | Close the current tab |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | Cycle forward / backward through open tabs |
| `Ctrl+L` or `Alt+D` | Focus and select the URL / search bar |
| `Ctrl+F` | Open the in-page Find Bar (`Esc` closes it) |
| `Ctrl+R` or `F5` | Reload current page |
| `Ctrl+Shift+R` or `Shift+F5` | Hard reload (bypass cache) |
| `Alt+Left` / `Alt+Right` | Navigate Back / Forward |
| `Ctrl++` (`Ctrl+=`) / `Ctrl+-` | Zoom in / Zoom out (`+0.1` / `-0.1`, clamped `0.3..=3.0`) |
| `Ctrl+0` | Reset zoom to configured default (`1.05` or `EXPLORER_DEFAULT_ZOOM`) |
| `F11` | Toggle fullscreen |
| `Ctrl+Q` | Quit Explorer cleanly (runs `close_request` teardown) |

---

## 5. Environment Variables & Hidden Configurations

### Core & Runtime Overrides

| Variable | Default | Description |
| :--- | :--- | :--- |
| `EXPLORER_APP_ID` (`VELOX_APP_ID`) | `io.explorer.Browser` | Override the GTK/D-Bus Application ID. |
| `EXPLORER_PROFILE_DIR` (`VELOX_PROFILE_DIR`) | `~/.local/share/explorer` | Custom persistent profile directory (cookies, storage, sessions). |
| `EXPLORER_CACHE_DIR` | `~/.cache/explorer` | Custom cache directory (HTTP cache, compiled content filters). |
| `EXPLORER_DEFAULT_ZOOM` | `1.05` | Default page zoom factor for new tabs and `Ctrl+0` (valid range: `0.5` to `2.5`). |
| `EXPLORER_SW_RENDER` | unset | If set, disables hardware acceleration (`HardwareAccelerationPolicy::Never`). |
| `EXPLORER_NON_UNIQUE` | unset | If set, runs an isolated process instead of attaching to the primary D-Bus instance. |
| `EXPLORER_NO_SPELLCHECK` | unset | If set, disables WebKit spell checking. |
| `EXPLORER_NO_SESSION` | unset | If set, disables session persistence (`session.json` save / tab restore). |
| `EXPLORER_WEBEXT_DIR` | build-time path / `~/.local/lib/explorer` | Override directory containing `libexplorer_webext.so`. |
| `XDG_DOWNLOAD_DIR` | `$HOME/Downloads` | Target directory for file downloads. |

### Performance, Hardware & Power Tuning Overrides

| Variable | Default | Description |
| :--- | :--- | :--- |
| `EXPLORER_DISABLE_TUNING` | unset (`0`) | Set to `1` to bypass **all** `src/tuning/` optimizations and run pure upstream WebKitGTK behavior. |
| `EXPLORER_TURBO` | unset | If set, forces AC Turbo Skia/JSC multi-threading even while running on battery. |
| `EXPLORER_ALLOW_DGPU` | unset | If set, permits discrete NVIDIA GPU usage instead of isolating rendering to the AMD iGPU. |
| `EXPLORER_NO_REFRESH_SWITCH` | unset | If set, disables automatic Mutter display refresh rate switching between AC and battery. |

### Build-Time Configuration

| Variable | Default | Description |
| :--- | :--- | :--- |
| `EXPLORER_WEBKIT_DIR` | auto (`/opt/explorer-webkit`, `/opt/velox-webkit`, or system) | Custom WebKitGTK 6.0 prefix used by `build.rs` for headers, `pkg-config`, and `-Wl,-rpath`. |

### Internal Engine Defaults (Injected by `EngineTuning::prepare_process_env`)

Unless already set in the user's environment, Explorer sets the following defaults before `gtk::init()`:

- `GSK_RENDERER=gl`
- `WEBKIT_DISABLE_DMABUF_RENDERER=0`
- `WEBKIT_DISABLE_DMABUF_ATLAS=0`
- `WEBKIT_SKIA_ENABLE_DDL=1`
- `WEBKIT_SKIA_MSAA_SAMPLE_COUNT=0`
- `MESA_SHADER_CACHE_MAX_SIZE=2G`
- `WEBKIT_WEB_RENDER_DEVICE_FILE=<discovered AMD iGPU /dev/dri/renderD*>`
- `__GLX_VENDOR_LIBRARY_NAME=mesa` (when dGPU is blocked)
- `WEBKIT_SKIA_GPU_PAINTING_THREADS` / `WEBKIT_SKIA_CPU_PAINTING_THREADS` (scaled dynamically up to 16 threads in AC Turbo, or `1` / `2` on battery)

---

## 6. UI & Theme Specifications

- **Palette:** `#080C14` window canvas, `#131C2C` elevated control cards, `#334155` subtle 1px borders, `#38BDF8` active cyan accent, `#F87171` coral close hover.
- **Top Bezel (`.top-bezel`):** `44px` minimum height; all navigation buttons, menu buttons, and Cairo window controls share an exact `36x34px` allocation (`y = 5`).
- **Tab Bar (`.tab-bar`):** `38px` horizontal tab strip that automatically hides when only `1` tab is open, ensuring single-tab viewports start at `x = 0` with zero wasted vertical or horizontal space.
- **URL Bar (`entry.url-bar`):** `34px` height inside `adw::Clamp(maximum_size = 880, tightening_threshold = 480)` with `10.6pt` medium-weight text.
- **Power HUD Pill (`menubutton.power-pill > button`):** `32px` min-height capsule (`border-radius: 999px`) displaying live mode (`AC Turbo` or `Zero-Fan`), battery wattage (battery mode only), and fan RPM.

---

## 7. Architecture Tradeoffs & Runtime Behavior

- **`ProcessSwapOnCrossSiteNavigation = false`:** Intentional memory optimization reducing multi-tab PSS by ~43% vs Chrome (pages share WebProcesses safely without site-swap overhead).
- **Per-Thread Background Throttling:** Inactive non-media tabs (`media_pages` exemption tracks audio playback) transition every TID in `/proc/<pid>/task/*` to `SCHED_BATCH` + `IOPRIO_CLASS_IDLE` on battery and revert cleanly to `SCHED_OTHER` + `IOPRIO_CLASS_BE/4` when foregrounded or on AC power.
- **DRM / Widevine Status:** Fedora's WebKitGTK ships without EME/Widevine CDM (`navigator.requestMediaKeySystemAccess` unavailable), so DRM-protected OTT streams require Chrome.
