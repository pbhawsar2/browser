#include "explorer/policy/media_policy.hpp"
#include "explorer/policy/navigation_policy.hpp"
#include "explorer/policy/power_policy.hpp"
#include "explorer/policy/throttle_planner.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::cerr << "[test_policy] FAIL at line " << __LINE__ << ": " #cond << "\n"; \
            std::exit(1);                                                                \
        }                                                                                \
    } while (0)

int main() {
    using namespace explorer::policy;
    using namespace explorer::hal;

    // 1. PowerPolicyEngine bidirectional manual override + source reset.
    PowerPolicyEngine pwr(false);  // Boot on AC
    CHECK(!pwr.isBatteryMode());
    CHECK(pwr.targetRefreshHz() == 144.0);
    CHECK(pwr.timerSlackNs() == 50'000UL);

    pwr.setManualTurbo(false);  // Force battery saver on AC
    CHECK(pwr.isBatteryMode());
    CHECK(pwr.targetRefreshHz() == 60.0);
    CHECK(pwr.telemetryIntervalMs(false) == 5000);
    CHECK(pwr.telemetryIntervalMs(true) == 2000);

    pwr.setPowerSource(true);  // Switching source clears manual override
    CHECK(!pwr.manualTurbo().has_value());
    CHECK(pwr.isBatteryMode());

    TelemetrySnapshot snap;
    snap.on_battery = true;
    snap.batt_watts = 6.2f;
    snap.fan_rpm = 0;
    CHECK(pwr.formatPillText(snap) == "Zero-Fan · 6.2W · 0 RPM");

    pwr.setPowerSource(false);
    snap.on_battery = false;
    CHECK(pwr.formatPillText(snap) == "AC Turbo · 0 RPM");

    // 2. MediaCodecPolicy disables SW AV1 on Cezanne (no HW AV1) unless dGPU allowed.
    VaCapabilities cezanne{"/dev/dri/renderD128", true, true, true, false};
    MediaCodecPolicy media(cezanne, false);
    CHECK(media.shouldDisableSoftwareAv1());
    CHECK(media.hardwareDecodeAvailable());
    media.setHwOnlyVideo(false);
    CHECK(!media.shouldDisableSoftwareAv1());
    media.setHwOnlyVideo(true);
    CHECK(media.shouldDisableSoftwareAv1());

    MediaCodecPolicy media_dgpu(cezanne, true);
    CHECK(!media_dgpu.shouldDisableSoftwareAv1());
    media_dgpu.setHwOnlyVideo(true);
    CHECK(!media_dgpu.shouldDisableSoftwareAv1());

    // 3. PidThrottlePlanner never throttles shared PID with active or audio tab.
    PidThrottlePlanner planner;
    planner.registerTab(1, 100);
    planner.registerTab(2, 100);
    planner.registerTab(3, 200);
    planner.registerTab(4, 300);

    auto t1 = planner.reconcile({1}, {4}, true);
    CHECK(t1.size() == 1 && t1[0].pid == 200 && t1[0].throttle == true);

    auto t2 = planner.reconcile({1}, {4}, false);
    CHECK(t2.size() == 1 && t2[0].pid == 200 && t2[0].throttle == false);

    // 3b. Cross-site navigation onto an already-throttled PID stays consistent (single PID state).
    auto t3 = planner.reconcile({1}, {4}, true);
    CHECK(t3.size() == 1 && t3[0].pid == 200 && t3[0].throttle == true);
    planner.registerTab(5, 200);  // New background tab shares already-throttled PID 200
    auto t4 = planner.reconcile({1}, {4}, true);
    CHECK(t4.empty());  // Must NOT redundantly re-throttle PID 200
    auto t5 = planner.reconcile({5}, {4}, true);  // Activating tab 5 unthrottles PID 200
    bool unthrottled_200 = false;
    for (const auto& tr : t5) {
        if (tr.pid == 200 && !tr.throttle) unthrottled_200 = true;
    }
    CHECK(unthrottled_200);
    planner.unregisterTab(3);
    planner.unregisterTab(5);  // Orphaned PID 200 cleanly pruned
    CHECK(planner.releaseAll().size() <= 2);

    // 4. Navigation policy: Google default search + URL normalization + strict secure-origin checks + 1.00x zoom math.
    CHECK(normalizeOmnibarInput("github.com") == "https://github.com");
    CHECK(normalizeOmnibarInput("localhost:3000") == "http://localhost:3000");
    CHECK(normalizeOmnibarInput("ryzen 5800h linux") == "https://www.google.com/search?q=ryzen+5800h+linux");

    CHECK(isSecureWebOrigin("https://github.com/login"));
    CHECK(isSecureWebOrigin("http://localhost"));
    CHECK(isSecureWebOrigin("http://localhost:3000/auth"));
    CHECK(isSecureWebOrigin("http://127.0.0.1:8080/login"));
    CHECK(!isSecureWebOrigin("http://localhost.evil.com"));
    CHECK(!isSecureWebOrigin("http://127.0.0.1.evil.com"));
    CHECK(!isSecureWebOrigin("http://example.com"));
    CHECK(!isSecureWebOrigin("javascript:alert(1)"));
    CHECK(!isSecureWebOrigin("https://"));

    double lvl = zoomFactorToCefLevel(kDefaultPageZoomFactor);
    CHECK(std::abs(cefLevelToZoomFactor(lvl) - 1.00) < 1e-6);
    CHECK(std::abs(cefLevelToZoomFactor(zoomFactorToCefLevel(1.10)) - 1.10) < 1e-6);
    CHECK(std::abs(cefLevelToZoomFactor(zoomFactorToCefLevel(0.90)) - 0.90) < 1e-6);

    std::cout << "[test_policy] ALL PASSED\n";
    return 0;
}
