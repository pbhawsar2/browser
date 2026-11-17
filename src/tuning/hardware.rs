//! Low-level Linux / AMD Cezanne / Fedora hardware discovery, VA-API probe,
//! power/thermal telemetry, DBus refresh/PPD control, and process I/O tuning.
//!
//! All probes degrade gracefully (`Option`/`Result`) so future Fedora releases
//! or different laptops continue operating without hard failures.

use gtk4::glib::variant::ToVariant;
use gtk4::{gio, glib};
use std::ffi::{c_int, c_ulong, c_void};
use std::fs;
use std::os::unix::io::AsRawFd;
use std::path::{Path, PathBuf};
use std::time::Instant;

type VADisplay = *mut c_void;
type VAStatus = c_int;
type VAProfile = c_int;
type VAEntrypoint = c_int;

const VA_STATUS_SUCCESS: VAStatus = 0;
const VA_ENTRYPOINT_VLD: VAEntrypoint = 1;
const VA_PROFILE_H264_CONSTRAINED_BASELINE: VAProfile = 13;
const VA_PROFILE_H264_MAIN: VAProfile = 6;
const VA_PROFILE_H264_HIGH: VAProfile = 7;
const VA_PROFILE_HEVC_MAIN: VAProfile = 17;
const VA_PROFILE_HEVC_MAIN10: VAProfile = 18;
const VA_PROFILE_VP9_PROFILE0: VAProfile = 19;
const VA_PROFILE_VP9_PROFILE2: VAProfile = 21;
const VA_PROFILE_AV1_PROFILE0: VAProfile = 32;

unsafe extern "C" {
    fn vaGetDisplayDRM(fd: c_int) -> VADisplay;
    fn vaInitialize(dpy: VADisplay, major_version: *mut c_int, minor_version: *mut c_int) -> VAStatus;
    fn vaTerminate(dpy: VADisplay) -> VAStatus;
    fn vaMaxNumProfiles(dpy: VADisplay) -> c_int;
    fn vaMaxNumEntrypoints(dpy: VADisplay) -> c_int;
    fn vaQueryConfigProfiles(
        dpy: VADisplay,
        profile_list: *mut VAProfile,
        num_profiles: *mut c_int,
    ) -> VAStatus;
    fn vaQueryConfigEntrypoints(
        dpy: VADisplay,
        profile: VAProfile,
        entrypoint_list: *mut VAEntrypoint,
        num_entrypoints: *mut c_int,
    ) -> VAStatus;
}

/// Hardware VLD video decode capabilities discovered on the primary iGPU.
#[derive(Debug, Clone, Default)]
pub struct VaCapabilities {
    pub render_node: String,
    pub h264: bool,
    pub hevc: bool,
    pub vp9: bool,
    pub av1: bool,
}

impl VaCapabilities {
    pub fn summary(&self) -> String {
        let mut codecs = Vec::new();
        if self.vp9 {
            codecs.push("VP9");
        }
        if self.h264 {
            codecs.push("H264");
        }
        if self.hevc {
            codecs.push("HEVC");
        }
        if self.av1 {
            codecs.push("AV1");
        }
        if codecs.is_empty() {
            "SW Fallback".to_string()
        } else {
            codecs.join(" · ")
        }
    }
}

/// Discovers the best integrated GPU DRM render node (AMD `0x1002` or Intel `0x8086`),
/// skipping discrete NVIDIA nodes (`0x10de`) unless no iGPU exists.
pub fn detect_igpu_render_node() -> String {
    let mut fallback = None;
    if let Ok(entries) = fs::read_dir("/sys/class/drm") {
        let mut nodes: Vec<PathBuf> = entries
            .filter_map(|e| e.ok().map(|x| x.path()))
            .filter(|p| {
                p.file_name()
                    .and_then(|n| n.to_str())
                    .is_some_and(|n| n.starts_with("renderD"))
            })
            .collect();
        nodes.sort();

        for node in nodes {
            let name = match node.file_name().and_then(|n| n.to_str()) {
                Some(s) => s,
                None => continue,
            };
            let dev_path = format!("/dev/dri/{name}");
            let vendor = fs::read_to_string(node.join("device/vendor"))
                .unwrap_or_default()
                .trim()
                .to_ascii_lowercase();
            if vendor == "0x1002" || vendor == "0x8086" {
                return dev_path;
            }
            if fallback.is_none() && Path::new(&dev_path).exists() {
                fallback = Some(dev_path);
            }
        }
    }
    fallback.unwrap_or_else(|| "/dev/dri/renderD128".to_string())
}

/// Probes libva hardware decode profiles (`VLD` entrypoint) on `render_node`.
pub fn probe_va_capabilities(render_node: &str) -> VaCapabilities {
    let mut caps = VaCapabilities {
        render_node: render_node.to_string(),
        ..Default::default()
    };

    let Ok(file) = fs::OpenOptions::new().read(true).write(true).open(render_node) else {
        return caps;
    };

    unsafe {
        let dpy = vaGetDisplayDRM(file.as_raw_fd());
        if dpy.is_null() {
            return caps;
        }
        let mut major = 0;
        let mut minor = 0;
        if vaInitialize(dpy, &mut major, &mut minor) != VA_STATUS_SUCCESS {
            return caps;
        }

        let max_p = vaMaxNumProfiles(dpy).max(0) as usize;
        let max_e = vaMaxNumEntrypoints(dpy).max(0) as usize;
        if max_p > 0 && max_e > 0 {
            let mut profiles = vec![0 as VAProfile; max_p];
            let mut n_profiles: c_int = 0;
            if vaQueryConfigProfiles(dpy, profiles.as_mut_ptr(), &mut n_profiles)
                == VA_STATUS_SUCCESS
            {
                let mut entrypoints = vec![0 as VAEntrypoint; max_e];
                for &prof in profiles.iter().take(n_profiles.max(0) as usize) {
                    let mut n_ep: c_int = 0;
                    if vaQueryConfigEntrypoints(dpy, prof, entrypoints.as_mut_ptr(), &mut n_ep)
                        != VA_STATUS_SUCCESS
                    {
                        continue;
                    }
                    let has_vld = entrypoints
                        .iter()
                        .take(n_ep.max(0) as usize)
                        .any(|&ep| ep == VA_ENTRYPOINT_VLD);
                    if !has_vld {
                        continue;
                    }
                    match prof {
                        VA_PROFILE_H264_CONSTRAINED_BASELINE
                        | VA_PROFILE_H264_MAIN
                        | VA_PROFILE_H264_HIGH => caps.h264 = true,
                        VA_PROFILE_HEVC_MAIN | VA_PROFILE_HEVC_MAIN10 => caps.hevc = true,
                        VA_PROFILE_VP9_PROFILE0 | VA_PROFILE_VP9_PROFILE2 => caps.vp9 = true,
                        VA_PROFILE_AV1_PROFILE0 => caps.av1 = true,
                        _ => {}
                    }
                }
            }
        }
        vaTerminate(dpy);
    }

    caps
}

/// Fast synchronous boot-time battery detection prior to GObject/D-Bus initialization.
pub fn is_on_battery_boot() -> bool {
    let Ok(entries) = fs::read_dir("/sys/class/power_supply") else {
        return false;
    };
    let mut saw_mains_offline = false;
    let mut saw_discharging = false;

    for entry in entries.flatten() {
        let dir = entry.path();
        let kind = fs::read_to_string(dir.join("type")).unwrap_or_default();
        match kind.trim() {
            "Mains" => {
                if fs::read_to_string(dir.join("online")).unwrap_or_default().trim() == "1" {
                    return false;
                }
                saw_mains_offline = true;
            }
            "Battery" => {
                let status = fs::read_to_string(dir.join("status")).unwrap_or_default();
                if status.trim().eq_ignore_ascii_case("Discharging") {
                    saw_discharging = true;
                }
            }
            _ => {}
        }
    }
    saw_discharging || saw_mains_offline
}

/// Pre-discovered sysfs / proc paths so periodic UI telemetry never scans directories.
#[derive(Debug)]
pub struct SensorPaths {
    ac_online: Option<PathBuf>,
    batt_status: Option<PathBuf>,
    batt_power_uw: Option<PathBuf>,
    batt_volt_uv: Option<PathBuf>,
    batt_curr_ua: Option<PathBuf>,
    rapl_energy_uj: Option<PathBuf>,
    cpu_temp_mc: Option<PathBuf>,
    igpu_busy: Option<PathBuf>,
    igpu_freq_hz: Option<PathBuf>,
    igpu_temp_mc: Option<PathBuf>,
    igpu_power_uw: Option<PathBuf>,
    fan_inputs: Vec<PathBuf>,
    dgpu_runtime_status: Option<PathBuf>,
    prev_rapl: Option<(u64, Instant)>,
    prev_stat: Option<(u64, u64)>,
}

/// Live thermal, power, iGPU, fan, and CPU utilization snapshot.
#[derive(Debug, Clone, Default)]
pub struct TelemetrySnapshot {
    pub on_battery: bool,
    pub cpu_percent: f32,
    pub cpu_temp_c: Option<f32>,
    pub pkg_watts: Option<f32>,
    pub batt_watts: Option<f32>,
    pub igpu_busy: Option<u32>,
    pub igpu_mhz: Option<u32>,
    pub igpu_temp_c: Option<f32>,
    pub igpu_watts: Option<f32>,
    pub fan_rpm: Option<u32>,
    pub dgpu_state: Option<String>,
}

impl SensorPaths {
    pub fn discover() -> Self {
        let mut s = Self {
            ac_online: None,
            batt_status: None,
            batt_power_uw: None,
            batt_volt_uv: None,
            batt_curr_ua: None,
            rapl_energy_uj: readable(PathBuf::from(
                "/sys/class/powercap/intel-rapl:0/energy_uj",
            )),
            cpu_temp_mc: None,
            igpu_busy: None,
            igpu_freq_hz: None,
            igpu_temp_mc: None,
            igpu_power_uw: None,
            fan_inputs: Vec::new(),
            dgpu_runtime_status: None,
            prev_rapl: None,
            prev_stat: None,
        };

        if let Ok(entries) = fs::read_dir("/sys/class/power_supply") {
            for e in entries.flatten() {
                let p = e.path();
                match fs::read_to_string(p.join("type")).unwrap_or_default().trim() {
                    "Mains" if s.ac_online.is_none() => s.ac_online = readable(p.join("online")),
                    "Battery" if s.batt_status.is_none() => {
                        s.batt_status = readable(p.join("status"));
                        s.batt_power_uw = readable(p.join("power_now"));
                        s.batt_volt_uv = readable(p.join("voltage_now"));
                        s.batt_curr_ua = readable(p.join("current_now"));
                    }
                    _ => {}
                }
            }
        }

        if let Ok(entries) = fs::read_dir("/sys/class/hwmon") {
            for e in entries.flatten() {
                let p = e.path();
                let name = fs::read_to_string(p.join("name")).unwrap_or_default();
                let n = name.trim();
                if (n == "k10temp" || n == "coretemp") && s.cpu_temp_mc.is_none() {
                    s.cpu_temp_mc = readable(p.join("temp1_input"));
                } else if n == "amdgpu" || n == "i915" {
                    if s.igpu_temp_mc.is_none() {
                        s.igpu_temp_mc = readable(p.join("temp1_input"));
                    }
                    if s.igpu_freq_hz.is_none() {
                        s.igpu_freq_hz = readable(p.join("freq1_input"));
                    }
                    if s.igpu_power_uw.is_none() {
                        s.igpu_power_uw = readable(p.join("power1_input"));
                    }
                }
                for idx in 1..=4 {
                    if let Some(fan) = readable(p.join(format!("fan{idx}_input"))) {
                        s.fan_inputs.push(fan);
                    }
                }
            }
        }

        // AMD/DRM busy% + NVIDIA PCI runtime PM state
        if let Ok(entries) = fs::read_dir("/sys/class/drm") {
            for e in entries.flatten() {
                let name = e.file_name();
                if !name.to_string_lossy().starts_with("card") || name.to_string_lossy().contains('-') {
                    continue;
                }
                let dev = e.path().join("device");
                let vendor = fs::read_to_string(dev.join("vendor")).unwrap_or_default();
                let v = vendor.trim();
                if v == "0x1002" && s.igpu_busy.is_none() {
                    s.igpu_busy = readable(dev.join("gpu_busy_percent"));
                } else if v == "0x10de" && s.dgpu_runtime_status.is_none() {
                    s.dgpu_runtime_status = readable(dev.join("power/runtime_status"));
                }
            }
        }

        s
    }

    pub fn sample(&mut self) -> TelemetrySnapshot {
        let mut out = TelemetrySnapshot::default();

        let ac_on = self
            .ac_online
            .as_deref()
            .and_then(read_u64)
            .map(|v| v == 1);
        let batt_discharging = self
            .batt_status
            .as_deref()
            .and_then(|p| fs::read_to_string(p).ok())
            .is_some_and(|st| st.trim().eq_ignore_ascii_case("Discharging"));
        out.on_battery = batt_discharging || ac_on == Some(false);

        if let Some(cpu_pct) = self.sample_cpu_percent() {
            out.cpu_percent = cpu_pct;
        }
        out.cpu_temp_c = self.cpu_temp_mc.as_deref().and_then(read_u64).map(|mc| mc as f32 / 1000.0);
        out.igpu_temp_c = self.igpu_temp_mc.as_deref().and_then(read_u64).map(|mc| mc as f32 / 1000.0);
        out.igpu_mhz = self.igpu_freq_hz.as_deref().and_then(read_u64).map(|hz| (hz / 1_000_000) as u32);
        out.igpu_watts = self.igpu_power_uw.as_deref().and_then(read_u64).map(|uw| uw as f32 / 1_000_000.0);
        out.igpu_busy = self.igpu_busy.as_deref().and_then(read_u64).map(|b| b as u32);

        out.pkg_watts = self.sample_rapl_watts();
        out.batt_watts = self.sample_batt_watts();

        let max_fan = self
            .fan_inputs
            .iter()
            .filter_map(|p| read_u64(p))
            .max()
            .map(|v| v as u32);
        out.fan_rpm = max_fan;

        out.dgpu_state = self
            .dgpu_runtime_status
            .as_deref()
            .and_then(|p| fs::read_to_string(p).ok())
            .map(|s| s.trim().to_string());

        out
    }

    fn sample_rapl_watts(&mut self) -> Option<f32> {
        let energy = self.rapl_energy_uj.as_deref().and_then(read_u64)?;
        let now = Instant::now();
        let watts = self.prev_rapl.and_then(|(prev_e, prev_t)| {
            let dt = now.duration_since(prev_t).as_secs_f64();
            if dt > 0.15 && energy >= prev_e {
                Some(((energy - prev_e) as f64 / (dt * 1_000_000.0)) as f32)
            } else {
                None
            }
        });
        self.prev_rapl = Some((energy, now));
        watts
    }

    fn sample_batt_watts(&self) -> Option<f32> {
        if let Some(uw) = self.batt_power_uw.as_deref().and_then(read_u64) {
            return Some(uw as f32 / 1_000_000.0);
        }
        let uv = self.batt_volt_uv.as_deref().and_then(read_u64)? as f64;
        let ua = self.batt_curr_ua.as_deref().and_then(read_u64)? as f64;
        Some(((uv * ua) / 1e12) as f32)
    }

    fn sample_cpu_percent(&mut self) -> Option<f32> {
        let raw = fs::read_to_string("/proc/stat").ok()?;
        let line = raw.lines().next()?;
        let mut nums = line
            .split_whitespace()
            .skip(1)
            .filter_map(|t| t.parse::<u64>().ok());
        let user = nums.next()?;
        let nice = nums.next()?;
        let sys = nums.next()?;
        let idle = nums.next()?;
        let iowait = nums.next().unwrap_or(0);
        let irq = nums.next().unwrap_or(0);
        let softirq = nums.next().unwrap_or(0);
        let steal = nums.next().unwrap_or(0);

        let total = user + nice + sys + idle + iowait + irq + softirq + steal;
        let idle_all = idle + iowait;
        let pct = self.prev_stat.and_then(|(pt, pi)| {
            let dt = total.saturating_sub(pt);
            let di = idle_all.saturating_sub(pi);
            (dt > 0).then(|| ((dt - di) as f32 * 100.0) / dt as f32)
        });
        self.prev_stat = Some((total, idle_all));
        pct
    }
}

fn readable(p: PathBuf) -> Option<PathBuf> {
    fs::File::open(&p).ok().map(|_| p)
}

fn read_u64(p: &Path) -> Option<u64> {
    fs::read_to_string(p).ok()?.trim().parse::<u64>().ok()
}

/// Computes `ApplyMonitorsConfig` parameters if any built-in `eDP-*` panel needs
/// switching to `target_hz`. Returns `None` when already at target or unavailable.
fn build_edp_monitors_params(state: &glib::Variant, target_hz: f64) -> Option<glib::Variant> {
    let serial = state.child_value(0).get::<u32>()?;
    let monitors = state.child_value(1);
    let logical_monitors = state.child_value(2);

    let mut target_mode_by_conn = std::collections::HashMap::<String, String>::new();
    for i in 0..monitors.n_children() {
        let mon = monitors.child_value(i);
        let mon_id = mon.child_value(0);
        let connector = mon_id.child_value(0).get::<String>().unwrap_or_default();
        if !connector.starts_with("eDP") {
            continue;
        }
        let modes = mon.child_value(1);
        let mut cur_w = None;
        let mut cur_h = None;
        let mut best_id: Option<(String, f64, bool)> = None;

        for m in 0..modes.n_children() {
            let mode = modes.child_value(m);
            let w = mode.child_value(1).get::<i32>().unwrap_or(0);
            let h = mode.child_value(2).get::<i32>().unwrap_or(0);
            let hz = mode.child_value(3).get::<f64>().unwrap_or(0.0);
            let props = mode.child_value(6);
            let is_current = props
                .get::<std::collections::HashMap<String, glib::Variant>>()
                .and_then(|d| d.get("is-current").and_then(|v| v.get::<bool>()))
                .unwrap_or(false);
            if is_current {
                cur_w = Some(w);
                cur_h = Some(h);
                if (hz - target_hz).abs() < 2.0 {
                    return None;
                }
            }
        }

        for m in 0..modes.n_children() {
            let mode = modes.child_value(m);
            let w = mode.child_value(1).get::<i32>().unwrap_or(0);
            let h = mode.child_value(2).get::<i32>().unwrap_or(0);
            let hz = mode.child_value(3).get::<f64>().unwrap_or(0.0);
            if let (Some(cw), Some(ch)) = (cur_w, cur_h)
                && (w != cw || h != ch)
            {
                continue;
            }
            let dist = (hz - target_hz).abs();
            let is_cur = mode
                .child_value(6)
                .get::<std::collections::HashMap<String, glib::Variant>>()
                .and_then(|d| d.get("is-current").and_then(|v| v.get::<bool>()))
                .unwrap_or(false);
            if best_id.as_ref().is_none_or(|(_, bd, _)| dist < *bd) {
                let mode_id = mode.child_value(0).get::<String>().unwrap_or_default();
                best_id = Some((mode_id, dist, is_cur));
            }
        }

        if let Some((mode_id, dist, is_cur)) = best_id {
            if is_cur || dist > 20.0 {
                return None;
            }
            target_mode_by_conn.insert(connector, mode_id);
        }
    }

    if target_mode_by_conn.is_empty() {
        return None;
    }

    type LogicalMon = (
        i32,
        i32,
        f64,
        u32,
        bool,
        Vec<(String, String, std::collections::HashMap<String, glib::Variant>)>,
    );
    let mut new_logical: Vec<LogicalMon> = Vec::with_capacity(logical_monitors.n_children());
    for i in 0..logical_monitors.n_children() {
        let lm = logical_monitors.child_value(i);
        let x = lm.child_value(0).get::<i32>().unwrap_or(0);
        let y = lm.child_value(1).get::<i32>().unwrap_or(0);
        let scale = lm.child_value(2).get::<f64>().unwrap_or(1.0);
        let transform = lm.child_value(3).get::<u32>().unwrap_or(0);
        let primary = lm.child_value(4).get::<bool>().unwrap_or(false);
        let mons = lm.child_value(5);

        let mut new_mons = Vec::new();
        for j in 0..mons.n_children() {
            let conn = mons.child_value(j).child_value(0).get::<String>().unwrap_or_default();
            let chosen_mode = if let Some(mid) = target_mode_by_conn.get(&conn) {
                mid.clone()
            } else {
                find_current_mode_id(&monitors, &conn).unwrap_or_default()
            };
            if !chosen_mode.is_empty() {
                new_mons.push((conn, chosen_mode, std::collections::HashMap::new()));
            }
        }
        new_logical.push((x, y, scale, transform, primary, new_mons));
    }

    let method: u32 = 1; // temporary (non-persistent)
    let empty_props: std::collections::HashMap<String, glib::Variant> = std::collections::HashMap::new();
    Some((serial, method, new_logical, empty_props).to_variant())
}

/// Switches the built-in laptop panel (`eDP-*`) to `target_hz` asynchronously via
/// GNOME Mutter DisplayConfig D-Bus, avoiding main-thread startup/toggle stalls.
pub fn set_edp_refresh_rate_async(target_hz: f64) {
    gio::bus_get(gio::BusType::Session, gio::Cancellable::NONE, move |res| {
        let Ok(bus) = res else {
            return;
        };
        let bus_apply = bus.clone();
        bus.call(
            Some("org.gnome.Mutter.DisplayConfig"),
            "/org/gnome/Mutter/DisplayConfig",
            "org.gnome.Mutter.DisplayConfig",
            "GetCurrentState",
            None,
            Some(glib::VariantTy::new(
                "(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})",
            ).unwrap()),
            gio::DBusCallFlags::NONE,
            2000,
            gio::Cancellable::NONE,
            move |state_res| {
                let Ok(state) = state_res else {
                    return;
                };
                let Some(params) = build_edp_monitors_params(&state, target_hz) else {
                    return;
                };
                bus_apply.call(
                    Some("org.gnome.Mutter.DisplayConfig"),
                    "/org/gnome/Mutter/DisplayConfig",
                    "org.gnome.Mutter.DisplayConfig",
                    "ApplyMonitorsConfig",
                    Some(&params),
                    None,
                    gio::DBusCallFlags::NONE,
                    2000,
                    gio::Cancellable::NONE,
                    |_| {},
                );
            },
        );
    });
}

fn find_current_mode_id(monitors: &glib::Variant, conn: &str) -> Option<String> {
    for i in 0..monitors.n_children() {
        let mon = monitors.child_value(i);
        let c = mon.child_value(0).child_value(0).get::<String>()?;
        if c != conn {
            continue;
        }
        let modes = mon.child_value(1);
        for m in 0..modes.n_children() {
            let mode = modes.child_value(m);
            let props = mode
                .child_value(6)
                .get::<std::collections::HashMap<String, glib::Variant>>()?;
            if props.get("is-current").and_then(|v| v.get::<bool>()) == Some(true) {
                return mode.child_value(0).get::<String>();
            }
        }
    }
    None
}

/// Acquires an AC `"performance"` hold asynchronously from `power-profiles-daemon`.
pub fn hold_performance_profile_async(on_cookie: impl FnOnce(Option<u32>) + 'static) {
    let cb = std::cell::RefCell::new(Some(on_cookie));
    gio::bus_get(gio::BusType::System, gio::Cancellable::NONE, move |res| {
        let Ok(bus) = res else {
            if let Some(f) = cb.borrow_mut().take() {
                f(None);
            }
            return;
        };
        let args = ("performance", "Explorer AC Turbo", "io.explorer.Browser").to_variant();
        bus.call(
            Some("net.hadess.PowerProfiles"),
            "/net/hadess/PowerProfiles",
            "net.hadess.PowerProfiles",
            "HoldProfile",
            Some(&args),
            Some(glib::VariantTy::new("(u)").unwrap()),
            gio::DBusCallFlags::NONE,
            1500,
            gio::Cancellable::NONE,
            move |reply_res| {
                let cookie = reply_res.ok().and_then(|r| r.child_value(0).get::<u32>());
                if let Some(f) = cb.borrow_mut().take() {
                    f(cookie);
                }
            },
        );
    });
}

/// Releases a previously held `power-profiles-daemon` cookie asynchronously.
pub fn release_performance_profile_async(cookie: u32) {
    gio::bus_get(gio::BusType::System, gio::Cancellable::NONE, move |res| {
        if let Ok(bus) = res {
            bus.call(
                Some("net.hadess.PowerProfiles"),
                "/net/hadess/PowerProfiles",
                "net.hadess.PowerProfiles",
                "ReleaseProfile",
                Some(&(cookie,).to_variant()),
                None,
                gio::DBusCallFlags::NONE,
                1500,
                gio::Cancellable::NONE,
                |_| {},
            );
        }
    });
}

/// Tunes Linux kernel timer slack (`PR_SET_TIMERSLACK`) on the UI process.
pub fn apply_timer_slack_ns(ns: u64) -> Option<u64> {
    unsafe {
        if libc::prctl(libc::PR_SET_TIMERSLACK, ns as c_ulong, 0, 0, 0) == 0 {
            Some(ns)
        } else {
            None
        }
    }
}

fn apply_tid_throttling(tid: i32, idle_class: bool) {
    const IOPRIO_WHO_PROCESS: libc::c_int = 1;
    const IOPRIO_CLASS_BE: libc::c_int = 2;
    const IOPRIO_CLASS_IDLE: libc::c_int = 3;
    const IOPRIO_CLASS_SHIFT: libc::c_int = 13;

    let ioprio = if idle_class {
        IOPRIO_CLASS_IDLE << IOPRIO_CLASS_SHIFT
    } else {
        (IOPRIO_CLASS_BE << IOPRIO_CLASS_SHIFT) | 4
    };
    // `SCHED_BATCH` <-> `SCHED_OTHER` is reversible without CAP_SYS_NICE, whereas
    // `setpriority(..., 12)` cannot be restored back to 0 under RLIMIT_NICE=0.
    let sched_policy = if idle_class {
        libc::SCHED_BATCH
    } else {
        libc::SCHED_OTHER
    };
    let sched_param = libc::sched_param { sched_priority: 0 };

    unsafe {
        libc::syscall(libc::SYS_ioprio_set, IOPRIO_WHO_PROCESS, tid, ioprio);
        libc::sched_setscheduler(tid, sched_policy, &sched_param);
    }
}

/// Tunes per-thread I/O priority and reversible batch scheduling across every
/// thread in `pid` (`/proc/<pid>/task/*`) without halting IPC event loops.
pub fn set_process_ioprio(pid: i32, idle_class: bool) {
    if pid <= 1 {
        return;
    }
    let task_dir = format!("/proc/{pid}/task");
    let mut applied_any = false;
    if let Ok(entries) = fs::read_dir(&task_dir) {
        for entry in entries.flatten() {
            if let Ok(tid) = entry.file_name().to_string_lossy().parse::<i32>()
                && tid > 1
            {
                apply_tid_throttling(tid, idle_class);
                applied_any = true;
            }
        }
    }
    if !applied_any {
        apply_tid_throttling(pid, idle_class);
    }
}

/// Watches UPower `OnBattery` changes asynchronously on the system D-Bus.
pub fn attach_upower_watcher(on_change: impl Fn(bool) + 'static) {
    gio::bus_get(gio::BusType::System, gio::Cancellable::NONE, move |res| {
        let Ok(bus) = res else {
            return;
        };
        let sub = bus.subscribe_to_signal(
            Some("org.freedesktop.UPower"),
            Some("org.freedesktop.DBus.Properties"),
            Some("PropertiesChanged"),
            Some("/org/freedesktop/UPower"),
            Some("org.freedesktop.UPower"),
            gio::DBusSignalFlags::NONE,
            move |sig| {
                let changed = sig.parameters.child_value(1);
                if let Some(dict) = changed.get::<std::collections::HashMap<String, glib::Variant>>()
                    && let Some(val) = dict.get("OnBattery").and_then(|v| v.get::<bool>())
                {
                    on_change(val);
                }
            },
        );
        // Keep subscription alive for the process lifetime.
        let _ = Box::leak(Box::new((bus, sub)));
    });
}

/// Watches GNOME Mutter `MonitorsChanged` so cached refresh state resets cleanly
/// whenever the user alters display settings manually.
pub fn attach_monitors_changed_watcher(on_changed: impl Fn() + 'static) {
    gio::bus_get(gio::BusType::Session, gio::Cancellable::NONE, move |res| {
        let Ok(bus) = res else {
            return;
        };
        let sub = bus.subscribe_to_signal(
            Some("org.gnome.Mutter.DisplayConfig"),
            Some("org.gnome.Mutter.DisplayConfig"),
            Some("MonitorsChanged"),
            Some("/org/gnome/Mutter/DisplayConfig"),
            None,
            gio::DBusSignalFlags::NONE,
            move |_| on_changed(),
        );
        let _ = Box::leak(Box::new((bus, sub)));
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn per_tid_batch_and_ioprio_throttling_is_reversible_unprivileged() {
        let mut child = match std::process::Command::new("/bin/sleep").arg("2").spawn() {
            Ok(c) => c,
            Err(_) => return,
        };
        let pid = child.id() as i32;
        set_process_ioprio(pid, true);
        unsafe {
            assert_eq!(libc::sched_getscheduler(pid), libc::SCHED_BATCH);
        }
        set_process_ioprio(pid, false);
        unsafe {
            assert_eq!(libc::sched_getscheduler(pid), libc::SCHED_OTHER);
        }
        let _ = child.kill();
        let _ = child.wait();
    }
}
