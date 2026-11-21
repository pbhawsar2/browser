#include "explorer/policy/throttle_planner.hpp"

namespace explorer::policy {

void PidThrottlePlanner::pruneOrphanedPid(pid_t pid) {
    if (pid <= 1) return;
    for (const auto& [tid, p] : tab_pids_) {
        if (p == pid) return;  // Still referenced by another tab
    }
    throttled_pids_.erase(pid);
}

void PidThrottlePlanner::registerTab(uint64_t tab_id, pid_t pid) {
    if (pid <= 1) return;
    pid_t old_pid = 0;
    if (auto it = tab_pids_.find(tab_id); it != tab_pids_.end()) {
        old_pid = it->second;
    }
    tab_pids_[tab_id] = pid;
    if (old_pid != 0 && old_pid != pid) {
        pruneOrphanedPid(old_pid);
    }
}

void PidThrottlePlanner::unregisterTab(uint64_t tab_id) {
    auto it = tab_pids_.find(tab_id);
    if (it == tab_pids_.end()) return;
    pid_t old_pid = it->second;
    tab_pids_.erase(it);
    pruneOrphanedPid(old_pid);
}

std::vector<PidTransition> PidThrottlePlanner::reconcile(
    const std::unordered_set<uint64_t>& active_tabs,
    const std::unordered_set<uint64_t>& media_tabs,
    bool battery_mode) {
    std::unordered_set<pid_t> keep_awake_pids;
    std::unordered_set<pid_t> all_pids;
    for (const auto& [tab_id, pid] : tab_pids_) {
        all_pids.insert(pid);
        if (!battery_mode || active_tabs.count(tab_id) || media_tabs.count(tab_id)) {
            keep_awake_pids.insert(pid);
        }
    }

    std::vector<PidTransition> out;
    for (pid_t pid : all_pids) {
        const bool want_throttled = (keep_awake_pids.count(pid) == 0);
        const bool is_throttled = (throttled_pids_.count(pid) != 0);
        if (is_throttled != want_throttled) {
            if (want_throttled) throttled_pids_.insert(pid);
            else throttled_pids_.erase(pid);
            out.push_back(PidTransition{pid, want_throttled});
        }
    }
    return out;
}

std::vector<PidTransition> PidThrottlePlanner::releaseAll() {
    std::vector<PidTransition> out;
    out.reserve(throttled_pids_.size());
    for (pid_t pid : throttled_pids_) {
        out.push_back(PidTransition{pid, false});
    }
    throttled_pids_.clear();
    return out;
}

}  // namespace explorer::policy
