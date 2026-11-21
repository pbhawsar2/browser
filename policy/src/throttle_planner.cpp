#include "explorer/policy/throttle_planner.hpp"

namespace explorer::policy {

void PidThrottlePlanner::registerTab(uint64_t tab_id, pid_t pid) {
    if (pid <= 1) return;
    tabs_[tab_id] = TabPidInfo{pid, false};
}

void PidThrottlePlanner::unregisterTab(uint64_t tab_id) {
    tabs_.erase(tab_id);
}

std::vector<PidTransition> PidThrottlePlanner::reconcile(
    const std::unordered_set<uint64_t>& active_tabs,
    const std::unordered_set<uint64_t>& media_tabs,
    bool battery_mode) {
    std::unordered_set<pid_t> keep_awake_pids;
    for (const auto& [tab_id, info] : tabs_) {
        if (!battery_mode || active_tabs.count(tab_id) || media_tabs.count(tab_id)) {
            keep_awake_pids.insert(info.pid);
        }
    }

    std::vector<PidTransition> out;
    std::unordered_set<pid_t> handled;
    for (auto& [tab_id, info] : tabs_) {
        bool want_throttled = (keep_awake_pids.count(info.pid) == 0);
        if (info.throttled != want_throttled) {
            info.throttled = want_throttled;
            if (handled.insert(info.pid).second) {
                out.push_back(PidTransition{info.pid, want_throttled});
            }
        }
    }
    return out;
}

std::vector<PidTransition> PidThrottlePlanner::releaseAll() {
    std::vector<PidTransition> out;
    std::unordered_set<pid_t> handled;
    for (auto& [tab_id, info] : tabs_) {
        if (info.throttled) {
            info.throttled = false;
            if (handled.insert(info.pid).second) {
                out.push_back(PidTransition{info.pid, false});
            }
        }
    }
    return out;
}

}  // namespace explorer::policy
