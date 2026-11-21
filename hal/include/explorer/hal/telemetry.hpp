#pragma once
// SOLID Interface Segregation: Pre-discovered sysfs/hwmon/proc hardware telemetry sampler.

#include "explorer/hal/types.hpp"
#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

namespace explorer::hal {

class ITelemetryMonitor {
public:
    virtual ~ITelemetryMonitor() = default;
    virtual TelemetrySnapshot sample() = 0;
};

class LinuxTelemetryMonitor final : public ITelemetryMonitor {
public:
    LinuxTelemetryMonitor();
    TelemetrySnapshot sample() override;

private:
    std::optional<std::filesystem::path> ac_online_;
    std::optional<std::filesystem::path> batt_status_;
    std::optional<std::filesystem::path> batt_power_uw_;
    std::optional<std::filesystem::path> batt_volt_uv_;
    std::optional<std::filesystem::path> batt_curr_ua_;
    std::optional<std::filesystem::path> rapl_energy_uj_;
    std::optional<std::filesystem::path> cpu_temp_mc_;
    std::optional<std::filesystem::path> igpu_busy_;
    std::optional<std::filesystem::path> igpu_freq_hz_;
    std::optional<std::filesystem::path> igpu_temp_mc_;
    std::optional<std::filesystem::path> igpu_power_uw_;
    std::vector<std::filesystem::path> fan_inputs_;
    std::optional<std::filesystem::path> dgpu_runtime_status_;

    std::optional<std::pair<uint64_t, std::chrono::steady_clock::time_point>> prev_rapl_;
    std::optional<std::pair<uint64_t, uint64_t>> prev_stat_;

    std::optional<float> sampleRaplWatts();
    std::optional<float> sampleBattWatts() const;
    std::optional<float> sampleCpuPercent();
};

}  // namespace explorer::hal
