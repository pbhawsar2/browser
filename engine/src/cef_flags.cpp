#include "explorer/engine/cef_flags.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace explorer::engine {

namespace {

std::string joinCsv(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += ',';
        out += items[i];
    }
    return out;
}

}  // namespace

void applyHardwareEngineFlags(CefRefPtr<CefCommandLine> cmd,
                              const explorer::hal::HardwareProfile& hw,
                              const explorer::policy::MediaCodecPolicy& media_policy,
                              const std::string& wm_class) {
    // Always keep browser clean of first-run prompts / default checks and set Wayland AppID.
    cmd->AppendSwitch("no-first-run");
    cmd->AppendSwitch("no-default-browser-check");
    cmd->AppendSwitchWithValue("class", wm_class.empty() ? "explorer" : wm_class);

    if (hw.tuning_disabled) {
        return;
    }

    // 1. Native Wayland + AMD Cezanne hardware graphics backend (default: zero-copy ANGLE GL;
    //    optional EXPLORER_GPU_BACKEND=vulkan|graphite for RADV Vulkan/Skia Graphite A/B testing).
    std::string gpu_backend = "angle";
    if (const char* gb = std::getenv("EXPLORER_GPU_BACKEND"); gb && gb[0]) {
        gpu_backend = gb;
    }
    std::vector<std::string> enable_features = {
        "AcceleratedVideoDecodeLinuxGL",
        "AcceleratedVideoDecodeLinuxZeroCopyGL",
        "OverlayUnsupportedVideoSurfaces",
        "VaapiIgnoreDriverChecks",
        "CanvasOopRasterization",
        "BackForwardCache",
        "ParallelDownloading",
        "WaylandPerSurfaceScale",
        "WaylandUiScale",
        "WebAssemblyTiering",
        "WebAssemblyDynamicTiering",
        "V8VmFuture",
    };

    if (std::getenv("EXPLORER_SW_RENDER") != nullptr) {
        cmd->AppendSwitch("disable-gpu");
    } else if (gpu_backend == "vulkan" || gpu_backend == "graphite") {
        cmd->AppendSwitchWithValue("ozone-platform-hint", "auto");
        cmd->AppendSwitchWithValue("use-gl", "angle");
        cmd->AppendSwitchWithValue("use-angle", "vulkan");
        if (!hw.allow_dgpu && !hw.igpu_render_node.empty()) {
            cmd->AppendSwitchWithValue("render-node-override", hw.igpu_render_node);
        }
        cmd->AppendSwitch("enable-gpu-rasterization");
        cmd->AppendSwitch("enable-zero-copy");
        cmd->AppendSwitch("ignore-gpu-blocklist");
        cmd->AppendSwitch("enable-smooth-scrolling");
        enable_features.push_back("Vulkan");
        enable_features.push_back("DefaultANGLEVulkan");
        enable_features.push_back("VulkanFromANGLE");
        if (gpu_backend == "graphite") {
            enable_features.push_back("SkiaGraphite");
        }
    } else {
        cmd->AppendSwitchWithValue("ozone-platform-hint", "auto");
        cmd->AppendSwitchWithValue("use-gl", "angle");
        cmd->AppendSwitchWithValue("use-angle", "gl");
        if (!hw.allow_dgpu && !hw.igpu_render_node.empty()) {
            cmd->AppendSwitchWithValue("render-node-override", hw.igpu_render_node);
        }
        cmd->AppendSwitch("disable-vulkan");  // Avoid Wayland Ozone Vulkan init warning/overhead
        cmd->AppendSwitch("enable-gpu-rasterization");
        cmd->AppendSwitch("enable-zero-copy");
        cmd->AppendSwitch("ignore-gpu-blocklist");
        cmd->AppendSwitch("enable-smooth-scrolling");
    }

    // 2. Strip Chrome background bloat / telemetry / sync / updater / ad-tracking APIs.
    cmd->AppendSwitch("disable-background-networking");
    cmd->AppendSwitch("disable-component-update");
    cmd->AppendSwitch("disable-sync");
    cmd->AppendSwitch("disable-domain-reliability");
    cmd->AppendSwitch("metrics-recording-only");
    cmd->AppendSwitch("no-pings");
    // Prevent WebRTC from leaking local private LAN IPs while keeping video calls working.
    cmd->AppendSwitchWithValue("webrtc-ip-handling-policy", "default_public_interface_only");

    // 3. Hardware VA-API VCN decode + Wayland fractional scaling + BFCache + HTTP/3 + V8/WASM tiering.
    cmd->AppendSwitch("enable-quic");
    cmd->AppendSwitchWithValue("num-raster-threads", "4");
    cmd->AppendSwitchWithValue("renderer-process-limit", "8");

    bool turbo = !hw.on_battery || (std::getenv("EXPLORER_TURBO") != nullptr);
    if (!turbo) {
        enable_features.push_back("BatterySaverModeAlignWakeUps");
        enable_features.push_back("ToggleFreezingOnBatterySaverMode");
        enable_features.push_back("IntensiveWakeUpThrottling");
    }

    // 4. Disable bloat, Google ad-tracking Privacy Sandbox APIs, and software AV1 on Cezanne.
    std::vector<std::string> disable_features = {
        "OptimizationHints",
        "OptimizationTargetPrediction",
        "MediaRouter",
        "Translate",
        "AutofillServerCommunication",
        "InterestFeedContentSuggestions",
        "PrivacySandboxAdsAPIsOverride",
        "BrowsingTopics",
        "Fledge",
        "AttributionReportingCrossAppWeb",
    };
    if (media_policy.shouldDisableSoftwareAv1()) {
        disable_features.push_back("AV1Decoder");
    }

    // 5. Ryzen 7 5800H (8C/16T, 16MB L3) V8 GC/JIT tuning.
    const char* v8_override = std::getenv("EXPLORER_V8_FLAGS");
    const char* v8_prof = std::getenv("EXPLORER_V8_PROFILE");
    if (v8_override && v8_override[0]) {
        cmd->AppendSwitchWithValue("js-flags", v8_override);
    } else if (!v8_prof || std::string(v8_prof) != "stock") {
        cmd->AppendSwitchWithValue(
            "js-flags",
            "--max-semi-space-size=32 --concurrent-recompilation --parallel-scavenge");
    }

    // 6. Optional Widevine CDM auto-discovery for DRM streaming (Netflix/Prime/Hotstar).
    std::error_code ec;
    const std::filesystem::path wv_dir("/opt/google/chrome/WidevineCdm");
    if (std::filesystem::exists(wv_dir / "manifest.json", ec)) {
        cmd->AppendSwitch("enable-widevine");
    }

    cmd->AppendSwitchWithValue("enable-features", joinCsv(enable_features));
    cmd->AppendSwitchWithValue("disable-features", joinCsv(disable_features));
}

}  // namespace explorer::engine
