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

class IPasswordVault {
public:
    virtual ~IPasswordVault() = default;
    virtual bool saveOrUpdate(const std::string& origin,
                              const std::string& username,
                              const std::string& password) = 0;
    virtual bool remove(const std::string& origin, const std::string& username) = 0;
    virtual std::vector<Credential> findForOrigin(const std::string& origin) const = 0;
    virtual std::vector<Credential> listAll() const = 0;
};

class AesGcmPasswordVault final : public IPasswordVault {
public:
    explicit AesGcmPasswordVault(std::filesystem::path storage_dir);

    bool saveOrUpdate(const std::string& origin,
                      const std::string& username,
                      const std::string& password) override;
    bool remove(const std::string& origin, const std::string& username) override;
    std::vector<Credential> findForOrigin(const std::string& origin) const override;
    std::vector<Credential> listAll() const override;

    static std::string normalizeOrigin(const std::string& raw_url);

private:
    bool ensureKey();
    bool loadFromDisk();
    bool persistToDisk() const;

    std::filesystem::path key_path_;
    std::filesystem::path enc_path_;
    std::vector<unsigned char> key_;  // 32 bytes (AES-256)
    std::vector<Credential> entries_;
};

}  // namespace explorer::vault
