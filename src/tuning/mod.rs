//! Isolated hardware, power, content-blocker, and engine optimization layer.
//!
//! Keeping all Fedora 45 / AMD 5800H / Optimus / WebKitGTK tuning isolated here
//! allows seamless upstream UI/browser upgrades and instant bypass via
//! `EXPLORER_DISABLE_TUNING=1`.

pub mod content_blocker;
pub mod hardware;
pub mod power;
pub mod webkit_engine;

use gtk4::Entry;
use std::path::Path;
use webkit6::{NetworkSession, Settings, UserContentManager, WebView};

pub use hardware::{detect_igpu_render_node, is_on_battery_boot, probe_va_capabilities};
pub use power::PowerSupervisor;
pub use webkit_engine::{Prefetcher, EXPLORER_USER_AGENT};

/// Returns `true` when the user opted out of hardware/engine optimizations.
pub fn is_disabled() -> bool {
    std::env::var_os("EXPLORER_DISABLE_TUNING").is_some_and(|v| !v.is_empty() && v != "0")
}

/// Facade for initializing and attaching all validated engine optimizations.
pub struct EngineTuning;

impl EngineTuning {
    /// Sets verified WebKit/JSC/Skia/DRM environment flags before GTK init.
    pub fn prepare_process_env(on_battery: bool) {
        webkit_engine::apply_startup_env(on_battery);
    }

    pub fn tune_settings(settings: &Settings, hardware_accel: bool) {
        webkit_engine::tune_webkit_settings(settings, hardware_accel);
    }

    pub fn build_content_manager(
        filter_dir: &Path,
        hw_only_video: bool,
        battery_mode: bool,
    ) -> UserContentManager {
        content_blocker::build_user_content_manager(filter_dir, hw_only_video, battery_mode)
    }

    /// Per-tab hooks (scroll scaling + link-hover DNS prefetch).
    pub fn attach_webview(web_view: &WebView, prefetch: &Prefetcher) {
        webkit_engine::attach_scroll_tuner(web_view);
        webkit_engine::attach_link_hover_preconnect(web_view, prefetch);
    }

    /// Window-level omnibar DNS prefetch hook; call once per `Entry`.
    pub fn attach_omnibar(url_entry: &Entry, prefetch: &Prefetcher) {
        webkit_engine::attach_omnibar_preconnect(url_entry, prefetch);
    }

    pub fn make_prefetcher(session: &NetworkSession) -> Prefetcher {
        webkit_engine::make_prefetcher(session)
    }

    pub fn prewarm_network(session: &NetworkSession) {
        webkit_engine::prewarm_startup_hosts(session);
    }
}
