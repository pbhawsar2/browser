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

    // 1. PrefsStore defaults (adblock ON, autohide OFF for zero-stutter scroll, cookies ON, theme_mode "system")
    PrefsStore prefs_store(tmp_dir / "prefs.json");
    UserPrefs def = prefs_store.load();
    CHECK(def.adblock_enabled == true);
    CHECK(def.autohide_topbar == false);
    CHECK(def.save_cookies == true);
    CHECK(def.theme_mode == "system");

    UserPrefs custom;
    custom.adblock_enabled = false;
    custom.autohide_topbar = true;
    custom.save_cookies = false;
    custom.theme_mode = "dark";
    prefs_store.save(custom);

    UserPrefs reloaded = prefs_store.load();
    CHECK(reloaded.adblock_enabled == false);
    CHECK(reloaded.autohide_topbar == true);
    CHECK(reloaded.save_cookies == false);
    CHECK(reloaded.theme_mode == "dark");
    auto prefs_perms = fs::status(tmp_dir / "prefs.json", ec).permissions();
    CHECK((prefs_perms & fs::perms::others_all) == fs::perms::none);

    // 1b. SitePermissionsStore per-origin & global permissions roundtrip + 0600 security
    {
        SitePermissionsStore pstore(tmp_dir / "permissions.json");
        SitePermissions gdef = pstore.getForOrigin("https://gemini.google.com");
        CHECK(gdef.microphone == true);
        CHECK(gdef.camera == true);
        CHECK(gdef.location == true);
        CHECK(gdef.notifications == true);
        CHECK(gdef.clipboard == true);
        CHECK(gdef.screen_capture == true);
        CHECK(gdef.storage == true);
        CHECK(gdef.sound == true);
        CHECK(gdef.popups == true);

        CHECK(pstore.togglePermission("https://gemini.google.com", "microphone") == false);
        CHECK(pstore.togglePermission("https://gemini.google.com", "camera") == false);
        CHECK(pstore.togglePermission("https://gemini.google.com", "location") == false);
        CHECK(pstore.togglePermission("https://gemini.google.com", "sound") == false);

        SitePermissions gemini_p = pstore.getForOrigin("https://gemini.google.com");
        CHECK(gemini_p.microphone == false);
        CHECK(gemini_p.camera == false);
        CHECK(gemini_p.location == false);
        CHECK(gemini_p.sound == false);
        CHECK(gemini_p.notifications == true);
        CHECK(gemini_p.storage == true);

        // Other origins remain unaffected
        SitePermissions other_p = pstore.getForOrigin("https://meet.google.com");
        CHECK(other_p.microphone == true);
        CHECK(other_p.camera == true);
    }
    {
        // Reload from disk and verify persistence + resetForOrigin
        SitePermissionsStore pstore2(tmp_dir / "permissions.json");
        auto perm_file_perms = fs::status(tmp_dir / "permissions.json", ec).permissions();
        CHECK((perm_file_perms & fs::perms::others_all) == fs::perms::none);
        CHECK((perm_file_perms & fs::perms::group_all) == fs::perms::none);

        SitePermissions gemini_p = pstore2.getForOrigin("https://gemini.google.com");
        CHECK(gemini_p.microphone == false);
        CHECK(gemini_p.camera == false);
        CHECK(gemini_p.location == false);
        CHECK(gemini_p.sound == false);

        pstore2.resetForOrigin("https://gemini.google.com");
        SitePermissions reset_p = pstore2.getForOrigin("https://gemini.google.com");
        CHECK(reset_p.microphone == true);
        CHECK(reset_p.camera == true);
        CHECK(reset_p.location == true);
        CHECK(reset_p.sound == true);
    }

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
    auto sess_perms = fs::status(tmp_dir / "session.json", ec).permissions();
    CHECK((sess_perms & fs::perms::others_all) == fs::perms::none);
    CHECK((sess_perms & fs::perms::group_all) == fs::perms::none);

    // 3. Base64 HTML data URL generation never contains unencoded '#' (which truncates data URLs)
    std::string chrome_url = topChromeHtmlDataUrl();
    std::string hud_url = powerHudHtmlDataUrl();
    CHECK(chrome_url == topChromeHtmlDataUrl());  // Cached identity
    CHECK(hud_url == powerHudHtmlDataUrl());
    CHECK(chrome_url.rfind("data:text/html;charset=utf-8;base64,", 0) == 0);
    CHECK(hud_url.rfind("data:text/html;charset=utf-8;base64,", 0) == 0);
    CHECK(chrome_url.find('#') == std::string::npos);
    CHECK(hud_url.find('#') == std::string::npos);

    fs::remove_all(tmp_dir, ec);
    std::cout << "[test_app_stores] ALL PASSED\n";
    return 0;
}
