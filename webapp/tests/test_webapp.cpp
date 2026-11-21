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

    LinuxWebAppInstaller installer(tmp / "apps", tmp / "icons", "/usr/bin/true");
    auto app = installer.install("YouTube Music", "https://music.youtube.com", "");
    CHECK(app.app_id.rfind("explorer-app-youtube-music-", 0) == 0);
    CHECK(fs::exists(app.desktop_path));

    auto list = installer.listInstalled();
    CHECK(list.size() == 1);
    CHECK(list[0].name == "YouTube Music");
    CHECK(list[0].url == "https://music.youtube.com");

    CHECK(installer.uninstall(app.app_id));
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
