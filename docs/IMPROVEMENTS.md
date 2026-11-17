# Explorer — Verified Performance, Stability & Battery Improvement Plan

**Scope of this document:** Complete end-to-end review of Explorer and its hardware/OS integration on this
machine (HP Victus 16, AMD Ryzen 7 5800H + RTX 3050 Ti, Fedora 45), with **every claim verified by running
tests on 17 Nov 2026**. No code changes and no system-level changes were made. Each section gives the exact
technical recipe to implement the improvement yourself.

**Test-engineering note:** everything marked ✅ was directly observed in this session. Anything marked
`VERIFY` must be confirmed before relying on it.

---

## 1. Verified machine & integration inventory

| Component | Measured state (this session) |
| :--- | :--- |
| CPU | Ryzen 7 5800H (Cezanne, 8C/16T, 16 MB L3), microcode `0xa500014`, `amd-pstate-epp` driver, EPP `balance_performance` |
| iGPU | Cezanne Vega (radeonsi, Mesa 26.2.6, DRM 3.64) — `/dev/dri/renderD128` (vendor `0x1002`) |
| dGPU | RTX 3050 Ti Mobile, NVIDIA 620.70.06, RTD3 (`NVreg_DynamicPowerManagement=0x02`) — `runtime_status: suspended` ✅ throughout browsing & playback |
| RAM/Swap | 16 GB + zram (zstd, 7.5 GB); measured compression ratio **3.79×** (899.8 MB data → 237.2 MB) ✅ |
| Display | eDP-1 `1920x1080 @ 144.005 Hz` and `@ 60.031 Hz` ✅ (the 60/144 Hz power switcher targets are valid) |
| Kernel | 7.3.3-300.fc45 — cmdline: `amd_pstate=active workqueue.power_efficient=1 pcie_aspm=force nvidia.NVreg_DynamicPowerManagement=0x02 rd.driver.blacklist=nouveau,nova_core` |
| Power mgmt | `tuned` + `tuned-ppd` active; 5 custom HP Victus profiles; `net.hadess.PowerProfiles.HoldProfile/ReleaseProfile` **exist on tuned-ppd** ✅ (the browser's AC-Turbo hold works) |
| Engine | Custom **PGO + ThinLTO WebKitGTK 2.54.2** at `/opt/velox-webkit` ✅ (verified via `ldd` of the release binary) |
| ASPM | GPUs and NVMe `LnkCtl: ASPM Disabled` under `balanced` (policy `default` = BIOS); battery profiles switch policy to `powersupersave` ✅ |
| sysctl (AC `balanced`) | `swappiness=10`, `page-cluster=0`, `dirty_writeback=500cs`, `nmi_watchdog=1` ✅ |

**Engine integration checks (all verified):**

- All `WEBKIT_SKIA_*` env-var names exist in the custom WebKit build ✅; all `JSC_*` tunables exist in its JSC ✅.
- The VA profile **numeric constants** in `src/tuning/hardware.rs` are **byte-for-byte correct** against upstream `va.h` (H264Main=6, H264High=7, H264ConstrainedBaseline=13, HEVCMain=17, HEVCMain10=18, VP9Profile0=19, VP9Profile2=21, AV1Profile0=32, VLD=1) ✅.
- `GST_PLUGIN_FEATURE_RANK` **symbolic ranks parse correctly** (`dav1ddec:NONE` → rank 0, `vavp9dec:MAX` → 2147483647) ✅.
- `cargo test`: **6/6 pass** ✅.

---

## 2. Runtime test results (video/efficiency)

Test vector: locally generated 1080p30 H.264 High 4:2:0 8-bit MP4 (12 Mbps) served over HTTP with Range
support, played in a loop inside the actual release binary; package power measured via
`/sys/class/powercap/intel-rapl:0/energy_uj` (root) over 20 s windows; playback confirmed by jiffie-precise
`utime` deltas and in-page video state (`readyState=4`, `currentTime` advancing).

| Scenario | Package power | Video-proc CPU | Note |
| :--- | :--- | :--- | :--- |
| Desktop idle (no browser) | **2.76 W** | — | baseline |
| Browser idle, Wayland, 144 Hz | **3.04 W** | ~0 % | browser overhead ≈ **+0.3 W** — excellent |
| Playback, **HW decode** (ranks force `vah264dec`) | **6.69 W** (Wayland) / 7.14 W (X11) | **4.2 %** (X11) / **3.7 %** (Wayland) of 1 core | VCN does the work |
| Playback, **SW decode** (`vah264dec:NONE` forces `avdec_h264`) | **7.69 W** (X11) | **15.6 %** of 1 core | for A/B only |

**Conclusions (verified):**

1. Hardware decode is **4.2× cheaper in CPU** than software decode and **0.55 W cheaper** at the package
   level even in a compositor-bound measurement. The existing VA-API rank pipeline is working end-to-end.
2. The X11 path (GDK_BACKEND=x11, used only for testability) costs ~0.45 W more than the Wayland path —
   production Wayland is the right default (it already is).
3. The dGPU stayed `suspended` during all playback/browsing except a **one-time transient wake right after
   launch** (see Finding F5).
4. The MSE codec gatekeeper and `mediaCapabilities` shim behave exactly as designed: AV1 reports
   `supported=true, smooth=true, powerEfficient=false` ✅ (software `dav1d`), so YouTube correctly negotiates
   **VP9 profile-0/2 or H.264 with VCN hardware decode** — the optimal choice on Cezanne, which has **no AV1
   hardware decode** (verified: `vaav1dec` element missing, no AV1 profile in `vainfo`).

---

## 3. Platform support matrix for streaming (verified at runtime)

Probed the exact engine Explorer links against (custom `/opt/velox-webkit` WebKit 2.54.2) with an
in-process WebView:

| Platform | Status | Evidence |
| :--- | :--- | :--- |
| YouTube | ✅ Full, HW VP9/H.264 | MSE VP9 p0/p2 + avc1 supported; AV1 powerEfficient=false routed away |
| Dailymotion / Vimeo / X / Facebook / Instagram video | ✅ Works | plain MSE (`avc1`, `vp09`, `mp4a`, `opus` all `true`) |
| DRM-free HLS/DASH sites | ✅ Works | GStreamer `hlsdemux`/`adaptivedemux` present (gst-plugins-bad installed) |
| **Netflix** | ❌ Blocked | `navigator.requestMediaKeySystemAccess` **does not exist** in this build (`hasEME: false`) |
| **Amazon Prime Video** | ❌ Blocked | same — Widevine EME required |
| **JioHotstar** | ❌ Blocked | same — Widevine L3/L1 required |
| **Zee5 / SonyLIV / MX Player premium** | ❌ Blocked (most content) | same — Widevine required |
| Dolby audio (ec-3) / HEVC via MSE | ✅ decode-side ready | `ec-3 => true`, `hvc1 => true` — moot without DRM |

**Why (verified):** this WebKitGTK build — and *every* Fedora WebKitGTK build — has **no EME/CDM backend**
(no OpenCDM, no Widevine GMP host). The Widevine CDM binary exists only inside
`/opt/google/chrome/WidevineCdm` (Chrome's own host). GStreamer's `widevinedecrypt` element is not shipped
by Fedora and requires an ARM-TEE OEMCrypto stack that does not exist on x86 desktops.

**Realistic options (pick consciously):**

1. **Honest fallback (recommended):** when EME is unavailable and a page fails with a DRM error, offer
   "Open in Chrome" handoff (you already have the `--app=<url>` WebApp infrastructure — a
   `Exec=google-chrome --app=<url>` desktop entry is trivial). Implement as UI affordance only.
2. **Do not pursue** third-party "WebKitGTK Widevine patches": they require patching WebKit's CDM
   (OpenCDM) and a L3 CDM daemon; nothing upstream-maintained exists for x86 Fedora in 2026.
3. Keep YouTube/short-form/DRM-free as Explorer's strength: your HW-decode path is verified excellent.

---

## 4. Findings (bugs / risks) — with evidence

### F1. Startup runs a nested main-loop pump (reentrancy hazard)
`content_blocker::build_user_content_manager` calls `MainContext::iteration(false)` in a `while` loop for up
to **250 ms** while the window is being constructed. Events dispatched during the pump can re-enter app code
before `BrowserState` exists.

**How to fix (recipe, do not apply yet):** delete the pump entirely. `UserContentFilterStore::load` /
`save` callbacks already run on the main loop and `UserContentManager::add_filter` may be called after
WebViews exist — WebKit applies filters to loads from that point forward (the adblock store callback
attaches within a few ms anyway since the compiled filter is cached in `$XDG_CACHE_HOME/explorer/filters`).
If you want first-load coverage, create the *first* WebView inside the filter-loaded callback instead.

### F2. Synchronous D-Bus calls on the main thread during startup
`sync_system_power()` (called from `build_and_present`) executes, synchronously, with 2 s timeouts each:
`bus_get_sync(session)` → `GetCurrentState` → `ApplyMonitorsConfig`, and `bus_get_sync(system)` →
`HoldProfile`. A slow/hung bus stalls cold start up to ~4 s (your measured 514 ms is the healthy case).

**Recipe:** make them async: replace `call_sync` with `bus.call(...)` + callbacks (same Variant types);
schedule the first `sync_system_power()` via `glib::timeout_add_local_once(1200 ms)` after the window is
mapped so the refresh-rate switch never competes with first paint. Keep the synchronous variants only in
the UPower change handler (it is already event-driven).

### F3. `pkg_watts` telemetry is dead for non-root users (verified)
`/sys/class/powercap/intel-rapl:0/energy_uj` is **root-only on this kernel** (`cat` as user → Permission
denied). `SensorPaths::sample_rapl_watts()` therefore always yields `None` → the HUD "CPU: …W" and the
battery-pill RAPL fallback silently never work in production.

**Recipe:** at `SensorPaths::discover()`, probe-read the file once; on EACCES set a `rapl_available=false`
flag and (a) hide the watts field in the HUD CPU row, (b) use `amdgpu` hwmon `power1_input` (user-readable,
verified present) for an "iGPU W" row instead. No sysctl changes needed (do **not** chmod the RAPL file).

### F4. `LIBVA_DRIVER_NAME=radeonsi` forced process-wide causes a verified VA failure on the NVIDIA node
The env var is set globally, so any VA display GStreamer creates for the **NVIDIA** DRM node tries to load
radeonsi on it and fails — observed in the wild:
`VA error: radeonsi_drv_video.so init failed / vaInitialize: resource allocation failed (vadisplaydrm1)`.
The AMD decode path is unaffected (HW decode verified working), but it also means that if you ever launch
with `EXPLORER_ALLOW_DGPU=1` and expect NVDEC/libva-nvidia-driver, it will be broken.

**Recipe:** delete the `LIBVA_DRIVER_NAME` block in `apply_startup_env()` — Mesa VA auto-selection was
verified to pick `radeonsi` correctly by PCI ID on this system without forcing (this machine's
`LIBVA_DRIVERS_PATH=/usr/lib64/dri-freeworld:/usr/lib64/dri` comes from your session env, not the code, and
auto-selection resolved cleanly). If you keep it, document the NVDEC conflict.

### F5. dGPU transient wake at launch (verified)
Right after launch the UI process opened 5 fds on the NVIDIA card (`pci id for fd …: 10de:25a0` in the log)
and `runtime_status` read `active` once, returning to `suspended` afterwards. Each launch costs one RTD3
wake cycle (~2–4 J + fan tick).

**Recipe:** the `/opt/velox-webkit` patch already skips `0x10de` in WebKit's `DisplayVBlankMonitorDRM`;
the remaining opens come from GDK's DRM monitor enumeration. Verify which component opens the node with
`sudo fuser -v /dev/dri/card0` while holding the browser before window map. If it is GDK, guard with
`GDK_DISABLE_DMONITOR`? (not available) — the practical mitigation is a systemd user drop-in that pre-warms
nothing; accept the transient wake, or add the skip to GDK's `gdk.wayland` monitor probe in a downstream
patch. Cost is small; treat as low priority.

### F6. Throttling may only re-nice the main thread (per-task semantics)
`set_process_ioprio()` calls `setpriority(PRIO_PROCESS, pid, …)` and `ioprio_set(IOPRIO_WHO_PROCESS, pid)`.
On Linux, **nice and ioprio are per-task (per-thread)**; applying them to a TGID affects the group-leader
thread, while WebKit's busy render/compositor threads keep their original priority.
`VERIFY` with: `nice -n -5 explorer &` → background a tab → `ps -Lo nice,comm -p <WebKitWebProc>` — if the
`Compositor`/`WebCore` threads show nice 0 while the leader shows 12, the bug is real.

**Recipe:** iterate `/proc/<pid>/task/*` and apply both calls per TID. Keep the `is_webkit_web_process(pid)`
guard. Add a unit-test using the existing `decide_pid_transitions` harness (logic unchanged).

### F7. `ProcessSwapOnCrossSiteNavigation=false` tradeoff
All tabs share one WebProcess per view: lower memory (your PSS numbers prove it) but a single renderer
crash can take down every tab in that process and site-isolation is off. **Deliberate choice — document it**
in TECHNICAL.md rather than change it; if crash-isolation matters later, flip the feature on battery-free
sessions only and re-measure PSS.

### F8. Content blocker can break OTT login/payment flows
Rules block `googletagmanager.com/gtm.js`, `segment.io`, `segment.com`, `amplitude.com`, `hotjar.com`.
Several Indian OTT apps (Zee5/JioHotstar web logins, payment SDKs) pull Segment/GTM as part of their auth
funnel. **Recipe:** add narrow exception rules *above* the block rules, e.g. WebKit content-blocker
`{"trigger":{"url-filter":".*","if-domain":["*zeenews.*","*jiohotstar.*","*zee5.*"],…}` with
`action.ignore` for those domains, or split the list into "trackers" vs "auth-adjacent" and only enable the
second on non-media sites. WebKit DFA supports `if-domain`/`unless-domain` (no `|` alternation — per your
own FINDINGS note).

### F9. Session file written synchronously on every URI notify
`connect_uri_notify` → `save_session_now()` does a tmp-write + rename on **every** redirect/navigation.
**Recipe:** debounce: mark dirty in `Cell<bool>`, run a 1 s `glib::timeout_add_local` that flushes if dirty;
also flush on `close-request` (already done). Same for `activate_tab`.

### F10. 2 s telemetry timer runs even on battery
Verified reading ~15 sysfs files + `/proc/stat` every 2 s. **Recipe:** re-arm the `timeout_add_local` with
2 s on AC and 5 s when `supervisor.is_battery_mode()` (return `ControlFlow::Continue` after re-adding with
the new interval). The pill already diffs before redraw, so nothing changes visually.

### F11. Background-tab throttle keys on audio only
`media_pages` is populated from `is_playing_audio`. A muted/background video tab is throttle-eligible.
Acceptable today (throttle only applies in battery mode and the *active* tab is exempt), but for
backgrounded PiP-style playback the popup tab is active, so no regression. **No change needed — document.**

### F12. `last_hz` staleness after manual display changes
If the user changes the mode in GNOME Settings, the browser won't re-apply its target until the next AC/battery
transition (cached `last_hz`). **Recipe:** subscribe to Mutter's `MonitorsChanged` D-Bus signal and reset
`last_hz` to `None`, letting the next `sync_system_power()` re-evaluate.

---

## 5. System-level improvements (manual; nothing was changed)

Each item states the measured current value first.

### S1. Leverage zram more under memory pressure (optional, battery profiles)
- Current: `vm.swappiness=10` while zstd zram compresses **3.79×**.
- Recipe: in `/etc/tuned/profiles/balanced-battery` and `powersave` set `vm.swappiness=100` (keep 10 on AC
  profiles). This lets anon pages migrate into compressed RAM under pressure instead of forcing file-cache
  eviction. Test on your workload: `zramctl` before/after; look for `swappiness`-driven `DATA` growth with
  unchanged UX. If any jank appears, revert to 10. (Their `10` is a deliberate anti-lag choice — weigh it.)

### S2. Disable the NMI watchdog on AC too (micro-win)
- Current: `kernel.nmi_watchdog=1` on AC profiles (0 on battery).
- Recipe: add `kernel.nmi_watchdog=0` to `[sysctl]` of `balanced` and `throughput-performance`. One fewer
  periodic perf-counter interrupt per CPU.

### S3. Switch the iGPU power profile to `VIDEO` during media playback (needs root)
- Current: `pp_power_profile_mode` stays `default/3D_FULL_SCREEN`.
- Recipe: add a tuned `script` hook in the battery profiles:
  ```bash
  # /etc/tuned/profiles/balanced-battery/scripts/battery_media.sh (chmod +x)
  echo 3 > /sys/class/drm/card1/device/pp_power_profile_mode   # 3 = VIDEO
  ```
  and a matching `restore` that writes back `0`. TuneD runs as root, so this works where the browser
  cannot. Verify with `cat /sys/class/drm/card1/device/pp_power_profile_mode` during a video.

### S4. Experiment: Panel Self-Refresh on `powersave` only
- Current: `panel_power_savings=0` in all profiles (deliberate anti-flicker).
- Recipe: try `panel_power_savings=1` **only** in `powersave` (max battery saver) for a week. Expect
  ~0.3–1 W on static content; revert if you see flicker/grey-shift. Your eDP panel is 144 Hz — PSR
  behaviour varies by panel; this is an experiment, not a recommendation.

### S5. Keep `pcie_aspm=force` under review (safety)
- Measured: GPUs + NVMe run `ASPM Disabled` under `balanced` anyway (BIOS-default policy), so `force`
  currently buys nothing on AC; battery uses policy `powersupersave` which does not need `force`.
- Recipe: test one boot with `pcie_aspm=force` **removed**; compare `lspci -vv | grep LnkCtl` and
  battery `powersupersave` behaviour (deep L1 still applies). If identical, drop the parameter — `force`
  can destabilise marginal devices and bypasses firmware blacklists.

### S6. Already-optimal — do not touch
`amd_pstate=active` + EPP mapping, `workqueue.power_efficient=1`, NVIDIA RTD3 `0x02` (verified suspended),
zram zstd sizing, ALPM `med_power_with_dipm`, dirty-writeback batching on battery — all verified correct.

---

## 6. Priority-ordered work plan (browser code, none applied)

| # | Item | Type | Expected effect | Effort |
| :-- | :--- | :--- | :--- | :--- |
| 1 | F6 per-thread nice/ioprio (`VERIFY` first) | stability | correct background-tab throttling on battery → meaningful multi-tab battery win | S |
| 2 | F2 async startup D-Bus | stability/latency | removes worst-case multi-second stall; est. **−100–300 ms** typical cold start | M |
| 3 | F1 remove main-loop pump | stability | eliminates reentrancy class of bugs at startup | S |
| 4 | F3 RAPL fallback / hide | correctness | HUD watts stop being silently dead | S |
| 5 | F9 session-write debounce | I/O | fewer writes under redirects | S |
| 6 | F10 telemetry 2s→5s on battery | battery | small wakeup reduction | S |
| 7 | F4 drop forced `LIBVA_DRIVER_NAME` | correctness | kills the verified NVIDIA-node VA error; unblocks future NVDEC opt-in | S |
| 8 | F12 `MonitorsChanged` re-sync | UX | refresh switch heals after user mode changes | S |
| 9 | F8 auth-path blocker exceptions | compatibility | avoids OTT login breakage | M |
| 10 | §3 DRM handoff affordance | product | clear path for Netflix/Hotstar/Zee users | M |

After each change: `cargo test && cargo build --release` (0 warnings expected), re-run the Section 2
playback A/B, and confirm the dGPU stays `suspended` with
`cat /sys/bus/pci/devices/0000:01:00.0/power/runtime_status`.

---

## 7. Reproduction commands (all verified in this session)

```bash
# Engine capabilities of the exact build Explorer links
GDK_BACKEND=wayland LD_LIBRARY_PATH=/opt/velox-webkit/lib64 \
  gjs -c 'imports.gi.versions={WebKit:"6.0"};const s=imports.gi.WebKit.Settings.new();print("EME check via page probe; see §3")'

# VA hardware profiles
vainfo --display wayland 2>/dev/null | grep -E "H264|HEVC|VP9|AV1"

# GStreamer rank env parsing (symbolic)
GST_PLUGIN_FEATURE_RANK="dav1ddec:NONE" gst-inspect-1.0 dav1ddec | grep Rank

# PPD hold support
busctl introspect net.hadess.PowerProfiles /net/hadess/PowerProfiles | grep HoldProfile

# Display modes
busctl --user call org.gnome.Mutter.DisplayConfig /org/gnome/Mutter/DisplayConfig \
  org.gnome.Mutter.DisplayConfig GetCurrentState   # 144.005 / 60.031 on eDP-1

# dGPU state during browsing
cat /sys/bus/pci/devices/0000:01:00.0/power/runtime_status

# Package power (root)
sudo cat /sys/class/powercap/intel-rapl:0/energy_uj   # delta / seconds = watts

# Per-thread nice of a throttled web process (F6 verify)
ps -Lo nice,comm -p $(pgrep WebKitWebProc)
```

---

## 8. Test-harness postmortem (what the tests had to overcome)

For future benchmarking on this machine, three harness pitfalls were identified and solved during this
session — they are *not* browser bugs:

1. `python3 -m http.server` does not implement HTTP **Range** → MP4 moov-seek fails → WebKit reports
   `MEDIA_ERR_SRC_NOT_SUPPORTED` (err 4). Use a Range-capable server and correct `Content-Type` per file.
2. `videotestsrc → x264enc` defaults to **High 4:4:4 10-bit**, which no VCN decoder supports — always
   pin `video/x-raw,format=YV12` before the encoder.
3. GTK freezes the frame clock for occluded Wayland windows, pausing video — confirm playback with
   `/proc/<pid>/stat` `utime` jiffie deltas (100 jiffies = 1 s), not `ps cputime` (1 s granularity).
