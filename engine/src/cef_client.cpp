#include "explorer/engine/cef_client.hpp"
#include "explorer/policy/navigation_policy.hpp"

#include "include/wrapper/cef_helpers.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace explorer::engine {

namespace {

explorer::shield::ResourceKind mapCefResourceType(CefRequest::ResourceType rt) {
    using namespace explorer::shield;
    switch (rt) {
        case RT_MAIN_FRAME:
        case RT_NAVIGATION_PRELOAD_MAIN_FRAME: return ResourceKind::MainFrame;
        case RT_SUB_FRAME:
        case RT_NAVIGATION_PRELOAD_SUB_FRAME: return ResourceKind::SubFrame;
        case RT_SCRIPT:
        case RT_WORKER:
        case RT_SHARED_WORKER:
        case RT_SERVICE_WORKER: return ResourceKind::Script;
        case RT_IMAGE:
        case RT_FAVICON: return ResourceKind::Image;
        case RT_STYLESHEET: return ResourceKind::Stylesheet;
        case RT_FONT_RESOURCE: return ResourceKind::Font;
        case RT_MEDIA: return ResourceKind::Media;
        case RT_OBJECT:
        case RT_PLUGIN_RESOURCE: return ResourceKind::Object;
        case RT_XHR:
        case RT_PREFETCH: return ResourceKind::XhrOrFetch;
        case RT_PING: return ResourceKind::Ping;
        case RT_CSP_REPORT: return ResourceKind::CspReport;
        default: return ResourceKind::Other;
    }
}

std::filesystem::path resolveDownloadPath(const std::string& suggested_name) {
    std::filesystem::path dir;
    if (const char* xdg_dl = std::getenv("XDG_DOWNLOAD_DIR"); xdg_dl && xdg_dl[0]) {
        dir = xdg_dl;
    } else if (const char* home = std::getenv("HOME"); home && home[0]) {
        dir = std::filesystem::path(home) / "Downloads";
    } else {
        dir = "/tmp";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::string clean = suggested_name.empty() ? "download" : suggested_name;
    auto slash = clean.find_last_of("/\\");
    if (slash != std::string::npos) clean = clean.substr(slash + 1);
    // Strip control chars, path traversal, and leading dots (prevent hidden dotfiles like .bashrc).
    std::string safe;
    safe.reserve(clean.size());
    for (unsigned char c : clean) {
        if (c < 0x20 || c == 0x7f || c == '/' || c == '\\') continue;
        safe.push_back(static_cast<char>(c));
    }
    while (!safe.empty() && safe.front() == '.') safe.erase(safe.begin());
    while (safe.find("..") != std::string::npos) {
        safe.replace(safe.find(".."), 2, "_");
    }
    if (safe.empty()) safe = "download";
    clean = std::move(safe);

    std::filesystem::path candidate = dir / clean;
    if (!std::filesystem::exists(candidate, ec)) return candidate;

    auto stem = candidate.stem().string();
    auto ext = candidate.extension().string();
    for (int i = 1; i <= 32; ++i) {
        auto next = dir / (stem + " (" + std::to_string(i) + ")" + ext);
        if (!std::filesystem::exists(next, ec)) return next;
    }
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    return dir / (stem + " (" + std::to_string(now_ms) + ")" + ext);
}

}  // namespace

ExplorerCefClient::ExplorerCefClient(
    uint64_t tab_id,
    ITabObserver* observer,
    std::shared_ptr<const explorer::shield::IContentShield> shield,
    double default_zoom_factor)
    : tab_id_(tab_id),
      observer_(observer),
      shield_(std::move(shield)),
      current_zoom_level_(explorer::policy::zoomFactorToCefLevel(default_zoom_factor)) {}

bool ExplorerCefClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> /*browser*/,
                                                 CefRefPtr<CefFrame> /*frame*/,
                                                 CefProcessId /*source_process*/,
                                                 CefRefPtr<CefProcessMessage> message) {
    CEF_REQUIRE_UI_THREAD();
    if (!observer_ || !message) return false;
    if (message->GetName() == "explorer.renderer-pid") {
        auto args = message->GetArgumentList();
        if (args && args->GetSize() >= 1) {
            pid_t pid = static_cast<pid_t>(args->GetInt(0));
            observer_->onTabRendererPid(tab_id_, pid);
            return true;
        }
    } else if (message->GetName() == "explorer.topbar-reveal") {
        auto args = message->GetArgumentList();
        if (args && args->GetSize() >= 1) {
            observer_->onTabScrollRevealTopBar(tab_id_, args->GetBool(0));
            return true;
        }
    } else if (message->GetName() == "explorer.media-state") {
        auto args = message->GetArgumentList();
        if (args && args->GetSize() >= 1) {
            observer_->onTabMediaStateChanged(tab_id_, args->GetBool(0));
            return true;
        }
    } else if (message->GetName() == "explorer.login-capture") {
        auto args = message->GetArgumentList();
        if (args && args->GetSize() >= 3) {
            observer_->onTabLoginSubmitted(args->GetString(0).ToString(),
                                           args->GetString(1).ToString(),
                                           args->GetString(2).ToString());
            return true;
        }
    }
    return false;
}

void ExplorerCefClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    if (browser && browser->GetHost()) {
        browser->GetHost()->SetZoomLevel(current_zoom_level_);
    }
    if (observer_) {
        observer_->onTabCreated(tab_id_);
    }
}

bool ExplorerCefClient::OnBeforePopup(CefRefPtr<CefBrowser> /*browser*/,
                                      CefRefPtr<CefFrame> /*frame*/,
                                      int /*popup_id*/,
                                      const CefString& target_url,
                                      const CefString& /*target_frame_name*/,
                                      cef_window_open_disposition_t /*target_disposition*/,
                                      bool /*user_gesture*/,
                                      const CefPopupFeatures& /*popupFeatures*/,
                                      CefWindowInfo& /*windowInfo*/,
                                      CefRefPtr<CefClient>& /*client*/,
                                      CefBrowserSettings& /*settings*/,
                                      CefRefPtr<CefDictionaryValue>& /*extra_info*/,
                                      bool* /*no_javascript_access*/) {
    CEF_REQUIRE_UI_THREAD();
    std::string url = target_url.ToString();
    if (observer_ && !url.empty()) {
        observer_->onTabPopupRequested(url);
        return true;  // Open cleanly in a new Explorer tab instead of an unmanaged popup
    }
    return false;
}

bool ExplorerCefClient::DoClose(CefRefPtr<CefBrowser> /*browser*/) {
    CEF_REQUIRE_UI_THREAD();
    if (observer_) {
        return observer_->onTabDoClose(tab_id_);
    }
    return false;
}

void ExplorerCefClient::OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) {
    CEF_REQUIRE_UI_THREAD();
    active_downloads_.clear();
    if (observer_) {
        observer_->onTabClosed(tab_id_);
    }
}

void ExplorerCefClient::OnAddressChange(CefRefPtr<CefBrowser> /*browser*/,
                                        CefRefPtr<CefFrame> frame,
                                        const CefString& url) {
    CEF_REQUIRE_UI_THREAD();
    if (observer_ && frame && frame->IsMain()) {
        observer_->onTabAddressChanged(tab_id_, url.ToString());
    }
}

void ExplorerCefClient::OnTitleChange(CefRefPtr<CefBrowser> /*browser*/,
                                      const CefString& title) {
    CEF_REQUIRE_UI_THREAD();
    if (observer_) {
        observer_->onTabTitleChanged(tab_id_, title.ToString());
    }
}

void ExplorerCefClient::OnFaviconURLChange(CefRefPtr<CefBrowser> /*browser*/,
                                           const std::vector<CefString>& icon_urls) {
    CEF_REQUIRE_UI_THREAD();
    if (observer_ && !icon_urls.empty()) {
        // Prefer PNG/SVG/apple-touch-icon or the last (typically highest-resolution) icon over 16x16 .ico.
        std::string chosen = icon_urls.back().ToString();
        for (const auto& u : icon_urls) {
            std::string s = u.ToString();
            if (s.find(".png") != std::string::npos || s.find(".svg") != std::string::npos) {
                chosen = std::move(s);
            }
        }
        observer_->onTabFaviconChanged(tab_id_, chosen);
    }
}

void ExplorerCefClient::OnLoadingProgressChange(CefRefPtr<CefBrowser> /*browser*/,
                                                double progress) {
    CEF_REQUIRE_UI_THREAD();
    if (observer_) {
        observer_->onTabProgressChanged(tab_id_, progress);
    }
}

void ExplorerCefClient::OnFullscreenModeChange(CefRefPtr<CefBrowser> /*browser*/,
                                               bool fullscreen) {
    CEF_REQUIRE_UI_THREAD();
    if (observer_) {
        observer_->onTabFullscreenChanged(tab_id_, fullscreen);
    }
}

void ExplorerCefClient::OnLoadingStateChange(CefRefPtr<CefBrowser> /*browser*/,
                                             bool isLoading,
                                             bool canGoBack,
                                             bool canGoForward) {
    CEF_REQUIRE_UI_THREAD();
    if (observer_) {
        observer_->onTabLoadingChanged(tab_id_, isLoading, canGoBack, canGoForward);
    }
}

void ExplorerCefClient::OnLoadStart(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    TransitionType /*transition_type*/) {
    CEF_REQUIRE_UI_THREAD();
    if (browser && frame && frame->IsMain() && browser->GetHost()) {
        // Maintain the tab's active zoom level consistently across navigations
        if (std::abs(browser->GetHost()->GetZoomLevel() - current_zoom_level_) > 1e-4) {
            browser->GetHost()->SetZoomLevel(current_zoom_level_);
        }
    }
}

bool ExplorerCefClient::OnOpenURLFromTab(CefRefPtr<CefBrowser> /*browser*/,
                                         CefRefPtr<CefFrame> /*frame*/,
                                         const CefString& target_url,
                                         cef_window_open_disposition_t target_disposition,
                                         bool /*user_gesture*/) {
    CEF_REQUIRE_UI_THREAD();
    if (observer_ &&
        (target_disposition == CEF_WOD_NEW_BACKGROUND_TAB ||
         target_disposition == CEF_WOD_NEW_FOREGROUND_TAB ||
         target_disposition == CEF_WOD_NEW_WINDOW)) {
        observer_->onTabPopupRequested(target_url.ToString());
        return true;
    }
    return false;
}

CefRefPtr<CefResourceRequestHandler> ExplorerCefClient::GetResourceRequestHandler(
    CefRefPtr<CefBrowser> /*browser*/,
    CefRefPtr<CefFrame> /*frame*/,
    CefRefPtr<CefRequest> /*request*/,
    bool /*is_navigation*/,
    bool is_download,
    const CefString& /*request_initiator*/,
    bool& /*disable_default_handling*/) {
    if (is_download) {
        return nullptr;
    }
    return this;
}

CefResourceRequestHandler::ReturnValue ExplorerCefClient::OnBeforeResourceLoad(
    CefRefPtr<CefBrowser> /*browser*/,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefCallback> /*callback*/) {
    if (!request) {
        return RV_CONTINUE;
    }
    std::string url = request->GetURL().ToString();

    // Send Global Privacy Control (Sec-GPC: 1) & Do-Not-Track (DNT: 1) on outgoing web requests.
    if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0) {
        CefRequest::HeaderMap headers;
        request->GetHeaderMap(headers);
        headers.erase("Sec-GPC");
        headers.erase("DNT");
        headers.emplace("Sec-GPC", "1");
        headers.emplace("DNT", "1");
        request->SetHeaderMap(headers);
    }

    if (!shield_) {
        return RV_CONTINUE;
    }
    std::string initiator;
    if (frame) {
        initiator = frame->GetURL().ToString();
    }
    if (initiator.empty()) {
        initiator = request->GetReferrerURL().ToString();
    }

    auto decision = shield_->evaluate(url, initiator, mapCefResourceType(request->GetResourceType()));
    return decision.block ? RV_CANCEL : RV_CONTINUE;
}

bool ExplorerCefClient::OnBeforeDownload(CefRefPtr<CefBrowser> /*browser*/,
                                         CefRefPtr<CefDownloadItem> /*download_item*/,
                                         const CefString& suggested_name,
                                         CefRefPtr<CefBeforeDownloadCallback> callback) {
    CEF_REQUIRE_UI_THREAD();
    if (!callback) return false;
    auto dest = resolveDownloadPath(suggested_name.ToString());
    callback->Continue(dest.string(), false);
    return true;
}

void ExplorerCefClient::OnDownloadUpdated(CefRefPtr<CefBrowser> /*browser*/,
                                          CefRefPtr<CefDownloadItem> download_item,
                                          CefRefPtr<CefDownloadItemCallback> callback) {
    CEF_REQUIRE_UI_THREAD();
    if (!download_item) return;

    DownloadRecord rec;
    rec.id = download_item->GetId();
    rec.full_path = download_item->GetFullPath().ToString();
    rec.filename = std::filesystem::path(rec.full_path).filename().string();
    if (rec.filename.empty()) rec.filename = download_item->GetSuggestedFileName().ToString();
    if (rec.filename.empty()) rec.filename = "download";
    rec.received_bytes = download_item->GetReceivedBytes();
    rec.total_bytes = download_item->GetTotalBytes();
    rec.percent = download_item->GetPercentComplete();
    rec.in_progress = download_item->IsInProgress();
    rec.is_complete = download_item->IsComplete();
    rec.is_canceled = download_item->IsCanceled();

    if (rec.in_progress && callback) {
        active_downloads_[rec.id] = callback;
    } else {
        active_downloads_.erase(rec.id);
    }

    if (observer_) {
        observer_->onTabDownloadUpdated(rec);
    }
}

void ExplorerCefClient::cancelDownload(uint32_t download_id) {
    CEF_REQUIRE_UI_THREAD();
    auto it = active_downloads_.find(download_id);
    if (it != active_downloads_.end() && it->second) {
        it->second->Cancel();
        active_downloads_.erase(it);
    }
}

bool ExplorerCefClient::OnPreKeyEvent(CefRefPtr<CefBrowser> /*browser*/,
                                      const CefKeyEvent& event,
                                      CefEventHandle /*os_event*/,
                                      bool* is_keyboard_shortcut) {
    CEF_REQUIRE_UI_THREAD();
    if (!observer_ || event.type != KEYEVENT_RAWKEYDOWN) {
        return false;
    }

    const bool ctrl = (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0;
    const bool shift = (event.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;
    const bool alt = (event.modifiers & EVENTFLAG_ALT_DOWN) != 0;
    const int vk = event.windows_key_code;

    auto dispatch = [&](ShortcutAction act) {
        if (is_keyboard_shortcut) *is_keyboard_shortcut = true;
        observer_->onTabShortcut(act);
        return true;
    };

    if (ctrl && !alt) {
        if (vk == 'T') return dispatch(ShortcutAction::NewTab);
        if (vk == 'W') return dispatch(ShortcutAction::CloseTab);
        if (vk == 'L') return dispatch(ShortcutAction::FocusOmnibar);
        if (vk == 'F') return dispatch(ShortcutAction::FindInPage);
        if (vk == 'Q') return dispatch(ShortcutAction::QuitApp);
        if (vk == 'R') return dispatch(shift ? ShortcutAction::HardReload : ShortcutAction::Reload);
        if (vk == 'I' && shift) return dispatch(ShortcutAction::ToggleDevTools);
        if (vk == 0x09) return dispatch(shift ? ShortcutAction::PrevTab : ShortcutAction::NextTab);  // Tab
        if (vk == 0xBB || vk == 0x6B) return dispatch(ShortcutAction::ZoomIn);                       // + / =
        if (vk == 0xBD || vk == 0x6D) return dispatch(ShortcutAction::ZoomOut);                      // -
        if (vk == '0' || vk == 0x60) return dispatch(ShortcutAction::ZoomReset);
    }
    if (alt && !ctrl) {
        if (vk == 'D') return dispatch(ShortcutAction::FocusOmnibar);
        if (vk == 0x25) return dispatch(ShortcutAction::GoBack);     // Left
        if (vk == 0x27) return dispatch(ShortcutAction::GoForward);  // Right
    }
    if (!ctrl && !alt) {
        if (vk == 0x74) return dispatch(shift ? ShortcutAction::HardReload : ShortcutAction::Reload);  // F5
        if (vk == 0x7A) return dispatch(ShortcutAction::ToggleFullscreen);                             // F11
        if (vk == 0x7B) return dispatch(ShortcutAction::ToggleDevTools);                               // F12
        if (vk == 0x1B) {
            observer_->onTabShortcut(ShortcutAction::Escape);
            return false;  // Let web page also receive Escape
        }
    }
    return false;
}

}  // namespace explorer::engine
