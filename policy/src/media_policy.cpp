#include "explorer/policy/media_policy.hpp"

namespace explorer::policy {

MediaCodecPolicy::MediaCodecPolicy(const explorer::hal::VaCapabilities& va, bool allow_dgpu)
    : va_av1_(va.av1),
      allow_dgpu_(allow_dgpu),
      disable_sw_av1_(!va.av1 && !allow_dgpu),
      hw_decode_available_(va.h264 || va.vp9 || va.hevc || va.av1) {}

}  // namespace explorer::policy
