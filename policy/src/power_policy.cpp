#include "explorer/policy/power_policy.hpp"

#include <cstdio>

namespace explorer::policy {

PowerPolicyEngine::PowerPolicyEngine(bool on_battery_boot)
    : on_battery_(on_battery_boot) {}

void PowerPolicyEngine::setPowerSource(bool on_battery) {
    if (on_battery_ != on_battery) {
        // Manual override never outlives the power source it was toggled on.
        manual_turbo_.reset();
    }
    on_battery_ = on_battery;
}

void PowerPolicyEngine::setManualTurbo(std::optional<bool> turbo) {
    manual_turbo_ = turbo;
}

void PowerPolicyEngine::setCinema60Hz(bool enabled) {
    cinema_60hz_ = enabled;
}

bool PowerPolicyEngine::isBatteryMode() const {
    if (manual_turbo_.has_value()) {
        return !*manual_turbo_;
    }
    return on_battery_;
}

PowerMode PowerPolicyEngine::mode() const {
    return isBatteryMode() ? PowerMode::BatteryZeroFan : PowerMode::AcTurbo;
}

double PowerPolicyEngine::targetRefreshHz() const {
    return (isBatteryMode() || cinema_60hz_) ? 60.0 : 144.0;
}

unsigned long PowerPolicyEngine::timerSlackNs() const {
    return isBatteryMode() ? 2'000'000UL : 50'000UL;
}

int PowerPolicyEngine::telemetryIntervalMs(bool hud_open) const {
    return (isBatteryMode() && !hud_open) ? 5000 : 2000;
}

std::string PowerPolicyEngine::formatPillText(const explorer::hal::TelemetrySnapshot& snap) const {
    std::string out = (mode() == PowerMode::BatteryZeroFan) ? "Zero-Fan" : "AC Turbo";

    if (snap.on_battery) {
        auto w = snap.batt_watts.has_value() ? snap.batt_watts : snap.pkg_watts;
        if (w.has_value()) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), " · %.1fW", *w);
            out += buf;
        }
    }
    if (snap.fan_rpm.has_value()) {
        out += " · " + std::to_string(*snap.fan_rpm) + " RPM";
    }
    return out;
}

}  // namespace explorer::policy
