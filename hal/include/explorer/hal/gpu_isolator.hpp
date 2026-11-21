#pragma once
// SOLID Interface Segregation: GPU discovery & Optimus dGPU isolation.

#include <string>
#include <utility>

namespace explorer::hal {

class IGpuIsolator {
public:
    virtual ~IGpuIsolator() = default;
    /// Returns {render_node_path, pci_vendor_hex} preferring integrated AMD (0x1002) or Intel (0x8086).
    virtual std::pair<std::string, std::string> discoverIgpuRenderNode() const = 0;
    /// Configures Mesa/EGL/GLX/Vulkan/VA-API env vars to keep discrete NVIDIA dGPU in D3cold.
    virtual void applyIsolationEnv(const std::string& vendor, bool allow_dgpu) const = 0;
};

class AmdCezanneGpuIsolator final : public IGpuIsolator {
public:
    std::pair<std::string, std::string> discoverIgpuRenderNode() const override;
    void applyIsolationEnv(const std::string& vendor, bool allow_dgpu) const override;
};

}  // namespace explorer::hal
