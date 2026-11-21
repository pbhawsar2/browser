#include "explorer/hal/telemetry.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <string_view>

namespace explorer::hal {

namespace fs = std::filesystem;

namespace {

/// Zero-allocation POSIX read into a stack buffer (eliminates ifstream/string heap churn per tick).
ssize_t readSmallFile(const fs::path& p, char* buf, size_t cap) {
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = ::read(fd, buf, cap);
    ::close(fd);
    return n;
}

std::optional<fs::path> readablePath(const fs::path& p) {
    // Verify actual open() readability (catches 0400 root-only files like intel-rapl energy_uj).
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ::close(fd);
        return p;
    }
    return std::nullopt;
}

std::optional<std::string> readTrimmed(const fs::path& p) {
    char buf[256];
    ssize_t n = readSmallFile(p, buf, sizeof(buf));
    if (n <= 0) return std::nullopt;
    std::string_view sv(buf, static_cast<size_t>(n));
    auto nl = sv.find('\n');
    if (nl != std::string_view::npos) sv = sv.substr(0, nl);
    while (!sv.empty() && (sv.back() == '\r' || sv.back() == ' ')) {
        sv.remove_suffix(1);
    }
    return std::string(sv);
}

std::optional<uint64_t> readU64(const fs::path& p) {
    char buf[64];
    ssize_t n = readSmallFile(p, buf, sizeof(buf));
    if (n <= 0) return std::nullopt;
    const char* start = buf;
    const char* end = buf + n;
    while (start < end && (*start == ' ' || *start == '\t')) ++start;
    uint64_t val = 0;
    auto [ptr, ec] = std::from_chars(start, end, val);
    if (ec != std::errc{} || ptr == start) return std::nullopt;
    return val;
}

}  // namespace

LinuxTelemetryMonitor::LinuxTelemetryMonitor() {
    rapl_energy_uj_ = readablePath("/sys/class/powercap/intel-rapl:0/energy_uj");

    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/power_supply", ec)) {
        auto kind = readTrimmed(e.path() / "type").value_or("");
        if (kind == "Mains" && !ac_online_) {
            ac_online_ = readablePath(e.path() / "online");
        } else if (kind == "Battery" && !batt_status_) {
            batt_status_ = readablePath(e.path() / "status");
            batt_capacity_ = readablePath(e.path() / "capacity");
            batt_power_uw_ = readablePath(e.path() / "power_now");
            batt_volt_uv_ = readablePath(e.path() / "voltage_now");
            batt_curr_ua_ = readablePath(e.path() / "current_now");
        }
    }

    for (const auto& e : fs::directory_iterator("/sys/class/hwmon", ec)) {
        auto name = readTrimmed(e.path() / "name").value_or("");
        if ((name == "k10temp" || name == "coretemp") && !cpu_temp_mc_) {
            cpu_temp_mc_ = readablePath(e.path() / "temp1_input");
        } else if (name == "amdgpu" || name == "i915") {
            if (!igpu_temp_mc_) igpu_temp_mc_ = readablePath(e.path() / "temp1_input");
            if (!igpu_freq_hz_) igpu_freq_hz_ = readablePath(e.path() / "freq1_input");
            if (!igpu_power_uw_) igpu_power_uw_ = readablePath(e.path() / "power1_input");
        }
        for (int idx = 1; idx <= 4; ++idx) {
            if (auto f = readablePath(e.path() / ("fan" + std::to_string(idx) + "_input"))) {
                fan_inputs_.push_back(*f);
            }
        }
    }

    for (const auto& e : fs::directory_iterator("/sys/class/drm", ec)) {
        auto name = e.path().filename().string();
        if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos) continue;
        auto dev = e.path() / "device";
        auto vendor = readTrimmed(dev / "vendor").value_or("");
        std::transform(vendor.begin(), vendor.end(), vendor.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (vendor == "0x1002" && !igpu_busy_) {
            igpu_busy_ = readablePath(dev / "gpu_busy_percent");
        } else if (vendor == "0x8086") {
            if (!igpu_freq_hz_) {
                igpu_freq_hz_ = readablePath(e.path() / "gt_act_freq_mhz");
            }
        } else if (vendor == "0x10de" && !dgpu_runtime_status_) {
            dgpu_runtime_status_ = readablePath(dev / "power" / "runtime_status");
        }
    }
}

TelemetrySnapshot LinuxTelemetryMonitor::sample() {
    std::lock_guard<std::mutex> lock(sample_mu_);
    TelemetrySnapshot out;

    std::optional<bool> ac_on;
    if (ac_online_) {
        if (auto v = readU64(*ac_online_)) ac_on = (*v == 1);
    }
    bool discharging = false;
    if (batt_status_) {
        auto st = readTrimmed(*batt_status_).value_or("");
        if (!st.empty()) out.batt_status_str = st;
        std::string st_low = st;
        std::transform(st_low.begin(), st_low.end(), st_low.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        discharging = (st_low == "discharging");
    }
    if (batt_capacity_) {
        if (auto cap = readU64(*batt_capacity_)) {
            out.batt_capacity_pct = static_cast<uint32_t>(std::min<uint64_t>(*cap, 100ULL));
        }
    }
    out.on_battery = discharging || (batt_status_.has_value() && ac_on.has_value() && !*ac_on);

    if (auto pct = sampleCpuPercent()) out.cpu_percent = *pct;
    if (cpu_temp_mc_) {
        if (auto mc = readU64(*cpu_temp_mc_)) out.cpu_temp_c = static_cast<float>(*mc) / 1000.0f;
    }
    if (igpu_temp_mc_) {
        if (auto mc = readU64(*igpu_temp_mc_)) out.igpu_temp_c = static_cast<float>(*mc) / 1000.0f;
    }
    if (igpu_freq_hz_) {
        if (auto hz = readU64(*igpu_freq_hz_)) {
            // hwmon freq1_input is in Hz (> 100000), DRM gt_act_freq_mhz is already in MHz.
            out.igpu_mhz = static_cast<uint32_t>(*hz > 100'000ULL ? (*hz / 1'000'000ULL) : *hz);
        }
    }
    if (igpu_power_uw_) {
        if (auto uw = readU64(*igpu_power_uw_)) out.igpu_watts = static_cast<float>(*uw) / 1'000'000.0f;
    }
    if (igpu_busy_) {
        if (auto b = readU64(*igpu_busy_)) out.igpu_busy = static_cast<uint32_t>(*b);
    }

    out.pkg_watts = sampleRaplWatts();
    // On AMD Ryzen APUs where intel-rapl is 0400 root-only, amdgpu power1_input (PPT) reports APU package power.
    if (!out.pkg_watts.has_value() && out.igpu_watts.has_value()) {
        out.pkg_watts = out.igpu_watts;
    }
    out.batt_watts = sampleBattWatts();

    out.fan_rpms.reserve(fan_inputs_.size());
    for (const auto& f : fan_inputs_) {
        if (auto rpm = readU64(f)) {
            uint32_t r = static_cast<uint32_t>(*rpm);
            out.fan_rpms.push_back(r);
            out.fan_rpm = out.fan_rpm.has_value() ? std::max(*out.fan_rpm, r) : r;
        }
    }

    if (dgpu_runtime_status_) {
        out.dgpu_state = readTrimmed(*dgpu_runtime_status_);
    }

    return out;
}

TelemetrySnapshot LinuxTelemetryMonitor::sampleLite() {
    std::lock_guard<std::mutex> lock(sample_mu_);
    TelemetrySnapshot out;
    std::optional<bool> ac_on;
    if (ac_online_) {
        if (auto v = readU64(*ac_online_)) ac_on = (*v == 1);
    }
    bool discharging = false;
    if (batt_status_) {
        char sbuf[64];
        ssize_t sn = readSmallFile(*batt_status_, sbuf, sizeof(sbuf));
        if (sn >= 11) {
            // Case-insensitive stack check for "discharging" (zero heap string allocation)
            discharging = ((sbuf[0] == 'D' || sbuf[0] == 'd') &&
                           (sbuf[1] == 'i' || sbuf[1] == 'I') &&
                           (sbuf[2] == 's' || sbuf[2] == 'S') &&
                           (sbuf[3] == 'c' || sbuf[3] == 'C'));
        }
    }
    out.on_battery = discharging || (batt_status_.has_value() && ac_on.has_value() && !*ac_on);
    // Keep /proc/stat delta warm on the background thread (<4us stack read) so opening HUD shows instant 2s CPU%!
    if (auto pct = sampleCpuPercent()) out.cpu_percent = *pct;
    if (out.on_battery) {
        out.batt_watts = sampleBattWatts();
    }
    out.fan_rpms.reserve(fan_inputs_.size());
    for (const auto& f : fan_inputs_) {
        if (auto rpm = readU64(f)) {
            uint32_t r = static_cast<uint32_t>(*rpm);
            out.fan_rpms.push_back(r);
            out.fan_rpm = out.fan_rpm.has_value() ? std::max(*out.fan_rpm, r) : r;
        }
    }
    return out;
}

std::optional<float> LinuxTelemetryMonitor::sampleRaplWatts() {
    if (!rapl_energy_uj_) return std::nullopt;
    auto energy = readU64(*rapl_energy_uj_);
    if (!energy) return std::nullopt;

    auto now = std::chrono::steady_clock::now();
    std::optional<float> watts;
    if (prev_rapl_) {
        double dt = std::chrono::duration<double>(now - prev_rapl_->second).count();
        if (dt > 0.15 && dt < 30.0 && *energy >= prev_rapl_->first) {
            float w = static_cast<float>(static_cast<double>(*energy - prev_rapl_->first) / (dt * 1'000'000.0));
            if (w >= 0.0f && w <= 250.0f) {
                watts = w;
            }
        }
    }
    prev_rapl_ = {*energy, now};
    return watts;
}

std::optional<float> LinuxTelemetryMonitor::sampleBattWatts() const {
    if (batt_power_uw_) {
        if (auto uw = readU64(*batt_power_uw_)) {
            return static_cast<float>(*uw) / 1'000'000.0f;
        }
    }
    if (batt_volt_uv_ && batt_curr_ua_) {
        auto uv = readU64(*batt_volt_uv_);
        auto ua = readU64(*batt_curr_ua_);
        if (uv && ua) {
            return static_cast<float>((static_cast<double>(*uv) * static_cast<double>(*ua)) / 1e12);
        }
    }
    return std::nullopt;
}

std::optional<float> LinuxTelemetryMonitor::sampleCpuPercent() {
    char buf[256];
    ssize_t n = readSmallFile("/proc/stat", buf, sizeof(buf));
    if (n <= 4) return std::nullopt;

    const char* p = buf;
    const char* end = buf + n;
    // Only parse the first line ("cpu  user nice sys idle iowait irq softirq steal ...")
    for (const char* q = p; q < end; ++q) {
        if (*q == '\n') {
            end = q;
            break;
        }
    }
    // Skip leading "cpu" token
    while (p < end && *p != ' ' && *p != '\t') ++p;

    uint64_t fields[8]{};
    int parsed = 0;
    for (int i = 0; i < 8; ++i) {
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (p >= end) break;
        auto [next, ec] = std::from_chars(p, end, fields[i]);
        if (ec != std::errc{} || next == p) break;
        p = next;
        ++parsed;
    }
    if (parsed < 4) return std::nullopt;

    uint64_t total = fields[0] + fields[1] + fields[2] + fields[3] +
                     fields[4] + fields[5] + fields[6] + fields[7];
    uint64_t idle_all = fields[3] + fields[4];

    std::optional<float> pct;
    if (prev_stat_) {
        uint64_t dt = total >= prev_stat_->first ? total - prev_stat_->first : 0;
        uint64_t di = idle_all >= prev_stat_->second ? idle_all - prev_stat_->second : 0;
        if (dt > 0) {
            pct = static_cast<float>(dt - di) * 100.0f / static_cast<float>(dt);
        }
    }
    prev_stat_ = {total, idle_all};
    return pct;
}

}  // namespace explorer::hal
