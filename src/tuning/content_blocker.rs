//! Brave-inspired native WebKit DFA tracker blocker, VA-API VP9/H264 media
//! gatekeeper (blocks software AV1 decode), and battery shader CSS injection.

use gtk4::gio;
use std::path::Path;
use webkit6::{
    UserContentFilterStore, UserContentInjectedFrames, UserContentManager, UserScript,
    UserScriptInjectionTime, UserStyleLevel, UserStyleSheet,
};

/// Native WebKit DFA content-blocking ruleset (~55 third-party tracker/ad/beacon
/// endpoints). Cuts page load latency & CPU without breaking first-party apps
/// (Google, YouTube, Amazon, Flipkart, Adobe, Deloitte, GitHub).
const BLOCKER_RULES_JSON: &str = r#"[
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?doubleclick\\.net/.*","resource-type":["script","image","raw","document"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?googlesyndication\\.com/.*","resource-type":["script","image","raw","document"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?googleadservices\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?googletagservices\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?google-analytics\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?googletagmanager\\.com/gtm\\.js.*","resource-type":["script"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://pagead2\\.googlesyndication\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://adservice\\.google\\.[a-z.]+/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?amazon-adsystem\\.com/.*","resource-type":["script","image","raw","document"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://aax-[^/]+\\.amazon\\.[a-z.]+/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://fls-[^/]+\\.amazon\\.[a-z.]+/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://unagi[^/]*\\.amazon\\.[a-z.]+/.*","resource-type":["raw","image"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?scorecardresearch\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?quantserve\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?taboola\\.com/.*","resource-type":["script","image","raw","document"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?outbrain\\.com/.*","resource-type":["script","image","raw","document"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?criteo\\.com/.*","resource-type":["script","image","raw","document"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?criteo\\.net/.*","resource-type":["script","image","raw","document"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?pubmatic\\.com/.*","resource-type":["script","image","raw","document"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?rubiconproject\\.com/.*","resource-type":["script","image","raw","document"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?openx\\.net/.*","resource-type":["script","image","raw","document"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?adnxs\\.com/.*","resource-type":["script","image","raw","document"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?casalemedia\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?adsrvr\\.org/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?rlcdn\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?bidswitch\\.net/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?sharethrough\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?triplelift\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?smartadserver\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?hotjar\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?fullstory\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?clarity\\.ms/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?mixpanel\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?segment\\.io/.*","resource-type":["script","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?segment\\.com/.*","resource-type":["script","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?amplitude\\.com/.*","resource-type":["script","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?chartbeat\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?chartbeat\\.net/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?nr-data\\.net/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?bugsnag\\.com/.*","resource-type":["script","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?demdex\\.net/.*","resource-type":["script","image","raw","document"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?omtrdc\\.net/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?2o7\\.net/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://bat\\.bing\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://connect\\.facebook\\.net/.*/fbevents\\.js.*","resource-type":["script"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://www\\.facebook\\.com/tr/?(\\?.*)?$","resource-type":["image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://px\\.ads\\.linkedin\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://analytics\\.twitter\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://analytics\\.tiktok\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?ads-twitter\\.com/.*","resource-type":["script","image","raw"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?youtube\\.com/pagead.*","resource-type":["raw","image","script"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?youtube\\.com/ptracking.*","resource-type":["raw","image","script"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?youtube\\.com/api/stats/ads.*","resource-type":["raw","image","script"]},"action":{"type":"block"}},
  {"trigger":{"url-filter":"^https?://([^/]+\\.)?youtube\\.com/get_midroll_info.*","resource-type":["raw","image","script"]},"action":{"type":"block"}}
]"#;

/// Forces sites like YouTube to negotiate VA-API HW codecs (VP9 / H.264) when
/// software AV1 would spike CPU & battery/fan power.
const HARDWARE_GATEKEEPER_JS: &str = r#"(() => {
  const initPolicy = window.__explorerPolicy || { hwOnlyVideo: true, batteryMode: true };
  window.__explorerPolicy = initPolicy;

  const syncDom = () => {
    const root = document.documentElement;
    if (root) {
      root.dataset.explorerBattery = window.__explorerPolicy.batteryMode ? '1' : '0';
    }
  };
  syncDom();
  if (!document.documentElement) {
    document.addEventListener('DOMContentLoaded', syncDom, { once: true });
  }

  const isSwOnly = (mime) => {
    if (!window.__explorerPolicy.hwOnlyVideo || !mime) return false;
    const m = String(mime).toLowerCase();
    return m.includes('av01') || m.includes('av1');
  };

  if (window.MediaSource && typeof MediaSource.isTypeSupported === 'function') {
    const origIsType = MediaSource.isTypeSupported.bind(MediaSource);
    MediaSource.isTypeSupported = function(mime) {
      if (isSwOnly(mime)) return false;
      return origIsType(mime);
    };
  }

  if (typeof HTMLMediaElement !== 'undefined' && HTMLMediaElement.prototype.canPlayType) {
    const origCanPlay = HTMLMediaElement.prototype.canPlayType;
    HTMLMediaElement.prototype.canPlayType = function(mime) {
      if (isSwOnly(mime)) return '';
      return origCanPlay.call(this, mime);
    };
  }

  if (navigator.mediaCapabilities && typeof navigator.mediaCapabilities.decodingInfo === 'function') {
    const origDec = navigator.mediaCapabilities.decodingInfo.bind(navigator.mediaCapabilities);
    navigator.mediaCapabilities.decodingInfo = async function(cfg) {
      const ct = cfg && cfg.video && cfg.video.contentType;
      if (isSwOnly(ct)) {
        return { supported: false, smooth: false, powerEfficient: false };
      }
      return origDec(cfg);
    };
  }
})();"#;

/// Removes heavy ambient glow / blur shaders on YouTube when battery mode is active.
const YOUTUBE_BATTERY_CSS: &str = r#"
:root[data-explorer-battery="1"] #cinematics,
:root[data-explorer-battery="1"] ytd-watch-flexy #cinematics-container,
:root[data-explorer-battery="1"] .ytp-ambient-light {
  display: none !important;
  opacity: 0 !important;
}
"#;

pub fn build_user_content_manager(
    filter_dir: &Path,
    hw_only_video: bool,
    battery_mode: bool,
) -> UserContentManager {
    let ucm = UserContentManager::new();
    if super::is_disabled() {
        return ucm;
    }

    let bootstrap = format!(
        "window.__explorerPolicy = {{ hwOnlyVideo: {hw_only_video}, batteryMode: {battery_mode} }};\n{HARDWARE_GATEKEEPER_JS}"
    );
    let script = UserScript::new(
        &bootstrap,
        UserContentInjectedFrames::AllFrames,
        UserScriptInjectionTime::Start,
        &[],
        &[],
    );
    ucm.add_script(&script);

    let css = UserStyleSheet::new(
        YOUTUBE_BATTERY_CSS,
        UserContentInjectedFrames::TopFrame,
        UserStyleLevel::User,
        &["https://*.youtube.com/*"],
        &[],
    );
    ucm.add_style_sheet(&css);

    let _ = std::fs::create_dir_all(filter_dir);
    let store = UserContentFilterStore::new(&filter_dir.to_string_lossy());
    let ucm_clone = ucm.clone();
    let store_save = store.clone();
    const FILTER_ID: &str = "explorer-adblock-v3";

    // Filter callbacks execute asynchronously on the GLib main loop and attach
    // the compiled DFA filter to `UserContentManager` without nested pumping.
    store.load(FILTER_ID, gio::Cancellable::NONE, move |res| match res {
        Ok(filter) => ucm_clone.add_filter(&filter),
        Err(_) => {
            let rules_bytes = gtk4::glib::Bytes::from_static(BLOCKER_RULES_JSON.as_bytes());
            store_save.save(FILTER_ID, &rules_bytes, gio::Cancellable::NONE, move |sres| {
                match sres {
                    Ok(filter) => ucm_clone.add_filter(&filter),
                    Err(e) => eprintln!("Explorer content-blocker compile error: {e}"),
                }
            });
        }
    });

    ucm
}

pub fn policy_update_js(hw_only_video: bool, battery_mode: bool) -> String {
    format!(
        "window.__explorerPolicy = {{ hwOnlyVideo: {hw_only_video}, batteryMode: {battery_mode} }};\
         if (document.documentElement) document.documentElement.dataset.explorerBattery = '{}';",
        if battery_mode { "1" } else { "0" }
    )
}
