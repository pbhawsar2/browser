#pragma once
// SOLID Interface Segregation: Laptop eDP display refresh rate controller (60 Hz battery <-> 144 Hz AC).

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace explorer::hal {

class IDisplayRefreshCtrl {
public:
    virtual ~IDisplayRefreshCtrl() = default;
    virtual void setEdpRefreshRateAsync(double target_hz) = 0;
    virtual void invalidateCache() = 0;
    virtual void startMonitorsChangedWatcher(std::function<void()> on_changed) = 0;
    virtual void shutdown() = 0;
};

/// Async GNOME Mutter DisplayConfig controller via sd-bus (zero main-thread stalls).
class MutterDisplayRefreshCtrl final : public IDisplayRefreshCtrl {
public:
    MutterDisplayRefreshCtrl() = default;
    ~MutterDisplayRefreshCtrl() override;

    void setEdpRefreshRateAsync(double target_hz) override;
    void invalidateCache() override;
    void startMonitorsChangedWatcher(std::function<void()> on_changed) override;
    void shutdown() override;

private:
    std::mutex mu_;
    int desired_hz_ = -1;
    int applied_hz_ = -1;
    bool apply_in_flight_ = false;
    std::atomic<int64_t> last_self_apply_ms_{0};
    std::thread apply_thread_;
    std::atomic<bool> stop_watcher_{false};
    int wake_fd_ = -1;
    std::thread watcher_thread_;
};

}  // namespace explorer::hal
