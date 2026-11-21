#include "explorer/policy/media_policy.hpp"
#include "explorer/policy/navigation_policy.hpp"
#include "explorer/policy/power_policy.hpp"
#include "explorer/policy/throttle_planner.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    using namespace explorer::policy;
    using namespace explorer::hal;

    // 1. PowerPolicyEngine bidirectional manual override + source reset.
    PowerPolicyEngine pwr(false);  // Boot on AC
    assert(!pwr.isBatteryMode());
    assert(pwr.targetRefreshHz() == 144.0);
    assert(pwr.timerSlackNs() == 50'000UL);

    pwr.setManualTurbo(false);  // Force battery saver on AC
    assert(pwr.isBatteryMode());
    assert(pwr.targetRefreshHz() == 60.0);
    assert(pwr.telemetryIntervalMs(false) == 5000);
    assert(pwr.telemetryIntervalMs(true) == 2000);

    pwr.setPowerSource(true);  // Switching source clears manual override
    assert(!pwr.manualTurbo().has_value());
    assert(pwr.isBatteryMode());

    TelemetrySnapshot snap;
    snap.on_battery = true;
    snap.batt_watts = 6.2f;
    snap.fan_rpm = 0;
    assert(pwr.formatPillText(snap) == "Zero-Fan · 6.2W · 0 RPM");

    pwr.setPowerSource(false);
    snap.on_battery = false;
    assert(pwr.formatPillText(snap) == "AC Turbo · 0 RPM");

    // 2. MediaCodecPolicy disables SW AV1 on Cezanne (no HW AV1) unless dGPU allowed.
    VaCapabilities cezanne{"/dev/dri/renderD128", true, true, true, false};
    MediaCodecPolicy media(cezanne, false);
    assert(media.shouldDisableSoftwareAv1());
    assert(media.hardwareDecodeAvailable());

    // 3. PidThrottlePlanner never throttles shared PID with active or audio tab.
    PidThrottlePlanner planner;
    planner.registerTab(1, 100);
    planner.registerTab(2, 100);
    planner.registerTab(3, 200);
    planner.registerTab(4, 300);

    auto t1 = planner.reconcile({1}, {4}, true);
    assert(t1.size() == 1 && t1[0].pid == 200 && t1[0].throttle == true);

    auto t2 = planner.reconcile({1}, {4}, false);
    assert(t2.size() == 1 && t2[0].pid == 200 && t2[0].throttle == false);

    // 4. Navigation policy: Google default search + URL normalization + 1.05x zoom math.
    assert(normalizeOmnibarInput("github.com") == "https://github.com");
    assert(normalizeOmnibarInput("localhost:3000") == "http://localhost:3000");
    assert(normalizeOmnibarInput("ryzen 5800h linux") == "https://www.google.com/search?q=ryzen+5800h+linux");
    double lvl = zoomFactorToCefLevel(kDefaultPageZoomFactor);
    assert(std::abs(cefLevelToZoomFactor(lvl) - 1.05) < 1e-6);

    std::cout << "[test_policy] ALL PASSED\n";
    return 0;
}
