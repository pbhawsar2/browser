#include "explorer/hal/gpu_isolator.hpp"
#include "explorer/hal/types.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

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

void setEnvDefault(const char* key, const char* value) {
    if (!std::getenv(key)) {
        ::setenv(key, value, 0);
    }
}

}  // namespace

std::pair<std::string, std::string> AmdCezanneGpuIsolator::discoverIgpuRenderNode() const {
    std::vector<fs::path> nodes;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/drm", ec)) {
        auto name = entry.path().filename().string();
        if (name.rfind("renderD", 0) == 0) {
            nodes.push_back(entry.path());
        }
    }
    std::sort(nodes.begin(), nodes.end());

    std::pair<std::string, std::string> fallback{"/dev/dri/renderD128", ""};
    for (const auto& node : nodes) {
        std::string vendor = readTrimmed(node / "device" / "vendor");
        std::transform(vendor.begin(), vendor.end(), vendor.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string dev = "/dev/dri/" + node.filename().string();
        if (vendor == "0x1002" || vendor == "0x8086") {
            return {dev, vendor};
        }
        if (fallback.second.empty() && fs::exists(dev, ec)) {
            fallback = {dev, vendor};
        }
    }
    return fallback;
}

void AmdCezanneGpuIsolator::applyIsolationEnv(const std::string& vendor, bool allow_dgpu) const {
    if (isTuningDisabled() || allow_dgpu) {
        return;
    }

    // Keep NVIDIA Optimus discrete GPU in D3cold (`suspended`) for 0 RPM quiet browsing.
    setEnvDefault("DRI_PRIME", "0");
    ::unsetenv("__NV_PRIME_RENDER_OFFLOAD");
    ::unsetenv("__VK_LAYER_NV_optimus");
    ::unsetenv("GBM_BACKEND");
    setEnvDefault("CUDA_VISIBLE_DEVICES", "-1");
    setEnvDefault("VK_LOADER_DRIVERS_DISABLE", "*nvidia*");

    std::error_code ec;
    if (fs::exists("/usr/share/glvnd/egl_vendor.d/50_mesa.json", ec)) {
        setEnvDefault("__EGL_VENDOR_LIBRARY_FILENAMES", "/usr/share/glvnd/egl_vendor.d/50_mesa.json");
        setEnvDefault("__GLX_VENDOR_LIBRARY_NAME", "mesa");
    }

    // Scoped strictly to Optimus isolation so EXPLORER_ALLOW_DGPU=1 remains free to use NVDEC.
    if (vendor == "0x1002") {
        setEnvDefault("LIBVA_DRIVER_NAME", "radeonsi");
        if (fs::exists("/usr/lib64/dri-freeworld", ec) || fs::exists("/usr/lib64/dri", ec)) {
            setEnvDefault("LIBVA_DRIVERS_PATH", "/usr/lib64/dri-freeworld:/usr/lib64/dri");
        }
        if (fs::exists("/usr/share/vulkan/icd.d/radeon_icd.x86_64.json", ec)) {
            setEnvDefault("VK_DRIVER_FILES", "/usr/share/vulkan/icd.d/radeon_icd.x86_64.json");
            setEnvDefault("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/radeon_icd.x86_64.json");
        }
    }
    setEnvDefault("MESA_SHADER_CACHE_MAX_SIZE", "2G");
}

}  // namespace explorer::hal
