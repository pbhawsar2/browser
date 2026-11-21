#include "explorer/webapp/webapp_installer.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::cerr << "[test_webapp] FAIL at line " << __LINE__ << ": " #cond << "\n"; \
            std::exit(1);                                                                \
        }                                                                                \
    } while (0)

int main() {
    using namespace explorer::webapp;
    namespace fs = std::filesystem;

    fs::path tmp = fs::temp_directory_path() / "explorer_v2_test_webapp";
    fs::remove_all(tmp);

    fs::path webapps_data = tmp / "profile" / "webapps";
    LinuxWebAppInstaller installer(tmp / "apps", tmp / "icons", "/usr/bin/true", webapps_data);
    auto app = installer.install("YouTube Music", "https://music.youtube.com", "");
    CHECK(app.app_id.rfind("explorer-app-youtube-music-", 0) == 0);
    CHECK(fs::exists(app.desktop_path));

    // Install a second instance of the same URL with a different account name + query parameters:
    // must get a distinct app_id, separate isolated profile directory, and pass desktop-file-validate!
    auto app2 = installer.install("YouTube Music Work", "https://music.youtube.com/watch?v=abc&hl=en-IN", "");
    CHECK(!app2.app_id.empty());
    CHECK(app2.app_id != app.app_id);
    CHECK(fs::exists(app2.desktop_path));
    if (std::system("command -v desktop-file-validate >/dev/null 2>&1") == 0) {
        std::string cmd = "desktop-file-validate \"" + app2.desktop_path.string() + "\"";
        CHECK(std::system(cmd.c_str()) == 0);
    }

    // Simulate isolated WebApp data (caches, cookies, sign-in vault, localStorage, permissions)
    fs::path app1_data = webapps_data / app.app_id;
    fs::path app2_data = webapps_data / app2.app_id;
    fs::create_directories(app1_data / "Cache");
    fs::create_directories(app1_data / "Local Storage");
    fs::create_directories(app2_data / "Cache");
    {
        std::ofstream(app1_data / "Cookies") << "session_token=account_1";
        std::ofstream(app1_data / "vault.enc") << "encrypted_vault_1";
        std::ofstream(app1_data / "Cache" / "data_0") << "cached_bytes_1";
        std::ofstream(app2_data / "Cookies") << "session_token=account_2";
    }
    CHECK(fs::exists(app1_data / "Cookies"));
    CHECK(fs::exists(app1_data / "vault.enc"));
    CHECK(fs::exists(app2_data / "Cookies"));

    auto list = installer.listInstalled();
    CHECK(list.size() == 2);
    CHECK(list[0].name == "YouTube Music");
    CHECK(list[0].url == "https://music.youtube.com");
    CHECK(list[1].name == "YouTube Music Work");

    // Uninstalling app1 must purge 100% of app1's caches, cookies, vault, and desktop entry
    // while leaving app2's isolated data completely intact!
    CHECK(installer.uninstall(app.app_id));
    CHECK(!fs::exists(app.desktop_path));
    CHECK(!fs::exists(app1_data));
    CHECK(fs::exists(app2_data / "Cookies"));

    CHECK(installer.uninstall(app2.app_id));
    CHECK(!fs::exists(app2_data));
    CHECK(installer.listInstalled().empty());

    // Security: reject path traversal in uninstall & dangerous/injected URLs in install
    CHECK(!installer.uninstall("explorer-app-../../etc/passwd"));
    CHECK(!installer.uninstall("explorer-app-foo/bar"));
    auto bad1 = installer.install("Evil", "javascript:alert(1)", "");
    CHECK(bad1.app_id.empty());
    auto bad2 = installer.install("Evil2", "https://example.com/a b;rm -rf /", "");
    CHECK(bad2.app_id.empty());

    // Unwritable destination returns empty InstalledWebApp (never reports false success)
    fs::path file_blocking_dir = tmp / "not_a_dir";
    {
        std::ofstream f(file_blocking_dir);
        f << "x";
    }
    LinuxWebAppInstaller broken_installer(file_blocking_dir / "apps", tmp / "icons", "/usr/bin/true");
    auto failed_app = broken_installer.install("Blocked", "https://example.com", "");
    CHECK(failed_app.app_id.empty());

    fs::remove_all(tmp);
    std::cout << "[test_webapp] ALL PASSED (install/list/uninstall roundtrip)\n";
    return 0;
}
