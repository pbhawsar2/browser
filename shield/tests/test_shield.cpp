#include "explorer/shield/content_shield.hpp"

#include <cassert>
#include <iostream>

int main() {
    using namespace explorer::shield;

    BraveAdblockShield shield({}, true);
    assert(shield.isEnabled());
    assert(shield.ruleCount() >= 45);

    // 1. Third-party ad script blocked by Brave adblock-rust.
    auto d1 = shield.evaluate("https://pagead2.googlesyndication.com/pagead/js/adsbygoogle.js",
                              "https://www.example.com/", ResourceKind::Script);
    assert(d1.block);

    // 2. YouTube ad endpoints blocked; YouTube main frame never blocked.
    auto d2 = shield.evaluate("https://www.youtube.com/pagead/viewthroughconversion",
                              "https://www.youtube.com/", ResourceKind::XhrOrFetch);
    assert(d2.block);
    auto d3 = shield.evaluate("https://www.youtube.com/watch?v=dQw4w9WgXcQ",
                              "https://www.youtube.com/", ResourceKind::MainFrame);
    assert(!d3.block);

    // 3. F8 fix: Segment/GTM blocked on normal sites, EXEMPTED on OTT/payment sites via @@ rules!
    auto d4 = shield.evaluate("https://cdn.segment.com/analytics.js/v1/abc/analytics.min.js",
                              "https://www.randomblog.com/", ResourceKind::Script);
    assert(d4.block);
    auto d5 = shield.evaluate("https://cdn.segment.com/analytics.js/v1/abc/analytics.min.js",
                              "https://www.jiohotstar.com/", ResourceKind::Script);
    assert(!d5.block);
    auto d6 = shield.evaluate("https://www.googletagmanager.com/gtm.js?id=GTM-123",
                              "https://www.zee5.com/", ResourceKind::Script);
    assert(!d6.block);

    // 4. Instant On/Off toggle disables blocking immediately.
    shield.setEnabled(false);
    assert(!shield.isEnabled());
    auto d7 = shield.evaluate("https://pagead2.googlesyndication.com/pagead/js/adsbygoogle.js",
                              "https://www.example.com/", ResourceKind::Script);
    assert(!d7.block);

    shield.setEnabled(true);
    assert(shield.blockedCount() == 3);
    std::cout << "[test_shield] ALL PASSED (Brave adblock-rust rules=" << shield.ruleCount()
              << " blocked=" << shield.blockedCount() << ")\n";
    return 0;
}
