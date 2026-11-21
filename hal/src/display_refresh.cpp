#include "explorer/hal/display_refresh.hpp"
#include "explorer/hal/types.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace explorer::hal {

namespace {

struct MonitorMode {
    std::string id;
    int32_t w = 0;
    int32_t h = 0;
    double hz = 0.0;
    bool is_current = false;
};

struct PhysicalMonitor {
    std::string connector;
    std::vector<MonitorMode> modes;
};

struct LogicalMonitor {
    int32_t x = 0;
    int32_t y = 0;
    double scale = 1.0;
    uint32_t transform = 0;
    bool primary = false;
    std::vector<std::string> connectors;
};

int64_t monotonicNowMs() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000LL + static_cast<int64_t>(ts.tv_nsec) / 1'000'000LL;
}

bool applyEdpRefreshSync(double target_hz, bool* did_apply_out) {
    if (did_apply_out) *did_apply_out = false;
    sd_bus* bus = nullptr;
    if (sd_bus_open_user(&bus) < 0 || !bus) return false;

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call_method(
        bus,
        "org.gnome.Mutter.DisplayConfig",
        "/org/gnome/Mutter/DisplayConfig",
        "org.gnome.Mutter.DisplayConfig",
        "GetCurrentState",
        &err,
        &reply,
        "");

    if (r < 0 || !reply) {
        sd_bus_error_free(&err);
        sd_bus_message_unref(reply);
        sd_bus_unref(bus);
        return false;
    }

    uint32_t serial = 0;
    std::vector<PhysicalMonitor> phys;
    std::vector<LogicalMonitor> logicals;

    bool parsed = (sd_bus_message_read(reply, "u", &serial) >= 0);

    // Parse a((ssss)a(siiddada{sv})a{sv})
    if (parsed && sd_bus_message_enter_container(reply, 'a', "((ssss)a(siiddada{sv})a{sv})") >= 0) {
        while (sd_bus_message_enter_container(reply, 'r', "(ssss)a(siiddada{sv})a{sv}") > 0) {
            PhysicalMonitor pm;
            const char *conn = nullptr, *v1 = nullptr, *v2 = nullptr, *v3 = nullptr;
            if (sd_bus_message_read(reply, "(ssss)", &conn, &v1, &v2, &v3) >= 0 && conn) {
                pm.connector = conn;
            }
            if (sd_bus_message_enter_container(reply, 'a', "(siiddada{sv})") >= 0) {
                while (sd_bus_message_enter_container(reply, 'r', "siiddada{sv}") > 0) {
                    MonitorMode mm;
                    const char* mid = nullptr;
                    double pref_scale = 1.0;
                    if (sd_bus_message_read(reply, "siidd", &mid, &mm.w, &mm.h, &mm.hz, &pref_scale) >= 0 && mid) {
                        mm.id = mid;
                    }
                    sd_bus_message_skip(reply, "ad");
                    if (sd_bus_message_enter_container(reply, 'a', "{sv}") >= 0) {
                        while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                            const char* k = nullptr;
                            if (sd_bus_message_read(reply, "s", &k) >= 0 && k && std::string(k) == "is-current") {
                                int cur = 0;
                                if (sd_bus_message_read(reply, "v", "b", &cur) >= 0) {
                                    mm.is_current = (cur != 0);
                                }
                            } else {
                                sd_bus_message_skip(reply, "v");
                            }
                            sd_bus_message_exit_container(reply);
                        }
                        sd_bus_message_exit_container(reply);
                    }
                    pm.modes.push_back(std::move(mm));
                    sd_bus_message_exit_container(reply);
                }
                sd_bus_message_exit_container(reply);
            }
            sd_bus_message_skip(reply, "a{sv}");
            phys.push_back(std::move(pm));
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_exit_container(reply);
    } else {
        parsed = false;
    }

    // Parse a(iiduba(ssss)a{sv})
    if (parsed && sd_bus_message_enter_container(reply, 'a', "(iiduba(ssss)a{sv})") >= 0) {
        while (sd_bus_message_enter_container(reply, 'r', "iiduba(ssss)a{sv}") > 0) {
            LogicalMonitor lm;
            int prim = 0;
            if (sd_bus_message_read(reply, "iidub", &lm.x, &lm.y, &lm.scale, &lm.transform, &prim) < 0) {
                parsed = false;
                sd_bus_message_exit_container(reply);
                break;
            }
            lm.primary = (prim != 0);
            if (sd_bus_message_enter_container(reply, 'a', "(ssss)") >= 0) {
                while (sd_bus_message_enter_container(reply, 'r', "ssss") > 0) {
                    const char *c = nullptr, *s1 = nullptr, *s2 = nullptr, *s3 = nullptr;
                    if (sd_bus_message_read(reply, "ssss", &c, &s1, &s2, &s3) >= 0 && c) {
                        lm.connectors.emplace_back(c);
                    }
                    sd_bus_message_exit_container(reply);
                }
                sd_bus_message_exit_container(reply);
            }
            sd_bus_message_skip(reply, "a{sv}");
            logicals.push_back(std::move(lm));
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_exit_container(reply);
    }

    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);

    if (!parsed) {
        sd_bus_unref(bus);
        return false;
    }

    std::map<std::string, std::string> target_mode_by_conn;
    std::map<std::string, std::string> current_mode_by_conn;

    for (const auto& pm : phys) {
        int cur_w = -1, cur_h = -1;
        for (const auto& m : pm.modes) {
            if (m.is_current) {
                cur_w = m.w;
                cur_h = m.h;
                current_mode_by_conn[pm.connector] = m.id;
                if (pm.connector.rfind("eDP", 0) == 0 && std::abs(m.hz - target_hz) < 2.0) {
                    sd_bus_unref(bus);
                    return true;  // Already at target Hz
                }
            }
        }
        if (pm.connector.rfind("eDP", 0) != 0) continue;

        double best_dist = 1e9;
        std::string best_id;
        bool best_is_cur = false;
        for (const auto& m : pm.modes) {
            if (cur_w > 0 && (m.w != cur_w || m.h != cur_h)) continue;
            double d = std::abs(m.hz - target_hz);
            if (d < best_dist) {
                best_dist = d;
                best_id = m.id;
                best_is_cur = m.is_current;
            }
        }
        // Fallback for non-144Hz high-refresh laptop panels (e.g. 120Hz / 165Hz / 240Hz) on AC Turbo:
        if (target_hz > 65.0 && best_dist > 20.0) {
            double max_hz = 65.0;
            for (const auto& m : pm.modes) {
                if (cur_w > 0 && (m.w != cur_w || m.h != cur_h)) continue;
                if (m.hz > max_hz) {
                    max_hz = m.hz;
                    best_id = m.id;
                    best_is_cur = m.is_current;
                    best_dist = 0.0;
                }
            }
        }
        if (!best_id.empty() && !best_is_cur && best_dist <= 20.0) {
            target_mode_by_conn[pm.connector] = best_id;
        }
    }

    if (target_mode_by_conn.empty()) {
        sd_bus_unref(bus);
        return true;  // No change needed or panel has no closer mode
    }

    sd_bus_message* req = nullptr;
    if (sd_bus_message_new_method_call(
            bus, &req,
            "org.gnome.Mutter.DisplayConfig",
            "/org/gnome/Mutter/DisplayConfig",
            "org.gnome.Mutter.DisplayConfig",
            "ApplyMonitorsConfig") < 0) {
        sd_bus_unref(bus);
        return false;
    }

    // Method 1 = temporary (non-persistent)
    sd_bus_message_append(req, "uu", serial, 1U);
    sd_bus_message_open_container(req, 'a', "(iiduba(ssa{sv}))");
    for (const auto& lm : logicals) {
        sd_bus_message_open_container(req, 'r', "iiduba(ssa{sv})");
        sd_bus_message_append(req, "iidub", lm.x, lm.y, lm.scale, lm.transform, lm.primary ? 1 : 0);
        sd_bus_message_open_container(req, 'a', "(ssa{sv})");
        for (const auto& c : lm.connectors) {
            std::string chosen = target_mode_by_conn.count(c) ? target_mode_by_conn[c] : current_mode_by_conn[c];
            if (!chosen.empty()) {
                sd_bus_message_open_container(req, 'r', "ssa{sv}");
                sd_bus_message_append(req, "ss", c.c_str(), chosen.c_str());
                sd_bus_message_open_container(req, 'a', "{sv}");
                sd_bus_message_close_container(req);
                sd_bus_message_close_container(req);
            }
        }
        sd_bus_message_close_container(req);
        sd_bus_message_close_container(req);
    }
    sd_bus_message_close_container(req);
    sd_bus_message_open_container(req, 'a', "{sv}");
    sd_bus_message_close_container(req);

    sd_bus_message* apply_reply = nullptr;
    int ar = sd_bus_call(bus, req, 2'000'000, &err, &apply_reply);
    bool ok = (ar >= 0);
    if (ok && did_apply_out) *did_apply_out = true;
    sd_bus_error_free(&err);
    sd_bus_message_unref(apply_reply);
    sd_bus_message_unref(req);
    sd_bus_unref(bus);
    return ok;
}

struct MonitorsChangedCtx {
    std::function<void()> cb;
};

int onMonitorsChanged(sd_bus_message* /*m*/, void* userdata, sd_bus_error* /*err*/) {
    auto* ctx = static_cast<MonitorsChangedCtx*>(userdata);
    if (ctx && ctx->cb) ctx->cb();
    return 0;
}

}  // namespace

MutterDisplayRefreshCtrl::~MutterDisplayRefreshCtrl() {
    shutdown();
}

void MutterDisplayRefreshCtrl::setEdpRefreshRateAsync(double target_hz) {
    if (isTuningDisabled() || std::getenv("EXPLORER_NO_REFRESH_SWITCH") != nullptr) {
        return;
    }
    int rounded = static_cast<int>(std::lround(target_hz));
    std::thread prev_worker;
    {
        std::lock_guard<std::mutex> lock(mu_);
        desired_hz_ = rounded;
        if (stop_watcher_.load() || apply_in_flight_ || desired_hz_ == applied_hz_) {
            return;
        }
        apply_in_flight_ = true;
        if (apply_thread_.joinable()) {
            prev_worker = std::move(apply_thread_);
        }
        apply_thread_ = std::thread([this]() {
            std::unique_lock<std::mutex> lock(mu_);
            while (!stop_watcher_.load() && desired_hz_ > 0 && desired_hz_ != applied_hz_) {
                int target = desired_hz_;
                lock.unlock();
                bool did_apply = false;
                bool ok = applyEdpRefreshSync(static_cast<double>(target), &did_apply);
                if (did_apply) {
                    last_self_apply_ms_.store(monotonicNowMs(), std::memory_order_relaxed);
                }
                lock.lock();
                if (ok) {
                    applied_hz_ = target;
                } else {
                    break;  // Leave applied_hz_ unchanged so a subsequent trigger can retry
                }
            }
            apply_in_flight_ = false;
        });
    }
    if (prev_worker.joinable()) {
        prev_worker.join();
    }
}

void MutterDisplayRefreshCtrl::invalidateCache() {
    std::lock_guard<std::mutex> lock(mu_);
    applied_hz_ = -1;
}

void MutterDisplayRefreshCtrl::startMonitorsChangedWatcher(std::function<void()> on_changed) {
    if (isTuningDisabled() || watcher_thread_.joinable()) return;

    stop_watcher_.store(false);
    if (wake_fd_ < 0) {
        wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    }
    watcher_thread_ = std::thread([this, cb = std::move(on_changed)]() mutable {
        sd_bus* bus = nullptr;
        if (sd_bus_open_user(&bus) < 0 || !bus) return;

        MonitorsChangedCtx ctx{[this, cb = std::move(cb)]() {
            int64_t last_ms = last_self_apply_ms_.load(std::memory_order_relaxed);
            if (last_ms > 0 && (monotonicNowMs() - last_ms) < 600) {
                return;  // Suppress echo from our own ApplyMonitorsConfig call
            }
            invalidateCache();
            if (cb) cb();
        }};
        sd_bus_slot* slot = nullptr;
        sd_bus_match_signal(
            bus,
            &slot,
            "org.gnome.Mutter.DisplayConfig",
            "/org/gnome/Mutter/DisplayConfig",
            "org.gnome.Mutter.DisplayConfig",
            "MonitorsChanged",
            onMonitorsChanged,
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
            if (wake_fd_ >= 0 && (fds[1].revents & POLLIN)) {
                uint64_t drained = 0;
                (void)::read(wake_fd_, &drained, sizeof(drained));
            }
        }

        sd_bus_slot_unref(slot);
        sd_bus_unref(bus);
    });
}

void MutterDisplayRefreshCtrl::shutdown() {
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
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (apply_thread_.joinable()) {
            worker = std::move(apply_thread_);
        }
    }
    if (worker.joinable()) {
        worker.join();
    }
}

}  // namespace explorer::hal
