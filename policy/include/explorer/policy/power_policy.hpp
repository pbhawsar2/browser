#pragma once
// SOLID Policy Engine (Pure Logic — Zero OS, D-Bus, UI, or Engine dependencies).

#include "explorer/hal/types.hpp"
#include <optional>
#include <string>

namespace explorer::policy {

enum class PowerMode {
    BatteryZeroFan,
    AcTurbo,
};

/// Pure state machine governing AC Turbo <-> Battery Zero-Fan mode, refresh rate, and HUD pill text.
class PowerPolicyEngine {
public:
    explicit PowerPolicyEngine(bool on_battery_boot);

    void setPowerSource(bool on_battery);
    void setManualTurbo(std::optional<bool> turbo);
    void setCinema60Hz(bool enabled);

    bool isOnBatteryRaw() const { return on_battery_; }
    std::optional<bool> manualTurbo() const { return manual_turbo_; }
    bool cinema60Hz() const { return cinema_60hz_; }

    /// Effective battery-saving mode: manual override wins, otherwise follows AC/battery source.
    bool isBatteryMode() const;
    PowerMode mode() const;
    bool wantsPerformanceHold() const { return !isBatteryMode(); }
    double targetRefreshHz() const;
    unsigned long timerSlackNs() const;
    int telemetryIntervalMs(bool hud_open) const;

    /// Formats the compact header pill (`Zero-Fan · 6.2W · 0 RPM` on battery; `AC Turbo · 0 RPM` on AC).
    std::string formatPillText(const explorer::hal::TelemetrySnapshot& snap) const;

private:
    bool on_battery_ = false;
    bool cinema_60hz_ = false;
    std::optional<bool> manual_turbo_;
};

}  // namespace explorer::policy
