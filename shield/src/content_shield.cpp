#include "explorer/shield/content_shield.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>
#include <vector>

extern char** environ;

extern "C" {
void* explorer_brave_shield_new(const char* rules_utf8);
void* explorer_brave_shield_new_from_dat(const unsigned char* dat_ptr, size_t dat_len, size_t rule_count);
int explorer_brave_shield_write_dat(const void* handle, const char* path_utf8);
size_t explorer_brave_shield_reload(void* handle, const char* rules_utf8);
size_t explorer_brave_shield_rule_count(const void* handle);
int explorer_brave_shield_check_slices(const void* handle,
                                       const char* url_ptr,
                                       size_t url_len,
                                       const char* source_ptr,
                                       size_t source_len,
                                       const char* rtype_ptr,
                                       size_t rtype_len);
void explorer_brave_shield_free(void* handle);
}

namespace explorer::shield {

namespace fs = std::filesystem;

namespace {

std::mutex g_builtin_dat_mu;
std::vector<unsigned char> g_builtin_dat_bytes;
size_t g_builtin_rule_count = 0;

// Built-in baseline EasyList / EasyPrivacy / YouTube-ad + OTT auth-exception rules.
// External updated lists in ~/.local/share/explorer-v2/filters/*.txt are automatically appended!
constexpr const char* kBuiltinRules = R"RULES(
! Explorer v2 Built-in Brave adblock-rust Filter Set (EasyList/uBlock syntax)
||doubleclick.net^
||googlesyndication.com^
||googleadservices.com^
||googletagservices.com^
||google-analytics.com^
||adservice.google.com^
||amazon-adsystem.com^
||scorecardresearch.com^
||quantserve.com^
||taboola.com^
||outbrain.com^
||criteo.com^
||criteo.net^
||pubmatic.com^
||rubiconproject.com^
||openx.net^
||adnxs.com^
||casalemedia.com^
||adsrvr.org^
||rlcdn.com^
||bidswitch.net^
||sharethrough.com^
||triplelift.com^
||smartadserver.com^
||fullstory.com^
||clarity.ms^
||mixpanel.com^
||chartbeat.com^
||chartbeat.net^
||nr-data.net^
||bugsnag.com^
||demdex.net^
||omtrdc.net^
||2o7.net^
||bat.bing.com^
||px.ads.linkedin.com^
||analytics.twitter.com^
||analytics.tiktok.com^
||ads-twitter.com^
||connect.facebook.net/*/fbevents.js
||facebook.com/tr$image,xmlhttprequest,ping
||youtube.com/pagead/
||youtube.com/ptracking
||youtube.com/api/stats/ads
||youtube.com/get_midroll_info
! Telemetry SDKs blocked globally EXCEPT on OTT/banking/payment auth funnels (F8 fix)
||googletagmanager.com/gtm.js$script
||segment.io^
||segment.com^
||amplitude.com^
||hotjar.com^
@@||googletagmanager.com^$domain=jiohotstar.com|hotstar.com|zee5.com|sonyliv.com|netflix.com|primevideo.com|amazon.in|razorpay.com|paytm.com|stripe.com
@@||segment.com^$domain=jiohotstar.com|hotstar.com|zee5.com|sonyliv.com|netflix.com|primevideo.com|amazon.in|razorpay.com|paytm.com|stripe.com
@@||segment.io^$domain=jiohotstar.com|hotstar.com|zee5.com|sonyliv.com|netflix.com|primevideo.com|amazon.in|razorpay.com|paytm.com|stripe.com
@@||amplitude.com^$domain=jiohotstar.com|hotstar.com|zee5.com|sonyliv.com|netflix.com|primevideo.com|amazon.in|razorpay.com|paytm.com|stripe.com
@@||hotjar.com^$domain=jiohotstar.com|hotstar.com|zee5.com|sonyliv.com|netflix.com|primevideo.com|amazon.in|razorpay.com|paytm.com|stripe.com
)RULES";

std::string_view resourceKindToBraveType(ResourceKind kind) {
    switch (kind) {
        case ResourceKind::MainFrame: return "document";
        case ResourceKind::SubFrame: return "subdocument";
        case ResourceKind::Script: return "script";
        case ResourceKind::Image: return "image";
        case ResourceKind::Stylesheet: return "stylesheet";
        case ResourceKind::Font: return "font";
        case ResourceKind::Media: return "media";
        case ResourceKind::Object: return "object";
        case ResourceKind::XhrOrFetch: return "xmlhttprequest";
        case ResourceKind::Ping: return "ping";
        case ResourceKind::CspReport: return "csp_report";
        case ResourceKind::Other: return "other";
    }
    return "other";
}

bool spawnCurlHttps(const char* url,
                    const std::string& out_path,
                    std::atomic<pid_t>& active_pid_slot,
                    const std::atomic<bool>& shutting_down) {
    if (shutting_down.load()) return false;
    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) return false;
    ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    const char* argv[] = {
        "curl", "-fsSL", "--proto", "=https", "--tlsv1.2",
        "--max-time", "20", "--max-filesize", "10485760",
        url, "-o", out_path.c_str(), nullptr
    };
    pid_t pid = -1;
    int rc = ::posix_spawnp(&pid, "curl", &actions, nullptr,
                            const_cast<char* const*>(argv), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    if (rc != 0 || pid <= 0) return false;

    active_pid_slot.store(pid, std::memory_order_release);
    if (shutting_down.load()) {
        ::kill(pid, SIGTERM);
    }

    int st = 0;
    while (::waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) {
            active_pid_slot.store(-1, std::memory_order_release);
            return false;
        }
    }
    active_pid_slot.store(-1, std::memory_order_release);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

}  // namespace

std::string BraveAdblockShield::computeFilterFingerprint() const {
    std::ostringstream fp;
    fp << "v1:" << std::hash<std::string_view>{}(kBuiltinRules);
    if (!filters_dir_.empty()) {
        std::vector<std::string> items;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(filters_dir_, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".txt") continue;
            auto sz = entry.file_size(ec);
            auto mt = entry.last_write_time(ec).time_since_epoch().count();
            items.push_back(entry.path().filename().string() + ":" + std::to_string(sz) + ":" + std::to_string(mt));
        }
        std::sort(items.begin(), items.end());
        for (const auto& it : items) fp << "|" << it;
    }
    return fp.str();
}

void BraveAdblockShield::saveBinaryCache(size_t rule_count, const std::string& fingerprint) const {
    if (filters_dir_.empty() || !handle_) return;
    std::error_code ec;
    fs::create_directories(filters_dir_, ec);
    fs::path dat_path = filters_dir_ / "engine.dat";
    fs::path dat_tmp = filters_dir_ / "engine.dat.tmp";
    fs::path meta_path = filters_dir_ / "engine.meta";
    fs::path meta_tmp = filters_dir_ / "engine.meta.tmp";

    if (explorer_brave_shield_write_dat(handle_, dat_tmp.c_str()) != 1) {
        fs::remove(dat_tmp, ec);
        return;
    }
    fs::rename(dat_tmp, dat_path, ec);
    if (ec) {
        fs::remove(dat_tmp, ec);
        return;
    }
    {
        std::ofstream out(meta_tmp, std::ios::trunc);
        if (!out.is_open()) return;
        out << rule_count << "\n" << fingerprint << "\n";
        out.flush();
    }
    fs::rename(meta_tmp, meta_path, ec);
}

BraveAdblockShield::BraveAdblockShield(fs::path filters_dir, bool enabled)
    : filters_dir_(std::move(filters_dir)), enabled_(enabled) {
    const std::string fp = computeFilterFingerprint();
    // Fast path: load precompiled binary `.dat` engine (~5-15ms) if fingerprint matches.
    if (!filters_dir_.empty()) {
        fs::path meta_path = filters_dir_ / "engine.meta";
        fs::path dat_path = filters_dir_ / "engine.dat";
        std::ifstream meta_in(meta_path);
        std::string cnt_line, fp_line;
        if (meta_in.is_open() && std::getline(meta_in, cnt_line) && std::getline(meta_in, fp_line) && fp_line == fp) {
            size_t cached_count = 0;
            try { cached_count = std::stoull(cnt_line); } catch (...) { cached_count = 0; }
            std::ifstream dat_in(dat_path, std::ios::binary | std::ios::ate);
            if (cached_count > 0 && dat_in.is_open()) {
                auto sz = dat_in.tellg();
                if (sz > 0 && sz < 64 * 1024 * 1024) {
                    dat_in.seekg(0, std::ios::beg);
                    std::vector<unsigned char> bytes(static_cast<size_t>(sz));
                    if (dat_in.read(reinterpret_cast<char*>(bytes.data()), sz)) {
                        handle_ = explorer_brave_shield_new_from_dat(bytes.data(), bytes.size(), cached_count);
                    }
                }
            }
        }
    }
    if (!handle_) {
        // Fast in-memory cache when using built-in baseline rules only (22x faster cold construction).
        const bool builtin_only = (fp.find('|') == std::string::npos);
        if (builtin_only) {
            std::lock_guard<std::mutex> lock(g_builtin_dat_mu);
            if (!g_builtin_dat_bytes.empty() && g_builtin_rule_count > 0) {
                handle_ = explorer_brave_shield_new_from_dat(
                    g_builtin_dat_bytes.data(), g_builtin_dat_bytes.size(), g_builtin_rule_count);
            }
        }
        if (!handle_) {
            std::string combined = loadCombinedRules();
            handle_ = explorer_brave_shield_new(combined.c_str());
            if (handle_) {
                size_t rcount = explorer_brave_shield_rule_count(handle_);
                if (!filters_dir_.empty()) {
                    saveBinaryCache(rcount, fp);
                } else if (builtin_only) {
                    std::lock_guard<std::mutex> lock(g_builtin_dat_mu);
                    if (g_builtin_dat_bytes.empty()) {
                        fs::path tmp_dat = fs::temp_directory_path() /
                                           ("explorer-builtin-" + std::to_string(::getpid()) + ".dat");
                        if (explorer_brave_shield_write_dat(handle_, tmp_dat.c_str()) == 1) {
                            std::ifstream in(tmp_dat, std::ios::binary | std::ios::ate);
                            if (in.is_open()) {
                                auto sz = in.tellg();
                                if (sz > 0 && sz < 8 * 1024 * 1024) {
                                    in.seekg(0, std::ios::beg);
                                    g_builtin_dat_bytes.resize(static_cast<size_t>(sz));
                                    if (in.read(reinterpret_cast<char*>(g_builtin_dat_bytes.data()), sz)) {
                                        g_builtin_rule_count = rcount;
                                    } else {
                                        g_builtin_dat_bytes.clear();
                                    }
                                }
                            }
                            std::error_code ec;
                            fs::remove(tmp_dat, ec);
                        }
                    }
                }
            }
        }
    }
}

BraveAdblockShield::~BraveAdblockShield() {
    shutting_down_.store(true);
    pid_t curl_pid = active_curl_pid_.load(std::memory_order_acquire);
    if (curl_pid > 1) {
        ::kill(curl_pid, SIGTERM);
    }
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(update_mu_);
        if (update_thread_.joinable()) {
            worker = std::move(update_thread_);
        }
    }
    if (worker.joinable()) {
        worker.join();
    }
    if (handle_) {
        explorer_brave_shield_free(handle_);
        handle_ = nullptr;
    }
}

std::string BraveAdblockShield::loadCombinedRules() const {
    std::ostringstream out;
    out << kBuiltinRules << "\n";
    if (!filters_dir_.empty()) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(filters_dir_, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".txt") continue;
            std::ifstream in(entry.path());
            if (in.is_open()) {
                out << "\n" << in.rdbuf() << "\n";
            }
        }
    }
    return out.str();
}

size_t BraveAdblockShield::ruleCount() const {
    return explorer_brave_shield_rule_count(handle_);
}

BlockDecision BraveAdblockShield::evaluate(std::string_view request_url,
                                           std::string_view initiator_origin,
                                           ResourceKind kind) const {
    if (!enabled_.load(std::memory_order_relaxed) || !handle_ || kind == ResourceKind::MainFrame) {
        return {false, ""};
    }

    const std::string_view rtype = resourceKindToBraveType(kind);
    int matched = explorer_brave_shield_check_slices(
        handle_,
        request_url.data(), request_url.size(),
        initiator_origin.data(), initiator_origin.size(),
        rtype.data(), rtype.size());
    if (matched == 1) {
        blocked_count_.fetch_add(1, std::memory_order_relaxed);
        return {true, "brave-adblock"};
    }
    return {false, ""};
}

void BraveAdblockShield::updateFilterListsAsync(std::function<void(bool, size_t)> on_done) {
    if (filters_dir_.empty() || shutting_down_.load()) {
        if (on_done) on_done(false, ruleCount());
        return;
    }
    if (updating_.exchange(true)) {
        if (on_done) on_done(false, ruleCount());
        return;
    }

    std::thread prev_worker;
    {
        std::lock_guard<std::mutex> lock(update_mu_);
        if (shutting_down_.load()) {
            updating_.store(false);
            if (on_done) on_done(false, ruleCount());
            return;
        }
        if (update_thread_.joinable()) {
            prev_worker = std::move(update_thread_);
        }
        update_thread_ = std::thread([this, cb = std::move(on_done)]() {
            std::error_code ec;
            fs::create_directories(filters_dir_, ec);

            struct Source {
                const char* name;
                const char* url;
            };
            static constexpr Source kSources[] = {
                {"peter-lowe.txt",
                 "https://pgl.yoyo.org/adservers/serverlist.php?hostformat=adblockplus&showintro=0&mimetype=plaintext"},
                {"easylist.txt", "https://easylist.to/easylist/easylist.txt"},
                {"easyprivacy.txt", "https://easylist.to/easylist/easyprivacy.txt"},
            };

            bool any_ok = false;
            for (const auto& s : kSources) {
                if (shutting_down_.load()) break;
                fs::path dest = filters_dir_ / s.name;
                fs::path tmp = dest;
                tmp += ".tmp";
                bool ok = spawnCurlHttps(s.url, tmp.string(), active_curl_pid_, shutting_down_);
                if (ok && fs::exists(tmp, ec) && fs::file_size(tmp, ec) > 1024) {
                    fs::rename(tmp, dest, ec);
                    any_ok = true;
                } else {
                    fs::remove(tmp, ec);
                }
            }

            size_t total = ruleCount();
            if (any_ok && !shutting_down_.load() && handle_) {
                std::string combined = loadCombinedRules();
                total = explorer_brave_shield_reload(handle_, combined.c_str());
                saveBinaryCache(total, computeFilterFingerprint());
            }
            updating_.store(false);
            if (cb && !shutting_down_.load()) cb(any_ok, total);
        });
    }
    if (prev_worker.joinable()) {
        prev_worker.join();
    }
}

}  // namespace explorer::shield
