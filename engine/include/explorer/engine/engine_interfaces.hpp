#pragma once
// SOLID Dependency Inversion: Engine-agnostic interfaces separating UI from CEF/Blink/V8.

#include <sys/types.h>
#include <cstdint>
#include <string>

namespace explorer::engine {

enum class ShortcutAction {
    NewTab,
    CloseTab,
    NextTab,
    PrevTab,
    FocusOmnibar,
    FindInPage,
    Reload,
    HardReload,
    GoBack,
    GoForward,
    ZoomIn,
    ZoomOut,
    ZoomReset,
    ToggleFullscreen,
    ToggleDevTools,
    QuitApp,
    Escape,
};

struct DownloadRecord {
    uint32_t id = 0;
    std::string filename;
    std::string full_path;
    int64_t received_bytes = 0;
    int64_t total_bytes = 0;
    int percent = 0;
    bool in_progress = false;
    bool is_complete = false;
    bool is_canceled = false;
};

/// Observer interface implemented by the UI shell (depends on abstraction, not CEF internals).
class ITabObserver {
public:
    virtual ~ITabObserver() = default;
    virtual void onTabAddressChanged(uint64_t tab_id, const std::string& url) = 0;
    virtual void onTabTitleChanged(uint64_t tab_id, const std::string& title) = 0;
    virtual void onTabFaviconChanged(uint64_t tab_id, const std::string& favicon_url) = 0;
    virtual void onTabProgressChanged(uint64_t tab_id, double progress) = 0;
    virtual void onTabLoadingChanged(uint64_t tab_id, bool is_loading, bool can_back, bool can_fwd) = 0;
    virtual void onTabFullscreenChanged(uint64_t tab_id, bool fullscreen) = 0;
    virtual void onTabPopupRequested(const std::string& url) = 0;
    virtual void onTabRendererPid(uint64_t tab_id, pid_t pid) = 0;
    virtual void onTabShortcut(ShortcutAction action) = 0;
    virtual void onTabDownloadUpdated(const DownloadRecord& record) = 0;
    virtual void onTabScrollRevealTopBar(uint64_t tab_id, bool reveal) = 0;
    virtual void onTabLoginSubmitted(const std::string& origin,
                                     const std::string& username,
                                     const std::string& password) = 0;
    virtual void onTabCreated(uint64_t tab_id) = 0;
    /// Called by CefLifeSpanHandler::DoClose. Return true to suppress top-level window close
    /// when closing a single tab cleanly inside a multi-tab window.
    virtual bool onTabDoClose(uint64_t tab_id) = 0;
    virtual void onTabClosed(uint64_t tab_id) = 0;
};

}  // namespace explorer::engine
