#include "explorer/hal/gpu_isolator.hpp"
#include "explorer/hal/power_governor.hpp"
#include "explorer/hal/process_throttler.hpp"
#include "explorer/hal/telemetry.hpp"
#include "explorer/hal/vaapi_prober.hpp"

#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::cerr << "[test_hal] FAIL at line " << __LINE__ << ": " #cond << "\n"; \
            std::exit(1);                                                             \
        }                                                                             \
    } while (0)

int main() {
    using namespace explorer::hal;

    // 1. GPU discovery finds a valid render node (AMD 0x1002 on Cezanne).
    AmdCezanneGpuIsolator gpu;
    auto [node, vendor] = gpu.discoverIgpuRenderNode();
    CHECK(!node.empty());
    std::cout << "[test_hal] discovered iGPU=" << node << " vendor=" << vendor << "\n";

    // 2. Direct O_RDWR libva probe succeeds cleanly without crashing.
    LibvaDrmProber va;
    auto caps = va.probe(node);
    std::cout << "[test_hal] VA-API summary=" << caps.summary() << "\n";
    if (vendor == "0x1002") {
        CHECK(caps.h264 && caps.vp9);
    }

    // 3. Per-TID SCHED_BATCH <-> SCHED_OTHER throttling is 100% reversible unprivileged.
    pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        ::execl("/bin/sleep", "sleep", "2", nullptr);
        ::_exit(127);
    }
    LinuxProcessThrottler throttler;
    CHECK(throttler.setProcessIdleClass(child, true));
    CHECK(::sched_getscheduler(child) == SCHED_BATCH);
    CHECK(throttler.setProcessIdleClass(child, false));
    CHECK(::sched_getscheduler(child) == SCHED_OTHER);
    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, 0);

    // 4. Telemetry monitor samples cleanly without throwing or failing on root-only RAPL.
    LinuxTelemetryMonitor mon;
    auto snap = mon.sample();
    auto lite = mon.sampleLite();
    CHECK(snap.on_battery == lite.on_battery);
    std::cout << "[test_hal] battery=" << snap.on_battery
              << " dgpu=" << snap.dgpu_state.value_or("n/a") << "\n";

    // 5. Timer slack syscall & clean governor shutdown succeed.
    LinuxPowerGovernor gov;
    CHECK(gov.applyTimerSlackNs(50'000UL));
    gov.shutdown();

    std::cout << "[test_hal] ALL PASSED\n";
    return 0;
}
