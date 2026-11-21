#pragma once
// Explorer v2 — Hardware Abstraction Layer (HAL) shared data types.
// Zero UI or browser-engine dependencies.

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace explorer::hal {

/// Returns true when the user bypassed all hardware/engine tuning via EXPLORER_DISABLE_TUNING=1.
inline bool isTuningDisabled() {
    const char* v = std::getenv("EXPLORER_DISABLE_TUNING");
    return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}

/// Hardware VLD video decode capabilities discovered via direct O_RDWR libva-drm probing.
struct VaCapabilities {
    std::string render_node;
    bool h264 = false;
    bool hevc = false;
    bool vp9 = false;
    bool av1 = false;

    std::string summary() const {
        std::string out;
        auto push = [&](const char* name) {
            if (!out.empty()) out += " · ";
            out += name;
        };
        if (vp9) push("VP9");
        if (h264) push("H264");
        if (hevc) push("HEVC");
        if (av1) push("AV1");
        return out.empty() ? "SW Fallback" : out;
    }
};

/// Boot-time hardware snapshot produced by HAL discovery before engine init.
struct HardwareProfile {
    std::string igpu_render_node;
    std::string igpu_vendor;
    bool on_battery = false;
    bool allow_dgpu = false;
    bool tuning_disabled = false;
    VaCapabilities va_caps;
};

/// Live thermal, power, iGPU, fan, CPU, battery, and Optimus dGPU state.
struct TelemetrySnapshot {
    bool on_battery = false;
    float cpu_percent = 0.0f;
    std::optional<float> cpu_temp_c;
    std::optional<float> pkg_watts;
    std::optional<float> batt_watts;
    std::optional<uint32_t> batt_capacity_pct;
    std::optional<std::string> batt_status_str;
    std::optional<uint32_t> igpu_busy;
    std::optional<uint32_t> igpu_mhz;
    std::optional<float> igpu_temp_c;
    std::optional<float> igpu_watts;
    std::optional<uint32_t> fan_rpm;
    std::vector<uint32_t> fan_rpms;
    std::optional<std::string> dgpu_state;
};

}  // namespace explorer::hal
