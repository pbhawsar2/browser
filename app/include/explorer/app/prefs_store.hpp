#pragma once
// Persistent user preferences (adblock toggle, auto-hide top bar on scroll, cookie persistence,
// system/dark/light theme mode) and per-origin Site Permissions store (microphone, camera,
// location, notifications, clipboard, screen capture, persistent storage, sound, popups).

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace explorer::app {

namespace detail {

inline void writeAtomicFile0600(const std::filesystem::path& file_path, const std::string& payload) {
    std::error_code ec;
    std::filesystem::create_directories(file_path.parent_path(), ec);
    auto tmp = file_path;
    tmp += ".tmp";
    ::unlink(tmp.c_str());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    size_t off = 0;
    while (off < payload.size()) {
        ssize_t n = ::write(fd, payload.data() + off, payload.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tmp.c_str());
            return;
        }
        if (n == 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            return;
        }
        off += static_cast<size_t>(n);
    }
    ::fchmod(fd, 0600);
    ::close(fd);
    std::filesystem::rename(tmp, file_path, ec);
    if (ec) {
        ::unlink(tmp.c_str());
    }
}

}  // namespace detail

struct UserPrefs {
    bool adblock_enabled = true;
    bool autohide_topbar = false;
    bool save_cookies = true;
    std::string theme_mode = "system";  // "system" | "dark" | "light"
};

class PrefsStore {
public:
    explicit PrefsStore(std::filesystem::path file_path)
        : file_path_(std::move(file_path)) {}

    UserPrefs load() const {
        UserPrefs p;
        std::ifstream in(file_path_);
        if (!in.is_open()) return p;
        std::string line;
        while (std::getline(in, line)) {
            if (line.find("\"adblock_enabled\"") != std::string::npos) {
                p.adblock_enabled = (line.find("true") != std::string::npos);
            } else if (line.find("\"autohide_topbar\"") != std::string::npos) {
                p.autohide_topbar = (line.find("true") != std::string::npos);
            } else if (line.find("\"save_cookies\"") != std::string::npos) {
                p.save_cookies = (line.find("true") != std::string::npos);
            } else if (line.find("\"theme_mode\"") != std::string::npos) {
                if (line.find("\"dark\"") != std::string::npos) p.theme_mode = "dark";
                else if (line.find("\"light\"") != std::string::npos) p.theme_mode = "light";
                else p.theme_mode = "system";
            }
        }
        return p;
    }

    void save(const UserPrefs& p) const {
        std::string mode = (p.theme_mode == "dark" || p.theme_mode == "light") ? p.theme_mode : "system";
        std::string payload =
            std::string("{\n  \"adblock_enabled\": ") + (p.adblock_enabled ? "true" : "false") +
            ",\n  \"autohide_topbar\": " + (p.autohide_topbar ? "true" : "false") +
            ",\n  \"save_cookies\": " + (p.save_cookies ? "true" : "false") +
            ",\n  \"theme_mode\": \"" + mode + "\"\n}\n";
        detail::writeAtomicFile0600(file_path_, payload);
    }

private:
    std::filesystem::path file_path_;
};

struct SitePermissions {
    bool microphone = true;
    bool camera = true;
    bool location = true;
    bool notifications = true;
    bool clipboard = true;
    bool screen_capture = true;
    bool storage = true;
    bool sound = true;
    bool popups = true;

    bool operator==(const SitePermissions& o) const {
        return microphone == o.microphone &&
               camera == o.camera &&
               location == o.location &&
               notifications == o.notifications &&
               clipboard == o.clipboard &&
               screen_capture == o.screen_capture &&
               storage == o.storage &&
               sound == o.sound &&
               popups == o.popups;
    }
};

class SitePermissionsStore {
public:
    explicit SitePermissionsStore(std::filesystem::path file_path)
        : file_path_(std::move(file_path)) {
        loadFromDisk();
    }

    SitePermissions getForOrigin(const std::string& origin) const {
        std::string key = normalizeKey(origin);
        auto it = rules_.find(key);
        if (it != rules_.end()) return it->second;
        auto def_it = rules_.find("*");
        if (def_it != rules_.end()) return def_it->second;
        return SitePermissions{};
    }

    void setForOrigin(const std::string& origin, const SitePermissions& perms) {
        std::string key = normalizeKey(origin);
        rules_[key] = perms;
        saveToDisk();
    }

    bool togglePermission(const std::string& origin, const std::string& perm_name) {
        std::string key = normalizeKey(origin);
        SitePermissions p = getForOrigin(key);
        bool next_val = true;
        if (perm_name == "microphone" || perm_name == "mic") {
            p.microphone = !p.microphone;
            next_val = p.microphone;
        } else if (perm_name == "camera") {
            p.camera = !p.camera;
            next_val = p.camera;
        } else if (perm_name == "location" || perm_name == "geolocation") {
            p.location = !p.location;
            next_val = p.location;
        } else if (perm_name == "notifications") {
            p.notifications = !p.notifications;
            next_val = p.notifications;
        } else if (perm_name == "clipboard") {
            p.clipboard = !p.clipboard;
            next_val = p.clipboard;
        } else if (perm_name == "screen_capture" || perm_name == "screen") {
            p.screen_capture = !p.screen_capture;
            next_val = p.screen_capture;
        } else if (perm_name == "storage") {
            p.storage = !p.storage;
            next_val = p.storage;
        } else if (perm_name == "sound") {
            p.sound = !p.sound;
            next_val = p.sound;
        } else if (perm_name == "popups") {
            p.popups = !p.popups;
            next_val = p.popups;
        } else {
            return false;
        }
        rules_[key] = p;
        saveToDisk();
        return next_val;
    }

    void resetForOrigin(const std::string& origin) {
        std::string key = normalizeKey(origin);
        if (rules_.erase(key) > 0) {
            saveToDisk();
        }
    }

    void clearAll() {
        rules_.clear();
        saveToDisk();
    }

    const std::map<std::string, SitePermissions>& allRules() const { return rules_; }

private:
    static std::string normalizeKey(const std::string& origin) {
        if (origin.empty() || origin == "*") return "*";
        std::string out;
        out.reserve(origin.size());
        for (unsigned char c : origin) {
            if (c <= 0x20 || c == 0x7f || c == '|' || c == '\n' || c == '\r' || c == '"') continue;
            out.push_back(static_cast<char>(std::tolower(c)));
        }
        while (!out.empty() && out.back() == '/') out.pop_back();
        return out.empty() ? "*" : out;
    }

    void loadFromDisk() {
        rules_.clear();
        std::ifstream in(file_path_);
        if (!in.is_open()) return;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto sep = line.find('|');
            if (sep == std::string::npos) continue;
            std::string key = normalizeKey(line.substr(0, sep));
            std::string bits = line.substr(sep + 1);
            if (bits.size() < 9) continue;
            SitePermissions p;
            p.microphone = (bits[0] != '0');
            p.camera = (bits[1] != '0');
            p.location = (bits[2] != '0');
            p.notifications = (bits[3] != '0');
            p.clipboard = (bits[4] != '0');
            p.screen_capture = (bits[5] != '0');
            p.storage = (bits[6] != '0');
            p.sound = (bits[7] != '0');
            p.popups = (bits[8] != '0');
            rules_[key] = p;
        }
    }

    void saveToDisk() const {
        std::ostringstream out;
        out << "# Explorer Site Permissions v1 (origin|mic,cam,loc,notif,clip,screen,storage,sound,popups)\n";
        for (const auto& [k, p] : rules_) {
            out << k << '|'
                << (p.microphone ? '1' : '0')
                << (p.camera ? '1' : '0')
                << (p.location ? '1' : '0')
                << (p.notifications ? '1' : '0')
                << (p.clipboard ? '1' : '0')
                << (p.screen_capture ? '1' : '0')
                << (p.storage ? '1' : '0')
                << (p.sound ? '1' : '0')
                << (p.popups ? '1' : '0')
                << '\n';
        }
        detail::writeAtomicFile0600(file_path_, out.str());
    }

    std::filesystem::path file_path_;
    std::map<std::string, SitePermissions> rules_;
};

}  // namespace explorer::app
