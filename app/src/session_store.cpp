#include "explorer/app/session_store.hpp"

#include <fstream>
#include <sstream>

namespace explorer::app {

namespace fs = std::filesystem;

namespace {

std::string escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out.push_back(c);
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
                    u.push_back(line[++i]);
                } else if (line[i] == '"') {
                    break;
                } else {
                    u.push_back(line[i]);
                }
            }
            if (!u.empty() && u.rfind("data:", 0) != 0) {
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

    fs::path tmp = file_path_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open()) return;
        out << "{\n";
        out << "  \"active\": " << data.active_index << ",\n";
        out << "  \"tabs\": [\n";
        for (size_t i = 0; i < data.urls.size(); ++i) {
            out << "    {\"url\":\"" << escapeJson(data.urls[i]) << "\"}";
            if (i + 1 < data.urls.size()) out << ",";
            out << "\n";
        }
        out << "  ]\n}\n";
        out.flush();
    }
    fs::rename(tmp, file_path_, ec);
}

}  // namespace explorer::app
