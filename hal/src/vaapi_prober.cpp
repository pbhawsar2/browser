#include "explorer/hal/vaapi_prober.hpp"
#include "explorer/hal/types.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace explorer::hal {

namespace {

std::mutex g_va_cache_mu;
std::string g_cached_node;
VaCapabilities g_cached_caps;
bool g_has_cached_caps = false;

std::string vaCacheFilePath() {
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && xdg[0]) {
        return std::string(xdg) + "/explorer-vaapi.cache";
    }
    return "/tmp/explorer-vaapi-" + std::to_string(::getuid()) + ".cache";
}

long long driverMtimeKey() {
    struct stat st{};
    for (const char* drv : {"/usr/lib64/dri-freeworld/radeonsi_drv_video.so",
                            "/usr/lib64/dri/radeonsi_drv_video.so",
                            "/usr/lib/x86_64-linux-gnu/dri/radeonsi_drv_video.so",
                            "/usr/lib64/dri/iHD_drv_video.so"}) {
        if (::stat(drv, &st) == 0) {
            return static_cast<long long>(st.st_mtim.tv_sec) ^
                   (static_cast<long long>(st.st_size) << 16);
        }
    }
    return 0;
}

}  // namespace

VaCapabilities LibvaDrmProber::probe(const std::string& render_node) const {
    VaCapabilities caps;
    caps.render_node = render_node;
    if (isTuningDisabled() || render_node.empty()) {
        return caps;
    }

    // 1. In-process memoization (~20ns)
    {
        std::lock_guard<std::mutex> lock(g_va_cache_mu);
        if (g_has_cached_caps && g_cached_node == render_node) {
            return g_cached_caps;
        }
    }

    // 2. Persistent tmpfs cache in XDG_RUNTIME_DIR (~15us vs ~120ms dlopen(radeonsi_drv_video.so + libLLVM.so))
    const long long drv_key = driverMtimeKey();
    const std::string cache_path = vaCacheFilePath();
    if (drv_key != 0) {
        std::ifstream in(cache_path);
        std::string c_node;
        long long c_key = 0;
        int h264 = 0, hevc = 0, vp9 = 0, av1 = 0;
        if (in >> c_node >> c_key >> h264 >> hevc >> vp9 >> av1) {
            if (c_node == render_node && c_key == drv_key) {
                caps.h264 = (h264 != 0);
                caps.hevc = (hevc != 0);
                caps.vp9 = (vp9 != 0);
                caps.av1 = (av1 != 0);
                std::lock_guard<std::mutex> lock(g_va_cache_mu);
                g_cached_node = render_node;
                g_cached_caps = caps;
                g_has_cached_caps = true;
                return caps;
            }
        }
    }

    // AMD Mesa requires O_RDWR on /dev/dri/renderD* so amdgpu_bo_cpu_map succeeds.
    int fd = ::open(render_node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return caps;
    }

    void* h_va = ::dlopen("libva.so.2", RTLD_LAZY | RTLD_LOCAL);
    void* h_drm = ::dlopen("libva-drm.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!h_va || !h_drm) {
        if (h_drm) ::dlclose(h_drm);
        if (h_va) ::dlclose(h_va);
        ::close(fd);
        return caps;
    }

    using VADisplay = void*;
    using vaGetDisplayDRM_t = VADisplay (*)(int);
    using vaInitialize_t = int (*)(VADisplay, int*, int*);
    using vaTerminate_t = int (*)(VADisplay);
    using vaMaxNumProfiles_t = int (*)(VADisplay);
    using vaMaxNumEntrypoints_t = int (*)(VADisplay);
    using vaQueryConfigProfiles_t = int (*)(VADisplay, int*, int*);
    using vaQueryConfigEntrypoints_t = int (*)(VADisplay, int, int*, int*);

    auto fn_get = reinterpret_cast<vaGetDisplayDRM_t>(::dlsym(h_drm, "vaGetDisplayDRM"));
    auto fn_init = reinterpret_cast<vaInitialize_t>(::dlsym(h_va, "vaInitialize"));
    auto fn_term = reinterpret_cast<vaTerminate_t>(::dlsym(h_va, "vaTerminate"));
    auto fn_maxp = reinterpret_cast<vaMaxNumProfiles_t>(::dlsym(h_va, "vaMaxNumProfiles"));
    auto fn_maxe = reinterpret_cast<vaMaxNumEntrypoints_t>(::dlsym(h_va, "vaMaxNumEntrypoints"));
    auto fn_qp = reinterpret_cast<vaQueryConfigProfiles_t>(::dlsym(h_va, "vaQueryConfigProfiles"));
    auto fn_qe = reinterpret_cast<vaQueryConfigEntrypoints_t>(::dlsym(h_va, "vaQueryConfigEntrypoints"));

    if (fn_get && fn_init && fn_term && fn_maxp && fn_maxe && fn_qp && fn_qe) {
        if (VADisplay dpy = fn_get(fd)) {
            int major = 0, minor = 0;
            if (fn_init(dpy, &major, &minor) == 0) {
                int max_p = std::max(0, fn_maxp(dpy));
                int max_e = std::max(0, fn_maxe(dpy));
                if (max_p > 0 && max_e > 0) {
                    std::vector<int> profiles(static_cast<size_t>(max_p));
                    std::vector<int> entrypoints(static_cast<size_t>(max_e));
                    int n_profiles = 0;
                    if (fn_qp(dpy, profiles.data(), &n_profiles) == 0) {
                        for (int i = 0; i < n_profiles; ++i) {
                            int n_ep = 0;
                            if (fn_qe(dpy, profiles[i], entrypoints.data(), &n_ep) != 0) {
                                continue;
                            }
                            bool has_vld = false;
                            for (int j = 0; j < n_ep; ++j) {
                                if (entrypoints[j] == 1) {  // VAEntrypointVLD
                                    has_vld = true;
                                    break;
                                }
                            }
                            if (!has_vld) continue;

                            // Upstream libva va.h numeric profile constants
                            switch (profiles[i]) {
                                case 6:   // VAProfileH264Main
                                case 7:   // VAProfileH264High
                                case 13:  // VAProfileH264ConstrainedBaseline
                                    caps.h264 = true;
                                    break;
                                case 17:  // VAProfileHEVCMain
                                case 18:  // VAProfileHEVCMain10
                                    caps.hevc = true;
                                    break;
                                case 19:  // VAProfileVP9Profile0
                                case 21:  // VAProfileVP9Profile2
                                    caps.vp9 = true;
                                    break;
                                case 32:  // VAProfileAV1Profile0
                                    caps.av1 = true;
                                    break;
                                default:
                                    break;
                            }
                        }
                    }
                }
                fn_term(dpy);
            }
        }
    }

    ::dlclose(h_drm);
    ::dlclose(h_va);
    ::close(fd);

    if (caps.h264 || caps.hevc || caps.vp9 || caps.av1) {
        {
            std::lock_guard<std::mutex> lock(g_va_cache_mu);
            g_cached_node = render_node;
            g_cached_caps = caps;
            g_has_cached_caps = true;
        }
        if (drv_key != 0) {
            std::string tmp_path = cache_path + ".tmp";
            int cfd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (cfd >= 0) {
                char line[256];
                int len = std::snprintf(line, sizeof(line), "%s %lld %d %d %d %d\n",
                                        render_node.c_str(), drv_key,
                                        caps.h264 ? 1 : 0, caps.hevc ? 1 : 0,
                                        caps.vp9 ? 1 : 0, caps.av1 ? 1 : 0);
                if (len > 0) (void)::write(cfd, line, static_cast<size_t>(len));
                ::close(cfd);
                (void)::rename(tmp_path.c_str(), cache_path.c_str());
            }
        }
    }
    return caps;
}

}  // namespace explorer::hal
