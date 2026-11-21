#include "explorer/shield/content_shield.hpp"

#include <cstdlib>
#include <iostream>

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::cerr << "[test_shield] FAIL at line " << __LINE__ << ": " #cond << "\n"; \
            std::exit(1);                                                                \
        }                                                                                \
    } while (0)

int main() {
    using namespace explorer::shield;

    BraveAdblockShield shield({}, true);
    CHECK(shield.isEnabled());
    CHECK(shield.ruleCount() >= 45);

    // 1. Third-party ad script blocked by Brave adblock-rust.
    auto d1 = shield.evaluate("https://pagead2.googlesyndication.com/pagead/js/adsbygoogle.js",
                              "https://www.example.com/", ResourceKind::Script);
    CHECK(d1.block);

    // 2. YouTube ad endpoints blocked; YouTube main frame never blocked.
    auto d2 = shield.evaluate("https://www.youtube.com/pagead/viewthroughconversion",
                              "https://www.youtube.com/", ResourceKind::XhrOrFetch);
    CHECK(d2.block);
    auto d3 = shield.evaluate("https://www.youtube.com/watch?v=dQw4w9WgXcQ",
                              "https://www.youtube.com/", ResourceKind::MainFrame);
    CHECK(!d3.block);

    // 3. F8 fix: Segment/GTM blocked on normal sites, EXEMPTED on OTT/payment sites via @@ rules!
    auto d4 = shield.evaluate("https://cdn.segment.com/analytics.js/v1/abc/analytics.min.js",
                              "https://www.randomblog.com/", ResourceKind::Script);
    CHECK(d4.block);
    auto d5 = shield.evaluate("https://cdn.segment.com/analytics.js/v1/abc/analytics.min.js",
                              "https://www.jiohotstar.com/", ResourceKind::Script);
    CHECK(!d5.block);
    auto d6 = shield.evaluate("https://www.googletagmanager.com/gtm.js?id=GTM-123",
                              "https://www.zee5.com/", ResourceKind::Script);
    CHECK(!d6.block);

    // 4. ResourceKind::Ping matches $ping rule (facebook.com/tr$image,xmlhttprequest,ping).
    auto dp = shield.evaluate("https://www.facebook.com/tr?id=123",
                              "https://www.example.com/", ResourceKind::Ping);
    CHECK(dp.block);

    // 5. Instant On/Off toggle disables blocking immediately.
    shield.setEnabled(false);
    CHECK(!shield.isEnabled());
    auto d7 = shield.evaluate("https://pagead2.googlesyndication.com/pagead/js/adsbygoogle.js",
                              "https://www.example.com/", ResourceKind::Script);
    CHECK(!d7.block);

    shield.setEnabled(true);
    CHECK(shield.blockedCount() == 4);
    std::cout << "[test_shield] ALL PASSED (Brave adblock-rust rules=" << shield.ruleCount()
              << " blocked=" << shield.blockedCount() << ")\n";
    return 0;
}
