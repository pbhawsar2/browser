#include "explorer/app/session_store.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace explorer::app {

namespace fs = std::filesystem;

namespace {

std::string escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else if (c == 0xE2 && i + 2 < s.size() &&
                   static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 2]) == 0xA8 ||
                    static_cast<unsigned char>(s[i + 2]) == 0xA9)) {
            out += (static_cast<unsigned char>(s[i + 2]) == 0xA8) ? "\\u2028" : "\\u2029";
            i += 2;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

}  // namespace

SessionStore::SessionStore(fs::path file_path)
    : file_path_(std::move(file_path)) {}

SessionData SessionStore::load() const {
    SessionData data;
    std::ifstream in(file_path_);
    if (!in.is_open()) return data;

    std::string line;
    while (std::getline(in, line)) {
        // Lightweight line-oriented JSON format written by saveAtomic:
        // "active": N
        // "url": "..."
        auto pos_active = line.find("\"active\":");
        if (pos_active != std::string::npos) {
            try {
                data.active_index = std::stoull(line.substr(pos_active + 9));
            } catch (...) {
            }
            continue;
        }
        auto pos_url = line.find("\"url\":\"");
        if (pos_url != std::string::npos) {
            size_t start = pos_url + 7;
            std::string u;
            for (size_t i = start; i < line.size(); ++i) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    char nxt = line[++i];
                    if (nxt == 'n') u.push_back('\n');
                    else if (nxt == 'r') u.push_back('\r');
                    else if (nxt == 't') u.push_back('\t');
                    else if (nxt == 'u' && i + 4 < line.size()) {
                        uint32_t cp = 0;
                        bool ok = true;
                        for (int k = 1; k <= 4; ++k) {
                            unsigned char hc = static_cast<unsigned char>(line[i + k]);
                            cp <<= 4;
                            if (hc >= '0' && hc <= '9') cp |= (hc - '0');
                            else if (hc >= 'a' && hc <= 'f') cp |= (hc - 'a' + 10);
                            else if (hc >= 'A' && hc <= 'F') cp |= (hc - 'A' + 10);
                            else { ok = false; break; }
                        }
                        if (ok) {
                            if (cp <= 0x7F) {
                                u.push_back(static_cast<char>(cp));
                            } else if (cp <= 0x7FF) {
                                u.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
                                u.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            } else {
                                u.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
                                u.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                                u.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            }
                            i += 4;
                        } else {
                            u.push_back('u');
                        }
                    } else {
                        u.push_back(nxt);
                    }
                } else if (line[i] == '"') {
                    break;
                } else {
                    u.push_back(line[i]);
                }
            }
            if (u.rfind("https://", 0) == 0 || u.rfind("http://", 0) == 0) {
                data.urls.push_back(std::move(u));
            }
        }
    }
    if (data.active_index >= data.urls.size()) {
        data.active_index = 0;
    }
    return data;
}

void SessionStore::saveAtomic(const SessionData& data) const {
    std::error_code ec;
    fs::create_directories(file_path_.parent_path(), ec);

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"active\": " << data.active_index << ",\n";
    oss << "  \"tabs\": [\n";
    for (size_t i = 0; i < data.urls.size(); ++i) {
        oss << "    {\"url\":\"" << escapeJson(data.urls[i]) << "\"}";
        if (i + 1 < data.urls.size()) oss << ",";
        oss << "\n";
    }
    oss << "  ]\n}\n";
    std::string payload = oss.str();

    fs::path tmp = file_path_;
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
    fs::rename(tmp, file_path_, ec);
    if (ec) {
        ::unlink(tmp.c_str());
    }
}

}  // namespace explorer::app
