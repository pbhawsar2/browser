#include "explorer/hal/vaapi_prober.hpp"
#include "explorer/hal/types.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

namespace explorer::hal {

VaCapabilities LibvaDrmProber::probe(const std::string& render_node) const {
    VaCapabilities caps;
    caps.render_node = render_node;
    if (isTuningDisabled() || render_node.empty()) {
        return caps;
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
    return caps;
}

}  // namespace explorer::hal
