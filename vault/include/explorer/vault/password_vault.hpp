#pragma once
// SOLID Encrypted Password Vault Interface & OpenSSL AES-256-GCM Implementation.
// Zero plaintext credentials ever written to disk; per-user 256-bit key stored with 0600 permissions.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace explorer::vault {

struct Credential {
    std::string origin;    // e.g. "https://github.com"
    std::string username;
    std::string password;
};

/// Metadata-only credential projection (zero plaintext password copies in memory).
struct CredentialMeta {
    std::string origin;
    std::string username;
};

class IPasswordVault {
public:
    virtual ~IPasswordVault() = default;
    virtual bool saveOrUpdate(const std::string& origin,
                              const std::string& username,
                              const std::string& password) = 0;
    virtual bool remove(const std::string& origin, const std::string& username) = 0;
    virtual std::vector<Credential> findForOrigin(const std::string& origin) const = 0;
    virtual size_t countForOrigin(const std::string& origin) const = 0;
    virtual std::vector<CredentialMeta> listMetadata() const = 0;
    virtual std::vector<Credential> listAll() const = 0;
};

class AesGcmPasswordVault final : public IPasswordVault {
public:
    explicit AesGcmPasswordVault(std::filesystem::path storage_dir);
    ~AesGcmPasswordVault() override;

    bool saveOrUpdate(const std::string& origin,
                      const std::string& username,
                      const std::string& password) override;
    bool remove(const std::string& origin, const std::string& username) override;
    std::vector<Credential> findForOrigin(const std::string& origin) const override;
    size_t countForOrigin(const std::string& origin) const override;
    std::vector<CredentialMeta> listMetadata() const override;
    std::vector<Credential> listAll() const override;

    static std::string normalizeOrigin(const std::string& raw_url);

private:
    bool ensureKey();
    bool decryptEntriesFromDisk(std::vector<Credential>& out) const;
    bool persistEntriesToDisk(const std::vector<Credential>& entries);
    static void cleanseEntries(std::vector<Credential>& entries);

    std::filesystem::path key_path_;
    std::filesystem::path enc_path_;
    std::vector<unsigned char> key_;  // 32 bytes (AES-256)
    // Deep-sleep memory footprint: only lightweight (origin, username) metadata is kept resident;
    // plaintext passwords are decrypted on-demand and immediately scrubbed with OPENSSL_cleanse!
    std::vector<CredentialMeta> meta_;
};

}  // namespace explorer::vault
