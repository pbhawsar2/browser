#include "explorer/app/chrome_ui.hpp"
#include "explorer/app/prefs_store.hpp"
#include "explorer/app/session_store.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>

#define CHECK(cond)                                                                           \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::cerr << "[test_app_stores] FAIL at line " << __LINE__ << ": " #cond << "\n"; \
            std::exit(1);                                                                     \
        }                                                                                     \
    } while (0)

int main() {
    namespace fs = std::filesystem;
    using namespace explorer::app;

    auto tmp_dir = fs::temp_directory_path() / "explorer_app_stores_test";
    std::error_code ec;
    fs::remove_all(tmp_dir, ec);
    fs::create_directories(tmp_dir, ec);

    // 1. PrefsStore defaults (adblock ON, autohide OFF for zero-stutter scroll, cookies ON)
    PrefsStore prefs_store(tmp_dir / "prefs.json");
    UserPrefs def = prefs_store.load();
    CHECK(def.adblock_enabled == true);
    CHECK(def.autohide_topbar == false);
    CHECK(def.save_cookies == true);

    UserPrefs custom;
    custom.adblock_enabled = false;
    custom.autohide_topbar = true;
    custom.save_cookies = true;
    prefs_store.save(custom);

    UserPrefs reloaded = prefs_store.load();
    CHECK(reloaded.adblock_enabled == false);
    CHECK(reloaded.autohide_topbar == true);
    CHECK(reloaded.save_cookies == true);

    // 2. SessionStore atomic write + active index preservation
    SessionStore sess_store(tmp_dir / "session.json");
    SessionData sd;
    sd.urls = {"https://example.com", "https://github.com/trending", "https://www.youtube.com"};
    sd.active_index = 1;
    sess_store.saveAtomic(sd);

    SessionData sd_loaded = sess_store.load();
    CHECK(sd_loaded.urls.size() == 3);
    CHECK(sd_loaded.urls[0] == "https://example.com");
    CHECK(sd_loaded.urls[1] == "https://github.com/trending");
    CHECK(sd_loaded.active_index == 1);

    // 3. Base64 HTML data URL generation never contains unencoded '#' (which truncates data URLs)
    std::string chrome_url = topChromeHtmlDataUrl();
    std::string hud_url = powerHudHtmlDataUrl();
    CHECK(chrome_url.rfind("data:text/html;charset=utf-8;base64,", 0) == 0);
    CHECK(hud_url.rfind("data:text/html;charset=utf-8;base64,", 0) == 0);
    CHECK(chrome_url.find('#') == std::string::npos);
    CHECK(hud_url.find('#') == std::string::npos);

    fs::remove_all(tmp_dir, ec);
    std::cout << "[test_app_stores] ALL PASSED\n";
    return 0;
}
