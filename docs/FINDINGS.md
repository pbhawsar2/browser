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
- **Fix:** Discover the AMD iGPU render node dynamically, set `WEBKIT_WEB_RENDER_DEVICE_FILE` + `__GLX_VENDOR_LIBRARY_NAME=mesa`, and skip vendor `0x10de` in `DisplayVBlankMonitorDRM` unless `EXPLORER_ALLOW_DGPU` is set.
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

## 7. Measured Performance & Memory Footprint

Measured on Fedora 45 Wayland (Ryzen 7 5800H, 16 threads, custom PGO + ThinLTO WebKitGTK 2.54.2 engine) compared against Google Chrome:

| Metric | Explorer | Chrome | Notes |
| :--- | :--- | :--- | :--- |
| **Cold Launch Time** | `514.9 ms` | `460.4 ms` | Single native binary + pre-warmed spare tab (`~2 ms` `Ctrl+T` switch) |
| **Idle Memory (PSS)** | `278.3 MiB` | `493.4 MiB` | `-43.6%` lower baseline memory |
| **Speedometer 3.1** | `17.2` (`638.7 MiB` PSS) | `23.6` (`982.9 MiB` PSS) | `+29.1%` uplift over stock WebKitGTK via 16-thread AC Turbo scaling |
| **Canvas `rAF` Smoothness** | `141.46 fps` (`288.2 MiB` PSS) | `560.8 MiB` PSS | Native `EventControllerScroll` `3.0x` wheel / `1.85x` trackpad scaling |
