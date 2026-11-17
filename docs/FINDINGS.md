# Engineering Findings & Lessons Learned

Key hardware, WebKitGTK, GTK4, and performance findings discovered while building and profiling **Explorer** on Fedora 45 Wayland (AMD Ryzen 7 5800H / 16 threads, AMD Cezanne iGPU + NVIDIA RTX 3050 Ti Optimus).

---

## 1. Architecture Separation for Long-Term Upgradeability

- **Problem:** Early patches mixed hardware workarounds, power heuristics, and UI code into monolithic modules, making future Fedora (`45 -> 50 -> 60`) and WebKitGTK upgrades fragile.
- **Solution:** Split the codebase strictly into `src/core/` (pure upstream GTK4/Libadwaita/WebKitGTK) and `src/tuning/` (isolated OS/hardware/engine pipeline behind `EngineTuning` and `EXPLORER_DISABLE_TUNING=1`).
- **Result:** Updating GTK4/WebKitGTK requires touching only `src/core/` (or nothing at all thanks to `build.rs` auto-fallback to system `webkitgtk-6.0`).

---

## 2. `libva` DRM Render Node Requires `O_RDWR` on AMD Mesa

- **Finding:** Opening `/dev/dri/renderD128` with `O_RDONLY` caused Mesa's `amdgpu_bo_cpu_map` to fail with `-13 (EACCES)` during `vaInitialize`, falsely reporting software fallback.
- **Fix:** Opening the render node with `OpenOptions::new().read(true).write(true)` (`O_RDWR`) in `src/tuning/hardware.rs` allows `vaQueryConfigEntrypoints` (`VLD`) to succeed cleanly, reporting `VP9 · H264 · HEVC`.

---

## 3. Keeping NVIDIA Optimus dGPU Suspended Eliminates 2D Scroll Stutter

- **Finding:** Allowing WebKitGTK or Mesa to wake the discrete RTX 3050 Ti on Wayland introduces PCIe DMA-BUF cross-GPU copy overhead during 2D compositing and spikes idle power draw and fan noise.
- **Fix:** Discover the AMD iGPU render node dynamically, set `WEBKIT_WEB_RENDER_DEVICE_FILE` + `__GLX_VENDOR_LIBRARY_NAME=mesa` + `LIBVA_DRIVER_NAME=radeonsi` (when `EXPLORER_ALLOW_DGPU` is unset, preventing GStreamer's VA scan from loading `/usr/lib64/dri/nvidia_drv_video.so` on `renderD129`), and skip vendor `0x10de` in `DisplayVBlankMonitorDRM`.
- **Result:** The RTX 3050 Ti stays `suspended` during YouTube/browsing while keeping fan speed at `0 RPM`.

---

## 4. WebKit `UserContentFilterStore` Rejects `|` Disjunctions

- **Finding:** WebKit's native DFA Content Blocker compiler rejects regex `|` alternation (e.g. `(com|net)`), silently failing to compile the entire rule list.
- **Fix:** Split every domain/path alternation into separate rules (`explorer-adblock-v3` in `src/tuning/content_blocker.rs`).
- **Result:** Zero regex parse errors and verified blocking of third-party trackers (`doubleclick`, Google Analytics) with negligible runtime CPU overhead.

---

## 5. GTK4 Layout & CSS Pitfalls Fixed

1. **`GtkRevealer` Horizontal Width Leak:** A horizontal `GtkRevealer(SlideRight)` wrapping a `220px` sidebar continued reserving `220px` of horizontal space even when collapsed, pushing web viewports to the right. Removing the left sidebar and switching to an auto-hiding `38px` top horizontal `.tab-bar` (`set_visible(tabs.len() > 1)`) guarantees web viewports start at `x = 0`.
2. **`GtkMenuButton` CSS Selector:** Styling `button.power-pill` matches nothing because `GtkMenuButton` wraps an internal `GtkToggleButton`. Targeting `menubutton.power-pill > button` is required for the capsule styling to apply.
3. **`app.quit()` vs `w.close()`:** Calling `adw::Application::quit()` directly skips `window.connect_close_request`, leaving `running.lock` and `power-profiles-daemon` holds behind. Iterating `app.windows()` and calling `w.close()` ensures clean session & power teardown on `Ctrl+Q`.

---

## 6. Benign Redirect Cancellations in `WebView`

- **Finding:** Sites such as YouTube perform rapid internal navigation replacements (e.g. `?themeRefresh=1`) that emit `load-failed` with cancellation errors, which previously flashed an error page.
- **Fix:** `connect_load_failed` in `src/core/window.rs` checks for `NetworkError::Cancelled`, `PolicyError::FrameLoadInterruptedByPolicyChange`, `gio::IOErrorEnum::Cancelled`, or `"Load request cancelled"` and returns early.

---

## 7. Measured Performance, Memory & Power Benchmarks

Measured on Fedora 45 Wayland (AMD Ryzen 7 5800H 8C/16T, Cezanne iGPU, RTX 3050 Ti suspended, custom PGO + ThinLTO WebKitGTK 2.54.2) on the stabilized release binary (`scripts/install.sh`):

### Browser vs. Upstream WebKit vs. Google Chrome

| Benchmark | Explorer | Upstream (`EXPLORER_DISABLE_TUNING=1`) | Google Chrome |
| :--- | :--- | :--- | :--- |
| **Cold Launch Time** (3-run median) | `505.8 ms` (`496.6 ms` min) | — | `468.7 ms` (`467.3 ms` min) |
| **Cold Launch Memory (PSS)** | `249.0 MiB` (`14` procs) | — | `456.3 MiB` (`15` procs) |
| **144 Hz Canvas `rAF`** (`390` frames) | **`141.46 fps`** (`7.069 ms` mean, `258.8 MiB`) | `138.74 fps` (`7.208 ms` mean, `287.4 MiB`) | `137.45 fps` (`7.275 ms` mean, `523.6 MiB`) |
| **Speedometer 3.1** (cold / warm cache) | `17.4` (`627.0 MiB`) / **`20.33`** (`621.6 MiB`) | `17.4` (`641.0 MiB`) | `23.7` (`926.5 MiB`) |
| **8-Tab Multi-Tab Workload** | `542.9 MiB` (`62` procs) | — | — |
| **Live YouTube Playback** | `988.8 MiB` (`RTX 3050 Ti: suspended`) | — | — |

### Power & Hardware Video Decode (1080p30 H.264 12 Mbps, RAPL Package Power)

Recorded in [`docs/IMPROVEMENTS.md`](IMPROVEMENTS.md#L37-L56) over 20 s windows:

| Scenario | Package Power | Video Process CPU | Notes |
| :--- | :--- | :--- | :--- |
| Desktop idle (no browser) | `2.76 W` | — | System baseline |
| Explorer idle (Wayland, 144 Hz) | `3.04 W` | `~0%` | `+0.28 W` browser idle overhead |
| Playback, **HW decode** (`vah264dec`, Wayland) | **`6.69 W`** | **`3.7%`** (1 core) | VCN hardware decode (`7.14 W` / `4.2%` on X11) |
| Playback, **SW decode** (`avdec_h264`, X11) | `7.69 W` | `15.6%` (1 core) | `4.2x` higher CPU usage without VA-API |

---

## 8. Linux Per-Thread Throttling & Process Design Tradeoffs

- **Per-TID `SCHED_BATCH` + `ioprio` (`set_process_ioprio`):**
  - On Linux kernels, `ioprio_set(IOPRIO_WHO_PROCESS, pid)` and `setpriority(PRIO_PROCESS, pid)` target only the main TID, leaving background `WebKitWebProcess` worker/JIT/compositor threads unaffected.
  - Furthermore, under Fedora's unprivileged `ulimit -e == 0` (`RLIMIT_NICE`), raising nice (`0 -> 12`) is **irreversible** (`12 -> 0` fails with `EACCES`).
  - **Solution:** Enumerate every TID in `/proc/<pid>/task/*` and toggle **`SCHED_BATCH` ↔ `SCHED_OTHER`** alongside **`IOPRIO_CLASS_IDLE` ↔ `IOPRIO_CLASS_BE/4`** — both are 100% reversible without `CAP_SYS_NICE`.
- **Non-Root Sensor Probing:** `intel-rapl:0/energy_uj` is `0400 root:root` on Fedora (`EACCES`), so `SensorPaths::discover` verifies `File::open` readability and surfaces `amdgpu` `hwmon` `power1_input` (`igpu_watts`) directly in the Power HUD.
- **Process Sharing & Audio-Keyed Throttling:** Disabling `ProcessSwapOnCrossSiteNavigation` keeps PSS ~43% below Chrome while `media_pages` exempts background tabs playing audio from throttling.

---

## 9. Empirical Audit of `docs/IMPROVEMENTS.md` (`F1`–`F12`)

Every recommendation in [`docs/IMPROVEMENTS.md`](IMPROVEMENTS.md) was audited against the live codebase and tested on this machine (`Fedora 45`, `kernel 7.3.3`, `Ryzen 7 5800H`, `Cezanne iGPU` + `RTX 3050 Ti`). Below is the verified verdict, where the review was wrong or incomplete, and what was implemented:

| ID | Review Claim | Empirical Finding & Correction | Action Taken |
| :--- | :--- | :--- | :--- |
| **F1** | Nested `MainContext::iteration` pump in `content_blocker.rs` causes reentrancy risk | **Confirmed.** `UserContentFilterStore::load`/`save` callbacks already run on the main loop and attach to `UserContentManager` asynchronously. | Removed the `ctx.iteration(false)` pump in `src/tuning/content_blocker.rs`. |
| **F2** | Sync D-Bus calls stall startup (`set_edp_refresh_rate`, `HoldProfile`) | **Confirmed.** Blocking `bus_get_sync` / `call_sync` ran before `window.present()`. | Converted Mutter `DisplayConfig`, `UPower`, and `PPD` D-Bus calls to async `gio::bus_get` / `bus.call`, deferring initial `sync_system_power()` via `glib::idle_add_local_once` after `window.present()` (cut cold launch `514.9 ms -> 505.8 ms`). |
| **F3** | `pkg_watts` RAPL is `root`-only (`EACCES`); use `amdgpu` `power1_input` | **Confirmed + extra bug found.** `SensorPaths::discover()` used `fs::metadata` (`stat`), so it stored the `0400` RAPL path and failed `open()` every 2 s. `amdgpu` `power1_input` was already sampled into `TelemetrySnapshot::igpu_watts`, just not shown. | Changed `readable()` to `fs::File::open` and surfaced `igpu_watts` in the Power HUD (`iGPU: ... · 6.0W`). |
| **F4** | Remove `LIBVA_DRIVER_NAME=radeonsi` because Mesa auto-selects it | **Partially WRONG / dangerous regression caught by testing.** While `vainfo` on `/dev/dri/renderD128` auto-selects `radeonsi`, `/usr/lib64/dri/nvidia_drv_video.so` is present on this system. Removing `LIBVA_DRIVER_NAME=radeonsi` caused GStreamer's VA scan during YouTube playback to load `nvidia_drv_video.so` on `renderD129`, **waking the RTX 3050 Ti to `active` at `t=4s..12s`**. | Kept `LIBVA_DRIVER_NAME=radeonsi` scoped strictly inside the Optimus isolation guard (`on_battery \|\| EXPLORER_ALLOW_DGPU` unset). Confirmed `youtube_dgpu_state = "suspended"` throughout live playback while leaving `EXPLORER_ALLOW_DGPU=1` free to use NVDEC. |
| **F5** | One-time transient dGPU open at launch via GDK | **Confirmed low priority.** RTD3 returns the dGPU to `suspended` without holding fds open. | Kept zero distro GTK/GDK patches for Fedora upgradeability. |
| **F6** | Apply `setpriority(..., 12)` and `ioprio_set` across `/proc/<pid>/task/*` | **Half-right, proposed fix had a FATAL bug.** Kernel test proved `setpriority`/`ioprio_set` on `pid` only touched the leader TID (`python3` worker TID stayed `nice=0`). **However**, under unprivileged `ulimit -e == 0`, raising nice (`0 -> 12`) is **irreversible** (`12 -> 0` fails with `errno 13 EACCES`), which would permanently lock WebKit threads at `nice=12` after returning to foreground/AC. | Removed `setpriority(..., 12)`. Enumerate `/proc/<pid>/task/*` and toggle **`SCHED_BATCH` ↔ `SCHED_OTHER`** + **`IOPRIO_CLASS_IDLE` ↔ `IOPRIO_CLASS_BE/4`** per TID — verified `rc=0` reversible without root (`per_tid_batch_and_ioprio_throttling_is_reversible_unprivileged`). |
| **F7** | `ProcessSwapOnCrossSiteNavigation=false` trades site isolation for lower RAM | **Confirmed deliberate design.** Keeps multi-tab PSS ~43%–45% lower than Chrome. | Kept `false`; documented tradeoff in `docs/TECHNICAL.md`. |
| **F8** | Add adblock exception rules (`"action": {"type": "ignore"}`) for OTT auth | **Valid caution, invalid WebKit JSON syntax in review.** WebKit Content Blocker uses `"type": "ignore-previous-rules"`, not `"ignore"`. | Left curated ruleset untouched for stability; documented syntax correction here. |
| **F9** | `save_session_now()` writes synchronously on every `notify::uri` | **Confirmed.** Redirect chains triggered repeated tmp-write + rename calls. | Added 600 ms debounced `schedule_session_save()` with synchronous flush on `connect_close_request`. |
| **F10** | Fixed 2 s telemetry timer runs on battery | **Confirmed.** | Switched to adaptive `2 s` (AC or open HUD popover) vs `5 s` (battery with HUD closed). |
| **F11** | Background throttling keys on `is_playing_audio` | **Confirmed acceptable.** Active foreground tab is never throttled. | Kept as-is; documented in `docs/TECHNICAL.md`. |
| **F12** | `last_hz` goes stale if user changes display mode manually | **Confirmed.** | Subscribed to Mutter `MonitorsChanged` signal to reset `last_hz` to `None`. |
