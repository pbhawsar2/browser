// Explorer v2 — Main Entry Point
// Wires SOLID modules: HAL -> Policy -> Brave Shield -> AES-256-GCM Vault -> WebApp Installer
// -> CEF 158 Alloy Engine -> Frameless WebUI Chrome & Pure Chromeless App Mode.

#include "explorer/app/browser_window.hpp"
#include "explorer/engine/cef_flags.hpp"
#include "explorer/hal/display_refresh.hpp"
#include "explorer/hal/gpu_isolator.hpp"
#include "explorer/hal/power_governor.hpp"
#include "explorer/hal/process_throttler.hpp"
#include "explorer/hal/telemetry.hpp"
#include "explorer/hal/vaapi_prober.hpp"
#include "explorer/policy/media_policy.hpp"
#include "explorer/policy/navigation_policy.hpp"
#include "explorer/shield/content_shield.hpp"
#include "explorer/vault/password_vault.hpp"
#include "explorer/webapp/webapp_installer.hpp"

#include "include/cef_app.h"
#include "include/cef_browser_process_handler.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"
#include "include/wrapper/cef_helpers.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

std::filesystem::path defaultProfileDir() {
    if (const char* env = std::getenv("EXPLORER_PROFILE_DIR"); env && env[0]) {
        return std::filesystem::path(env);
    }
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && xdg[0]) {
        return std::filesystem::path(xdg) / "explorer";
    }
    if (const char* home = std::getenv("HOME"); home && home[0]) {
        return std::filesystem::path(home) / ".local" / "share" / "explorer";
    }
    return "/tmp/explorer-profile";
}

double configuredDefaultZoom() {
    if (const char* z = std::getenv("EXPLORER_DEFAULT_ZOOM"); z && z[0]) {
        try {
            double v = std::stod(z);
            if (v >= 0.5 && v <= 2.5) return v;
        } catch (...) {
        }
    }
    return explorer::policy::kDefaultPageZoomFactor;
}

/// V8 native bridge installed ONLY on trusted Explorer Chrome & HUD contexts.
class ChromeHostV8Handler : public CefV8Handler {
public:
    explicit ChromeHostV8Handler(CefRefPtr<CefBrowser> browser)
        : browser_(std::move(browser)) {}

    bool Execute(const CefString& name,
                 CefRefPtr<CefV8Value> /*object*/,
                 const CefV8ValueList& arguments,
                 CefRefPtr<CefV8Value>& retval,
                 CefString& /*exception*/) override {
        if (name != "post" || arguments.empty() || !browser_ || !browser_->GetMainFrame()) {
            return false;
        }
        std::string cmd = arguments[0]->GetStringValue().ToString();
        std::string arg = (arguments.size() >= 2 && arguments[1]->IsString())
                              ? arguments[1]->GetStringValue().ToString()
                              : "";
        auto msg = CefProcessMessage::Create("explorer.chrome-cmd");
        msg->GetArgumentList()->SetString(0, cmd);
        msg->GetArgumentList()->SetString(1, arg);
        browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        retval = CefV8Value::CreateBool(true);
        return true;
    }

private:
    CefRefPtr<CefBrowser> browser_;
    IMPLEMENT_REFCOUNTING(ChromeHostV8Handler);
};

/// Minimal, restricted V8 callback on web content ONLY for scroll-reveal top bar & login capture
/// (zero access to Chrome commands, filesystem, vault reads, or hardware controls!).
class ContentPageBridgeHandler : public CefV8Handler {
public:
    explicit ContentPageBridgeHandler(CefRefPtr<CefBrowser> browser)
        : browser_(std::move(browser)) {}

    bool Execute(const CefString& name,
                 CefRefPtr<CefV8Value> /*object*/,
                 const CefV8ValueList& arguments,
                 CefRefPtr<CefV8Value>& retval,
                 CefString& /*exception*/) override {
        if (!browser_ || !browser_->GetMainFrame()) return false;
        if (name == "__expTopbarReveal" && arguments.size() == 1 && arguments[0]->IsBool()) {
            auto msg = CefProcessMessage::Create("explorer.topbar-reveal");
            msg->GetArgumentList()->SetBool(0, arguments[0]->GetBoolValue());
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
            retval = CefV8Value::CreateBool(true);
            return true;
        }
        if (name == "__expMediaState" && arguments.size() == 1 && arguments[0]->IsBool()) {
            auto msg = CefProcessMessage::Create("explorer.media-state");
            msg->GetArgumentList()->SetBool(0, arguments[0]->GetBoolValue());
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
            retval = CefV8Value::CreateBool(true);
            return true;
        }
        if (name == "__expLoginCapture" && arguments.size() >= 2 &&
            arguments[0]->IsString() && arguments[1]->IsString()) {
            // Security: NEVER trust origin from untrusted page JS; read browser-verified frame URL!
            std::string frame_url = browser_->GetMainFrame()->GetURL().ToString();
            if (!explorer::policy::isSecureWebOrigin(frame_url)) {
                return false;
            }
            auto msg = CefProcessMessage::Create("explorer.login-capture");
            msg->GetArgumentList()->SetString(0, frame_url);
            msg->GetArgumentList()->SetString(1, arguments[0]->GetStringValue());
            msg->GetArgumentList()->SetString(2, arguments[1]->GetStringValue());
            browser_->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
            retval = CefV8Value::CreateBool(true);
            return true;
        }
        return false;
    }

private:
    CefRefPtr<CefBrowser> browser_;
    IMPLEMENT_REFCOUNTING(ContentPageBridgeHandler);
};

class ExplorerCefApp : public CefApp,
                       public CefBrowserProcessHandler,
                       public CefRenderProcessHandler {
public:
    ExplorerCefApp(explorer::app::WindowLaunchConfig launch_cfg,
                   explorer::app::RuntimeServices services,
                   explorer::policy::MediaCodecPolicy media_policy)
        : launch_cfg_(std::move(launch_cfg)),
          services_(std::move(services)),
          media_policy_(std::move(media_policy)) {}

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    void OnBeforeCommandLineProcessing(const CefString& /*process_type*/,
                                       CefRefPtr<CefCommandLine> cmd) override {
        std::string wm_class = launch_cfg_.app_id.empty() ? "explorer" : launch_cfg_.app_id;
        explorer::engine::applyHardwareEngineFlags(cmd, services_.hw, media_policy_, wm_class);
    }

    void OnContextInitialized() override {
        CEF_REQUIRE_UI_THREAD();
        window_ = std::make_unique<explorer::app::BrowserWindow>(launch_cfg_, services_);
        window_->show();
    }

    bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command_line,
                                     const CefString& /*current_directory*/) override {
        CEF_REQUIRE_UI_THREAD();
        if (!window_) return false;
        std::vector<std::string> urls;
        if (command_line) {
            CefCommandLine::ArgumentList args;
            command_line->GetArguments(args);
            for (const auto& a : args) {
                std::string s = a.ToString();
                if (!s.empty() && s[0] != '-') urls.push_back(s);
            }
        }
        if (!urls.empty()) window_->openExternalUrls(urls);
        else window_->activateWindow();
        return true;
    }

    void OnBrowserCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefDictionaryValue> extra_info) override {
        if (!browser) return;
        if (extra_info && extra_info->HasKey("is_explorer_chrome") &&
            extra_info->GetBool("is_explorer_chrome")) {
            trusted_chrome_ids_.insert(browser->GetIdentifier());
            return;
        }
        if (browser->GetMainFrame()) {
            auto msg = CefProcessMessage::Create("explorer.renderer-pid");
            msg->GetArgumentList()->SetInt(0, static_cast<int>(::getpid()));
            browser->GetMainFrame()->SendProcessMessage(PID_BROWSER, msg);
        }
    }

    void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override {
        if (browser) trusted_chrome_ids_.erase(browser->GetIdentifier());
    }

    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override {
        if (!browser || !frame || !context) return;

        // 1. Trusted Chrome/HUD WebUI: expose window.explorerHost.post(cmd, arg).
        if (trusted_chrome_ids_.count(browser->GetIdentifier()) != 0) {
            if (frame->IsMain() &&
                frame->GetURL().ToString().rfind("data:text/html;charset=utf-8;base64,", 0) == 0) {
                CefRefPtr<CefV8Value> global = context->GetGlobal();
                CefRefPtr<CefV8Value> host = CefV8Value::CreateObject(nullptr, nullptr);
                CefRefPtr<CefV8Handler> handler = new ChromeHostV8Handler(browser);
                host->SetValue("post", CefV8Value::CreateFunction("post", handler),
                               V8_PROPERTY_ATTRIBUTE_READONLY);
                global->SetValue("explorerHost", host, V8_PROPERTY_ATTRIBUTE_READONLY);
            }
            return;
        }

        // 2. Untrusted Web Content: NEVER expose explorerHost!
        //    Register write-only passive hooks for scroll auto-hide, media playback & login-form save prompt.
        if (frame->IsMain()) {
            CefRefPtr<CefV8Value> global = context->GetGlobal();
            CefRefPtr<CefV8Handler> cb = new ContentPageBridgeHandler(browser);
            const auto attr = static_cast<CefV8Value::PropertyAttribute>(
                V8_PROPERTY_ATTRIBUTE_DONTENUM | V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTDELETE);
            global->SetValue("__expTopbarReveal",
                             CefV8Value::CreateFunction("__expTopbarReveal", cb), attr);
            global->SetValue("__expMediaState",
                             CefV8Value::CreateFunction("__expMediaState", cb), attr);
            global->SetValue("__expLoginCapture",
                             CefV8Value::CreateFunction("__expLoginCapture", cb), attr);

            static const char* kPageUxHooksJs = R"JS((() => {
                if (window.__explorerUxWired) return;
                window.__explorerUxWired = true;
                let lastY = window.scrollY || 0;
                let hidden = false;
                let ticking = false;
                let lastToggleTs = 0;
                const setBar = (reveal) => {
                    if (hidden === !reveal) return;
                    if (!reveal) {
                        const docH = document.documentElement ? document.documentElement.scrollHeight : 0;
                        if (docH < window.innerHeight + 450) return;
                    }
                    const now = performance.now();
                    if (!reveal && (now - lastToggleTs) < 350) return;
                    lastToggleTs = now;
                    hidden = !reveal;
                    try { if (typeof __expTopbarReveal === 'function') __expTopbarReveal(!!reveal); } catch {}
                };
                window.addEventListener('scroll', () => {
                    if (ticking) return;
                    ticking = true;
                    requestAnimationFrame(() => {
                        ticking = false;
                        const y = window.scrollY || 0;
                        const dy = y - lastY;
                        if (y < 28 || dy < -24) setBar(true);
                        else if (y > 110 && dy > 28) setBar(false);
                        lastY = y;
                    });
                }, { passive: true });
                window.addEventListener('mousemove', (e) => {
                    if (hidden && e.clientY <= 8) setBar(true);
                }, { passive: true });
                let lastMediaPlaying = false;
                const syncMedia = () => {
                    const els = document.querySelectorAll('video, audio');
                    let playing = false;
                    for (let i = 0; i < els.length; ++i) {
                        if (!els[i].paused && !els[i].ended && els[i].readyState > 1) {
                            playing = true;
                            break;
                        }
                    }
                    if (playing !== lastMediaPlaying) {
                        lastMediaPlaying = playing;
                        try { if (typeof __expMediaState === 'function') __expMediaState(playing); } catch {}
                    }
                };
                document.addEventListener('play', syncMedia, true);
                document.addEventListener('playing', syncMedia, true);
                document.addEventListener('pause', syncMedia, true);
                document.addEventListener('ended', syncMedia, true);
                document.addEventListener('emptied', syncMedia, true);
                document.addEventListener('submit', (e) => {
                    const form = e.target;
                    if (!form || !form.querySelector) return;
                    const pw = form.querySelector('input[type="password"]');
                    if (!pw || !pw.value) return;
                    const usr = form.querySelector('input[type="email"], input[type="text"], input[autocomplete="username"]');
                    if (usr && usr.value && location.origin && location.origin.startsWith('http')) {
                        try { __expLoginCapture(usr.value.trim(), pw.value); } catch {}
                    }
                }, true);
            })();)JS";
            frame->ExecuteJavaScript(kPageUxHooksJs, "explorer://ux-hooks", 0);
        }

        // 3. Hardware AV1 gatekeeper (forces VCN VP9 on Cezanne instead of CPU dav1d).
        if (services_.hw.tuning_disabled || !media_policy_.shouldDisableSoftwareAv1()) {
            return;
        }
        static const char* kAv1GatekeeperJs = R"JS((() => {
            if (window.__explorerAv1Gated) return;
            window.__explorerAv1Gated = true;
            const isAv1 = (m) => !!m && (String(m).toLowerCase().includes('av01') || String(m).toLowerCase().includes('av1'));
            if (window.MediaSource && typeof MediaSource.isTypeSupported === 'function') {
                const orig = MediaSource.isTypeSupported.bind(MediaSource);
                MediaSource.isTypeSupported = (m) => isAv1(m) ? false : orig(m);
            }
            if (typeof HTMLMediaElement !== 'undefined' && HTMLMediaElement.prototype.canPlayType) {
                const orig = HTMLMediaElement.prototype.canPlayType;
                HTMLMediaElement.prototype.canPlayType = function(m) { return isAv1(m) ? '' : orig.call(this, m); };
            }
            if (navigator.mediaCapabilities && typeof navigator.mediaCapabilities.decodingInfo === 'function') {
                const orig = navigator.mediaCapabilities.decodingInfo.bind(navigator.mediaCapabilities);
                navigator.mediaCapabilities.decodingInfo = async (cfg) => {
                    const ct = cfg && cfg.video && cfg.video.contentType;
                    if (isAv1(ct)) return { supported: false, smooth: false, powerEfficient: false };
                    return orig(cfg);
                };
            }
        })();)JS";
        frame->ExecuteJavaScript(kAv1GatekeeperJs, "explorer://hw-codec-gatekeeper", 0);
    }

private:
    explorer::app::WindowLaunchConfig launch_cfg_;
    explorer::app::RuntimeServices services_;
    explorer::policy::MediaCodecPolicy media_policy_;
    std::unique_ptr<explorer::app::BrowserWindow> window_;
    std::unordered_set<int> trusted_chrome_ids_;

    IMPLEMENT_REFCOUNTING(ExplorerCefApp);
};

}  // namespace

int main(int argc, char* argv[]) {
    bool is_subprocess = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]).rfind("--type=", 0) == 0) {
            is_subprocess = true;
            break;
        }
    }

    // 1. Hardware Abstraction Layer (before CEF spawns GPU/renderer processes).
    explorer::hal::AmdCezanneGpuIsolator gpu_iso;
    explorer::hal::LibvaDrmProber va_prober;
    auto power_gov = std::make_shared<explorer::hal::LinuxPowerGovernor>();

    explorer::hal::HardwareProfile hw;
    hw.tuning_disabled = explorer::hal::isTuningDisabled();
    hw.allow_dgpu = (std::getenv("EXPLORER_ALLOW_DGPU") != nullptr);
    auto [node, vendor] = gpu_iso.discoverIgpuRenderNode();
    hw.igpu_render_node = node;
    hw.igpu_vendor = vendor;
    gpu_iso.applyIsolationEnv(vendor, hw.allow_dgpu);

    if (!is_subprocess) {
        hw.on_battery = power_gov->isOnBatteryBoot();
        hw.va_caps = va_prober.probe(node);
        bool turbo = !hw.on_battery || (std::getenv("EXPLORER_TURBO") != nullptr);
        power_gov->applyTimerSlackNs(turbo ? 1UL : 2'000'000UL);
        ::setenv("EXPLORER_ON_BATTERY", hw.on_battery ? "1" : "0", 1);
        ::setenv("EXPLORER_TURBO_SLACK", turbo ? "1" : "0", 1);
        ::setenv("EXPLORER_VA_AV1", hw.va_caps.av1 ? "1" : "0", 1);
    } else {
        const char* on_batt = std::getenv("EXPLORER_ON_BATTERY");
        hw.on_battery = (on_batt != nullptr && on_batt[0] == '1');
        const char* va_av1 = std::getenv("EXPLORER_VA_AV1");
        hw.va_caps.av1 = (va_av1 != nullptr && va_av1[0] == '1');
        const char* ts = std::getenv("EXPLORER_TURBO_SLACK");
        bool turbo = (ts != nullptr && ts[0] == '1');
        power_gov->applyTimerSlackNs(turbo ? 1UL : 2'000'000UL);
    }

    explorer::policy::MediaCodecPolicy media_policy(hw.va_caps, hw.allow_dgpu);

    // 2. Parse CLI arguments.
    explorer::app::WindowLaunchConfig launch_cfg;
    launch_cfg.profile_dir = defaultProfileDir();
    launch_cfg.default_zoom = configuredDefaultZoom();
    launch_cfg.disable_session = (std::getenv("EXPLORER_NO_SESSION") != nullptr);
    int remote_debug_port = 0;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a.rfind("--app=", 0) == 0) {
            launch_cfg.app_mode_url = a.substr(6);
        } else if (a == "--app" && i + 1 < argc) {
            launch_cfg.app_mode_url = argv[++i];
        } else if (a.rfind("--app-id=", 0) == 0) {
            launch_cfg.app_id = a.substr(9);
        } else if (a == "--app-id" && i + 1 < argc) {
            launch_cfg.app_id = argv[++i];
        } else if (a.rfind("--profile=", 0) == 0) {
            launch_cfg.profile_dir = a.substr(10);
        } else if (a.rfind("--remote-debugging-port=", 0) == 0) {
            try { remote_debug_port = std::stoi(a.substr(24)); } catch (...) {}
        } else if (a == "--no-session") {
            launch_cfg.disable_session = true;
        } else if (!a.empty() && a[0] != '-') {
            launch_cfg.initial_urls.push_back(a);
        }
    }

    std::filesystem::path base_profile_dir = launch_cfg.profile_dir;
    // Installed WebApps get their own isolated cache/profile subdir under profile_dir/webapps/<app_id>
    // Sanitize --app-id strictly to prevent path traversal (e.g. --app-id=../../evil).
    if (!launch_cfg.app_mode_url.empty()) {
        std::string sub;
        for (unsigned char c : launch_cfg.app_id) {
            if (std::isalnum(c) || c == '-' || c == '_') {
                sub.push_back(static_cast<char>(c));
            } else if (c == '.' && !sub.empty() && sub.back() != '.') {
                sub.push_back('.');
            }
            if (sub.size() >= 80) break;
        }
        while (!sub.empty() && sub.back() == '.') sub.pop_back();
        if (sub.empty()) sub = "default-app";
        launch_cfg.app_id = sub;
        launch_cfg.profile_dir = base_profile_dir / "webapps" / sub;
    }

    // Tell GNOME Wayland / XDG desktop shell which .desktop launcher owns this window
    std::string desktop_id = (launch_cfg.app_id.empty() ? "explorer" : launch_cfg.app_id) + ".desktop";
    ::setenv("CHROME_DESKTOP", desktop_id.c_str(), 1);

    explorer::app::RuntimeServices services;
    services.hw = hw;
    if (!is_subprocess) {
        services.power_gov = power_gov;
        services.display_ctrl = std::make_shared<explorer::hal::MutterDisplayRefreshCtrl>();
        services.throttler = std::make_shared<explorer::hal::LinuxProcessThrottler>();
        services.telemetry = std::make_shared<explorer::hal::LinuxTelemetryMonitor>();
        services.shield = std::make_shared<explorer::shield::BraveAdblockShield>(
            base_profile_dir / "filters", true);
        services.vault = std::make_shared<explorer::vault::AesGcmPasswordVault>(base_profile_dir);
        const char* home = std::getenv("HOME");
        if (home && home[0]) {
            std::filesystem::path h(home);
            std::filesystem::path data_home = h / ".local/share";
            if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && xdg[0]) {
                data_home = xdg;
            }
            services.webapp_installer = std::make_shared<explorer::webapp::LinuxWebAppInstaller>(
                data_home / "applications",
                data_home / "icons/hicolor",
                h / ".local/bin/explorer");
        }
    }

    CefMainArgs main_args(argc, argv);
    CefRefPtr<ExplorerCefApp> app(new ExplorerCefApp(launch_cfg, services, media_policy));

    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0) {
        return exit_code;
    }

    std::error_code ec;
    std::filesystem::create_directories(launch_cfg.profile_dir, ec);

    CefSettings settings;
    // Enable Chromium's full multi-process user-namespace + seccomp-bpf sandbox by default!
    settings.no_sandbox = (std::getenv("EXPLORER_NO_SANDBOX") != nullptr);
    settings.remote_debugging_port = remote_debug_port;
    settings.log_severity = LOGSEVERITY_WARNING;
    CefString(&settings.root_cache_path).FromString(launch_cfg.profile_dir.string());

    if (!CefInitialize(main_args, settings, app, nullptr)) {
        std::cerr << "Explorer: CefInitialize failed\n";
        return 1;
    }

    CefRunMessageLoop();
    CefShutdown();
    return 0;
}
