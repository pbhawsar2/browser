#pragma once
// Pure multi-tab PID throttle planner (shared-PID safe, active & audio-playing tabs exempt).

#include <sys/types.h>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace explorer::policy {

struct TabPidInfo {
    pid_t pid = 0;
    bool throttled = false;
};

struct PidTransition {
    pid_t pid = 0;
    bool throttle = false;  // true -> SCHED_BATCH+IOPRIO_IDLE, false -> SCHED_OTHER+IOPRIO_BE/4
};

class PidThrottlePlanner {
public:
    void registerTab(uint64_t tab_id, pid_t pid);
    void unregisterTab(uint64_t tab_id);

    /// Computes minimal safe transitions per unique PID.
    std::vector<PidTransition> reconcile(const std::unordered_set<uint64_t>& active_tabs,
                                         const std::unordered_set<uint64_t>& media_tabs,
                                         bool battery_mode);

    std::vector<PidTransition> releaseAll();

private:
    void pruneOrphanedPid(pid_t pid);

    std::unordered_map<uint64_t, pid_t> tab_pids_;
    std::unordered_set<pid_t> throttled_pids_;
};

}  // namespace explorer::policy
