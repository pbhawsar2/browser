#include "explorer/hal/process_throttler.hpp"
#include "explorer/hal/types.hpp"

#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <filesystem>
#include <string>

namespace explorer::hal {

namespace fs = std::filesystem;

namespace {

bool applyTidThrottling(pid_t tid, bool idle_class) {
    constexpr int IOPRIO_WHO_PROCESS = 1;
    constexpr int IOPRIO_CLASS_BE = 2;
    constexpr int IOPRIO_CLASS_IDLE = 3;
    constexpr int IOPRIO_CLASS_SHIFT = 13;

    int ioprio = idle_class ? (IOPRIO_CLASS_IDLE << IOPRIO_CLASS_SHIFT)
                            : ((IOPRIO_CLASS_BE << IOPRIO_CLASS_SHIFT) | 4);
    // SCHED_BATCH <-> SCHED_OTHER is 100% reversible without CAP_SYS_NICE (unlike nice 0 -> 12).
    int policy = idle_class ? SCHED_BATCH : SCHED_OTHER;
    sched_param param{};
    param.sched_priority = 0;

    ::syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, tid, ioprio);
    return ::sched_setscheduler(tid, policy, &param) == 0;
}

}  // namespace

bool LinuxProcessThrottler::setProcessIdleClass(pid_t pid, bool idle_class) const {
    if (isTuningDisabled() || pid <= 1) {
        return false;
    }

    fs::path task_dir = "/proc/" + std::to_string(pid) + "/task";
    std::error_code ec;
    bool applied_any = false;
    bool ok_all = true;

    for (const auto& entry : fs::directory_iterator(task_dir, ec)) {
        try {
            pid_t tid = std::stoi(entry.path().filename().string());
            if (tid > 1) {
                applied_any = true;
                if (!applyTidThrottling(tid, idle_class)) {
                    ok_all = false;
                }
            }
        } catch (...) {
        }
    }

    if (!applied_any) {
        return applyTidThrottling(pid, idle_class);
    }
    return ok_all;
}

}  // namespace explorer::hal
