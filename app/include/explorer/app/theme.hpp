#pragma once
// Explorer Dark Aesthetic UI constants (calibrated for crisp 1080p Wayland readability).

#include "include/internal/cef_types.h"

namespace explorer::app::theme {

inline constexpr cef_color_t kBgCanvas = CefColorSetARGB(255, 8, 12, 20);        // #080C14
inline constexpr cef_color_t kBgBezel = CefColorSetARGB(255, 12, 18, 30);        // #0C121E
inline constexpr cef_color_t kBgControl = CefColorSetARGB(255, 19, 28, 44);      // #131C2C
inline constexpr cef_color_t kBgTabActive = CefColorSetARGB(255, 24, 39, 63);    // #18273F
inline constexpr cef_color_t kBgTabIdle = CefColorSetARGB(255, 17, 24, 39);      // #111827
inline constexpr cef_color_t kAccentCyan = CefColorSetARGB(255, 56, 189, 248);   // #38BDF8
inline constexpr cef_color_t kAccentGreen = CefColorSetARGB(255, 52, 211, 153);  // #34D399
inline constexpr cef_color_t kDangerCoral = CefColorSetARGB(255, 248, 113, 113); // #F87171
inline constexpr cef_color_t kTextPrimary = CefColorSetARGB(255, 248, 250, 252); // #F8FAFC
inline constexpr cef_color_t kTextMuted = CefColorSetARGB(255, 148, 163, 184);   // #94A3B8

inline constexpr int kTopBezelHeight = 44;
inline constexpr int kNavBtnW = 36;
inline constexpr int kNavBtnH = 34;
inline constexpr int kTabBarHeight = 38;
inline constexpr int kFindBarHeight = 38;

}  // namespace explorer::app::theme
