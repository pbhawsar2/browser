#include "explorer/webapp/webapp_installer.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

int main() {
    using namespace explorer::webapp;
    namespace fs = std::filesystem;

    fs::path tmp = fs::temp_directory_path() / "explorer_v2_test_webapp";
    fs::remove_all(tmp);

    LinuxWebAppInstaller installer(tmp / "apps", tmp / "icons", "/usr/bin/true");
    auto app = installer.install("YouTube Music", "https://music.youtube.com", "");
    assert(app.app_id.rfind("explorer-app-youtube-music-", 0) == 0);
    assert(fs::exists(app.desktop_path));

    auto list = installer.listInstalled();
    assert(list.size() == 1);
    assert(list[0].name == "YouTube Music");
    assert(list[0].url == "https://music.youtube.com");

    assert(installer.uninstall(app.app_id));
    assert(installer.listInstalled().empty());

    // Security: reject path traversal in uninstall & dangerous/injected URLs in install
    assert(!installer.uninstall("explorer-app-../../etc/passwd"));
    assert(!installer.uninstall("explorer-app-foo/bar"));
    auto bad1 = installer.install("Evil", "javascript:alert(1)", "");
    assert(bad1.app_id.empty());
    auto bad2 = installer.install("Evil2", "https://example.com/a b;rm -rf /", "");
    assert(bad2.app_id.empty());

    fs::remove_all(tmp);
    std::cout << "[test_webapp] ALL PASSED (install/list/uninstall roundtrip)\n";
    return 0;
}
