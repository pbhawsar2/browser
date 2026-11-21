#include "explorer/webapp/webapp_installer.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

extern char** environ;

namespace explorer::webapp {

namespace fs = std::filesystem;

namespace {

constexpr const char* kPrefix = "explorer-app-";

std::string sanitizeDesktopValue(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) continue;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

bool isValidHttpUrl(const std::string& url) {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) return false;
    for (unsigned char c : url) {
        if (c <= 0x20 || c == 0x7f || c == '"' || c == '\'' || c == '`' || c == '\\') return false;
    }
    return url.size() <= 2048;
}

bool isValidAppId(const std::string& app_id) {
    if (app_id.rfind(kPrefix, 0) != 0 || app_id.size() > 80) return false;
    for (unsigned char c : app_id) {
        if (!(std::isalnum(c) || c == '-')) return false;
    }
    return true;
}

/// Escapes `%` as `%%` and quotes arguments containing spaces per FreeDesktop Exec= specification.
std::string escapeDesktopExecArg(const std::string& s) {
    bool needs_quote = (s.find_first_of(" \t\"\\") != std::string::npos);
    std::string out;
    out.reserve(s.size() + 12);
    if (needs_quote) out.push_back('"');
    for (char c : s) {
        if (c == '%') out += "%%";
        else if (needs_quote && (c == '"' || c == '\\' || c == '$' || c == '`')) {
            out.push_back('\\');
            out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    if (needs_quote) out.push_back('"');
    return out;
}

/// Executes an external helper directly via posix_spawnp (ZERO /bin/sh shell invocation, async-signal-safe in MT process).
bool runProcessNoShell(const std::vector<std::string>& argv) {
    if (argv.empty()) return false;
    std::vector<char*> cargs;
    cargs.reserve(argv.size() + 1);
    for (const auto& a : argv) cargs.push_back(const_cast<char*>(a.c_str()));
    cargs.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) return false;
    ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid = -1;
    int rc = ::posix_spawnp(&pid, cargs[0], &actions, nullptr, cargs.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    if (rc != 0 || pid <= 0) return false;

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
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

std::string extractOriginUrl(const std::string& url) {
    auto scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) return "";
    auto slash_pos = url.find('/', scheme_pos + 3);
    return (slash_pos == std::string::npos) ? url : url.substr(0, slash_pos);
}

/// Detects downloaded image magic bytes so ImageMagick never misinterprets `.raw` as an Adobe DNG camera file!
std::string detectImageExtension(const fs::path& file_path) {
    std::ifstream in(file_path, std::ios::binary);
    if (!in.is_open()) return "";
    unsigned char hdr[64]{};
    in.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    auto n = in.gcount();
    if (n < 4) return "";

    if (hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G') return ".png";
    if (hdr[0] == 0x00 && hdr[1] == 0x00 && (hdr[2] == 0x01 || hdr[2] == 0x02) && hdr[3] == 0x00) return ".ico";
    if (hdr[0] == 0xFF && hdr[1] == 0xD8 && hdr[2] == 0xFF) return ".jpg";
    if (hdr[0] == 'G' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == '8') return ".gif";
    if (n >= 12 && hdr[0] == 'R' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == 'F' &&
        hdr[8] == 'W' && hdr[9] == 'E' && hdr[10] == 'B' && hdr[11] == 'P') {
        return ".webp";
    }
    std::string prefix(reinterpret_cast<const char*>(hdr), static_cast<size_t>(n));
    if (prefix.find("<svg") != std::string::npos || prefix.find("<?xml") != std::string::npos) {
        if (prefix.find("<html") == std::string::npos && prefix.find("<!DOCTYPE html") == std::string::npos) {
            return ".svg";
        }
    }
    return "";
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
    std::string clean_url = sanitizeDesktopValue(url);
    if (!isValidHttpUrl(clean_url)) {
        return InstalledWebApp{};
    }

    std::error_code ec;
    fs::create_directories(apps_dir_, ec);

    std::string clean_name = sanitizeDesktopValue(name.empty() ? "Web App" : name);
    std::string app_id = makeAppId(clean_name, clean_url);
    std::string icon_name = "io.explorer.Browser";

    // Best-effort favicon download & crisp multi-size PNG icon generation via posix_spawnp (ZERO shell!).
    std::vector<std::string> icon_candidates;
    auto pushUniqueCandidate = [&](const std::string& u) {
        if (u.empty()) return;
        if (std::find(icon_candidates.begin(), icon_candidates.end(), u) == icon_candidates.end()) {
            icon_candidates.push_back(u);
        }
    };
    if (isValidHttpUrl(favicon_url)) {
        pushUniqueCandidate(favicon_url);
        std::string origin = extractOriginUrl(clean_url);
        if (!origin.empty()) {
            pushUniqueCandidate(origin + "/apple-touch-icon.png");
            pushUniqueCandidate(origin + "/favicon.ico");
            pushUniqueCandidate("https://www.google.com/s2/favicons?sz=256&domain_url=" + origin);
        }
    }

    if (!icon_candidates.empty()) {
        fs::path png_dir = icons_dir_ / "256x256" / "apps";
        fs::create_directories(png_dir, ec);
        fs::path dest_png = png_dir / (app_id + ".png");

        for (const auto& cand_url : icon_candidates) {
            if (!isValidHttpUrl(cand_url)) continue;
            // Create a private, unpredictable temp directory (0700) to avoid /tmp symlink races.
            std::string dir_tmpl = (fs::temp_directory_path() / "explorer-icon-XXXXXX").string();
            char* tmp_dir_c = ::mkdtemp(dir_tmpl.data());
            if (!tmp_dir_c) continue;
            fs::path tmp_dir(tmp_dir_c);
            fs::path tmp_dl = tmp_dir / "icon.dl";
            bool fetched = runProcessNoShell(
                {"curl", "-fsSL", "--proto", "=http,https", "--max-time", "5", "--max-filesize", "2097152",
                 "-A", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36",
                 cand_url, "-o", tmp_dl.string()});
            std::string ext = fetched ? detectImageExtension(tmp_dl) : "";
            if (!ext.empty()) {
                fs::path tmp_typed = tmp_dir / ("icon" + ext);
                fs::rename(tmp_dl, tmp_typed, ec);
                // For multi-layer .ico files, [-1] selects the highest-resolution frame (e.g. 48x48/64x64 instead of 16x16).
                std::string frame_sel = (ext == ".ico") ? "[-1]" : "[0]";
                fs::path p128 = icons_dir_ / "128x128" / "apps" / (app_id + ".png");
                fs::path p64 = icons_dir_ / "64x64" / "apps" / (app_id + ".png");
                fs::path p48 = icons_dir_ / "48x48" / "apps" / (app_id + ".png");
                for (const char* sz : {"128x128", "64x64", "48x48"}) {
                    fs::create_directories(icons_dir_ / sz / "apps", ec);
                }
                // Single ImageMagick invocation generates all 4 hicolor sizes (4x fewer process spawns).
                bool converted = runProcessNoShell({
                    "magick", tmp_typed.string() + frame_sel, "-background", "none",
                    "(", "+clone", "-resize", "256x256", "-write", dest_png.string(), "+delete", ")",
                    "(", "+clone", "-resize", "128x128", "-write", p128.string(), "+delete", ")",
                    "(", "+clone", "-resize", "64x64", "-write", p64.string(), "+delete", ")",
                    "-resize", "48x48", p48.string()
                });
                if (converted && fs::exists(dest_png, ec) && fs::file_size(dest_png, ec) > 100) {
                    icon_name = app_id;
                    fs::remove_all(tmp_dir, ec);
                    break;
                }
            }
            fs::remove_all(tmp_dir, ec);
        }
    }

    fs::path desktop_file = apps_dir_ / (app_id + ".desktop");
    {
        std::ofstream out(desktop_file, std::ios::trunc);
        if (!out.is_open()) {
            return InstalledWebApp{};
        }
        out << "[Desktop Entry]\n"
            << "Version=1.0\n"
            << "Type=Application\n"
            << "Name=" << clean_name << "\n"
            << "Comment=" << clean_name << " (Explorer Web App)\n"
            << "Exec=" << escapeDesktopExecArg(browser_bin_.string())
            << " --app=" << escapeDesktopExecArg(clean_url)
            << " --app-id=" << app_id << "\n"
            << "Icon=" << icon_name << "\n"
            << "Terminal=false\n"
            << "Categories=Network;WebBrowser;\n"
            << "StartupNotify=true\n"
            << "StartupWMClass=" << app_id << "\n"
            << "X-Explorer-WebApp-URL=" << clean_url << "\n";
        out.flush();
        if (!out.good()) {
            return InstalledWebApp{};
        }
    }

    runProcessNoShell({"gtk-update-icon-cache", "-f", "-t", icons_dir_.string()});
    runProcessNoShell({"update-desktop-database", apps_dir_.string()});

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
        if (!isValidAppId(app.app_id)) continue;
        app.desktop_path = entry.path();
        std::ifstream in(entry.path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("Name=", 0) == 0) app.name = line.substr(5);
            else if (line.rfind("Icon=", 0) == 0) app.icon_name = line.substr(5);
            else if (line.rfind("X-Explorer-WebApp-URL=", 0) == 0) app.url = line.substr(22);
        }
        if (!app.name.empty() && isValidHttpUrl(app.url)) {
            out.push_back(std::move(app));
        }
    }
    std::sort(out.begin(), out.end(),
              [](const InstalledWebApp& a, const InstalledWebApp& b) { return a.name < b.name; });
    return out;
}

bool LinuxWebAppInstaller::uninstall(const std::string& app_id) const {
    // Strictly reject any path traversal (e.g., explorer-app-../../foo).
    if (!isValidAppId(app_id)) return false;
    std::error_code ec;
    bool removed = fs::remove(apps_dir_ / (app_id + ".desktop"), ec);
    for (const char* sz : {"256x256", "128x128", "64x64", "48x48"}) {
        fs::remove(icons_dir_ / sz / "apps" / (app_id + ".png"), ec);
    }
    if (removed) {
        runProcessNoShell({"gtk-update-icon-cache", "-f", "-t", icons_dir_.string()});
        runProcessNoShell({"update-desktop-database", apps_dir_.string()});
    }
    return removed;
}

void LinuxWebAppInstaller::launch(const InstalledWebApp& app) const {
    if (!isValidAppId(app.app_id) || !isValidHttpUrl(app.url)) return;
    // Pre-allocate strings BEFORE fork() so the child performs only async-signal-safe syscalls,
    // and double-fork + waitpid so the launched WebApp never leaves a zombie process.
    std::string bin_str = browser_bin_.string();
    std::string app_arg = "--app=" + app.url;
    std::string id_arg = "--app-id=" + app.app_id;
    const char* bin = bin_str.c_str();
    const char* arg1 = app_arg.c_str();
    const char* arg2 = id_arg.c_str();

    pid_t pid = ::fork();
    if (pid < 0) return;
    if (pid == 0) {
        ::setsid();
        pid_t grandchild = ::fork();
        if (grandchild == 0) {
            ::execl(bin, bin, arg1, arg2, nullptr);
            ::_exit(127);
        }
        ::_exit(0);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

}  // namespace explorer::webapp
