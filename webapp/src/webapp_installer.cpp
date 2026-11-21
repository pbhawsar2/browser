#include "explorer/webapp/webapp_installer.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>

namespace explorer::webapp {

namespace fs = std::filesystem;

namespace {

constexpr const char* kPrefix = "explorer-app-";

std::string sanitizeDesktopValue(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\0') continue;
        out.push_back(c);
    }
    return out;
}

std::string slugify(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
        } else if (!out.empty() && out.back() != '-') {
            out.push_back('-');
        }
        if (out.size() >= 28) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "webapp" : out;
}

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

}  // namespace

std::string LinuxWebAppInstaller::makeAppId(const std::string& name, const std::string& url) {
    std::size_t h = std::hash<std::string>{}(url);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%08x", static_cast<unsigned int>(h & 0xFFFFFFFFU));
    return std::string(kPrefix) + slugify(name) + "-" + hex;
}

LinuxWebAppInstaller::LinuxWebAppInstaller(fs::path apps_dir,
                                           fs::path icons_dir,
                                           fs::path browser_bin)
    : apps_dir_(std::move(apps_dir)),
      icons_dir_(std::move(icons_dir)),
      browser_bin_(std::move(browser_bin)) {}

InstalledWebApp LinuxWebAppInstaller::install(const std::string& name,
                                              const std::string& url,
                                              const std::string& favicon_url) const {
    std::error_code ec;
    fs::create_directories(apps_dir_, ec);

    std::string clean_name = sanitizeDesktopValue(name.empty() ? "Web App" : name);
    std::string clean_url = sanitizeDesktopValue(url);
    std::string app_id = makeAppId(clean_name, clean_url);
    std::string icon_name = "io.explorer.Browser";

    // Best-effort favicon download & crisp 256px PNG icon generation (falls back to Explorer icon).
    if (!favicon_url.empty() &&
        (favicon_url.rfind("https://", 0) == 0 || favicon_url.rfind("http://", 0) == 0)) {
        fs::path png_dir = icons_dir_ / "256x256" / "apps";
        fs::create_directories(png_dir, ec);
        fs::path tmp_raw = fs::temp_directory_path() / (app_id + ".raw");
        fs::path dest_png = png_dir / (app_id + ".png");

        std::string cmd = "curl -fsSL --max-time 4 " + shellQuote(favicon_url) +
                          " -o " + shellQuote(tmp_raw.string()) +
                          " && magick " + shellQuote(tmp_raw.string() + "[0]") +
                          " -background none -resize 256x256 " + shellQuote(dest_png.string()) +
                          " >/dev/null 2>&1";
        if (std::system(cmd.c_str()) == 0 && fs::exists(dest_png, ec) && fs::file_size(dest_png, ec) > 100) {
            icon_name = app_id;
        }
        fs::remove(tmp_raw, ec);
    }

    fs::path desktop_file = apps_dir_ / (app_id + ".desktop");
    {
        std::ofstream out(desktop_file, std::ios::trunc);
        out << "[Desktop Entry]\n"
            << "Version=1.0\n"
            << "Type=Application\n"
            << "Name=" << clean_name << "\n"
            << "Comment=" << clean_name << " (Explorer Web App)\n"
            << "Exec=" << browser_bin_.string() << " --app=" << clean_url << " --app-id=" << app_id << "\n"
            << "Icon=" << icon_name << "\n"
            << "Terminal=false\n"
            << "Categories=Network;WebBrowser;\n"
            << "StartupNotify=true\n"
            << "StartupWMClass=" << app_id << "\n"
            << "X-Explorer-WebApp-URL=" << clean_url << "\n";
    }

    std::string refresh = "gtk-update-icon-cache -f -t " + shellQuote(icons_dir_.string()) +
                          " >/dev/null 2>&1; update-desktop-database " + shellQuote(apps_dir_.string()) +
                          " >/dev/null 2>&1 || true";
    (void)std::system(refresh.c_str());

    return InstalledWebApp{app_id, clean_name, clean_url, icon_name, desktop_file};
}

std::vector<InstalledWebApp> LinuxWebAppInstaller::listInstalled() const {
    std::vector<InstalledWebApp> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(apps_dir_, ec)) {
        auto fname = entry.path().filename().string();
        if (fname.rfind(kPrefix, 0) != 0 || entry.path().extension() != ".desktop") continue;

        InstalledWebApp app;
        app.app_id = entry.path().stem().string();
        app.desktop_path = entry.path();
        std::ifstream in(entry.path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("Name=", 0) == 0) app.name = line.substr(5);
            else if (line.rfind("Icon=", 0) == 0) app.icon_name = line.substr(5);
            else if (line.rfind("X-Explorer-WebApp-URL=", 0) == 0) app.url = line.substr(22);
        }
        if (!app.name.empty() && !app.url.empty()) {
            out.push_back(std::move(app));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const InstalledWebApp& a, const InstalledWebApp& b) { return a.name < b.name; });
    return out;
}

bool LinuxWebAppInstaller::uninstall(const std::string& app_id) const {
    if (app_id.rfind(kPrefix, 0) != 0) return false;
    std::error_code ec;
    bool removed = fs::remove(apps_dir_ / (app_id + ".desktop"), ec);
    fs::remove(icons_dir_ / "256x256" / "apps" / (app_id + ".png"), ec);
    return removed;
}

void LinuxWebAppInstaller::launch(const InstalledWebApp& app) const {
    pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        std::string app_arg = "--app=" + app.url;
        std::string id_arg = "--app-id=" + app.app_id;
        ::execl(browser_bin_.c_str(), browser_bin_.c_str(), app_arg.c_str(), id_arg.c_str(), nullptr);
        ::_exit(127);
    }
}

}  // namespace explorer::webapp
