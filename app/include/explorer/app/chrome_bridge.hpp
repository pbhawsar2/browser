#pragma once
// SOLID Chrome UI Bridge: isolates trusted WebUI chrome/HUD IPC, frameless drag regions,
// and smart context menus (allows Cut/Copy/Paste inside Omnibar/Find inputs, blocks View Source on chrome).

#include "explorer/engine/engine_interfaces.hpp"

#include "include/cef_client.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_drag_handler.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_request_handler.h"

#include <string>
#include <vector>

namespace explorer::app {

class IChromeCommandSink {
public:
    virtual ~IChromeCommandSink() = default;
    virtual void onChromeCommand(const std::string& cmd, const std::string& arg) = 0;
    virtual void onChromeDraggableRegionsChanged(const std::vector<CefDraggableRegion>& regions) = 0;
    virtual void onChromeReady(bool is_hud) = 0;
    virtual void onChromeShortcut(explorer::engine::ShortcutAction action) = 0;
    virtual void onChromeBrowserCreated(bool is_hud) = 0;
    virtual void onChromeBrowserClosed(bool is_hud) = 0;
};

class ChromeUiClient : public CefClient,
                       public CefLifeSpanHandler,
                       public CefContextMenuHandler,
                       public CefDragHandler,
                       public CefLoadHandler,
                       public CefRequestHandler,
                       public CefKeyboardHandler {
public:
    ChromeUiClient(IChromeCommandSink* sink, bool is_hud)
        : sink_(sink), is_hud_(is_hud) {}

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefDragHandler> GetDragHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
    CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }

    bool OnBeforeBrowse(CefRefPtr<CefBrowser> /*browser*/,
                        CefRefPtr<CefFrame> /*frame*/,
                        CefRefPtr<CefRequest> request,
                        bool /*user_gesture*/,
                        bool /*is_redirect*/) override {
        if (!request) return true;
        std::string url = request->GetURL().ToString();
        // Defense-in-depth: block privileged Chrome/HUD views from navigating away from internal data: HTML.
        return url.rfind("data:text/html;charset=utf-8;base64,", 0) != 0;
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> /*browser*/) override {
        if (sink_) sink_->onChromeBrowserCreated(is_hud_);
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) override {
        if (sink_) sink_->onChromeBrowserClosed(is_hud_);
    }

    void OnBeforeContextMenu(CefRefPtr<CefBrowser> /*browser*/,
                             CefRefPtr<CefFrame> /*frame*/,
                             CefRefPtr<CefContextMenuParams> params,
                             CefRefPtr<CefMenuModel> model) override {
        if (!model) return;
        // Keep native Cut/Copy/Paste/Select All when right-clicking the Omnibar or Find input.
        if (params && params->IsEditable()) {
            return;
        }
        // Suppress browser "View Source / Back / Print" menu on UI buttons and HUD cards.
        model->Clear();
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> /*browser*/,
                                  CefRefPtr<CefFrame> /*frame*/,
                                  CefProcessId /*source_process*/,
                                  CefRefPtr<CefProcessMessage> message) override {
        if (!sink_ || !message) return false;
        if (message->GetName() == "explorer.chrome-cmd") {
            auto args = message->GetArgumentList();
            if (args && args->GetSize() >= 2) {
                sink_->onChromeCommand(args->GetString(0).ToString(),
                                       args->GetString(1).ToString());
                return true;
            }
        }
        return false;
    }

    void OnDraggableRegionsChanged(CefRefPtr<CefBrowser> /*browser*/,
                                   CefRefPtr<CefFrame> /*frame*/,
                                   const std::vector<CefDraggableRegion>& regions) override {
        if (sink_ && !is_hud_) {
            sink_->onChromeDraggableRegionsChanged(regions);
        }
    }

    void OnLoadEnd(CefRefPtr<CefBrowser> /*browser*/,
                   CefRefPtr<CefFrame> frame,
                   int /*httpStatusCode*/) override {
        if (sink_ && frame && frame->IsMain()) {
            sink_->onChromeReady(is_hud_);
        }
    }

    bool OnPreKeyEvent(CefRefPtr<CefBrowser> /*browser*/,
                       const CefKeyEvent& event,
                       CefEventHandle /*os_event*/,
                       bool* is_keyboard_shortcut) override {
        if (!sink_ || event.type != KEYEVENT_RAWKEYDOWN) return false;
        const bool ctrl = (event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0;
        const bool shift = (event.modifiers & EVENTFLAG_SHIFT_DOWN) != 0;
        const bool alt = (event.modifiers & EVENTFLAG_ALT_DOWN) != 0;
        const int vk = event.windows_key_code;

        auto fire = [&](explorer::engine::ShortcutAction a) {
            if (is_keyboard_shortcut) *is_keyboard_shortcut = true;
            sink_->onChromeShortcut(a);
            return true;
        };

        if (ctrl && !alt) {
            if (vk == 'T') return fire(explorer::engine::ShortcutAction::NewTab);
            if (vk == 'W') return fire(explorer::engine::ShortcutAction::CloseTab);
            if (vk == 'L') return fire(explorer::engine::ShortcutAction::FocusOmnibar);
            if (vk == 'F') return fire(explorer::engine::ShortcutAction::FindInPage);
            if (vk == 'Q') return fire(explorer::engine::ShortcutAction::QuitApp);
            if (vk == 'R') return fire(shift ? explorer::engine::ShortcutAction::HardReload
                                             : explorer::engine::ShortcutAction::Reload);
            if (vk == 'I' && shift) return fire(explorer::engine::ShortcutAction::ToggleDevTools);
            if (vk == 0x09) return fire(shift ? explorer::engine::ShortcutAction::PrevTab
                                              : explorer::engine::ShortcutAction::NextTab);
            if (vk == 0xBB || vk == 0x6B) return fire(explorer::engine::ShortcutAction::ZoomIn);
            if (vk == 0xBD || vk == 0x6D) return fire(explorer::engine::ShortcutAction::ZoomOut);
            if (vk == '0' || vk == 0x60) return fire(explorer::engine::ShortcutAction::ZoomReset);
        }
        if (alt && !ctrl) {
            if (vk == 'D') return fire(explorer::engine::ShortcutAction::FocusOmnibar);
        }
        if (!ctrl && !alt) {
            if (vk == 0x7A) return fire(explorer::engine::ShortcutAction::ToggleFullscreen);
            if (vk == 0x7B) return fire(explorer::engine::ShortcutAction::ToggleDevTools);
            if (vk == 0x1B && is_hud_) return fire(explorer::engine::ShortcutAction::Escape);
        }
        return false;
    }

    void detach() { sink_ = nullptr; }

private:
    IChromeCommandSink* sink_;
    bool is_hud_;
    IMPLEMENT_REFCOUNTING(ChromeUiClient);
};

}  // namespace explorer::app
