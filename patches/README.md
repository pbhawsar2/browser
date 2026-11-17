# Custom WebKitGTK Engine & Upgrade Recipe

`Explorer` separates **Core UI/Browser Logic** (`src/core/`) from **Hardware/Engine Tuning** (`src/tuning/`) and **Optional Custom WebKitGTK Builds**.

## Upgrading on Future Fedora Releases (Fedora 45 -> 50 -> 60)

1. **Zero-Patch System Build (Default Fallback):**
   `build.rs` automatically falls back to your distro's upstream `webkitgtk-6.0` package via `pkg-config` whenever no `/opt/explorer-webkit` or `/opt/velox-webkit` prefix is present (or set `PKG_CONFIG_PATH=""` and remove `/opt/velox-webkit`). Core UI, tabs, Google search, WebApps, adblocking, scroll scaling, and VA-API work out of the box.
2. **Optional Custom PGO + ThinLTO + `-march=znver3` WebKitGTK Build:**
   - Optional engine patches applied in `/opt/velox-webkit` (`webkitgtk-2.54.2`):
     1. Default `PreferPageRenderingUpdatesNear60FPS = false` (also enforced dynamically at runtime in `src/tuning/webkit_engine.rs`).
     2. Skip discrete NVIDIA vendor `0x10de` in `DisplayVBlankMonitorDRM` unless `EXPLORER_ALLOW_DGPU` / `VELOX_ALLOW_DGPU` is set (also isolated at runtime via `WEBKIT_WEB_RENDER_DEVICE_FILE` + Mesa ICD env vars).
   - Compiled with Clang 2-stage PGO (`Speedometer 3.1` + media/compositor counters) + `ThinLTO` + `-O3 -march=znver3 -fno-semantic-interposition`.
   - Point `EXPLORER_WEBKIT_DIR=/opt/<prefix>` (and `PKG_CONFIG_PATH=/opt/<prefix>/lib64/pkgconfig` when building) to swap WebKit engine builds cleanly without editing `src/core/`.
