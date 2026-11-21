#pragma once
// CEF 158 Alloy-mode per-tab client wiring Display, LifeSpan, Load, Request->Shield, Download, Keyboard.

#include "explorer/engine/engine_interfaces.hpp"
#include "explorer/shield/content_shield.hpp"

#include "include/cef_client.h"
#include "include/cef_display_handler.h"
#include "include/cef_download_handler.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_permission_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_resource_request_handler.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace explorer::engine {

class ExplorerCefClient : public CefClient,
                          public CefLifeSpanHandler,
                          public CefDisplayHandler,
                          public CefLoadHandler,
                          public CefPermissionHandler,
                          public CefRequestHandler,
                          public CefResourceRequestHandler,
                          public CefDownloadHandler,
                          public CefKeyboardHandler {
public:
    ExplorerCefClient(uint64_t tab_id,
                      ITabObserver* observer,
                      std::shared_ptr<const explorer::shield::IContentShield> shield,
                      double default_zoom_factor);

    // CefClient
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
    CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
    CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override;

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       int popup_id,
                       const CefString& target_url,
                       const CefString& target_frame_name,
                       cef_window_open_disposition_t target_disposition,
                       bool user_gesture,
                       const CefPopupFeatures& popupFeatures,
                       CefWindowInfo& windowInfo,
                       CefRefPtr<CefClient>& client,
                       CefBrowserSettings& settings,
                       CefRefPtr<CefDictionaryValue>& extra_info,
                       bool* no_javascript_access) override;
    bool DoClose(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    // CefDisplayHandler
    void OnAddressChange(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         const CefString& url) override;
    void OnTitleChange(CefRefPtr<CefBrowser> browser,
                       const CefString& title) override;
    void OnFaviconURLChange(CefRefPtr<CefBrowser> browser,
                            const std::vector<CefString>& icon_urls) override;
    void OnLoadingProgressChange(CefRefPtr<CefBrowser> browser,
                                 double progress) override;
    void OnFullscreenModeChange(CefRefPtr<CefBrowser> browser,
                                bool fullscreen) override;

    // CefLoadHandler
    void OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                              bool isLoading,
                              bool canGoBack,
                              bool canGoForward) override;
    void OnLoadStart(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     TransitionType transition_type) override;
    void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   int httpStatusCode) override;

    // CefPermissionHandler (Alloy runtime media & web permissions: Mic, Camera, Screen, Location, Clipboard, Storage, etc.)
    bool OnRequestMediaAccessPermission(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        const CefString& requesting_origin,
        uint32_t requested_permissions,
        CefRefPtr<CefMediaAccessCallback> callback) override;
    bool OnShowPermissionPrompt(
        CefRefPtr<CefBrowser> browser,
        uint64_t prompt_id,
        const CefString& requesting_origin,
        uint32_t requested_permissions,
        CefRefPtr<CefPermissionPromptCallback> callback) override;

    // CefRequestHandler / CefResourceRequestHandler (IO thread)
    bool OnOpenURLFromTab(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          const CefString& target_url,
                          cef_window_open_disposition_t target_disposition,
                          bool user_gesture) override;
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefRequest> request,
        bool is_navigation,
        bool is_download,
        const CefString& request_initiator,
        bool& disable_default_handling) override;
    ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefFrame> frame,
                                     CefRefPtr<CefRequest> request,
                                     CefRefPtr<CefCallback> callback) override;

    // CefDownloadHandler
    bool OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefDownloadItem> download_item,
                          const CefString& suggested_name,
                          CefRefPtr<CefBeforeDownloadCallback> callback) override;
    void OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefDownloadItem> download_item,
                           CefRefPtr<CefDownloadItemCallback> callback) override;

    void cancelDownload(uint32_t download_id);
    void setZoomLevel(double cef_zoom_level) { current_zoom_level_ = cef_zoom_level; }

    // CefKeyboardHandler
    bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                       const CefKeyEvent& event,
                       CefEventHandle os_event,
                       bool* is_keyboard_shortcut) override;

    void detachObserver() { observer_ = nullptr; }

private:
    uint64_t tab_id_;
    ITabObserver* observer_;
    std::shared_ptr<const explorer::shield::IContentShield> shield_;
    double current_zoom_level_;
    std::unordered_map<uint32_t, CefRefPtr<CefDownloadItemCallback>> active_downloads_;

    IMPLEMENT_REFCOUNTING(ExplorerCefClient);
};

}  // namespace explorer::engine
