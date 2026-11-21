#pragma once
// Pure URL normalization & Google default search routing policy.

#include <string>
#include <string_view>

namespace explorer::policy {

inline constexpr double kDefaultPageZoomFactor = 1.05;
inline constexpr const char* kHomeUrl = "explorer://start";

/// Normalizes raw omnibar input into a navigable URL or Google search query.
std::string normalizeOmnibarInput(std::string_view raw);

/// Returns true only for https:// origins or exact http://localhost / http://127.0.0.1 loopback origins
/// (rejecting prefix spoofing such as http://localhost.evil.com or http://127.0.0.1.evil.com).
bool isSecureWebOrigin(std::string_view url_or_origin);

/// Converts a linear zoom factor (e.g. 1.05) to Chromium's logarithmic zoom level (`log(f)/log(1.2)`).
double zoomFactorToCefLevel(double factor);

/// Converts Chromium's logarithmic zoom level back to a linear zoom factor.
double cefLevelToZoomFactor(double level);

}  // namespace explorer::policy
