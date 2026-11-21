#pragma once
// Persistent user preferences (adblock toggle, auto-hide top bar on scroll, cookie persistence).

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace explorer::app {

struct UserPrefs {
    bool adblock_enabled = true;
    bool autohide_topbar = false;
    bool save_cookies = true;
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
            }
        }
        return p;
    }

    void save(const UserPrefs& p) const {
        std::error_code ec;
        std::filesystem::create_directories(file_path_.parent_path(), ec);
        auto tmp = file_path_;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out.is_open()) return;
            out << "{\n"
                << "  \"adblock_enabled\": " << (p.adblock_enabled ? "true" : "false") << ",\n"
                << "  \"autohide_topbar\": " << (p.autohide_topbar ? "true" : "false") << ",\n"
                << "  \"save_cookies\": " << (p.save_cookies ? "true" : "false") << "\n"
                << "}\n";
            out.flush();
        }
        ::chmod(tmp.c_str(), 0600);
        std::filesystem::rename(tmp, file_path_, ec);
        ::chmod(file_path_.c_str(), 0600);
    }

private:
    std::filesystem::path file_path_;
};

}  // namespace explorer::app
