#include "explorer/app/browser_window.hpp"
#include "explorer/app/chrome_ui.hpp"
#include "explorer/app/start_page.hpp"
#include "explorer/app/theme.hpp"
#include "explorer/policy/navigation_policy.hpp"

#include "include/base/cef_bind.h"
#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_cookie.h"
#include "include/cef_image.h"
#include "include/cef_parser.h"
#include "include/views/cef_box_layout.h"
#include "include/views/cef_fill_layout.h"
#include "include/views/cef_window_delegate.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include <fcntl.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace explorer::app {

namespace {

constexpr int kAccelNewTab = 101;
constexpr int kAccelCloseTab = 102;
constexpr int kAccelNextTab = 103;
constexpr int kAccelPrevTab = 104;
constexpr int kAccelFocusOmnibar = 105;
constexpr int kAccelFind = 106;
constexpr int kAccelReload = 107;
constexpr int kAccelHardReload = 108;
constexpr int kAccelBack = 109;
constexpr int kAccelForward = 110;
constexpr int kAccelZoomIn = 111;
constexpr int kAccelZoomOut = 112;
constexpr int kAccelZoomReset = 113;
constexpr int kAccelFullscreen = 114;
constexpr int kAccelDevTools = 115;
constexpr int kAccelQuit = 116;

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else if (c == 0xE2 && i + 2 < s.size() &&
                           static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                           (static_cast<unsigned char>(s[i + 2]) == 0xA8 ||
                            static_cast<unsigned char>(s[i + 2]) == 0xA9)) {
                    out += (static_cast<unsigned char>(s[i + 2]) == 0xA8) ? "\\u2028" : "\\u2029";
                    i += 2;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

/// Extracts a string value for `key` from a JSON object using Chromium's RFC 8259 parser (CefParseJSON).
std::string extractJsonField(const std::string& json, const std::string& key) {
    CefRefPtr<CefValue> parsed = CefParseJSON(json, JSON_PARSER_RFC);
    if (!parsed || parsed->GetType() != VTYPE_DICTIONARY) return "";
    CefRefPtr<CefDictionaryValue> dict = parsed->GetDictionary();
    if (!dict || !dict->HasKey(key) || dict->GetType(key) != VTYPE_STRING) return "";
    return dict->GetString(key).ToString();
}

void cleanseSecret(std::string& s) {
    volatile char* p = s.data();
    for (size_t i = 0; i < s.size(); ++i) {
        p[i] = '\0';
    }
    s.clear();
}

class GuardedUiTask : public CefTask {
public:
    GuardedUiTask(std::shared_ptr<std::atomic<bool>> token, std::function<void()> fn)
        : token_(std::move(token)), fn_(std::move(fn)) {}

    void Execute() override {
        if (token_ && token_->load() && fn_) {
            fn_();
        }
    }

private:
    std::shared_ptr<std::atomic<bool>> token_;
    std::function<void()> fn_;
    IMPLEMENT_REFCOUNTING(GuardedUiTask);
};

void spawnDetachedXdgOpen(const std::string& target) {
    if (target.empty()) return;
    const char* arg = target.c_str();
    pid_t pid = ::fork();
    if (pid < 0) return;
    if (pid == 0) {
        ::setsid();
        pid_t grandchild = ::fork();
        if (grandchild == 0) {
            ::execlp("xdg-open", "xdg-open", arg, nullptr);
            ::_exit(127);
        }
        ::_exit(0);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

bool runPasswordVerifierHelper(const char* helper_path,
                               const char* const argv[],
                               const char* pass_bytes,
                               size_t pass_len,
                               bool append_newline) {
    if (::access(helper_path, X_OK) != 0) return false;
    int pipefd[2]{-1, -1};
    // O_CLOEXEC prevents leaking pipe FDs into concurrent CEF/curl child processes.
    if (::pipe2(pipefd, O_CLOEXEC) != 0) return false;

    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return false;
    }
    ::posix_spawn_file_actions_adddup2(&actions, pipefd[0], STDIN_FILENO);
    ::posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    ::posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid = -1;
    int rc = ::posix_spawn(&pid, helper_path, &actions, nullptr,
                           const_cast<char* const*>(argv), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(pipefd[0]);
    if (rc != 0 || pid <= 0) {
        ::close(pipefd[1]);
        return false;
    }

    size_t off = 0;
    while (off < pass_len) {
        ssize_t n = ::write(pipefd[1], pass_bytes + off, pass_len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        off += static_cast<size_t>(n);
    }
    char term = append_newline ? '\n' : '\0';
    while (::write(pipefd[1], &term, 1) < 0 && errno == EINTR) {}
    ::close(pipefd[1]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/// Verifies the current Linux user's login/sudo password against PAM (`unix_chkpwd` or `sudo`).
bool verifyLinuxSystemPassword(const std::string& sys_password) {
    if (sys_password.empty()) return false;

    std::string username;
    if (struct passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_name) {
        username = pw->pw_name;
    } else if (const char* env_u = std::getenv("USER"); env_u && env_u[0]) {
        username = env_u;
    }

    const char* chkpwd_bins[] = {"/usr/sbin/unix_chkpwd", "/sbin/unix_chkpwd", "/usr/bin/unix_chkpwd"};
    for (const char* bin : chkpwd_bins) {
        if (::access(bin, X_OK) != 0 || username.empty()) continue;
        const char* argv_user[] = {bin, username.c_str(), "nullok", nullptr};
        return runPasswordVerifierHelper(bin, argv_user, sys_password.data(), sys_password.size(), false);
    }

    if (::access("/usr/bin/sudo", X_OK) == 0) {
        const char* argv_sudo[] = {"/usr/bin/sudo", "-k", "-S", "-p", "", "/usr/bin/true", nullptr};
        return runPasswordVerifierHelper("/usr/bin/sudo", argv_sudo, sys_password.data(), sys_password.size(), true);
    }
    return false;
}

CefRefPtr<CefImage> loadAppIconImage(const std::string& app_id) {
    std::vector<std::filesystem::path> candidates;
    if (const char* home = std::getenv("HOME"); home && home[0]) {
        std::filesystem::path h(home);
        if (!app_id.empty()) {
            candidates.push_back(h / ".local/share/icons/hicolor/256x256/apps" / (app_id + ".png"));
        }
        candidates.push_back(h / ".local/share/icons/hicolor/256x256/apps/io.explorer.Browser.png");
    }
    char exe_buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (n > 0) {
        exe_buf[n] = '\0';
        candidates.push_back(std::filesystem::path(exe_buf).parent_path() / "explorer-icon.png");
    }

    for (const auto& path : candidates) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in.is_open()) continue;
        auto sz = in.tellg();
        if (sz <= 0 || sz > 4 * 1024 * 1024) continue;
        in.seekg(0, std::ios::beg);
        std::vector<char> bytes(static_cast<size_t>(sz));
        if (!in.read(bytes.data(), sz)) continue;
        auto img = CefImage::CreateImage();
        if (img && img->AddPNG(1.0f, bytes.data(), bytes.size())) return img;
    }
    return nullptr;
}

class FramelessWindowDelegate : public CefWindowDelegate {
public:
    FramelessWindowDelegate(BrowserWindow* owner, bool frameless)
        : owner_(owner), frameless_(frameless) {}

    void OnWindowCreated(CefRefPtr<CefWindow> window) override {
        if (owner_) owner_->handleWindowCreated(window);
    }
    void OnWindowDestroyed(CefRefPtr<CefWindow> /*window*/) override {
        if (owner_) owner_->handleWindowDestroyed();
    }
    bool CanClose(CefRefPtr<CefWindow> /*window*/) override {
        return owner_ ? owner_->handleCanClose() : true;
    }
    bool OnAccelerator(CefRefPtr<CefWindow> /*window*/, int command_id) override {
        return owner_ ? owner_->handleAccelerator(command_id) : false;
    }
    bool IsFrameless(CefRefPtr<CefWindow> /*window*/) override { return frameless_; }
    bool CanResize(CefRefPtr<CefWindow> /*window*/) override { return true; }
    bool CanMaximize(CefRefPtr<CefWindow> /*window*/) override { return true; }
    bool CanMinimize(CefRefPtr<CefWindow> /*window*/) override { return true; }
    CefSize GetPreferredSize(CefRefPtr<CefView> /*view*/) override { return CefSize(1280, 820); }
    cef_runtime_style_t GetWindowRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }
    bool GetLinuxWindowProperties(CefRefPtr<CefWindow> /*window*/,
                                  CefLinuxWindowProperties& properties) override {
        std::string app_id = owner_ ? owner_->linuxAppId() : "explorer";
        CefString(&properties.wayland_app_id).FromString(app_id);
        CefString(&properties.wm_class_class).FromString(app_id);
        CefString(&properties.wm_class_name).FromString(app_id);
        return true;
    }

private:
    BrowserWindow* owner_;
    bool frameless_;
    IMPLEMENT_REFCOUNTING(FramelessWindowDelegate);
};

class ContentBrowserViewDelegate : public CefBrowserViewDelegate {
public:
    cef_runtime_style_t GetBrowserRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }

private:
    IMPLEMENT_REFCOUNTING(ContentBrowserViewDelegate);
};

class TopChromeViewDelegate : public CefBrowserViewDelegate {
public:
    explicit TopChromeViewDelegate(BrowserWindow* owner) : owner_(owner) {}

    CefSize GetPreferredSize(CefRefPtr<CefView> /*view*/) override {
        int h = owner_ ? owner_->desiredChromeHeight() : theme::kTopBezelHeight;
        return CefSize(1280, h);
    }
    CefSize GetMinimumSize(CefRefPtr<CefView> /*view*/) override {
        int h = owner_ ? owner_->desiredChromeHeight() : theme::kTopBezelHeight;
        return CefSize(320, h);
    }
    cef_runtime_style_t GetBrowserRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }

private:
    BrowserWindow* owner_;
    IMPLEMENT_REFCOUNTING(TopChromeViewDelegate);
};

class HudOverlayViewDelegate : public CefBrowserViewDelegate {
public:
    CefSize GetPreferredSize(CefRefPtr<CefView> /*view*/) override { return CefSize(488, 580); }
    CefSize GetMinimumSize(CefRefPtr<CefView> /*view*/) override { return CefSize(488, 580); }
    cef_runtime_style_t GetBrowserRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }

private:
    IMPLEMENT_REFCOUNTING(HudOverlayViewDelegate);
};

CefRefPtr<CefDictionaryValue> makeTrustedChromeExtraInfo() {
    auto dict = CefDictionaryValue::Create();
    dict->SetBool("is_explorer_chrome", true);
    return dict;
}

}  // namespace

BrowserWindow::BrowserWindow(WindowLaunchConfig cfg, RuntimeServices services)
    : cfg_(std::move(cfg)),
      services_(std::move(services)),
      power_policy_(services_.hw.on_battery),
      session_store_(cfg_.profile_dir / "session.json"),
      prefs_store_(cfg_.profile_dir / "prefs.json"),
      prefs_(prefs_store_.load()) {
    if (services_.shield) {
        services_.shield->setEnabled(prefs_.adblock_enabled);
    }
}

BrowserWindow::~BrowserWindow() {
    life_token_->store(false);
    if (pending_login_) {
        cleanseSecret(pending_login_->password);
        pending_login_.reset();
    }
    if (chrome_client_) chrome_client_->detach();
    if (hud_client_) hud_client_->detach();
    for (auto& t : tabs_) {
        if (t.client) t.client->detachObserver();
    }
    for (auto& [id, rt] : retiring_tabs_) {
        if (rt.client) rt.client->detachObserver();
    }
    if (services_.power_gov) services_.power_gov->shutdown();
    if (services_.display_ctrl) services_.display_ctrl->shutdown();
}

void BrowserWindow::show() {
    CEF_REQUIRE_UI_THREAD();
    // Normal browser: frameless obsidian CSD.
    // Standalone --app= mode: native minimal desktop window with ZERO browser chrome (100% pure app look & feel!).
    const bool frameless = cfg_.app_mode_url.empty();
    CefWindow::CreateTopLevelWindow(new FramelessWindowDelegate(this, frameless));
}

void BrowserWindow::openExternalUrls(const std::vector<std::string>& urls) {
    CEF_REQUIRE_UI_THREAD();
    for (size_t i = 0; i < urls.size(); ++i) {
        openNewTab(explorer::policy::normalizeOmnibarInput(urls[i]), i + 1 == urls.size());
    }
    activateWindow();
}

void BrowserWindow::activateWindow() {
    CEF_REQUIRE_UI_THREAD();
    if (window_ && !window_->IsClosed()) {
        if (window_->IsMinimized()) window_->Restore();
        window_->Activate();
        window_->BringToTop();
    }
}

int BrowserWindow::desiredChromeHeight() const {
    if (!cfg_.app_mode_url.empty() || html_fullscreen_) return 0;
    if (scroll_hidden_ && !hud_visible_ && !find_visible_) return 0;
    int h = theme::kTopBezelHeight;
    if (tabs_.size() > 1) h += theme::kTabBarHeight;
    if (find_visible_) h += theme::kFindBarHeight;
    return h;
}

void BrowserWindow::handleWindowCreated(CefRefPtr<CefWindow> window) {
    CEF_REQUIRE_UI_THREAD();
    window_ = window;
    window_->SetTitle(cfg_.app_mode_url.empty() ? "Explorer" : "Web App");
    window_->SetBackgroundColor(theme::kBgCanvas);

    if (auto icon = loadAppIconImage(cfg_.app_id)) {
        window_->SetWindowIcon(icon);
        window_->SetWindowAppIcon(icon);
    }

    // Global native window accelerators (work regardless of focus & in --app= mode too).
    window_->SetAccelerator(kAccelNewTab, 'T', false, true, false, false);
    window_->SetAccelerator(kAccelCloseTab, 'W', false, true, false, false);
    window_->SetAccelerator(kAccelNextTab, 0x09, false, true, false, false);
    window_->SetAccelerator(kAccelPrevTab, 0x09, true, true, false, false);
    window_->SetAccelerator(kAccelFocusOmnibar, 'L', false, true, false, false);
    window_->SetAccelerator(kAccelFocusOmnibar, 'D', false, false, true, false);
    window_->SetAccelerator(kAccelFind, 'F', false, true, false, false);
    window_->SetAccelerator(kAccelReload, 'R', false, true, false, false);
    window_->SetAccelerator(kAccelHardReload, 'R', true, true, false, false);
    window_->SetAccelerator(kAccelReload, 0x74, false, false, false, false);
    window_->SetAccelerator(kAccelBack, 0x25, false, false, true, false);
    window_->SetAccelerator(kAccelForward, 0x27, false, false, true, false);
    window_->SetAccelerator(kAccelZoomIn, 0xBB, false, true, false, false);
    window_->SetAccelerator(kAccelZoomIn, 0xBB, true, true, false, false);
    window_->SetAccelerator(kAccelZoomIn, 0x6B, false, true, false, false);
    window_->SetAccelerator(kAccelZoomOut, 0xBD, false, true, false, false);
    window_->SetAccelerator(kAccelZoomOut, 0x6D, false, true, false, false);
    window_->SetAccelerator(kAccelZoomReset, '0', false, true, false, false);
    window_->SetAccelerator(kAccelZoomReset, 0x60, false, true, false, false);
    window_->SetAccelerator(kAccelFullscreen, 0x7A, false, false, false, false);
    window_->SetAccelerator(kAccelDevTools, 0x7B, false, false, false, false);
    window_->SetAccelerator(kAccelDevTools, 'I', true, true, false, false);
    window_->SetAccelerator(kAccelQuit, 'Q', false, true, false, false);

    root_panel_ = CefPanel::CreatePanel(nullptr);
    root_panel_->SetBackgroundColor(theme::kBgCanvas);
    CefBoxLayoutSettings root_cfg;
    root_cfg.horizontal = false;
    root_cfg.between_child_spacing = 0;
    auto root_layout = root_panel_->SetToBoxLayout(root_cfg);

    const bool is_app_mode = !cfg_.app_mode_url.empty();

    if (!is_app_mode) {
        CefBrowserSettings chrome_settings;
        chrome_settings.background_color = theme::kBgBezel;
        chrome_client_ = new ChromeUiClient(this, false);
        chrome_view_ = CefBrowserView::CreateBrowserView(
            chrome_client_, topChromeHtmlDataUrl(), chrome_settings,
            makeTrustedChromeExtraInfo(), nullptr, new TopChromeViewDelegate(this));
        root_panel_->AddChildView(chrome_view_);
    }

    content_panel_ = CefPanel::CreatePanel(nullptr);
    content_panel_->SetBackgroundColor(theme::kBgCanvas);
    content_panel_->SetToFillLayout();
    root_panel_->AddChildView(content_panel_);
    root_layout->SetFlexForView(content_panel_, 1);

    window_->AddChildView(root_panel_);

    // Control Panel HUD (hud_view_ / hud_overlay_) is created LAZILY on first open:
    // saves 1 Chromium renderer process + ~40-50 MiB RAM + ~40ms cold startup time!

    if (services_.telemetry) {
        last_snap_ = services_.telemetry->sampleLite();
    }

    if (is_app_mode) {
        openNewTab(explorer::policy::normalizeOmnibarInput(cfg_.app_mode_url), true);
    } else if (!cfg_.initial_urls.empty()) {
        for (size_t i = 0; i < cfg_.initial_urls.size(); ++i) {
            openNewTab(explorer::policy::normalizeOmnibarInput(cfg_.initial_urls[i]),
                       i + 1 == cfg_.initial_urls.size());
        }
    } else {
        SessionData prev;
        if (!cfg_.disable_session) prev = session_store_.load();
        if (!prev.urls.empty()) {
            for (size_t i = 0; i < prev.urls.size(); ++i) {
                openNewTab(prev.urls[i], i == prev.active_index);
            }
        } else {
            openNewTab(startPageDataUrl(), true);
        }
    }

    window_->Show();

    auto tok = life_token_;
    CefPostDelayedTask(TID_UI, new GuardedUiTask(tok, [this]() { syncSystemPower(); }), 150);

    if (services_.power_gov) {
        services_.power_gov->startBatteryWatcher([this, tok](bool on_batt) {
            if (tok->load()) {
                CefPostTask(TID_UI, new GuardedUiTask(tok, [this, on_batt]() {
                    onPowerSourceChangedFromThread(on_batt);
                }));
            }
        });
    }
    if (services_.display_ctrl) {
        services_.display_ctrl->startMonitorsChangedWatcher([this, tok]() {
            if (tok->load()) {
                CefPostTask(TID_UI, new GuardedUiTask(tok, [this]() { syncSystemPower(); }));
            }
        });
    }

    // Skip periodic telemetry polling in pure chromeless --app= mode (zero bezel/HUD).
    if (!is_app_mode) {
        CefPostDelayedTask(TID_UI, new GuardedUiTask(tok, [this]() { onTelemetryTick(); }), 2000);
    }
}

void BrowserWindow::handleWindowDestroyed() {
    CEF_REQUIRE_UI_THREAD();
    life_token_->store(false);
    if (!session_flushed_on_close_) {
        flushSessionNow();
    }

    if (services_.throttler) {
        for (const auto& t : throttle_planner_.releaseAll()) {
            services_.throttler->setProcessIdleClass(t.pid, false);
        }
    }
    if (services_.power_gov) services_.power_gov->shutdown();
    if (services_.display_ctrl) services_.display_ctrl->shutdown();

    hud_overlay_ = nullptr;
    window_ = nullptr;
    CefQuitMessageLoop();
}

bool BrowserWindow::handleCanClose() {
    CEF_REQUIRE_UI_THREAD();
    if (!session_flushed_on_close_) {
        flushSessionNow();
        session_flushed_on_close_ = true;
        session_dirty_ = false;
    }
    closing_ = true;

    // Snapshot browser handles before calling TryCloseBrowser() so synchronous OnBeforeClose
    // callbacks cannot invalidate tabs_ or retiring_tabs_ iterators.
    std::vector<CefRefPtr<CefBrowser>> browsers;
    browsers.reserve(tabs_.size() + retiring_tabs_.size());
    for (auto& t : tabs_) {
        if (t.view && t.view->GetBrowser()) {
            browsers.push_back(t.view->GetBrowser());
        }
    }
    for (auto& [id, rt] : retiring_tabs_) {
        if (rt.view && rt.view->GetBrowser()) {
            browsers.push_back(rt.view->GetBrowser());
        }
    }

    bool all_ready = true;
    for (auto& b : browsers) {
        if (b && b->GetHost() && !b->GetHost()->TryCloseBrowser()) {
            all_ready = false;
        }
    }

    if (all_ready) {
        if (hud_overlay_) {
            hud_overlay_->Destroy();
            hud_overlay_ = nullptr;
        }
        if (hud_view_) {
            if (auto hb = hud_view_->GetBrowser()) hb->GetHost()->CloseBrowser(true);
            hud_view_ = nullptr;
        }
        if (chrome_view_) {
            if (root_panel_) root_panel_->RemoveChildView(chrome_view_);
            if (auto cb = chrome_view_->GetBrowser()) cb->GetHost()->CloseBrowser(true);
            chrome_view_ = nullptr;
        }
    }
    return all_ready;
}

void BrowserWindow::checkFinishShutdown() {
    CEF_REQUIRE_UI_THREAD();
    if (!closing_) return;
    if (tabs_.empty() && retiring_tabs_.empty() && window_ && !window_->IsClosed()) {
        window_->Close();
    }
}

bool BrowserWindow::handleAccelerator(int command_id) {
    CEF_REQUIRE_UI_THREAD();
    using A = explorer::engine::ShortcutAction;
    switch (command_id) {
        case kAccelNewTab: onTabShortcut(A::NewTab); return true;
        case kAccelCloseTab: onTabShortcut(A::CloseTab); return true;
        case kAccelNextTab: onTabShortcut(A::NextTab); return true;
        case kAccelPrevTab: onTabShortcut(A::PrevTab); return true;
        case kAccelFocusOmnibar: onTabShortcut(A::FocusOmnibar); return true;
        case kAccelFind: onTabShortcut(A::FindInPage); return true;
        case kAccelReload: onTabShortcut(A::Reload); return true;
        case kAccelHardReload: onTabShortcut(A::HardReload); return true;
        case kAccelBack: onTabShortcut(A::GoBack); return true;
        case kAccelForward: onTabShortcut(A::GoForward); return true;
        case kAccelZoomIn: onTabShortcut(A::ZoomIn); return true;
        case kAccelZoomOut: onTabShortcut(A::ZoomOut); return true;
        case kAccelZoomReset: onTabShortcut(A::ZoomReset); return true;
        case kAccelFullscreen: onTabShortcut(A::ToggleFullscreen); return true;
        case kAccelDevTools: onTabShortcut(A::ToggleDevTools); return true;
        case kAccelQuit: onTabShortcut(A::QuitApp); return true;
        default: return false;
    }
}

void BrowserWindow::openNewTab(const std::string& url, bool activate) {
    CEF_REQUIRE_UI_THREAD();
    uint64_t id = next_tab_id_++;

    CefBrowserSettings bsettings;
    bsettings.background_color = theme::kBgCanvas;

    auto client = CefRefPtr<explorer::engine::ExplorerCefClient>(
        new explorer::engine::ExplorerCefClient(id, this, services_.shield, cfg_.default_zoom));
    auto bview = CefBrowserView::CreateBrowserView(
        client, url, bsettings, nullptr, nullptr, new ContentBrowserViewDelegate());

    TabEntry entry;
    entry.id = id;
    entry.url = url;
    entry.title = (url.rfind("data:", 0) == 0) ? "New Tab" : url;
    entry.zoom_factor = cfg_.default_zoom;
    entry.client = client;
    entry.view = bview;

    bview->SetVisible(false);
    content_panel_->AddChildView(bview);
    tabs_.push_back(std::move(entry));

    if (activate || active_tab_id_ == 0) {
        activateTab(id);
    } else {
        updateChromeLayout();
        pushChromeState();
    }
    scheduleSessionSave();
}

void BrowserWindow::activateTab(uint64_t tab_id) {
    CEF_REQUIRE_UI_THREAD();
    if (active_tab_id_ != 0 && active_tab_id_ != tab_id) {
        if (find_visible_) {
            if (auto* prev = activeTabEntry(); prev && prev->view && prev->view->GetBrowser()) {
                prev->view->GetBrowser()->GetHost()->StopFinding(true);
            }
        }
        html_fullscreen_ = false;
    }
    setHudVisible(false);
    scroll_hidden_ = false;
    active_tab_id_ = tab_id;
    for (auto& t : tabs_) {
        bool is_active = (t.id == tab_id);
        if (t.view) {
            t.view->SetVisible(is_active);
            if (is_active) t.view->RequestFocus();
        }
    }
    updateChromeLayout();
    pushChromeState();
    reconcileRendererPriorities();
    scheduleSessionSave();
}

void BrowserWindow::closeTab(uint64_t tab_id) {
    CEF_REQUIRE_UI_THREAD();
    if (closing_) return;
    auto it = std::find_if(tabs_.begin(), tabs_.end(),
                           [tab_id](const TabEntry& e) { return e.id == tab_id; });
    if (it == tabs_.end()) return;

    if (tabs_.size() == 1) {
        if (window_ && !window_->IsClosed()) window_->Close();
        return;
    }

    const size_t idx = static_cast<size_t>(std::distance(tabs_.begin(), it));
    const bool was_active = (it->id == active_tab_id_);

    TabEntry closing_entry = std::move(*it);
    tabs_.erase(it);
    throttle_planner_.unregisterTab(tab_id);

    if (closing_entry.view) {
        closing_entry.view->SetVisible(false);
        if (content_panel_) content_panel_->RemoveChildView(closing_entry.view);
    }

    auto browser = closing_entry.view ? closing_entry.view->GetBrowser() : nullptr;
    retiring_tabs_.emplace(tab_id, std::move(closing_entry));

    if (was_active && !tabs_.empty()) {
        const size_t next_idx = std::min(idx, tabs_.size() - 1);
        activateTab(tabs_[next_idx].id);
    } else {
        updateChromeLayout();
        pushChromeState();
    }
    scheduleSessionSave();

    if (browser && browser->GetHost()) {
        // ExplorerCefClient::DoClose -> onTabDoClose() returns true so CEF never closes
        // our top-level CefWindow when retiring an individual tab!
        browser->GetHost()->CloseBrowser(true);
    } else {
        finishRetireTab(tab_id);
    }
}

void BrowserWindow::finishRetireTab(uint64_t tab_id) {
    CEF_REQUIRE_UI_THREAD();
    if (!life_token_->load()) return;
    auto rit = retiring_tabs_.find(tab_id);
    if (rit == retiring_tabs_.end()) return;
    // Dropping the detached CefBrowserView tears down the child browser cleanly and fires OnBeforeClose.
    rit->second.view = nullptr;
}

void BrowserWindow::cycleTab(int delta) {
    if (tabs_.size() <= 1) return;
    size_t idx = 0;
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].id == active_tab_id_) {
            idx = i;
            break;
        }
    }
    size_t next = (idx + tabs_.size() + static_cast<size_t>(delta)) % tabs_.size();
    activateTab(tabs_[next].id);
}

void BrowserWindow::updateChromeLayout(bool force) {
    if (!chrome_view_) return;
    int h = desiredChromeHeight();
    if (!force && h == last_chrome_height_) return;
    last_chrome_height_ = h;
    chrome_view_->SetVisible(h > 0);
    chrome_view_->InvalidateLayout();
    if (root_panel_) root_panel_->InvalidateLayout();
    if (window_) window_->Layout();
}

void BrowserWindow::ensureHudCreated() {
    CEF_REQUIRE_UI_THREAD();
    if (hud_overlay_ || !window_ || !cfg_.app_mode_url.empty()) return;

    CefBrowserSettings hud_settings;
    hud_settings.background_color = CefColorSetARGB(255, 14, 21, 36);
    hud_client_ = new ChromeUiClient(this, true);
    hud_view_ = CefBrowserView::CreateBrowserView(
        hud_client_, powerHudHtmlDataUrl(), hud_settings,
        makeTrustedChromeExtraInfo(), nullptr, new HudOverlayViewDelegate());
    hud_overlay_ = window_->AddOverlayView(hud_view_, CEF_DOCKING_MODE_TOP_RIGHT, true);
    if (hud_overlay_) {
        hud_overlay_->SetInsets(CefInsets(48, 0, 0, 10));
        hud_overlay_->SetVisible(false);
    }
}

void BrowserWindow::setHudVisible(bool visible, const std::string& tab) {
    if (visible) {
        ensureHudCreated();
        if (!hud_overlay_) return;
        scroll_hidden_ = false;
        updateChromeLayout();
    }
    if (!hud_overlay_) return;
    bool changed = (hud_visible_ != visible);
    hud_visible_ = visible;
    hud_overlay_->SetVisible(visible);
    if (visible) {
        if (!tab.empty()) {
            active_hud_tab_ = tab;
            pending_hud_tab_ = tab;
        }
        if (changed) {
            last_hud_sig_.clear();
        }
        pushHudState(last_snap_);
        requestTelemetrySampleAsync(true, false);
        if (!pending_hud_tab_.empty() && hud_ready_ && hud_view_ && hud_view_->GetBrowser()) {
            hud_view_->GetBrowser()->GetMainFrame()->ExecuteJavaScript(
                "window.__selectTab('" + jsonEscape(pending_hud_tab_) + "');", "explorer://hud", 0);
            pending_hud_tab_.clear();
        }
        if (hud_view_) hud_view_->RequestFocus();
    } else if (changed) {
        if (hud_ready_ && hud_view_ && hud_view_->GetBrowser()) {
            hud_view_->GetBrowser()->GetMainFrame()->ExecuteJavaScript(
                "if(typeof clearRevealedPasswords==='function')clearRevealedPasswords();"
                "if(typeof cancelVaultAuth==='function')cancelVaultAuth();",
                "explorer://hud", 0);
        }
        if (auto* t = activeTabEntry(); t && t->view) t->view->RequestFocus();
    }
}

void BrowserWindow::setFindBarVisible(bool visible) {
    if (visible) scroll_hidden_ = false;
    if (find_visible_ == visible) {
        if (visible && chrome_view_ && chrome_view_->GetBrowser()) {
            chrome_view_->RequestFocus();
            chrome_view_->GetBrowser()->GetMainFrame()->ExecuteJavaScript("window.__focusFind()", "", 0);
        }
        return;
    }
    find_visible_ = visible;
    updateChromeLayout();
    pushChromeState();
    if (visible && chrome_view_ && chrome_view_->GetBrowser()) {
        chrome_view_->RequestFocus();
        chrome_view_->GetBrowser()->GetMainFrame()->ExecuteJavaScript("window.__focusFind()", "", 0);
    } else if (auto* t = activeTabEntry(); t && t->view && t->view->GetBrowser()) {
        t->view->GetBrowser()->GetHost()->StopFinding(true);
        t->view->RequestFocus();
    }
}

void BrowserWindow::setStatusBanner(const std::string& msg) {
    status_msg_ = msg;
    if (hud_visible_) pushHudState(last_snap_);
}

void BrowserWindow::pushChromeState() {
    if (!chrome_ready_ || !chrome_view_ || !chrome_view_->GetBrowser()) return;
    auto* active = activeTabEntry();
    std::string shown_url;
    bool can_back = false, can_fwd = false, loading = false;
    double progress = 0.0;
    int zoom_pct = static_cast<int>(std::lround(cfg_.default_zoom * 100.0));
    size_t site_creds = 0;

    if (active) {
        shown_url = (active->url.rfind("data:", 0) == 0) ? "" : active->url;
        can_back = active->can_back;
        can_fwd = active->can_fwd;
        loading = active->loading;
        progress = active->progress;
        zoom_pct = static_cast<int>(std::lround(active->zoom_factor * 100.0));
        if (services_.vault && !shown_url.empty()) {
            site_creds = services_.vault->countForOrigin(shown_url);
        }
        if (window_) {
            const std::string suffix = " — Explorer";
            std::string win_title = "Explorer";
            if (!active->title.empty()) {
                win_title = (active->title.size() >= suffix.size() &&
                             active->title.compare(active->title.size() - suffix.size(), suffix.size(), suffix) == 0)
                                ? active->title
                                : (active->title + suffix);
            }
            if (win_title != last_window_title_) {
                last_window_title_ = win_title;
                window_->SetTitle(win_title);
            }
        }
    }

    int active_dls = 0;
    for (const auto& d : downloads_) {
        if (d.in_progress) ++active_dls;
    }

    bool adblock_on = services_.shield ? services_.shield->isEnabled() : false;
    uint64_t blocked = services_.shield ? services_.shield->blockedCount() : 0;

    std::ostringstream js;
    js << "window.__updateChrome({"
       << "\"url\":\"" << jsonEscape(shown_url) << "\","
       << "\"canBack\":" << (can_back ? "true" : "false") << ","
       << "\"canFwd\":" << (can_fwd ? "true" : "false") << ","
       << "\"loading\":" << (loading ? "true" : "false") << ","
       << "\"progress\":" << progress << ","
       << "\"zoomPct\":" << zoom_pct << ","
       << "\"adblockOn\":" << (adblock_on ? "true" : "false") << ","
       << "\"blocked\":" << blocked << ","
       << "\"siteCreds\":" << site_creds << ","
       << "\"hasPendingLogin\":" << (pending_login_.has_value() ? "true" : "false") << ","
       << "\"activeDownloads\":" << active_dls << ","
       << "\"batteryMode\":" << (power_policy_.isBatteryMode() ? "true" : "false") << ","
       << "\"pillText\":\"" << jsonEscape(power_policy_.formatPillText(last_snap_)) << "\","
       << "\"findVisible\":" << (find_visible_ ? "true" : "false") << ","
       << "\"tabs\":[";
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (i > 0) js << ",";
        js << "{\"id\":" << tabs_[i].id
           << ",\"title\":\"" << jsonEscape(tabs_[i].title) << "\""
           << ",\"favicon\":\"" << jsonEscape(tabs_[i].favicon) << "\""
           << ",\"active\":" << (tabs_[i].id == active_tab_id_ ? "true" : "false") << "}";
    }
    js << "]});";
    std::string payload = js.str();
    if (payload == last_chrome_sig_) return;  // Zero redundant Chrome JS IPC when state is unchanged!
    last_chrome_sig_ = std::move(payload);
    chrome_view_->GetBrowser()->GetMainFrame()->ExecuteJavaScript(last_chrome_sig_, "explorer://chrome", 0);
}

void BrowserWindow::pushChromeTelemetry() {
    if (!chrome_ready_ || !chrome_view_ || !chrome_view_->GetBrowser()) return;
    bool adblock_on = services_.shield ? services_.shield->isEnabled() : false;
    uint64_t blocked = services_.shield ? services_.shield->blockedCount() : 0;
    std::string pill = power_policy_.formatPillText(last_snap_);
    bool batt = power_policy_.isBatteryMode();

    std::string sig = (batt ? "1|" : "0|") + pill + (adblock_on ? "|1|" : "|0|") + std::to_string(blocked);
    if (sig == last_telemetry_sig_) return;  // Zero idle JS IPC when nothing changed!
    last_telemetry_sig_ = std::move(sig);

    std::ostringstream js;
    js << "window.__updateTelemetry("
       << (batt ? "true" : "false") << ",\""
       << jsonEscape(pill) << "\","
       << (adblock_on ? "true" : "false") << ","
       << blocked << ");";
    chrome_view_->GetBrowser()->GetMainFrame()->ExecuteJavaScript(js.str(), "explorer://chrome-telemetry", 0);
}

void BrowserWindow::pushChromeProgress(bool loading, double progress) {
    if (!chrome_ready_ || !chrome_view_ || !chrome_view_->GetBrowser()) return;
    if (loading && progress > 0.05 && progress < 0.99 &&
        std::abs(progress - last_pushed_progress_) < 0.05) {
        return;
    }
    last_pushed_progress_ = loading ? progress : -1.0;
    std::ostringstream js;
    js << "window.__updateProgress(" << (loading ? "true" : "false") << "," << progress << ");";
    chrome_view_->GetBrowser()->GetMainFrame()->ExecuteJavaScript(js.str(), "explorer://chrome-progress", 0);
}

void BrowserWindow::pushHudState(const explorer::hal::TelemetrySnapshot& snap) {
    if (!hud_ready_ || !hud_view_ || !hud_view_->GetBrowser()) return;

    std::ostringstream cpu;
    {
        char cb[64];
        std::snprintf(cb, sizeof(cb), "%.0f%%", snap.cpu_percent);
        cpu << cb;
    }
    if (snap.cpu_temp_c) {
        char tb[32];
        std::snprintf(tb, sizeof(tb), " · %.1f°C", *snap.cpu_temp_c);
        cpu << tb;
    }
    if (snap.pkg_watts) {
        char wb[32];
        std::snprintf(wb, sizeof(wb), " · %.1fW", *snap.pkg_watts);
        cpu << wb;
    }
    std::string cpu_str = cpu.str();

    std::ostringstream igpu;
    if (snap.igpu_busy) igpu << *snap.igpu_busy << "%";
    if (snap.igpu_mhz) igpu << (igpu.tellp() > 0 ? " · " : "") << *snap.igpu_mhz << " MHz";
    if (snap.igpu_temp_c) {
        char tb[32];
        std::snprintf(tb, sizeof(tb), "%.1f°C", *snap.igpu_temp_c);
        igpu << (igpu.tellp() > 0 ? " · " : "") << tb;
    }

    std::ostringstream fan;
    if (snap.fan_rpms.size() >= 2) {
        fan << snap.fan_rpms[0] << " / " << snap.fan_rpms[1] << " RPM";
    } else if (snap.fan_rpm) {
        fan << *snap.fan_rpm << " RPM";
    }

    std::ostringstream pwr;
    pwr << (snap.on_battery ? "Battery" : "AC");
    if (snap.batt_capacity_pct) {
        pwr << " · " << *snap.batt_capacity_pct << "%";
    }
    if (snap.batt_status_str && !snap.batt_status_str->empty()) {
        pwr << " (" << *snap.batt_status_str;
        if (snap.batt_watts && *snap.batt_watts > 0.05f) {
            char wb[32];
            std::snprintf(wb, sizeof(wb), " · %.1fW", *snap.batt_watts);
            pwr << wb;
        }
        pwr << ")";
    } else if (snap.batt_watts && *snap.batt_watts > 0.05f) {
        char wb[32];
        std::snprintf(wb, sizeof(wb), " · %.1fW", *snap.batt_watts);
        pwr << wb;
    }

    std::string dgpu_str = snap.dgpu_state.value_or("n/a");
    if (dgpu_str == "suspended") dgpu_str = "suspended (D3cold)";

    auto* active = activeTabEntry();
    std::string cur_origin = active ? explorer::vault::AesGcmPasswordVault::normalizeOrigin(active->url) : "";
    std::string cur_title = (active && active->title != "New Tab") ? active->title : "";
    int zoom_pct = static_cast<int>(std::lround((active ? active->zoom_factor : cfg_.default_zoom) * 100.0));

    bool adblock_on = services_.shield ? services_.shield->isEnabled() : false;
    uint64_t blocked = services_.shield ? services_.shield->blockedCount() : 0;
    size_t rules = services_.shield ? services_.shield->ruleCount() : 0;

    std::ostringstream js;
    js << "window.__updateHud({"
       << "\"adblockOn\":" << (adblock_on ? "true" : "false") << ","
       << "\"autohide\":" << (prefs_.autohide_topbar ? "true" : "false") << ","
       << "\"batteryMode\":" << (power_policy_.isBatteryMode() ? "true" : "false") << ","
       << "\"cinema60\":" << (power_policy_.cinema60Hz() ? "true" : "false") << ","
       << "\"zoomPct\":" << zoom_pct << ","
       << "\"rules\":" << rules << ","
       << "\"blocked\":" << blocked << ","
       << "\"cpu\":\"" << jsonEscape(cpu_str) << "\","
       << "\"igpu\":\"" << jsonEscape(igpu.str().empty() ? "Idle" : igpu.str()) << "\","
       << "\"va\":\"" << jsonEscape(services_.hw.va_caps.summary()) << "\","
       << "\"dgpu\":\"" << jsonEscape(dgpu_str) << "\","
       << "\"fan\":\"" << jsonEscape(fan.str().empty() ? "0 RPM" : fan.str()) << "\","
       << "\"power\":\"" << jsonEscape(pwr.str()) << "\","
       << "\"currentOrigin\":\"" << jsonEscape(cur_origin) << "\","
       << "\"currentTitle\":\"" << jsonEscape(cur_title) << "\","
       << "\"statusMsg\":\"" << jsonEscape(status_msg_) << "\","
       << "\"pendingOrigin\":\"" << jsonEscape(pending_login_ ? pending_login_->origin : "") << "\","
       << "\"pendingUser\":\"" << jsonEscape(pending_login_ ? pending_login_->username : "") << "\","
       << "\"creds\":[";

    if (services_.vault) {
        auto all = services_.vault->listMetadata();
        for (size_t i = 0; i < all.size(); ++i) {
            if (i > 0) js << ",";
            // Never copy or send plaintext passwords to the HUD DOM!
            js << "{\"origin\":\"" << jsonEscape(all[i].origin)
               << "\",\"username\":\"" << jsonEscape(all[i].username) << "\"}";
        }
    }
    js << "],\"downloads\":[";
    for (size_t i = 0; i < downloads_.size(); ++i) {
        if (i > 0) js << ",";
        js << "{\"id\":" << downloads_[i].id
           << ",\"name\":\"" << jsonEscape(downloads_[i].filename) << "\""
           << ",\"path\":\"" << jsonEscape(downloads_[i].full_path) << "\""
           << ",\"percent\":" << downloads_[i].percent
           << ",\"inProgress\":" << (downloads_[i].in_progress ? "true" : "false")
           << ",\"complete\":" << (downloads_[i].is_complete ? "true" : "false") << "}";
    }
    js << "],\"webapps\":[";
    if (services_.webapp_installer) {
        if (!webapps_cache_valid_) {
            cached_webapps_ = services_.webapp_installer->listInstalled();
            webapps_cache_valid_ = true;
        }
        for (size_t i = 0; i < cached_webapps_.size(); ++i) {
            if (i > 0) js << ",";
            js << "{\"id\":\"" << jsonEscape(cached_webapps_[i].app_id)
               << "\",\"name\":\"" << jsonEscape(cached_webapps_[i].name)
               << "\",\"url\":\"" << jsonEscape(cached_webapps_[i].url) << "\"}";
        }
    }
    js << "]});";
    std::string payload = js.str();
    if (payload == last_hud_sig_) return;  // Zero redundant HUD IPC when state is unchanged!
    last_hud_sig_ = std::move(payload);
    hud_view_->GetBrowser()->GetMainFrame()->ExecuteJavaScript(last_hud_sig_, "explorer://hud", 0);
}

void BrowserWindow::onChromeCommand(const std::string& cmd, const std::string& arg) {
    CEF_REQUIRE_UI_THREAD();
    auto* t = activeTabEntry();
    CefRefPtr<CefBrowser> b = (t && t->view) ? t->view->GetBrowser() : nullptr;

    if (cmd == "back" && b && b->CanGoBack()) {
        setHudVisible(false);
        b->GoBack();
    } else if (cmd == "forward" && b && b->CanGoForward()) {
        setHudVisible(false);
        b->GoForward();
    } else if (cmd == "reload_or_stop" && b) {
        setHudVisible(false);
        if (t->loading) b->StopLoad();
        else b->Reload();
    } else if (cmd == "new_tab") {
        setHudVisible(false);
        scroll_hidden_ = false;
        openNewTab(startPageDataUrl(), true);
        if (chrome_view_ && chrome_view_->GetBrowser()) {
            chrome_view_->RequestFocus();
            chrome_view_->GetBrowser()->GetMainFrame()->ExecuteJavaScript("window.__focusOmnibar()", "", 0);
        }
    } else if (cmd == "navigate" && b) {
        setHudVisible(false);
        scroll_hidden_ = false;
        updateChromeLayout();
        std::string target = explorer::policy::normalizeOmnibarInput(arg);
        if (target == explorer::policy::kHomeUrl) target = startPageDataUrl();
        b->GetMainFrame()->LoadURL(target);
        if (t && t->view) t->view->RequestFocus();
    } else if (cmd == "focus_content") {
        setHudVisible(false);
        if (t && t->view) t->view->RequestFocus();
    } else if (cmd == "activate_tab") {
        try { activateTab(std::stoull(arg)); } catch (...) {}
    } else if (cmd == "close_tab") {
        try { closeTab(std::stoull(arg)); } catch (...) {}
    } else if (cmd == "open_panel") {
        status_msg_.clear();
        std::string target_tab = arg.empty() ? "power" : arg;
        if (hud_visible_ && (arg.empty() || active_hud_tab_ == target_tab)) {
            setHudVisible(false);
        } else {
            setHudVisible(true, target_tab);
        }
    } else if (cmd == "hud_tab_select") {
        status_msg_.clear();
        if (!arg.empty()) active_hud_tab_ = arg;
    } else if (cmd == "zoom_in") {
        onTabShortcut(explorer::engine::ShortcutAction::ZoomIn);
        if (!hud_visible_ && t && t->view) t->view->RequestFocus();
    } else if (cmd == "zoom_out") {
        onTabShortcut(explorer::engine::ShortcutAction::ZoomOut);
        if (!hud_visible_ && t && t->view) t->view->RequestFocus();
    } else if (cmd == "zoom_reset") {
        onTabShortcut(explorer::engine::ShortcutAction::ZoomReset);
        if (!hud_visible_ && t && t->view) t->view->RequestFocus();
    } else if (cmd == "close_hud") {
        setHudVisible(false);
    } else if (cmd == "toggle_adblock" && services_.shield) {
        prefs_.adblock_enabled = !services_.shield->isEnabled();
        services_.shield->setEnabled(prefs_.adblock_enabled);
        prefs_store_.save(prefs_);
        pushChromeState();
        setStatusBanner(prefs_.adblock_enabled ? "Brave Shield enabled" : "Brave Shield paused");
    } else if (cmd == "update_filters" && services_.shield) {
        setStatusBanner("Downloading EasyList & EasyPrivacy...");
        auto tok = life_token_;
        services_.shield->updateFilterListsAsync([this, tok](bool ok, size_t total) {
            if (tok->load()) {
                CefPostTask(TID_UI, new GuardedUiTask(tok, [this, ok, total]() {
                    onFilterUpdateFinished(ok, total);
                }));
            }
        });
    } else if (cmd == "toggle_autohide") {
        prefs_.autohide_topbar = !prefs_.autohide_topbar;
        if (!prefs_.autohide_topbar) scroll_hidden_ = false;
        prefs_store_.save(prefs_);
        broadcastAutohideConfig();
        updateChromeLayout();
        setStatusBanner(prefs_.autohide_topbar ? "Auto-hide top bar on scroll enabled"
                                               : "Top bar pinned visible");
    } else if (cmd == "toggle_turbo") {
        bool want_turbo = power_policy_.isBatteryMode();
        power_policy_.setManualTurbo(want_turbo);
        syncSystemPower();
        pushChromeState();
        pushHudState(last_snap_);
    } else if (cmd == "toggle_cinema60") {
        power_policy_.setCinema60Hz(!power_policy_.cinema60Hz());
        syncSystemPower();
        pushHudState(last_snap_);
    } else if (cmd == "open_devtools") {
        setHudVisible(false);
        onTabShortcut(explorer::engine::ShortcutAction::ToggleDevTools);
    } else if (cmd == "vault_save" && services_.vault) {
        std::string o = extractJsonField(arg, "origin");
        std::string u = extractJsonField(arg, "username");
        std::string p = extractJsonField(arg, "password");
        bool saved = services_.vault->saveOrUpdate(o, u, p);
        cleanseSecret(p);
        if (saved) {
            pushChromeState();
            setStatusBanner("Encrypted & saved credential for " + u);
        }
    } else if (cmd == "vault_accept_pending" && services_.vault && pending_login_) {
        if (services_.vault->saveOrUpdate(pending_login_->origin,
                                          pending_login_->username,
                                          pending_login_->password)) {
            std::string u = pending_login_->username;
            cleanseSecret(pending_login_->password);
            pending_login_.reset();
            pushChromeState();
            setStatusBanner("Saved encrypted login for " + u);
        }
    } else if (cmd == "vault_dismiss_pending") {
        if (pending_login_) {
            cleanseSecret(pending_login_->password);
            pending_login_.reset();
        }
        pushChromeState();
        pushHudState(last_snap_);
    } else if (cmd == "vault_delete" && services_.vault) {
        std::string o = extractJsonField(arg, "origin");
        std::string u = extractJsonField(arg, "username");
        if (services_.vault->remove(o, u)) {
            pushChromeState();
            setStatusBanner("Deleted saved login for " + u);
        }
    } else if (cmd == "vault_reveal" && services_.vault) {
        std::string o = extractJsonField(arg, "origin");
        std::string u = extractJsonField(arg, "username");
        std::string auth_pw = extractJsonField(arg, "authPassword");
        if (o.empty() || u.empty() || auth_pw.empty()) {
            cleanseSecret(auth_pw);
            return;
        }
        auto tok = life_token_;
        setStatusBanner("Verifying Linux login / root password...");
        CefPostTask(TID_FILE_USER_VISIBLE, new GuardedUiTask(tok, [this, tok, o, u, auth_pw = std::move(auth_pw)]() mutable {
            bool ok = verifyLinuxSystemPassword(auth_pw);
            cleanseSecret(auth_pw);
            if (tok->load()) {
                CefPostTask(TID_UI, new GuardedUiTask(tok, [this, ok, o, u]() {
                    onVaultRevealAuthFinished(ok, o, u);
                }));
            }
        }));
    } else if (cmd == "vault_fill" && services_.vault && b && b->GetMainFrame()) {
        std::string o = extractJsonField(arg, "origin");
        std::string u = extractJsonField(arg, "username");
        // Security: verify the live active frame's origin EXACTLY matches the credential's origin
        // and is HTTPS (or exact localhost/127.0.0.1) to prevent cross-origin credential exfiltration!
        std::string live_url = b->GetMainFrame()->GetURL().ToString();
        std::string live_origin = explorer::vault::AesGcmPasswordVault::normalizeOrigin(live_url);
        std::string req_origin = explorer::vault::AesGcmPasswordVault::normalizeOrigin(o);
        bool secure_origin = explorer::policy::isSecureWebOrigin(live_origin);
        if (!secure_origin || live_origin.empty() || live_origin != req_origin) {
            setStatusBanner("Blocked autofill: origin mismatch or insecure page");
            return;
        }
        for (const auto& c : services_.vault->findForOrigin(live_origin)) {
            if (c.username == u) {
                std::string fill_js =
                    "(() => {"
                    " const pw = document.querySelector('input[type=\"password\"]:not([disabled])');"
                    " const root = (pw && pw.form) ? pw.form : document;"
                    " const usr = root.querySelector('input[autocomplete=\"username\"], input[type=\"email\"], input[type=\"text\"][name*=\"user\" i], input[type=\"text\"][name*=\"login\" i], input[type=\"text\"][name*=\"email\" i], input[type=\"text\"][name*=\"id\" i], input[type=\"text\"][id*=\"user\" i], input[type=\"text\"][id*=\"login\" i], input[type=\"text\"]:not([disabled])');"
                    " const setVal = (el, val) => { if (!el) return; el.focus(); const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set; setter.call(el, val); el.dispatchEvent(new Event('input', {bubbles:true})); el.dispatchEvent(new Event('change', {bubbles:true})); };"
                    " setVal(usr, \"" + jsonEscape(c.username) + "\");"
                    " setVal(pw, \"" + jsonEscape(c.password) + "\");"
                    "})();";
                b->GetMainFrame()->ExecuteJavaScript(fill_js, "explorer://vault-fill", 0);
                setHudVisible(false);
                break;
            }
        }
    } else if (cmd == "dl_cancel") {
        try {
            uint32_t did = static_cast<uint32_t>(std::stoul(arg));
            for (auto& tab : tabs_) {
                if (tab.client) tab.client->cancelDownload(did);
            }
        } catch (...) {}
    } else if (cmd == "dl_open") {
        // Security: only open paths belonging to verified completed downloads from this session.
        for (const auto& d : downloads_) {
            if (d.is_complete && !d.full_path.empty() && d.full_path == arg) {
                spawnDetachedXdgOpen(d.full_path);
                break;
            }
        }
    } else if (cmd == "dl_open_dir") {
        const char* dl = std::getenv("XDG_DOWNLOAD_DIR");
        const char* home = std::getenv("HOME");
        std::string dir = (dl && dl[0]) ? std::string(dl)
                                        : (home ? (std::string(home) + "/Downloads") : "/tmp");
        spawnDetachedXdgOpen(dir);
    } else if (cmd == "dl_clear") {
        downloads_.erase(std::remove_if(downloads_.begin(), downloads_.end(),
                                        [](const auto& d) { return !d.in_progress; }),
                         downloads_.end());
        pushHudState(last_snap_);
    } else if (cmd == "webapp_install" && services_.webapp_installer && t) {
        if (t->url.rfind("http://", 0) == 0 || t->url.rfind("https://", 0) == 0) {
            std::string name = arg.empty() ? t->title : arg;
            std::string url = t->url;
            std::string fav = t->favicon;
            if (fav.empty()) {
                std::string orig = explorer::vault::AesGcmPasswordVault::normalizeOrigin(url);
                if (!orig.empty()) fav = orig + "/favicon.ico";
            }
            auto installer = services_.webapp_installer;
            auto tok = life_token_;
            setStatusBanner("Installing Web App: " + name + "...");
            CefPostTask(TID_FILE_USER_VISIBLE, new GuardedUiTask(tok, [this, tok, installer, name, url, fav]() {
                auto installed = installer->install(name, url, fav);
                bool ok = !installed.app_id.empty();
                std::string final_name = ok ? installed.name : name;
                if (tok->load()) {
                    CefPostTask(TID_UI, new GuardedUiTask(tok, [this, ok, final_name]() {
                        onWebAppInstallFinished(ok, final_name);
                    }));
                }
            }));
        } else {
            setStatusBanner("Open an http/https website first to install as App");
        }
    } else if (cmd == "webapp_launch" && services_.webapp_installer) {
        if (!webapps_cache_valid_) {
            cached_webapps_ = services_.webapp_installer->listInstalled();
            webapps_cache_valid_ = true;
        }
        for (const auto& a : cached_webapps_) {
            if (a.app_id == arg) {
                services_.webapp_installer->launch(a);
                setHudVisible(false);
                break;
            }
        }
    } else if (cmd == "webapp_uninstall" && services_.webapp_installer) {
        if (services_.webapp_installer->uninstall(arg)) {
            webapps_cache_valid_ = false;
            setStatusBanner("Uninstalled Web App");
        }
    } else if (cmd == "clear_site_data" && t && b) {
        if (auto cm = CefCookieManager::GetGlobalManager(nullptr)) {
            cm->DeleteCookies(t->url, "", nullptr);
        }
        b->GetMainFrame()->ExecuteJavaScript(
            "try{localStorage.clear();sessionStorage.clear();}catch(e){}", "", 0);
        setStatusBanner("Cleared cookies & storage for current site");
    } else if (cmd == "clear_all_data") {
        if (auto cm = CefCookieManager::GetGlobalManager(nullptr)) {
            cm->DeleteCookies("", "", nullptr);
        }
        if (b) {
            b->GetMainFrame()->ExecuteJavaScript(
                "try{localStorage.clear();sessionStorage.clear();}catch(e){}", "", 0);
        }
        setStatusBanner("Cleared all cookies & site data");
    } else if (cmd == "find_input" && b) {
        if (arg.empty()) b->GetHost()->StopFinding(true);
        else b->GetHost()->Find(arg, true, false, false);
    } else if (cmd == "find_next" && b) {
        if (arg.empty()) b->GetHost()->StopFinding(true);
        else b->GetHost()->Find(arg, true, false, true);
    } else if (cmd == "find_prev" && b) {
        if (!arg.empty()) b->GetHost()->Find(arg, false, false, true);
    } else if (cmd == "find_close") {
        setFindBarVisible(false);
    } else if (cmd == "win_min" && window_) {
        window_->Minimize();
    } else if (cmd == "win_max" && window_) {
        if (window_->IsMaximized()) window_->Restore();
        else window_->Maximize();
    } else if (cmd == "win_full" && window_) {
        bool next_full = !window_->IsFullscreen();
        if (!next_full) html_fullscreen_ = false;
        window_->SetFullscreen(next_full);
        updateChromeLayout();
    } else if (cmd == "win_close" && window_) {
        window_->Close();
    }
}

void BrowserWindow::onFilterUpdateFinished(bool ok, size_t total_rules) {
    CEF_REQUIRE_UI_THREAD();
    if (!life_token_->load()) return;
    setStatusBanner(ok ? ("Updated Brave Shield: " + std::to_string(total_rules) + " rules active")
                       : "Filter update failed (kept existing rules)");
}

void BrowserWindow::onWebAppInstallFinished(bool ok, std::string app_name) {
    CEF_REQUIRE_UI_THREAD();
    if (!life_token_->load()) return;
    if (ok) webapps_cache_valid_ = false;
    setStatusBanner(ok ? ("Installed app: " + app_name) : "Web App install failed");
}

void BrowserWindow::onVaultRevealAuthFinished(bool ok, std::string origin, std::string username) {
    CEF_REQUIRE_UI_THREAD();
    if (!life_token_->load() || !hud_ready_ || !hud_view_ || !hud_view_->GetBrowser()) return;
    std::string norm = explorer::vault::AesGcmPasswordVault::normalizeOrigin(origin);
    std::string found_pw;
    bool found = false;
    if (ok && services_.vault && !norm.empty()) {
        for (const auto& c : services_.vault->findForOrigin(norm)) {
            if (c.username == username) {
                found_pw = c.password;
                found = true;
                break;
            }
        }
    }
    std::string msg = (ok && found)
                          ? ("Verified — password revealed for " + username + " (auto-hides in 30s)")
                          : "Authentication failed: invalid Linux login / root password";
    status_msg_ = msg;
    std::ostringstream js;
    js << "if(typeof window.__onVaultRevealResult==='function')window.__onVaultRevealResult("
       << ((ok && found) ? "true" : "false") << ",\""
       << jsonEscape(norm) << "\",\""
       << jsonEscape(username) << "\",\""
       << jsonEscape(found_pw) << "\",\""
       << jsonEscape(msg) << "\");";
    cleanseSecret(found_pw);
    hud_view_->GetBrowser()->GetMainFrame()->ExecuteJavaScript(js.str(), "explorer://hud-vault-reveal", 0);
}

void BrowserWindow::onChromeDraggableRegionsChanged(const std::vector<CefDraggableRegion>& regions) {
    CEF_REQUIRE_UI_THREAD();
    if (window_) window_->SetDraggableRegions(regions);
}

void BrowserWindow::onChromeReady(bool is_hud) {
    CEF_REQUIRE_UI_THREAD();
    if (is_hud) {
        hud_ready_ = true;
        pushHudState(last_snap_);
        requestTelemetrySampleAsync(true, false);
        if (!pending_hud_tab_.empty() && hud_view_ && hud_view_->GetBrowser()) {
            hud_view_->GetBrowser()->GetMainFrame()->ExecuteJavaScript(
                "window.__selectTab('" + jsonEscape(pending_hud_tab_) + "');", "explorer://hud", 0);
            pending_hud_tab_.clear();
        }
    } else {
        chrome_ready_ = true;
        pushChromeState();
    }
}

void BrowserWindow::onChromeShortcut(explorer::engine::ShortcutAction action) {
    onTabShortcut(action);
}

void BrowserWindow::onChromeBrowserCreated(bool /*is_hud*/) {
    CEF_REQUIRE_UI_THREAD();
}

void BrowserWindow::onChromeBrowserClosed(bool is_hud) {
    CEF_REQUIRE_UI_THREAD();
    if (is_hud) hud_ready_ = false;
    else chrome_ready_ = false;
    checkFinishShutdown();
}

void BrowserWindow::onTabCreated(uint64_t /*tab_id*/) {
    CEF_REQUIRE_UI_THREAD();
}

void BrowserWindow::onTabAddressChanged(uint64_t tab_id, const std::string& url) {
    CEF_REQUIRE_UI_THREAD();
    if (auto* t = findTab(tab_id)) {
        if (t->url == url) return;
        t->url = url;
        if (tab_id == active_tab_id_) {
            scroll_hidden_ = false;
            updateChromeLayout();
            pushChromeState();
        }
        scheduleSessionSave();
    }
}

void BrowserWindow::onTabTitleChanged(uint64_t tab_id, const std::string& title) {
    CEF_REQUIRE_UI_THREAD();
    if (auto* t = findTab(tab_id)) {
        std::string clean = title.empty() ? "New Tab" : title;
        if (t->title == clean) return;
        t->title = std::move(clean);
        if (!cfg_.app_mode_url.empty() && window_) {
            window_->SetTitle(t->title);
        } else {
            pushChromeState();
        }
    }
}

void BrowserWindow::onTabFaviconChanged(uint64_t tab_id, const std::string& favicon_url) {
    CEF_REQUIRE_UI_THREAD();
    if (auto* t = findTab(tab_id)) {
        if (t->favicon == favicon_url) return;
        t->favicon = favicon_url;
        if (tabs_.size() > 1) pushChromeState();
    }
}

void BrowserWindow::onTabProgressChanged(uint64_t tab_id, double progress) {
    CEF_REQUIRE_UI_THREAD();
    if (auto* t = findTab(tab_id)) {
        t->progress = progress;
        if (tab_id == active_tab_id_) pushChromeProgress(t->loading, progress);
    }
}

void BrowserWindow::onTabLoadingChanged(uint64_t tab_id, bool is_loading, bool can_back, bool can_fwd) {
    CEF_REQUIRE_UI_THREAD();
    if (auto* t = findTab(tab_id)) {
        t->loading = is_loading;
        t->can_back = can_back;
        t->can_fwd = can_fwd;
        if (!is_loading) {
            t->progress = 1.0;
            if (prefs_.autohide_topbar && t->view && t->view->GetBrowser() &&
                t->view->GetBrowser()->GetMainFrame()) {
                auto msg = CefProcessMessage::Create("explorer.autohide-config");
                msg->GetArgumentList()->SetBool(0, true);
                t->view->GetBrowser()->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
            }
        }
        if (tab_id == active_tab_id_) pushChromeState();
    }
}

void BrowserWindow::onTabFullscreenChanged(uint64_t tab_id, bool fullscreen) {
    CEF_REQUIRE_UI_THREAD();
    if (tab_id != active_tab_id_) return;
    html_fullscreen_ = fullscreen;
    if (fullscreen) {
        setHudVisible(false);
        find_visible_ = false;
    }
    updateChromeLayout();
    if (window_) window_->SetFullscreen(fullscreen);
}

void BrowserWindow::onTabPopupRequested(const std::string& url) {
    CEF_REQUIRE_UI_THREAD();
    // Security: block web-initiated popups to privileged/dangerous schemes (file://, javascript:, chrome://, data:).
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        return;
    }
    if (!cfg_.app_mode_url.empty()) {
        if (auto* t = activeTabEntry(); t && t->view && t->view->GetBrowser()) {
            t->view->GetBrowser()->GetMainFrame()->LoadURL(url);
        }
        return;
    }
    openNewTab(url, true);
}

void BrowserWindow::onTabRendererPid(uint64_t tab_id, pid_t pid) {
    CEF_REQUIRE_UI_THREAD();
    throttle_planner_.registerTab(tab_id, pid);
    reconcileRendererPriorities();
}

void BrowserWindow::onTabDownloadUpdated(const explorer::engine::DownloadRecord& record) {
    CEF_REQUIRE_UI_THREAD();
    bool found = false;
    bool state_changed = false;
    bool percent_changed = false;
    for (auto& d : downloads_) {
        if (d.id == record.id) {
            state_changed = (d.in_progress != record.in_progress ||
                             d.is_complete != record.is_complete ||
                             d.is_canceled != record.is_canceled);
            percent_changed = (d.percent != record.percent);
            d = record;
            found = true;
            break;
        }
    }
    if (!found) {
        downloads_.insert(downloads_.begin(), record);
        if (downloads_.size() > 30) downloads_.pop_back();
        state_changed = true;
        percent_changed = true;
    }
    if (state_changed) {
        pushChromeState();
    }
    if (hud_visible_ && (state_changed || percent_changed)) {
        pushHudState(last_snap_);
    }
}

void BrowserWindow::onTabScrollRevealTopBar(uint64_t tab_id, bool reveal) {
    CEF_REQUIRE_UI_THREAD();
    if (!prefs_.autohide_topbar || !cfg_.app_mode_url.empty() || tab_id != active_tab_id_) return;
    if (hud_visible_ || find_visible_) return;
    bool want_hidden = !reveal;
    if (scroll_hidden_ != want_hidden) {
        scroll_hidden_ = want_hidden;
        updateChromeLayout();
    }
}

void BrowserWindow::onTabMediaStateChanged(uint64_t tab_id, bool playing) {
    CEF_REQUIRE_UI_THREAD();
    if (auto* t = findTab(tab_id)) {
        if (t->playing_media == playing) return;
        t->playing_media = playing;
        reconcileRendererPriorities();
    }
}

void BrowserWindow::onTabLoginSubmitted(const std::string& origin,
                                        const std::string& username,
                                        const std::string& password) {
    CEF_REQUIRE_UI_THREAD();
    std::string norm = explorer::vault::AesGcmPasswordVault::normalizeOrigin(origin);
    if (norm.empty() || username.empty() || password.empty()) return;
    if (pending_login_) {
        cleanseSecret(pending_login_->password);
    }
    pending_login_ = explorer::vault::Credential{norm, username, password};
    pushChromeState();
    if (hud_visible_) pushHudState(last_snap_);
}

void BrowserWindow::onTabShortcut(explorer::engine::ShortcutAction action) {
    CEF_REQUIRE_UI_THREAD();
    using A = explorer::engine::ShortcutAction;
    auto* t = activeTabEntry();
    CefRefPtr<CefBrowser> b = (t && t->view) ? t->view->GetBrowser() : nullptr;

    switch (action) {
        case A::NewTab:
            if (cfg_.app_mode_url.empty()) onChromeCommand("new_tab", "");
            break;
        case A::CloseTab:
            if (t) closeTab(t->id);
            break;
        case A::NextTab:
            if (cfg_.app_mode_url.empty()) cycleTab(+1);
            break;
        case A::PrevTab:
            if (cfg_.app_mode_url.empty()) cycleTab(-1);
            break;
        case A::FocusOmnibar:
            if (cfg_.app_mode_url.empty()) {
                setHudVisible(false);
                scroll_hidden_ = false;
                updateChromeLayout();
                if (chrome_view_ && chrome_view_->GetBrowser()) {
                    chrome_view_->RequestFocus();
                    chrome_view_->GetBrowser()->GetMainFrame()->ExecuteJavaScript("window.__focusOmnibar()", "", 0);
                }
            }
            break;
        case A::FindInPage:
            if (cfg_.app_mode_url.empty()) setFindBarVisible(true);
            break;
        case A::Reload:
            if (b) b->Reload();
            break;
        case A::HardReload:
            if (b) b->ReloadIgnoreCache();
            break;
        case A::GoBack:
            if (b && b->CanGoBack()) b->GoBack();
            break;
        case A::GoForward:
            if (b && b->CanGoForward()) b->GoForward();
            break;
        case A::ZoomIn:
            if (t && b && b->GetHost()) {
                t->zoom_factor = std::clamp(std::round((t->zoom_factor + 0.1) * 100.0) / 100.0, 0.3, 3.0);
                double lvl = explorer::policy::zoomFactorToCefLevel(t->zoom_factor);
                if (t->client) t->client->setZoomLevel(lvl);
                b->GetHost()->SetZoomLevel(lvl);
                pushChromeState();
                if (hud_visible_) pushHudState(last_snap_);
            }
            break;
        case A::ZoomOut:
            if (t && b && b->GetHost()) {
                t->zoom_factor = std::clamp(std::round((t->zoom_factor - 0.1) * 100.0) / 100.0, 0.3, 3.0);
                double lvl = explorer::policy::zoomFactorToCefLevel(t->zoom_factor);
                if (t->client) t->client->setZoomLevel(lvl);
                b->GetHost()->SetZoomLevel(lvl);
                pushChromeState();
                if (hud_visible_) pushHudState(last_snap_);
            }
            break;
        case A::ZoomReset:
            if (t && b && b->GetHost()) {
                t->zoom_factor = cfg_.default_zoom;
                double lvl = explorer::policy::zoomFactorToCefLevel(t->zoom_factor);
                if (t->client) t->client->setZoomLevel(lvl);
                b->GetHost()->SetZoomLevel(lvl);
                pushChromeState();
                if (hud_visible_) pushHudState(last_snap_);
            }
            break;
        case A::ToggleFullscreen:
            if (window_) {
                bool next_full = !window_->IsFullscreen();
                if (!next_full) html_fullscreen_ = false;
                window_->SetFullscreen(next_full);
                updateChromeLayout();
            }
            break;
        case A::ToggleDevTools:
            if (b && b->GetHost()) {
                if (b->GetHost()->HasDevTools()) {
                    b->GetHost()->CloseDevTools();
                } else {
                    CefWindowInfo wi;
                    CefBrowserSettings bs;
                    b->GetHost()->ShowDevTools(wi, nullptr, bs, CefPoint());
                }
            }
            break;
        case A::QuitApp:
            if (window_) window_->Close();
            break;
        case A::Escape:
            if (hud_visible_) setHudVisible(false);
            if (find_visible_) setFindBarVisible(false);
            break;
    }
}

bool BrowserWindow::onTabDoClose(uint64_t tab_id) {
    CEF_REQUIRE_UI_THREAD();
    // Full window shutdown: allow CEF standard top-level window close flow.
    if (closing_) return false;

    // Retiring individual tab (initiated by closeTab): NEVER close the parent CefWindow!
    if (retiring_tabs_.count(tab_id) != 0) {
        auto tok = life_token_;
        CefPostTask(TID_UI, new GuardedUiTask(tok, [this, tab_id]() { finishRetireTab(tab_id); }));
        return true;
    }

    // Tab-initiated close (e.g., webpage JS `window.close()`): if other tabs remain,
    // retire ONLY this tab cleanly and protect the parent window from closing!
    if (tabs_.size() > 1) {
        auto it = std::find_if(tabs_.begin(), tabs_.end(),
                               [tab_id](const TabEntry& e) { return e.id == tab_id; });
        if (it != tabs_.end()) {
            const size_t idx = static_cast<size_t>(std::distance(tabs_.begin(), it));
            const bool was_active = (it->id == active_tab_id_);
            TabEntry closing_entry = std::move(*it);
            tabs_.erase(it);
            throttle_planner_.unregisterTab(tab_id);
            if (closing_entry.view) {
                closing_entry.view->SetVisible(false);
                if (content_panel_) content_panel_->RemoveChildView(closing_entry.view);
            }
            retiring_tabs_.emplace(tab_id, std::move(closing_entry));
            if (was_active && !tabs_.empty()) {
                activateTab(tabs_[std::min(idx, tabs_.size() - 1)].id);
            } else {
                updateChromeLayout();
                pushChromeState();
            }
            scheduleSessionSave();
            auto tok = life_token_;
            CefPostTask(TID_UI, new GuardedUiTask(tok, [this, tab_id]() { finishRetireTab(tab_id); }));
            return true;
        }
    }

    return false;
}

void BrowserWindow::onTabClosed(uint64_t tab_id) {
    CEF_REQUIRE_UI_THREAD();
    throttle_planner_.unregisterTab(tab_id);

    if (auto rit = retiring_tabs_.find(tab_id); rit != retiring_tabs_.end()) {
        if (rit->second.client) rit->second.client->detachObserver();
        retiring_tabs_.erase(rit);
        if (closing_) checkFinishShutdown();
        return;
    }

    auto it = std::find_if(tabs_.begin(), tabs_.end(),
                           [tab_id](const TabEntry& e) { return e.id == tab_id; });
    if (it != tabs_.end()) {
        if (it->view && content_panel_) content_panel_->RemoveChildView(it->view);
        if (it->client) it->client->detachObserver();
        tabs_.erase(it);
    }

    if (closing_) {
        checkFinishShutdown();
        return;
    }

    if (tabs_.empty()) {
        if (window_ && !window_->IsClosed()) window_->Close();
        else CefQuitMessageLoop();
        return;
    }

    if (active_tab_id_ == tab_id) {
        activateTab(tabs_.back().id);
    } else {
        updateChromeLayout();
        pushChromeState();
    }
    scheduleSessionSave();
}

void BrowserWindow::onPowerSourceChangedFromThread(bool on_battery) {
    CEF_REQUIRE_UI_THREAD();
    if (!life_token_->load()) return;
    power_policy_.setPowerSource(on_battery);
    syncSystemPower();
    pushChromeState();
    if (hud_visible_) pushHudState(last_snap_);
}

void BrowserWindow::syncSystemPower() {
    CEF_REQUIRE_UI_THREAD();
    if (!life_token_->load() || services_.hw.tuning_disabled) return;
    const unsigned long slack_ns = power_policy_.timerSlackNs();
    if (services_.power_gov) {
        services_.power_gov->applyTimerSlackNs(slack_ns);
        services_.power_gov->setPerformanceHoldAsync(power_policy_.wantsPerformanceHold());
    }
    // Update env for any newly spawned subprocesses (sandboxed renderers cannot call prctl post-init).
    ::setenv("EXPLORER_TURBO_SLACK", power_policy_.isBatteryMode() ? "0" : "1", 1);
    if (services_.display_ctrl) {
        services_.display_ctrl->setEdpRefreshRateAsync(power_policy_.targetRefreshHz());
    }
    reconcileRendererPriorities();
}

void BrowserWindow::broadcastAutohideConfig() {
    CEF_REQUIRE_UI_THREAD();
    for (auto& t : tabs_) {
        if (t.view && t.view->GetBrowser() && t.view->GetBrowser()->GetMainFrame()) {
            auto msg = CefProcessMessage::Create("explorer.autohide-config");
            msg->GetArgumentList()->SetBool(0, prefs_.autohide_topbar);
            t.view->GetBrowser()->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
        }
    }
}

void BrowserWindow::reconcileRendererPriorities() {
    CEF_REQUIRE_UI_THREAD();
    if (!services_.throttler || services_.hw.tuning_disabled) return;
    std::unordered_set<uint64_t> active;
    if (active_tab_id_ != 0) active.insert(active_tab_id_);
    std::unordered_set<uint64_t> media;
    for (const auto& t : tabs_) {
        if (t.playing_media) media.insert(t.id);
    }
    auto actions = throttle_planner_.reconcile(active, media, power_policy_.isBatteryMode());
    for (const auto& act : actions) {
        pending_pid_throttles_[act.pid] = act.throttle;
    }
    drainPendingThrottleActions();
}

void BrowserWindow::drainPendingThrottleActions() {
    CEF_REQUIRE_UI_THREAD();
    if (throttle_in_flight_ || pending_pid_throttles_.empty() || !services_.throttler) return;
    throttle_in_flight_ = true;
    std::vector<explorer::policy::PidTransition> batch;
    batch.reserve(pending_pid_throttles_.size());
    for (const auto& [pid, throttle] : pending_pid_throttles_) {
        batch.push_back(explorer::policy::PidTransition{pid, throttle});
    }
    pending_pid_throttles_.clear();
    auto throttler = services_.throttler;
    auto tok = life_token_;
    CefPostTask(TID_FILE_BACKGROUND, new GuardedUiTask(tok, [this, tok, throttler, batch = std::move(batch)]() {
        for (const auto& act : batch) {
            throttler->setProcessIdleClass(act.pid, act.throttle);
        }
        if (tok->load()) {
            CefPostTask(TID_UI, new GuardedUiTask(tok, [this]() {
                throttle_in_flight_ = false;
                drainPendingThrottleActions();
            }));
        }
    }));
}

void BrowserWindow::requestTelemetrySampleAsync(bool full, bool from_tick) {
    CEF_REQUIRE_UI_THREAD();
    if (!life_token_->load() || !services_.telemetry) return;
    if (telemetry_in_flight_) {
        if (full) {
            telemetry_pending_full_ = true;
        }
        if (from_tick) {
            int next_ms = power_policy_.telemetryIntervalMs(hud_visible_);
            auto tok = life_token_;
            CefPostDelayedTask(TID_UI, new GuardedUiTask(tok, [this]() { onTelemetryTick(); }), next_ms);
        }
        return;
    }
    telemetry_in_flight_ = true;
    telemetry_pending_full_ = false;
    auto telemetry = services_.telemetry;
    auto tok = life_token_;
    CefPostTask(TID_FILE_BACKGROUND, new GuardedUiTask(tok, [this, tok, telemetry, full, from_tick]() {
        auto snap = full ? telemetry->sample() : telemetry->sampleLite();
        if (tok->load()) {
            CefPostTask(TID_UI, new GuardedUiTask(tok, [this, snap = std::move(snap), from_tick]() mutable {
                onTelemetrySampleReady(std::move(snap), from_tick);
            }));
        }
    }));
}

void BrowserWindow::onTelemetrySampleReady(explorer::hal::TelemetrySnapshot snap, bool from_tick) {
    CEF_REQUIRE_UI_THREAD();
    telemetry_in_flight_ = false;
    if (!life_token_->load()) return;

    last_snap_ = std::move(snap);
    if (last_snap_.on_battery != power_policy_.isOnBatteryRaw()) {
        power_policy_.setPowerSource(last_snap_.on_battery);
        syncSystemPower();
    }
    pushChromeTelemetry();
    if (hud_visible_) pushHudState(last_snap_);

    if (telemetry_pending_full_ && hud_visible_) {
        telemetry_pending_full_ = false;
        requestTelemetrySampleAsync(true, false);
    }

    if (from_tick) {
        int next_ms = power_policy_.telemetryIntervalMs(hud_visible_);
        auto tok = life_token_;
        CefPostDelayedTask(TID_UI, new GuardedUiTask(tok, [this]() { onTelemetryTick(); }), next_ms);
    }
}

void BrowserWindow::onTelemetryTick() {
    CEF_REQUIRE_UI_THREAD();
    if (!life_token_->load()) return;
    // Skip sysfs/hwmon polling while window is minimized and HUD is closed (UPower D-Bus still handles power events).
    if (window_ && window_->IsMinimized() && !hud_visible_) {
        auto tok = life_token_;
        CefPostDelayedTask(TID_UI, new GuardedUiTask(tok, [this]() { onTelemetryTick(); }), 5000);
        return;
    }
    if (services_.telemetry) {
        requestTelemetrySampleAsync(hud_visible_, true);
    } else {
        int next_ms = power_policy_.telemetryIntervalMs(hud_visible_);
        auto tok = life_token_;
        CefPostDelayedTask(TID_UI, new GuardedUiTask(tok, [this]() { onTelemetryTick(); }), next_ms);
    }
}

void BrowserWindow::scheduleSessionSave() {
    if (cfg_.disable_session || !cfg_.app_mode_url.empty() || session_dirty_ || session_flushed_on_close_) return;
    session_dirty_ = true;
    auto tok = life_token_;
    CefPostDelayedTask(TID_UI, new GuardedUiTask(tok, [this]() { onDebouncedSessionSave(); }), 600);
}

void BrowserWindow::onDebouncedSessionSave() {
    CEF_REQUIRE_UI_THREAD();
    if (!life_token_->load() || !session_dirty_ || session_flushed_on_close_) return;
    session_dirty_ = false;
    flushSessionNow();
}

void BrowserWindow::flushSessionNow() {
    if (cfg_.disable_session || !cfg_.app_mode_url.empty() || session_flushed_on_close_) return;
    SessionData data;
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].url.rfind("data:", 0) == 0) continue;
        if (tabs_[i].id == active_tab_id_) data.active_index = data.urls.size();
        data.urls.push_back(tabs_[i].url);
    }
    session_store_.saveAtomic(data);
}

BrowserWindow::TabEntry* BrowserWindow::activeTabEntry() {
    return findTab(active_tab_id_);
}

BrowserWindow::TabEntry* BrowserWindow::findTab(uint64_t tab_id) {
    for (auto& t : tabs_) {
        if (t.id == tab_id) return &t;
    }
    return nullptr;
}

}  // namespace explorer::app
