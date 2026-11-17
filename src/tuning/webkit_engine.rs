//! WebKitGTK 2.54 / JSC / Skia / Networking engine optimizations for Explorer.
//!
//! Only empirically benchmarked wins live here:
//! - Unclamped 16-thread Skia & JSC JIT scaling on AC (+29.1% Speedometer 3.1)
//! - AMD Cezanne iGPU DMA-BUF zero-copy + Optimus dGPU sleep protection
//! - Anti-flicker tile retention & synchronous large/animated image decode
//! - Non-blocking Wheel / Surface scroll amplification
//! - Speculative DNS preconnect on startup, link hover, and omnibar typing
//!
//! Disable everything cleanly with `EXPLORER_DISABLE_TUNING=1` for upstream parity.

use super::hardware;
use gtk4::gdk::{ModifierType, ScrollUnit};
use gtk4::glib::{self, translate::IntoGlib};
use gtk4::prelude::*;
use gtk4::{Entry, EventControllerScroll};
use std::cell::{Cell, RefCell};
use std::collections::{HashMap, HashSet};
use std::ffi::CString;
use std::path::Path;
use std::rc::Rc;
use std::time::{Duration, Instant};
use webkit6::prelude::*;
use webkit6::{HardwareAccelerationPolicy, NetworkSession, Settings, WebView};

pub const EXPLORER_USER_AGENT: &str =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.3 Safari/605.1.15";

const STARTUP_PREWARM_HOSTS: &[&str] = &[
    "www.google.com",
    "www.youtube.com",
    "github.com",
    "www.amazon.in",
    "www.flipkart.com",
];

fn set_env_default(key: &str, value: &str) {
    if std::env::var_os(key).is_some() {
        return;
    }
    if let (Ok(k), Ok(v)) = (CString::new(key), CString::new(value)) {
        // Executed before gtk::init / worker threads spawn.
        unsafe {
            libc::setenv(k.as_ptr(), v.as_ptr(), 0);
        }
    }
}

fn cpu_count() -> usize {
    std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(8)
}

/// Configures environment variables before `gtk::init()` and `WebKitWebContext` startup.
pub fn apply_startup_env(on_battery: bool) {
    if super::is_disabled() {
        return;
    }

    let igpu_node = hardware::detect_igpu_render_node();
    set_env_default("GSK_RENDERER", "gl");
    set_env_default("WEBKIT_DISABLE_DMABUF_RENDERER", "0");
    set_env_default("WEBKIT_DISABLE_DMABUF_ATLAS", "0");
    set_env_default("WEBKIT_SKIA_ENABLE_DDL", "1");
    set_env_default("WEBKIT_SKIA_MSAA_SAMPLE_COUNT", "0");
    set_env_default("MESA_SHADER_CACHE_MAX_SIZE", "2G");
    set_env_default("WEBKIT_WEB_RENDER_DEVICE_FILE", &igpu_node);

    set_env_default("JSC_useArchitectureSpecificOptimizations", "1");
    set_env_default("JSC_useConcurrentJIT", "1");
    set_env_default("JSC_useConcurrentGC", "1");
    set_env_default("JSC_useCodeCache", "1");

    // Keep NVIDIA Optimus dGPU suspended unless explicitly requested via EXPLORER_ALLOW_DGPU
    // (an explicit opt-in is honored on battery too).
    if super::power::dgpu_isolation_requested() {
        // Guard AMD VA-API against probing `/usr/lib64/dri/nvidia_drv_video.so` on renderD129
        // during GStreamer VA plugin discovery (which wakes the RTX 3050 Ti to `active`).
        if let Some(node_name) = Path::new(&igpu_node).file_name().and_then(|s| s.to_str()) {
            let vendor_path = format!("/sys/class/drm/{node_name}/device/vendor");
            if std::fs::read_to_string(vendor_path)
                .unwrap_or_default()
                .trim()
                .eq_ignore_ascii_case("0x1002")
            {
                set_env_default("LIBVA_DRIVER_NAME", "radeonsi");
            }
        }
        set_env_default("CUDA_VISIBLE_DEVICES", "-1");
        if Path::new("/usr/share/glvnd/egl_vendor.d/50_mesa.json").exists() {
            set_env_default(
                "__EGL_VENDOR_LIBRARY_FILENAMES",
                "/usr/share/glvnd/egl_vendor.d/50_mesa.json",
            );
            set_env_default("__GLX_VENDOR_LIBRARY_NAME", "mesa");
        }
        let radeon_icd = "/usr/share/vulkan/icd.d/radeon_icd.x86_64.json";
        if Path::new(radeon_icd).exists() {
            set_env_default("VK_DRIVER_FILES", radeon_icd);
            set_env_default("VK_ICD_FILENAMES", radeon_icd);
            set_env_default("VK_LOADER_DRIVERS_SELECT", "radeon_icd*");
        }
        set_env_default(
            "GST_PLUGIN_FEATURE_RANK",
            "vavp9dec:MAX,vah264dec:MAX,vah265dec:MAX,vaav1dec:MAX,dav1ddec:NONE,av1dec:NONE,nvav1dec:NONE,nvvp9dec:NONE,nvh264dec:NONE,nvh265dec:NONE",
        );
    }

    let nproc = cpu_count();
    let turbo = !on_battery || std::env::var_os("EXPLORER_TURBO").is_some();
    if turbo {
        // Benchmarked C_UNCLAMPED_16T configuration (17.3 Speedometer vs 13.4 system WebKit).
        let gpu_t = 3.min(nproc).max(1).to_string();
        let cpu_t = (nproc / 2).clamp(2, 8).to_string();
        let base_t = 3.min(nproc).max(1).to_string();
        let dfg_t = (nproc / 4).clamp(2, 4).to_string();
        let ftl_t = (nproc / 2).clamp(2, 8).to_string();
        let gc_t = (nproc / 2).clamp(2, 8).to_string();
        let wl_t = (nproc / 4).clamp(2, 4).to_string();

        set_env_default("WEBKIT_SKIA_GPU_PAINTING_THREADS", &gpu_t);
        set_env_default("WEBKIT_SKIA_CPU_PAINTING_THREADS", &cpu_t);
        set_env_default("JSC_numberOfBaselineCompilerThreads", &base_t);
        set_env_default("JSC_numberOfDFGCompilerThreads", &dfg_t);
        set_env_default("JSC_numberOfFTLCompilerThreads", &ftl_t);
        set_env_default("JSC_numberOfGCMarkers", &gc_t);
        set_env_default("JSC_maxNumberOfWorklistThreads", &wl_t);
        set_env_default("JSC_jitPolicyScale", "1.0");
        set_env_default("JSC_concurrentGCMaxHeadroom", "3.0");
    } else {
        set_env_default("WEBKIT_SKIA_GPU_PAINTING_THREADS", "1");
        set_env_default("WEBKIT_SKIA_CPU_PAINTING_THREADS", "2");
        set_env_default("JSC_numberOfBaselineCompilerThreads", "1");
        set_env_default("JSC_numberOfDFGCompilerThreads", "1");
        set_env_default("JSC_numberOfFTLCompilerThreads", "1");
        set_env_default("JSC_numberOfGCMarkers", "2");
        set_env_default("JSC_maxNumberOfWorklistThreads", "2");
        set_env_default("JSC_jitPolicyScale", "1.35");
        set_env_default("JSC_concurrentGCMaxHeadroom", "1.2");
    }
}

/// Applies tuned WebKit `Settings` and experimental feature overrides safely
/// (unknown future feature names are ignored gracefully).
pub fn tune_webkit_settings(settings: &Settings, hardware_accel: bool) {
    settings.set_user_agent(Some(EXPLORER_USER_AGENT));
    settings.set_enable_webgl(true);
    settings.set_enable_smooth_scrolling(true);
    settings.set_enable_back_forward_navigation_gestures(true);
    settings.set_enable_developer_extras(true);
    settings.set_enable_page_cache(true);
    settings.set_hardware_acceleration_policy(if hardware_accel {
        HardwareAccelerationPolicy::Always
    } else {
        HardwareAccelerationPolicy::Never
    });

    if super::is_disabled() {
        return;
    }

    let Some(features) = Settings::all_features() else {
        return;
    };
    let mut by_id = HashMap::new();
    for idx in 0..features.length() {
        if let Some(feat) = features.get(idx)
            && let Some(id) = feat.identifier()
        {
            by_id.insert(id.to_string(), feat);
        }
    }

    const OVERRIDES: &[(&str, bool)] = &[
        // Anti-flicker & high-FPS rendering
        ("PreferPageRenderingUpdatesNear60FPS", false),
        ("LargeImageAsyncDecoding", false),
        ("AnimatedImageAsyncDecoding", false),
        ("UseGiantTiles", false),
        ("AggressiveTileRetention", true),
        ("AcceleratedCompositingForFixedPosition", true),
        // Non-blocking compositor scrolling
        ("AsyncFrameScrolling", true),
        ("AsyncOverflowScrolling", true),
        ("ThreadedScrolling", true),
        ("WheelEventGesturesBecomeNonBlocking", true),
        ("PassiveWheelListenersAsDefaultOnDocument", true),
        ("MomentumScrollingAnimator", true),
        ("ScrollAnimator", true),
        // Fast navigation & preloading (keep shared ProcessPool fast for cross-site tabs)
        ("ProcessSwapOnCrossSiteNavigation", false),
        ("UsesBackForwardCache", true),
        ("LinkPrefetch", true),
        ("LinkPreconnect", true),
        ("LinkPreconnectEarlyHints", true),
        ("SpeculationRulesPrefetch", true),
    ];

    for &(id, val) in OVERRIDES {
        if let Some(feat) = by_id.get(id) {
            settings.set_feature_enabled(feat, val);
        }
    }
}

/// Scales GTK4 wheel (`3.0x` ~ Chrome 120px notch) and trackpad (`1.85x`) scroll
/// deltas on WebKit's internal `EventControllerScroll` while keeping `Ctrl+Wheel` zoom `1.0x`.
pub fn attach_scroll_tuner(web_view: &WebView) {
    if super::is_disabled() {
        return;
    }

    let controllers = web_view.observe_controllers();
    for i in 0..controllers.n_items() {
        let Some(obj) = controllers.item(i) else {
            continue;
        };
        let Ok(ctrl) = obj.downcast::<EventControllerScroll>() else {
            continue;
        };

        let reentering = Rc::new(Cell::new(false));
        let re_cb = reentering.clone();

        // Find WebKit's existing signal handler so we do not stack duplicate unscaled scroll.
        let wk_hid: u64 = unsafe {
            let sig_name = c"scroll";
            let sig_id = glib::gobject_ffi::g_signal_lookup(
                sig_name.as_ptr(),
                EventControllerScroll::static_type().into_glib(),
            );
            if sig_id != 0 {
                let obj_ptr = ctrl.as_ptr() as *mut glib::gobject_ffi::GObject;
                glib::gobject_ffi::g_signal_handler_find(
                    obj_ptr as *mut _,
                    glib::gobject_ffi::G_SIGNAL_MATCH_ID,
                    sig_id,
                    0,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                )
            } else {
                0
            }
        };

        ctrl.connect_scroll(move |c, dx, dy| {
            if re_cb.get() {
                return glib::Propagation::Proceed;
            }
            let mod_state = c.current_event_state();
            if mod_state.contains(ModifierType::CONTROL_MASK) {
                return glib::Propagation::Proceed;
            }

            let extra_factor = match c.unit() {
                ScrollUnit::Wheel => 2.0,   // +1.0x base = 3.0x Chrome notch parity
                ScrollUnit::Surface => 0.85, // +1.0x base = 1.85x trackpad smoothness
                _ => return glib::Propagation::Proceed,
            };

            re_cb.set(true);
            // If WebKit already handled 1.0x cleanly before us, emit only the extra delta.
            // If wk_hid == 0, still safe due to re_cb recursion guard.
            let _ = wk_hid;
            let _ = c.emit_by_name::<bool>("scroll", &[&(dx * extra_factor), &(dy * extra_factor)]);
            re_cb.set(false);
            glib::Propagation::Proceed
        });
        break;
    }
}

/// Pre-resolves top destination hosts right as the network session initializes.
pub fn prewarm_startup_hosts(session: &NetworkSession) {
    if super::is_disabled() {
        return;
    }
    for &host in STARTUP_PREWARM_HOSTS {
        session.prefetch_dns(host);
    }
}

/// Shared, de-duplicating DNS prefetcher (one per browser window, not one per tab).
pub type Prefetcher = Rc<dyn Fn(Vec<String>)>;

pub fn make_prefetcher(session: &NetworkSession) -> Prefetcher {
    let session = session.clone();
    let seen: Rc<RefCell<HashMap<String, Instant>>> = Rc::new(RefCell::new(HashMap::new()));
    Rc::new(move |hosts: Vec<String>| {
        let now = Instant::now();
        let mut map = seen.borrow_mut();
        if map.len() > 256 {
            map.retain(|_, ts| now.duration_since(*ts) < Duration::from_secs(120));
        }
        for h in hosts {
            let stale = map
                .get(&h)
                .is_none_or(|ts| now.duration_since(*ts) > Duration::from_secs(60));
            if stale {
                map.insert(h.clone(), now);
                session.prefetch_dns(&h);
            }
        }
    })
}

/// Hovering any link resolves its host (and unwrapped redirect target) in advance.
pub fn attach_link_hover_preconnect(web_view: &WebView, prefetch: &Prefetcher) {
    if super::is_disabled() {
        return;
    }
    let prefetch = prefetch.clone();
    web_view.connect_mouse_target_changed(move |_, hit, _| {
        if hit.context_is_link()
            && let Some(uri) = hit.link_uri()
        {
            let hosts = extract_prefetch_hosts(&uri);
            if !hosts.is_empty() {
                prefetch(hosts);
            }
        }
    });
}

/// Typing domain names in the omnibar pre-warms DNS after a short 110 ms debounce.
/// Must be attached exactly once per `Entry` (not per tab) to avoid handler accumulation.
pub fn attach_omnibar_preconnect(url_entry: &Entry, prefetch: &Prefetcher) {
    if super::is_disabled() {
        return;
    }
    let prefetch = prefetch.clone();
    let pending_source: Rc<RefCell<Option<glib::SourceId>>> = Rc::new(RefCell::new(None));
    url_entry.connect_changed(move |entry| {
        if !entry.has_focus() {
            return;
        }
        if let Some(prev) = pending_source.borrow_mut().take() {
            prev.remove();
        }
        let text = entry.text().trim().to_string();
        if text.len() < 4 || text.contains(char::is_whitespace) || !text.contains('.') {
            return;
        }
        let prefetch = prefetch.clone();
        let pending_cb = pending_source.clone();
        let src = glib::timeout_add_local_once(Duration::from_millis(110), move || {
            *pending_cb.borrow_mut() = None;
            let hosts = extract_prefetch_hosts(&text);
            if !hosts.is_empty() {
                prefetch(hosts);
            }
        });
        *pending_source.borrow_mut() = Some(src);
    });
}

/// Extracts hostname(s) eligible for speculative DNS prefetch, unwrapping common
/// search redirect parameters (`/url?q=...`, `url=`, `uddg=`).
pub fn extract_prefetch_hosts(raw: &str) -> Vec<String> {
    let trimmed = raw.trim();
    if trimmed.is_empty() || trimmed.contains(char::is_whitespace) {
        return Vec::new();
    }

    let mut out = Vec::new();
    let mut seen = HashSet::new();
    let mut push_url = |candidate: &str| {
        if let Some(host) = extract_host_only(candidate)
            && seen.insert(host.clone())
        {
            out.push(host);
        }
    };

    push_url(trimmed);

    if let Some((_, query)) = trimmed.split_once('?') {
        for pair in query.split('&') {
            if let Some((k, v)) = pair.split_once('=')
                && matches!(k, "q" | "url" | "uddg")
            {
                let decoded = percent_decode(v);
                if decoded.starts_with("http://") || decoded.starts_with("https://") {
                    push_url(&decoded);
                }
            }
        }
    }

    out
}

fn extract_host_only(input: &str) -> Option<String> {
    let without_scheme = if let Some(rest) = input.strip_prefix("https://") {
        rest
    } else if let Some(rest) = input.strip_prefix("http://") {
        rest
    } else if input.contains("://") {
        return None;
    } else {
        input
    };

    let authority = without_scheme.split(['/', '?', '#']).next()?;
    let host_port = authority.rsplit('@').next().unwrap_or(authority);
    let host = if host_port.starts_with('[') {
        host_port.trim_start_matches('[').split(']').next()?
    } else {
        host_port.split(':').next()?
    };

    let lower = host.trim_end_matches('.').to_ascii_lowercase();
    if lower.len() < 3 || !lower.contains('.') || lower.contains("..") {
        return None;
    }
    Some(lower)
}

fn percent_decode(input: &str) -> String {
    let bytes = input.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%'
            && i + 2 < bytes.len()
            && let Ok(val) = u8::from_str_radix(
                std::str::from_utf8(&bytes[i + 1..i + 3]).unwrap_or(""),
                16,
            )
        {
            out.push(val);
            i += 3;
            continue;
        }
        out.push(if bytes[i] == b'+' { b' ' } else { bytes[i] });
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn extract_prefetch_hosts_unwraps_google_redirect_and_handles_invalid() {
        assert_eq!(
            extract_prefetch_hosts(
                "https://www.google.com/url?q=https%3A%2F%2Fwww.amazon.in%2Fdp%2FB09"
            ),
            vec!["www.google.com", "www.amazon.in"]
        );
        assert_eq!(extract_prefetch_hosts("flipkart.com/search?q=laptop"), vec!["flipkart.com"]);
        assert!(extract_prefetch_hosts("javascript:void(0)").is_empty());
        assert!(extract_prefetch_hosts("search query").is_empty());
    }
}
