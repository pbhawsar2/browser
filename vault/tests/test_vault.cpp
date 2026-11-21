#include "explorer/vault/password_vault.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::cerr << "[test_vault] FAIL at line " << __LINE__ << ": " #cond << "\n"; \
            std::exit(1);                                                               \
        }                                                                               \
    } while (0)

int main() {
    using namespace explorer::vault;
    namespace fs = std::filesystem;

    fs::path tmp = fs::temp_directory_path() / "explorer_v2_test_vault";
    fs::remove_all(tmp);

    // 0. Strict origin normalization & rejection of non-web schemes / invalid ports / userinfo
    CHECK(AesGcmPasswordVault::normalizeOrigin("https://GitHub.com/login") == "https://github.com");
    CHECK(AesGcmPasswordVault::normalizeOrigin("https://github.com:443/settings") == "https://github.com");
    CHECK(AesGcmPasswordVault::normalizeOrigin("http://localhost:80/app") == "http://localhost");
    CHECK(AesGcmPasswordVault::normalizeOrigin("http://localhost:3000/api") == "http://localhost:3000");
    CHECK(AesGcmPasswordVault::normalizeOrigin("localhost:3000") == "https://localhost:3000");
    CHECK(AesGcmPasswordVault::normalizeOrigin("javascript:alert(1)").empty());
    CHECK(AesGcmPasswordVault::normalizeOrigin("javascript:123").empty());
    CHECK(AesGcmPasswordVault::normalizeOrigin("data:text/html,<h1>hi</h1>").empty());
    CHECK(AesGcmPasswordVault::normalizeOrigin("mailto:user@example.com").empty());
    CHECK(AesGcmPasswordVault::normalizeOrigin("about:blank").empty());
    CHECK(AesGcmPasswordVault::normalizeOrigin("file:///etc/passwd").empty());
    CHECK(AesGcmPasswordVault::normalizeOrigin("chrome://version").empty());
    CHECK(AesGcmPasswordVault::normalizeOrigin("https://evil.com@bank.com").empty());
    CHECK(AesGcmPasswordVault::normalizeOrigin("https://example.com:99999").empty());
    CHECK(AesGcmPasswordVault::normalizeOrigin("https://example.com:abc").empty());

    {
        AesGcmPasswordVault v(tmp);
        CHECK(v.saveOrUpdate("https://github.com/login", "piyush", "S3cr3t!Pass"));
        CHECK(v.saveOrUpdate("https://example.com/auth", "alice", "alpha123"));

        // Update existing credential cleanly
        CHECK(v.saveOrUpdate("https://github.com/settings", "piyush", "NewP@ss456"));

        // Security: reject non-web schemes (javascript:, file://, data:) and userinfo (@) spoofing
        CHECK(!v.saveOrUpdate("javascript:alert(1)", "u", "p"));
        CHECK(!v.saveOrUpdate("data:text/html,hi", "u", "p"));
        CHECK(!v.saveOrUpdate("file:///etc/passwd", "u", "p"));
        CHECK(!v.saveOrUpdate("https://evil.com@bank.com", "u", "p"));

        auto gh = v.findForOrigin("https://github.com/repo");
        CHECK(gh.size() == 1 && gh[0].username == "piyush" && gh[0].password == "NewP@ss456");
    }

    // Verify encrypted file on disk contains ZERO plaintext password
    {
        std::ifstream in(tmp / "vault.enc", std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(in)), {});
        CHECK(raw.find("NewP@ss456") == std::string::npos);
        CHECK(raw.find("alpha123") == std::string::npos);
    }

    // Reload from disk and verify decryption roundtrip + delete
    {
        AesGcmPasswordVault v2(tmp);
        auto all = v2.listAll();
        CHECK(all.size() == 2);
        auto gh = v2.findForOrigin("https://github.com");
        CHECK(gh.size() == 1 && gh[0].password == "NewP@ss456");
        CHECK(v2.remove("https://github.com", "piyush"));
        CHECK(v2.listAll().size() == 1);
    }

    // Tamper detection: corrupting 1 byte in ciphertext must fail GCM authentication
    {
        std::fstream f(tmp / "vault.enc", std::ios::binary | std::ios::in | std::ios::out);
        f.seekg(0, std::ios::end);
        auto end_pos = f.tellg();
        f.seekp(end_pos - std::streamoff(1));
        char b = 0;
        f.read(&b, 1);
        b ^= 0xFF;
        f.seekp(end_pos - std::streamoff(1));
        f.write(&b, 1);
    }
    {
        AesGcmPasswordVault v3(tmp);
        CHECK(v3.listAll().empty());
    }

    // Corrupt/truncated key file must fail closed and NEVER overwrite existing vault.key
    {
        std::ofstream bad_key(tmp / "vault.key", std::ios::binary | std::ios::trunc);
        bad_key << "short";
    }
    {
        AesGcmPasswordVault v4(tmp);
        CHECK(fs::file_size(tmp / "vault.key") == 5);
        CHECK(!v4.saveOrUpdate("https://github.com", "u", "p"));
    }

    fs::remove_all(tmp);
    std::cout << "[test_vault] ALL PASSED (AES-256-GCM encrypt/update/delete/tamper-check/origin-guard)\n";
    return 0;
}
