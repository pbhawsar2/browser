#pragma once
// Crash-safe atomic session persistence (tmp write + rename).

#include <filesystem>
#include <string>
#include <vector>

namespace explorer::app {

struct SessionData {
    std::vector<std::string> urls;
    size_t active_index = 0;
};

class SessionStore {
public:
    explicit SessionStore(std::filesystem::path file_path);

    SessionData load() const;
    void saveAtomic(const SessionData& data) const;

private:
    std::filesystem::path file_path_;
};

}  // namespace explorer::app
