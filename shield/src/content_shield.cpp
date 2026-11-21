#include "explorer/shield/content_shield.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

extern "C" {
void* explorer_brave_shield_new(const char* rules_utf8);
size_t explorer_brave_shield_reload(void* handle, const char* rules_utf8);
size_t explorer_brave_shield_rule_count(const void* handle);
int explorer_brave_shield_check(const void* handle,
                                const char* url_utf8,
                                const char* source_url_utf8,
                                const char* request_type_utf8);
void explorer_brave_shield_free(void* handle);
}

namespace explorer::shield {

namespace fs = std::filesystem;

namespace {

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

const char* resourceKindToBraveType(ResourceKind kind) {
    switch (kind) {
        case ResourceKind::MainFrame: return "document";
        case ResourceKind::SubFrame: return "subdocument";
        case ResourceKind::Script: return "script";
        case ResourceKind::Image: return "image";
        case ResourceKind::Stylesheet: return "stylesheet";
        case ResourceKind::XhrOrFetch: return "xmlhttprequest";
        case ResourceKind::Other: return "other";
    }
    return "other";
}

}  // namespace

BraveAdblockShield::BraveAdblockShield(fs::path filters_dir, bool enabled)
    : filters_dir_(std::move(filters_dir)), enabled_(enabled) {
    std::string combined = loadCombinedRules();
    handle_ = explorer_brave_shield_new(combined.c_str());
}

BraveAdblockShield::~BraveAdblockShield() {
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

    std::string url(request_url);
    std::string src(initiator_origin);
    int matched = explorer_brave_shield_check(
        handle_, url.c_str(), src.c_str(), resourceKindToBraveType(kind));
    if (matched == 1) {
        blocked_count_.fetch_add(1, std::memory_order_relaxed);
        return {true, "brave-adblock"};
    }
    return {false, ""};
}

void BraveAdblockShield::updateFilterListsAsync(std::function<void(bool, size_t)> on_done) {
    if (filters_dir_.empty()) {
        if (on_done) on_done(false, ruleCount());
        return;
    }

    std::thread([this, cb = std::move(on_done)]() {
        std::error_code ec;
        fs::create_directories(filters_dir_, ec);

        // Download reputed, lightweight, zero-breakage community lists (Peter Lowe + EasyPrivacy).
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
            fs::path dest = filters_dir_ / s.name;
            fs::path tmp = dest;
            tmp += ".tmp";
            std::string cmd = "curl -fsSL --max-time 20 '" + std::string(s.url) + "' -o '" +
                              tmp.string() + "' >/dev/null 2>&1";
            if (std::system(cmd.c_str()) == 0 && fs::exists(tmp, ec) && fs::file_size(tmp, ec) > 1024) {
                fs::rename(tmp, dest, ec);
                any_ok = true;
            } else {
                fs::remove(tmp, ec);
            }
        }

        size_t total = ruleCount();
        if (any_ok && handle_) {
            std::string combined = loadCombinedRules();
            total = explorer_brave_shield_reload(handle_, combined.c_str());
        }
        if (cb) cb(any_ok, total);
    }).detach();
}

}  // namespace explorer::shield
