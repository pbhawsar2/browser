#include "explorer/hal/power_governor.hpp"
#include "explorer/hal/types.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace explorer::hal {

namespace fs = std::filesystem;

namespace {

std::string readTrimmed(const fs::path& p) {
    std::ifstream in(p);
    std::string s;
    if (std::getline(in, s)) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
            s.pop_back();
        }
    }
    return s;
}

std::optional<uint32_t> holdPpdProfileSync() {
    sd_bus* bus = nullptr;
    if (sd_bus_open_system(&bus) < 0 || !bus) return std::nullopt;

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call_method(
        bus,
        "net.hadess.PowerProfiles",
        "/net/hadess/PowerProfiles",
        "net.hadess.PowerProfiles",
        "HoldProfile",
        &err,
        &reply,
        "sss",
        "performance",
        "Explorer AC Turbo",
        "io.explorer.Browser");

    std::optional<uint32_t> cookie;
    if (r >= 0 && reply) {
        uint32_t c = 0;
        if (sd_bus_message_read(reply, "u", &c) >= 0) {
            cookie = c;
        }
    }
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    return cookie;
}

void releasePpdProfileSync(uint32_t cookie) {
    sd_bus* bus = nullptr;
    if (sd_bus_open_system(&bus) < 0 || !bus) return;

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    sd_bus_call_method(
        bus,
        "net.hadess.PowerProfiles",
        "/net/hadess/PowerProfiles",
        "net.hadess.PowerProfiles",
        "ReleaseProfile",
        &err,
        &reply,
        "u",
        cookie);
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
}

struct BatteryWatchCtx {
    std::function<void(bool)> cb;
};

int onUpowerPropsChanged(sd_bus_message* m, void* userdata, sd_bus_error* /*err*/) {
    auto* ctx = static_cast<BatteryWatchCtx*>(userdata);
    if (!ctx || !ctx->cb) return 0;

    const char* iface = nullptr;
    if (sd_bus_message_read(m, "s", &iface) < 0 || !iface) return 0;
    if (std::string(iface) != "org.freedesktop.UPower") return 0;

    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return 0;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char* key = nullptr;
        if (sd_bus_message_read(m, "s", &key) >= 0 && key && std::string(key) == "OnBattery") {
            int val = 0;
            if (sd_bus_message_read(m, "v", "b", &val) >= 0) {
                ctx->cb(val != 0);
            }
        } else {
            sd_bus_message_skip(m, "v");
        }
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
    return 0;
}

}  // namespace

LinuxPowerGovernor::~LinuxPowerGovernor() {
    shutdown();
}

bool LinuxPowerGovernor::isOnBatteryBoot() const {
    std::error_code ec;
    bool mains_offline = false;
    bool discharging = false;
    for (const auto& entry : fs::directory_iterator("/sys/class/power_supply", ec)) {
        std::string kind = readTrimmed(entry.path() / "type");
        if (kind == "Mains") {
            if (readTrimmed(entry.path() / "online") == "1") {
                return false;
            }
            mains_offline = true;
        } else if (kind == "Battery") {
            std::string status = readTrimmed(entry.path() / "status");
            std::transform(status.begin(), status.end(), status.begin(), ::tolower);
            if (status == "discharging") {
                discharging = true;
            }
        }
    }
    return discharging || mains_offline;
}

bool LinuxPowerGovernor::applyTimerSlackNs(unsigned long ns) const {
    if (isTuningDisabled()) return false;
    return ::prctl(PR_SET_TIMERSLACK, ns, 0, 0, 0) == 0;
}

void LinuxPowerGovernor::setPerformanceHoldAsync(bool hold) {
    if (isTuningDisabled()) return;

    std::optional<uint32_t> to_release;
    bool need_acquire = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ppd_desired_ = hold;
        if (hold && !ppd_cookie_.has_value()) {
            need_acquire = true;
        } else if (!hold && ppd_cookie_.has_value()) {
            to_release = ppd_cookie_;
            ppd_cookie_.reset();
        }
    }

    if (to_release.has_value()) {
        uint32_t c = *to_release;
        std::thread([c]() { releasePpdProfileSync(c); }).detach();
    } else if (need_acquire) {
        std::thread([this]() {
            auto c = holdPpdProfileSync();
            if (!c.has_value()) return;
            bool release_now = false;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (ppd_desired_ && !stop_watcher_.load()) {
                    ppd_cookie_ = c;
                } else {
                    release_now = true;
                }
            }
            if (release_now) {
                releasePpdProfileSync(*c);
            }
        }).detach();
    }
}

void LinuxPowerGovernor::startBatteryWatcher(std::function<void(bool)> on_change) {
    if (isTuningDisabled() || watcher_thread_.joinable()) return;

    stop_watcher_.store(false);
    if (wake_fd_ < 0) {
        wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    }
    watcher_thread_ = std::thread([this, cb = std::move(on_change)]() mutable {
        sd_bus* bus = nullptr;
        if (sd_bus_open_system(&bus) < 0 || !bus) return;

        BatteryWatchCtx ctx{std::move(cb)};
        sd_bus_slot* slot = nullptr;
        sd_bus_match_signal(
            bus,
            &slot,
            "org.freedesktop.UPower",
            "/org/freedesktop/UPower",
            "org.freedesktop.DBus.Properties",
            "PropertiesChanged",
            onUpowerPropsChanged,
            &ctx);

        const int bus_fd = sd_bus_get_fd(bus);
        while (!stop_watcher_.load()) {
            int r = sd_bus_process(bus, nullptr);
            if (r < 0) break;
            if (r > 0) continue;

            if (bus_fd < 0) {
                sd_bus_wait(bus, 250'000);
                continue;
            }
            int events = sd_bus_get_events(bus);
            uint64_t timeout_usec = UINT64_MAX;
            sd_bus_get_timeout(bus, &timeout_usec);
            int timeout_ms = -1;  // Infinite wait -> 0 idle CPU wakeups/sec!
            if (timeout_usec != UINT64_MAX) {
                timespec ts{};
                ::clock_gettime(CLOCK_MONOTONIC, &ts);
                uint64_t now_usec = static_cast<uint64_t>(ts.tv_sec) * 1'000'000ULL +
                                    static_cast<uint64_t>(ts.tv_nsec) / 1'000ULL;
                timeout_ms = (timeout_usec > now_usec)
                                 ? static_cast<int>(std::min<uint64_t>((timeout_usec - now_usec + 999ULL) / 1000ULL, 60'000ULL))
                                 : 0;
            }
            pollfd fds[2]{};
            fds[0].fd = bus_fd;
            fds[0].events = static_cast<short>(events);
            fds[1].fd = wake_fd_;
            fds[1].events = POLLIN;
            int pr = ::poll(fds, (wake_fd_ >= 0) ? 2 : 1, timeout_ms);
            if (pr < 0 && errno != EINTR) break;
        }

        sd_bus_slot_unref(slot);
        sd_bus_unref(bus);
    });
}

void LinuxPowerGovernor::shutdown() {
    stop_watcher_.store(true);
    if (wake_fd_ >= 0) {
        uint64_t one = 1;
        (void)::write(wake_fd_, &one, sizeof(one));
    }
    if (watcher_thread_.joinable()) {
        watcher_thread_.join();
    }
    if (wake_fd_ >= 0) {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
    std::optional<uint32_t> cookie;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ppd_desired_ = false;
        cookie = ppd_cookie_;
        ppd_cookie_.reset();
    }
    if (cookie.has_value()) {
        releasePpdProfileSync(*cookie);
    }
}

}  // namespace explorer::hal
