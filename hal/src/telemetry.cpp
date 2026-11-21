#include "explorer/hal/telemetry.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

namespace explorer::hal {

namespace fs = std::filesystem;

namespace {

std::optional<fs::path> readablePath(const fs::path& p) {
    // Verify actual open() readability (catches 0400 root-only files like intel-rapl energy_uj).
    std::ifstream in(p);
    if (in.is_open()) {
        return p;
    }
    return std::nullopt;
}

std::optional<std::string> readTrimmed(const fs::path& p) {
    std::ifstream in(p);
    std::string s;
    if (!std::getline(in, s)) return std::nullopt;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

std::optional<uint64_t> readU64(const fs::path& p) {
    auto s = readTrimmed(p);
    if (!s.has_value() || s->empty()) return std::nullopt;
    try {
        return std::stoull(*s);
    } catch (...) {
        return std::nullopt;
    }
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
        std::transform(vendor.begin(), vendor.end(), vendor.begin(), ::tolower);
        if (vendor == "0x1002" && !igpu_busy_) {
            igpu_busy_ = readablePath(dev / "gpu_busy_percent");
        } else if (vendor == "0x10de" && !dgpu_runtime_status_) {
            dgpu_runtime_status_ = readablePath(dev / "power" / "runtime_status");
        }
    }
}

TelemetrySnapshot LinuxTelemetryMonitor::sample() {
    TelemetrySnapshot out;

    std::optional<bool> ac_on;
    if (ac_online_) {
        if (auto v = readU64(*ac_online_)) ac_on = (*v == 1);
    }
    bool discharging = false;
    if (batt_status_) {
        auto st = readTrimmed(*batt_status_).value_or("");
        std::transform(st.begin(), st.end(), st.begin(), ::tolower);
        discharging = (st == "discharging");
    }
    out.on_battery = discharging || (ac_on.has_value() && !*ac_on);

    if (auto pct = sampleCpuPercent()) out.cpu_percent = *pct;
    if (cpu_temp_mc_) {
        if (auto mc = readU64(*cpu_temp_mc_)) out.cpu_temp_c = static_cast<float>(*mc) / 1000.0f;
    }
    if (igpu_temp_mc_) {
        if (auto mc = readU64(*igpu_temp_mc_)) out.igpu_temp_c = static_cast<float>(*mc) / 1000.0f;
    }
    if (igpu_freq_hz_) {
        if (auto hz = readU64(*igpu_freq_hz_)) out.igpu_mhz = static_cast<uint32_t>(*hz / 1'000'000ULL);
    }
    if (igpu_power_uw_) {
        if (auto uw = readU64(*igpu_power_uw_)) out.igpu_watts = static_cast<float>(*uw) / 1'000'000.0f;
    }
    if (igpu_busy_) {
        if (auto b = readU64(*igpu_busy_)) out.igpu_busy = static_cast<uint32_t>(*b);
    }

    out.pkg_watts = sampleRaplWatts();
    out.batt_watts = sampleBattWatts();

    for (const auto& f : fan_inputs_) {
        if (auto rpm = readU64(f)) {
            uint32_t r = static_cast<uint32_t>(*rpm);
            out.fan_rpm = out.fan_rpm.has_value() ? std::max(*out.fan_rpm, r) : r;
        }
    }

    if (dgpu_runtime_status_) {
        out.dgpu_state = readTrimmed(*dgpu_runtime_status_);
    }

    return out;
}

TelemetrySnapshot LinuxTelemetryMonitor::sampleLite() {
    TelemetrySnapshot out;
    std::optional<bool> ac_on;
    if (ac_online_) {
        if (auto v = readU64(*ac_online_)) ac_on = (*v == 1);
    }
    bool discharging = false;
    if (batt_status_) {
        auto st = readTrimmed(*batt_status_).value_or("");
        std::transform(st.begin(), st.end(), st.begin(), ::tolower);
        discharging = (st == "discharging");
    }
    out.on_battery = discharging || (ac_on.has_value() && !*ac_on);
    if (out.on_battery) {
        out.batt_watts = sampleBattWatts();
    }
    for (const auto& f : fan_inputs_) {
        if (auto rpm = readU64(f)) {
            uint32_t r = static_cast<uint32_t>(*rpm);
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
        if (dt > 0.15 && *energy >= prev_rapl_->first) {
            watts = static_cast<float>(static_cast<double>(*energy - prev_rapl_->first) / (dt * 1'000'000.0));
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
    std::ifstream in("/proc/stat");
    std::string line;
    if (!std::getline(in, line)) return std::nullopt;

    std::istringstream iss(line);
    std::string tag;
    uint64_t user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (!(iss >> tag >> user >> nice >> sys >> idle)) return std::nullopt;
    iss >> iowait >> irq >> softirq >> steal;

    uint64_t total = user + nice + sys + idle + iowait + irq + softirq + steal;
    uint64_t idle_all = idle + iowait;

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
