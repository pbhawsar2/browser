#pragma once
// SOLID Interface Segregation: OS Power Governor (battery detection, timer slack, PPD AC Turbo hold).

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace explorer::hal {

class IPowerGovernor {
public:
    virtual ~IPowerGovernor() = default;
    virtual bool isOnBatteryBoot() const = 0;
    virtual bool applyTimerSlackNs(unsigned long ns) const = 0;
    virtual void setPerformanceHoldAsync(bool hold) = 0;
    virtual void startBatteryWatcher(std::function<void(bool)> on_change) = 0;
    virtual void shutdown() = 0;
};

/// Non-blocking Linux Power Governor backed by sysfs, prctl, and sd-bus (UPower + tuned-ppd).
class LinuxPowerGovernor final : public IPowerGovernor {
public:
    LinuxPowerGovernor() = default;
    ~LinuxPowerGovernor() override;

    bool isOnBatteryBoot() const override;
    bool applyTimerSlackNs(unsigned long ns) const override;
    void setPerformanceHoldAsync(bool hold) override;
    void startBatteryWatcher(std::function<void(bool)> on_change) override;
    void shutdown() override;

private:
    std::mutex mu_;
    void* ppd_bus_ = nullptr;
    std::optional<uint32_t> ppd_cookie_;
    bool ppd_desired_ = false;
    bool ppd_in_flight_ = false;
    std::thread ppd_thread_;
    std::atomic<bool> stop_watcher_{false};
    int wake_fd_ = -1;
    std::thread watcher_thread_;
};

}  // namespace explorer::hal
