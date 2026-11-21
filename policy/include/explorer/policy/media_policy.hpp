#pragma once
// Pure media codec policy: routes video decoding to hardware VCN and blocks CPU-heavy SW AV1.

#include "explorer/hal/types.hpp"

namespace explorer::policy {

class MediaCodecPolicy {
public:
    explicit MediaCodecPolicy(const explorer::hal::VaCapabilities& va, bool allow_dgpu);

    bool shouldDisableSoftwareAv1() const { return disable_sw_av1_; }
    bool hardwareDecodeAvailable() const { return hw_decode_available_; }
    void setHwOnlyVideo(bool enabled) { disable_sw_av1_ = enabled && !va_av1_ && !allow_dgpu_; }

private:
    bool va_av1_ = false;
    bool allow_dgpu_ = false;
    bool disable_sw_av1_ = false;
    bool hw_decode_available_ = false;
};

}  // namespace explorer::policy
