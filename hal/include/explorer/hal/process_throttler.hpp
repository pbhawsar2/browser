#pragma once
// SOLID Interface Segregation: Reversible unprivileged per-TID CPU/I/O throttler.

#include <sys/types.h>

namespace explorer::hal {

class IProcessThrottler {
public:
    virtual ~IProcessThrottler() = default;
    /// Transitions every thread in `pid` between SCHED_BATCH+IOPRIO_IDLE and SCHED_OTHER+IOPRIO_BE/4.
    virtual bool setProcessIdleClass(pid_t pid, bool idle_class) const = 0;
};

class LinuxProcessThrottler final : public IProcessThrottler {
public:
    bool setProcessIdleClass(pid_t pid, bool idle_class) const override;
};

}  // namespace explorer::hal
