#pragma once
// SOLID Interface Segregation: Direct O_RDWR libva-drm hardware VLD codec capability probe.

#include "explorer/hal/types.hpp"
#include <string>

namespace explorer::hal {

class IVaapiProber {
public:
    virtual ~IVaapiProber() = default;
    virtual VaCapabilities probe(const std::string& render_node) const = 0;
};

class LibvaDrmProber final : public IVaapiProber {
public:
    VaCapabilities probe(const std::string& render_node) const override;
};

}  // namespace explorer::hal
