#pragma once
// SOLID Network-Layer Content Shield powered by Brave's `adblock-rust` engine.
// Supports instant lock-free On/Off toggle, EasyList/uBlock syntax, OTT auth exceptions,
// and hot-reloading updated filter lists without restarting the browser.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace explorer::shield {

enum class ResourceKind {
    MainFrame,
    SubFrame,
    Script,
    Image,
    Stylesheet,
    Font,
    Media,
    Object,
    XhrOrFetch,
    Ping,
    CspReport,
    Other,
};

struct BlockDecision {
    bool block = false;
    const char* reason = "";
};

class IContentShield {
public:
    virtual ~IContentShield() = default;
    virtual BlockDecision evaluate(std::string_view request_url,
                                   std::string_view initiator_origin,
                                   ResourceKind kind) const = 0;
    virtual void setEnabled(bool enabled) = 0;
    virtual bool isEnabled() const = 0;
    virtual uint64_t blockedCount() const = 0;
    virtual size_t ruleCount() const = 0;
    virtual void updateFilterListsAsync(std::function<void(bool, size_t)> on_done) = 0;
};

/// Production shield backed by Brave's `adblock-rust` engine (`libexplorer_shield_brave.a`).
class BraveAdblockShield final : public IContentShield {
public:
    explicit BraveAdblockShield(std::filesystem::path filters_dir = {}, bool enabled = true);
    ~BraveAdblockShield() override;

    BraveAdblockShield(const BraveAdblockShield&) = delete;
    BraveAdblockShield& operator=(const BraveAdblockShield&) = delete;

    BlockDecision evaluate(std::string_view request_url,
                           std::string_view initiator_origin,
                           ResourceKind kind) const override;

    void setEnabled(bool enabled) override { enabled_.store(enabled, std::memory_order_relaxed); }
    bool isEnabled() const override { return enabled_.load(std::memory_order_relaxed); }
    uint64_t blockedCount() const override { return blocked_count_.load(std::memory_order_relaxed); }
    size_t ruleCount() const override;

    void updateFilterListsAsync(std::function<void(bool, size_t)> on_done) override;

private:
    std::string loadCombinedRules() const;

    void* handle_ = nullptr;
    std::filesystem::path filters_dir_;
    std::atomic<bool> enabled_{true};
    std::atomic<bool> updating_{false};
    std::atomic<bool> shutting_down_{false};
    std::mutex update_mu_;
    std::thread update_thread_;
    mutable std::atomic<uint64_t> blocked_count_{0};
};

}  // namespace explorer::shield
