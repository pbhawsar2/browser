//! Adaptive AC / Battery power supervisor for Explorer.
//!
//! Coordinates timer slack, `power-profiles-daemon` AC Turbo hold, eDP display
//! refresh scaling (60 Hz battery/cinema vs 144 Hz AC), per-tab background
//! nice+ioprio throttling, and GStreamer hardware video decoder policy updates.

use super::hardware::{
    self, SensorPaths, TelemetrySnapshot, VaCapabilities,
};
use gtk4::glib::variant::ToVariant;
use std::cell::{Cell, RefCell};
use std::collections::{HashMap, HashSet};
use std::fs;
use std::rc::Rc;
use webkit6::{UserMessage, WebContext};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PowerMode {
    BatteryZeroFan,
    AcTurbo,
}

impl PowerMode {
    pub fn pill_badge(self) -> &'static str {
        match self {
            PowerMode::BatteryZeroFan => "Zero-Fan",
            PowerMode::AcTurbo => "AC Turbo",
        }
    }
}

/// Formats the compact header pill (`Zero-Fan · 6.2W · 0 RPM` on battery; `AC Turbo · 0 RPM` on AC).
/// Wattage (`W`) is intentionally shown on battery power only.
pub fn format_pill_text(mode: PowerMode, t: &TelemetrySnapshot) -> String {
    let mut parts = vec![mode.pill_badge().to_string()];
    if t.on_battery
        && let Some(w) = t.batt_watts.or(t.pkg_watts)
    {
        parts.push(format!("{w:.1}W"));
    }
    if let Some(rpm) = t.fan_rpm {
        parts.push(format!("{rpm} RPM"));
    }
    parts.join(" · ")
}

#[derive(Debug, Clone)]
pub struct PowerSupervisor {
    inner: Rc<SupervisorInner>,
}

/// Effective power policy: a manual HUD choice wins, otherwise follow the power source.
pub fn effective_battery_mode(on_battery: bool, manual_turbo: Option<bool>) -> bool {
    match manual_turbo {
        Some(turbo) => !turbo,
        None => on_battery,
    }
}

/// The discrete GPU stays isolated unless the user explicitly opts in via `EXPLORER_ALLOW_DGPU`.
pub fn dgpu_isolation_requested() -> bool {
    std::env::var_os("EXPLORER_ALLOW_DGPU").is_none()
}

#[derive(Debug)]
struct SupervisorInner {
    on_battery: Cell<bool>,
    cinema_60hz: Cell<bool>,
    hw_only_video: Cell<bool>,
    manual_turbo: Cell<Option<bool>>,
    ppd_cookie: Cell<Option<u32>>,
    ppd_pending: Cell<bool>,
    last_hz: Cell<Option<u32>>,
    sensors: RefCell<SensorPaths>,
    telemetry: RefCell<TelemetrySnapshot>,
    va_caps: VaCapabilities,
    tab_pids: RefCell<HashMap<u64, TabPidInfo>>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TabPidInfo {
    pub pid: i32,
    pub throttled: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PidAction {
    Throttle(i32),
    Unthrottle(i32),
}

impl PowerSupervisor {
    pub fn new(on_battery: bool, va_caps: VaCapabilities) -> Self {
        let mut sensors = SensorPaths::discover();
        let telemetry = sensors.sample();
        // Only block AV1 when this GPU cannot hardware-decode it (software dav1d is a battery/fan hog).
        let hw_only_video = !va_caps.av1;
        Self {
            inner: Rc::new(SupervisorInner {
                on_battery: Cell::new(on_battery),
                cinema_60hz: Cell::new(false),
                hw_only_video: Cell::new(hw_only_video),
                manual_turbo: Cell::new(None),
                ppd_cookie: Cell::new(None),
                ppd_pending: Cell::new(false),
                last_hz: Cell::new(None),
                sensors: RefCell::new(sensors),
                telemetry: RefCell::new(telemetry),
                va_caps,
                tab_pids: RefCell::new(HashMap::new()),
            }),
        }
    }

    pub fn is_battery_mode(&self) -> bool {
        effective_battery_mode(self.inner.on_battery.get(), self.inner.manual_turbo.get())
    }

    pub fn on_battery_raw(&self) -> bool {
        self.inner.on_battery.get()
    }

    pub fn mode(&self) -> PowerMode {
        if self.is_battery_mode() {
            PowerMode::BatteryZeroFan
        } else {
            PowerMode::AcTurbo
        }
    }

    pub fn cinema_60hz(&self) -> bool {
        self.inner.cinema_60hz.get()
    }

    pub fn hw_only_video(&self) -> bool {
        self.inner.hw_only_video.get()
    }

    /// Manual HUD override: `Some(true)` forces Turbo, `Some(false)` forces battery-saver, `None` follows the power source.
    pub fn manual_turbo(&self) -> Option<bool> {
        self.inner.manual_turbo.get()
    }

    pub fn va_caps(&self) -> &VaCapabilities {
        &self.inner.va_caps
    }

    /// Records a power-source change; a manual HUD override never outlives the source it was set on.
    fn note_power_source(&self, on_batt: bool) {
        if self.inner.on_battery.get() != on_batt {
            self.inner.manual_turbo.set(None);
        }
        self.inner.on_battery.set(on_batt);
    }

    pub fn set_on_battery(&self, on_batt: bool) {
        self.note_power_source(on_batt);
        self.sync_system_power();
    }

    pub fn set_cinema_60hz(&self, enabled: bool) {
        self.inner.cinema_60hz.set(enabled);
        self.sync_system_power();
    }

    pub fn set_hw_only_video(&self, enabled: bool, web_context: &WebContext) {
        self.inner.hw_only_video.set(enabled);
        self.push_extension_policy(web_context);
    }

    /// HUD "AC Turbo Performance Hold" switch: works in both directions on AC and on battery.
    pub fn set_ac_turbo_override(&self, enabled: bool) {
        self.inner.manual_turbo.set(Some(enabled));
        self.sync_system_power();
    }

    pub fn telemetry(&self) -> TelemetrySnapshot {
        self.inner.telemetry.borrow().clone()
    }

    pub fn refresh_telemetry(&self) -> TelemetrySnapshot {
        let snap = self.inner.sensors.borrow_mut().sample();
        if snap.on_battery != self.inner.on_battery.get() {
            self.note_power_source(snap.on_battery);
            self.sync_system_power();
        }
        *self.inner.telemetry.borrow_mut() = snap.clone();
        snap
    }

    /// Formats the compact header status pill string (`"Zero-Fan · 6.2W · 0 RPM"` on battery, `"AC Turbo · 0 RPM"` on AC).
    pub fn pill_text(&self) -> String {
        let t = self.inner.telemetry.borrow();
        format_pill_text(self.mode(), &t)
    }

    /// Applies lightweight kernel timer slack synchronously without D-Bus I/O.
    pub fn apply_timer_slack(&self) {
        if super::is_disabled() {
            return;
        }
        hardware::apply_timer_slack_ns(if self.is_battery_mode() { 2_000_000 } else { 50_000 });
    }

    pub fn invalidate_last_hz(&self) {
        self.inner.last_hz.set(None);
    }

    /// Synchronizes timer slack, `power-profiles-daemon` performance hold,
    /// and eDP display refresh rate asynchronously with current power state.
    pub fn sync_system_power(&self) {
        if super::is_disabled() {
            return;
        }
        let battery_mode = self.is_battery_mode();
        self.apply_timer_slack();

        // Hold PPD `"performance"` on AC Turbo only; release cleanly on battery.
        let want_ppd_perf = !battery_mode;
        match (want_ppd_perf, self.inner.ppd_cookie.get(), self.inner.ppd_pending.get()) {
            (true, None, false) => {
                self.inner.ppd_pending.set(true);
                let sup = self.clone();
                hardware::hold_performance_profile_async(move |cookie_opt| {
                    sup.inner.ppd_pending.set(false);
                    if let Some(cookie) = cookie_opt {
                        if !sup.is_battery_mode() {
                            sup.inner.ppd_cookie.set(Some(cookie));
                        } else {
                            hardware::release_performance_profile_async(cookie);
                        }
                    }
                });
            }
            (false, Some(cookie), _) => {
                self.inner.ppd_cookie.set(None);
                hardware::release_performance_profile_async(cookie);
            }
            _ => {}
        }

        // Switch eDP refresh rate cleanly only when target Hz actually changes.
        if std::env::var_os("EXPLORER_NO_REFRESH_SWITCH").is_none() {
            let target_hz: u32 = if battery_mode || self.inner.cinema_60hz.get() {
                60
            } else {
                144
            };
            if self.inner.last_hz.get() != Some(target_hz) {
                self.inner.last_hz.set(Some(target_hz));
                hardware::set_edp_refresh_rate_async(target_hz as f64);
            }
        }
    }

    /// Broadcasts active GStreamer video decode policy to all WebProcess extensions.
    pub fn push_extension_policy(&self, web_context: &WebContext) {
        if super::is_disabled() {
            return;
        }
        let block_dgpu = dgpu_isolation_requested();
        let params = (self.inner.hw_only_video.get(), block_dgpu).to_variant();
        web_context.set_web_process_extensions_initialization_user_data(&params);
        let msg = UserMessage::new("explorer.power-policy", Some(&params));
        web_context.send_message_to_all_extensions(&msg);
    }

    pub fn register_page_pid(&self, page_id: u64, pid: i32) {
        if pid > 1 && is_webkit_web_process(pid) {
            self.inner
                .tab_pids
                .borrow_mut()
                .insert(page_id, TabPidInfo { pid, throttled: false });
        }
    }

    pub fn unregister_page(&self, page_id: u64) {
        self.inner.tab_pids.borrow_mut().remove(&page_id);
    }

    /// Applies nice+ioprio throttling to inactive background tabs (skipping tabs
    /// playing audio/video or sharing a PID with an active/media tab).
    pub fn reconcile_tab_priorities(
        &self,
        active_page_ids: &HashSet<u64>,
        media_page_ids: &HashSet<u64>,
    ) {
        if super::is_disabled() {
            return;
        }
        let battery_mode = self.is_battery_mode();
        let mut map = self.inner.tab_pids.borrow_mut();
        let actions = decide_pid_transitions(&mut map, active_page_ids, media_page_ids, battery_mode);
        for action in actions {
            match action {
                PidAction::Throttle(pid) => hardware::set_process_ioprio(pid, true),
                PidAction::Unthrottle(pid) => hardware::set_process_ioprio(pid, false),
            }
        }
    }

    pub fn release_all(&self) {
        if let Some(cookie) = self.inner.ppd_cookie.take() {
            hardware::release_performance_profile_async(cookie);
        }
        let mut map = self.inner.tab_pids.borrow_mut();
        for info in map.values_mut() {
            if info.throttled {
                hardware::set_process_ioprio(info.pid, false);
                info.throttled = false;
            }
        }
    }
}

/// Computes minimal safe priority transitions per unique WebKitWebProcess PID.
/// A shared PID stays unthrottled if *any* active or media page runs inside it.
pub fn decide_pid_transitions(
    map: &mut HashMap<u64, TabPidInfo>,
    active_pages: &HashSet<u64>,
    media_pages: &HashSet<u64>,
    battery_mode: bool,
) -> Vec<PidAction> {
    let mut keep_awake_pids = HashSet::new();
    for (&page_id, info) in map.iter() {
        if !battery_mode || active_pages.contains(&page_id) || media_pages.contains(&page_id) {
            keep_awake_pids.insert(info.pid);
        }
    }

    let mut actions = Vec::new();
    let mut handled_pids = HashSet::new();
    for info in map.values_mut() {
        let want_throttled = !keep_awake_pids.contains(&info.pid);
        if info.throttled != want_throttled {
            info.throttled = want_throttled;
            if handled_pids.insert(info.pid) {
                actions.push(if want_throttled {
                    PidAction::Throttle(info.pid)
                } else {
                    PidAction::Unthrottle(info.pid)
                });
            }
        }
    }
    actions
}

fn is_webkit_web_process(pid: i32) -> bool {
    fs::read_to_string(format!("/proc/{pid}/comm"))
        .map(|s| s.trim().starts_with("WebKitWebProc"))
        .unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shared_pid_with_active_or_media_tab_never_throttles() {
        let mut map = HashMap::from([
            (1, TabPidInfo { pid: 100, throttled: false }),
            (2, TabPidInfo { pid: 100, throttled: false }),
            (3, TabPidInfo { pid: 200, throttled: false }),
            (4, TabPidInfo { pid: 300, throttled: false }),
        ]);
        let active = HashSet::from([1]);
        let media = HashSet::from([4]);

        let actions = decide_pid_transitions(&mut map, &active, &media, true);
        assert_eq!(actions, vec![PidAction::Throttle(200)]);
        assert!(!map[&1].throttled);
        assert!(!map[&2].throttled);
        assert!(map[&3].throttled);
        assert!(!map[&4].throttled);

        // Switching back to AC unthrottles PID 200 cleanly.
        let unthrottle = decide_pid_transitions(&mut map, &active, &media, false);
        assert_eq!(unthrottle, vec![PidAction::Unthrottle(200)]);
        assert!(!map[&3].throttled);
    }

    #[test]
    fn manual_turbo_switch_works_in_both_directions_on_ac_and_battery() {
        // Follows the power source by default.
        assert!(!effective_battery_mode(false, None));
        assert!(effective_battery_mode(true, None));
        // On AC the HUD switch can now force battery-saver (previously a no-op).
        assert!(effective_battery_mode(false, Some(false)));
        // On battery the HUD switch can still force turbo.
        assert!(!effective_battery_mode(true, Some(true)));
    }

    #[test]
    fn pill_text_hides_refresh_rate_and_shows_watts_on_battery_only() {
        let ac_snap = TelemetrySnapshot {
            on_battery: false,
            pkg_watts: Some(18.4),
            batt_watts: None,
            fan_rpm: Some(0),
            cpu_percent: 6.0,
            cpu_temp_c: Some(60.0),
            igpu_busy: None,
            igpu_mhz: None,
            igpu_temp_c: None,
            igpu_watts: None,
            dgpu_state: None,
        };
        assert_eq!(format_pill_text(PowerMode::AcTurbo, &ac_snap), "AC Turbo · 0 RPM");

        let batt_snap = TelemetrySnapshot {
            on_battery: true,
            pkg_watts: Some(8.1),
            batt_watts: Some(6.2),
            fan_rpm: Some(0),
            ..ac_snap
        };
        assert_eq!(
            format_pill_text(PowerMode::BatteryZeroFan, &batt_snap),
            "Zero-Fan · 6.2W · 0 RPM"
        );
    }
}
