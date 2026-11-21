#pragma once
// Hardware-driven Chromium/Blink/V8/Skia/VA-API command-line flag builder.

#include "explorer/hal/types.hpp"
#include "explorer/policy/media_policy.hpp"
#include "include/cef_command_line.h"
#include <string>

namespace explorer::engine {

/// Applies AMD Cezanne Wayland/VA-API, Optimus isolation, bloat removal, and V8 tuning flags.
/// Cleanly no-ops (keeps upstream defaults) when hw.tuning_disabled is true.
void applyHardwareEngineFlags(CefRefPtr<CefCommandLine> cmd,
                              const explorer::hal::HardwareProfile& hw,
                              const explorer::policy::MediaCodecPolicy& media_policy,
                              const std::string& wm_class = "explorer-v2");

}  // namespace explorer::engine
