#pragma once
// Persistent user preferences (adblock toggle, auto-hide top bar on scroll, cookie persistence).

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
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
        std::string payload =
            std::string("{\n  \"adblock_enabled\": ") + (p.adblock_enabled ? "true" : "false") +
            ",\n  \"autohide_topbar\": " + (p.autohide_topbar ? "true" : "false") +
            ",\n  \"save_cookies\": " + (p.save_cookies ? "true" : "false") + "\n}\n";

        auto tmp = file_path_;
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
        std::filesystem::rename(tmp, file_path_, ec);
        if (ec) {
            ::unlink(tmp.c_str());
        }
    }

private:
    std::filesystem::path file_path_;
};

}  // namespace explorer::app
