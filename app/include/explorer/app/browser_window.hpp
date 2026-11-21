#pragma once
// Explorer v2 — Frameless Obsidian CSD Browser Window + Chromeless WebApp Mode
// Wires SOLID modules: HAL + Policy + Brave Shield + AES-256-GCM Vault + Downloads + WebApp Installer
// + Auto-Hide Top Bar on Scroll.

#include "explorer/app/chrome_bridge.hpp"
#include "explorer/app/prefs_store.hpp"
#include "explorer/app/session_store.hpp"
#include "explorer/engine/cef_client.hpp"
#include "explorer/engine/engine_interfaces.hpp"
#include "explorer/hal/display_refresh.hpp"
#include "explorer/hal/power_governor.hpp"
#include "explorer/hal/process_throttler.hpp"
#include "explorer/hal/telemetry.hpp"
#include "explorer/policy/power_policy.hpp"
#include "explorer/policy/throttle_planner.hpp"
#include "explorer/shield/content_shield.hpp"
#include "explorer/vault/password_vault.hpp"
#include "explorer/webapp/webapp_installer.hpp"

#include "include/views/cef_browser_view.h"
#include "include/views/cef_overlay_controller.h"
#include "include/views/cef_panel.h"
#include "include/views/cef_window.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace explorer::app {

struct WindowLaunchConfig {
    std::vector<std::string> initial_urls;
    std::string app_mode_url;
    std::string app_id;
    std::filesystem::path profile_dir;
    double default_zoom = 1.05;
    bool disable_session = false;
};

struct RuntimeServices {
    explorer::hal::HardwareProfile hw;
    std::shared_ptr<explorer::hal::IPowerGovernor> power_gov;
    std::shared_ptr<explorer::hal::IDisplayRefreshCtrl> display_ctrl;
    std::shared_ptr<explorer::hal::IProcessThrottler> throttler;
    std::shared_ptr<explorer::hal::ITelemetryMonitor> telemetry;
    std::shared_ptr<explorer::shield::IContentShield> shield;
    std::shared_ptr<explorer::vault::IPasswordVault> vault;
    std::shared_ptr<explorer::webapp::IWebAppInstaller> webapp_installer;
};

class BrowserWindow final : public explorer::engine::ITabObserver,
                            public IChromeCommandSink {
public:
    BrowserWindow(WindowLaunchConfig cfg, RuntimeServices services);
    ~BrowserWindow() override;

    void show();
    void openExternalUrls(const std::vector<std::string>& urls);
    void activateWindow();

    // ITabObserver (from web content tabs)
    void onTabAddressChanged(uint64_t tab_id, const std::string& url) override;
    void onTabTitleChanged(uint64_t tab_id, const std::string& title) override;
    void onTabFaviconChanged(uint64_t tab_id, const std::string& favicon_url) override;
    void onTabProgressChanged(uint64_t tab_id, double progress) override;
    void onTabLoadingChanged(uint64_t tab_id, bool is_loading, bool can_back, bool can_fwd) override;
    void onTabFullscreenChanged(uint64_t tab_id, bool fullscreen) override;
    void onTabPopupRequested(const std::string& url) override;
    void onTabRendererPid(uint64_t tab_id, pid_t pid) override;
    void onTabShortcut(explorer::engine::ShortcutAction action) override;
    void onTabDownloadUpdated(const explorer::engine::DownloadRecord& record) override;
    void onTabScrollRevealTopBar(uint64_t tab_id, bool reveal) override;
    void onTabMediaStateChanged(uint64_t tab_id, bool playing) override;
    void onTabLoginSubmitted(const std::string& origin,
                             const std::string& username,
                             const std::string& password) override;
    void onTabCreated(uint64_t tab_id) override;
    bool onTabDoClose(uint64_t tab_id) override;
    void onTabClosed(uint64_t tab_id) override;

    // IChromeCommandSink (from trusted WebUI top bar & Control Overlay)
    void onChromeCommand(const std::string& cmd, const std::string& arg) override;
    void onChromeDraggableRegionsChanged(const std::vector<CefDraggableRegion>& regions) override;
    void onChromeReady(bool is_hud) override;
    void onChromeShortcut(explorer::engine::ShortcutAction action) override;
    void onChromeBrowserCreated(bool is_hud) override;
    void onChromeBrowserClosed(bool is_hud) override;

    // CefWindowDelegate callbacks
    void handleWindowCreated(CefRefPtr<CefWindow> window);
    void handleWindowDestroyed();
    bool handleCanClose();
    bool handleAccelerator(int command_id);
    int desiredChromeHeight() const;
    std::string linuxAppId() const { return cfg_.app_id.empty() ? "explorer" : cfg_.app_id; }

    void onPowerSourceChangedFromThread(bool on_battery);
    void onFilterUpdateFinished(bool ok, size_t total_rules);
    void onWebAppInstallFinished(bool ok, std::string app_name);
    void onVaultRevealAuthFinished(bool ok, std::string origin, std::string username);
    void onTelemetryTick();
    void onDebouncedSessionSave();
    void finishRetireTab(uint64_t tab_id);

private:
    struct TabEntry {
        uint64_t id = 0;
        std::string url;
        std::string title = "New Tab";
        std::string favicon;
        double progress = 0.0;
        double zoom_factor = 1.05;
        bool loading = false;
        bool can_back = false;
        bool can_fwd = false;
        bool playing_media = false;
        CefRefPtr<explorer::engine::ExplorerCefClient> client;
        CefRefPtr<CefBrowserView> view;
    };

    void openNewTab(const std::string& url, bool activate);
    void activateTab(uint64_t tab_id);
    void closeTab(uint64_t tab_id);
    void cycleTab(int delta);
    void pushChromeState();
    void pushChromeTelemetry();
    void pushChromeProgress(bool loading, double progress);
    void pushHudState(const explorer::hal::TelemetrySnapshot& snap);
    void updateChromeLayout(bool force = false);
    void ensureHudCreated();
    void setHudVisible(bool visible, const std::string& tab = "");
    void setFindBarVisible(bool visible);
    void setStatusBanner(const std::string& msg);
    void syncSystemPower();
    void reconcileRendererPriorities();
    void scheduleSessionSave();
    void flushSessionNow();
    void checkFinishShutdown();
    TabEntry* activeTabEntry();
    TabEntry* findTab(uint64_t tab_id);

    WindowLaunchConfig cfg_;
    RuntimeServices services_;
    explorer::policy::PowerPolicyEngine power_policy_;
    explorer::policy::PidThrottlePlanner throttle_planner_;
    SessionStore session_store_;
    PrefsStore prefs_store_;
    UserPrefs prefs_;
    explorer::hal::TelemetrySnapshot last_snap_;

    CefRefPtr<CefWindow> window_;
    CefRefPtr<CefPanel> root_panel_;
    CefRefPtr<CefPanel> content_panel_;

    CefRefPtr<ChromeUiClient> chrome_client_;
    CefRefPtr<CefBrowserView> chrome_view_;
    CefRefPtr<ChromeUiClient> hud_client_;
    CefRefPtr<CefBrowserView> hud_view_;
    CefRefPtr<CefOverlayController> hud_overlay_;

    std::vector<TabEntry> tabs_;
    std::unordered_map<uint64_t, TabEntry> retiring_tabs_;
    std::vector<explorer::engine::DownloadRecord> downloads_;
    std::vector<explorer::webapp::InstalledWebApp> cached_webapps_;
    bool webapps_cache_valid_ = false;
    std::optional<explorer::vault::Credential> pending_login_;
    std::string status_msg_;
    std::string pending_hud_tab_;
    std::string active_hud_tab_ = "power";
    std::string last_telemetry_sig_;
    std::string last_hud_sig_;

    uint64_t active_tab_id_ = 0;
    uint64_t next_tab_id_ = 1;
    int last_chrome_height_ = -1;
    double last_pushed_progress_ = -1.0;
    bool chrome_ready_ = false;
    bool hud_ready_ = false;
    bool hud_visible_ = false;
    bool find_visible_ = false;
    bool scroll_hidden_ = false;
    bool html_fullscreen_ = false;
    bool closing_ = false;
    bool session_dirty_ = false;
    bool session_flushed_on_close_ = false;
    std::shared_ptr<std::atomic<bool>> life_token_ = std::make_shared<std::atomic<bool>>(true);
};

}  // namespace explorer::app
