#include "explorer/policy/navigation_policy.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace explorer::policy {

namespace {

std::string percentEncodeQuery(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

}  // namespace

std::string normalizeOmnibarInput(std::string_view raw) {
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) raw.remove_prefix(1);
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.remove_suffix(1);
    if (raw.empty()) return kHomeUrl;

    std::string s(raw);
    if (s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0 ||
        s.rfind("file://", 0) == 0 || s.rfind("about:", 0) == 0 ||
        s.rfind("chrome://", 0) == 0 || s.rfind("explorer://", 0) == 0 ||
        s.rfind("data:", 0) == 0) {
        return s;
    }

    bool has_space = std::any_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
    if (!has_space) {
        if (s == "localhost" || s.rfind("localhost:", 0) == 0 || s.rfind("127.", 0) == 0) {
            return "http://" + s;
        }
        auto dot = s.find('.');
        if (dot != std::string::npos && dot > 0 && dot + 1 < s.size()) {
            return "https://" + s;
        }
    }

    // Default search engine: Google
    return "https://www.google.com/search?q=" + percentEncodeQuery(s);
}

double zoomFactorToCefLevel(double factor) {
    if (factor <= 0.1) factor = 0.1;
    return std::log(factor) / std::log(1.2);
}

double cefLevelToZoomFactor(double level) {
    return std::pow(1.2, level);
}

}  // namespace explorer::policy
