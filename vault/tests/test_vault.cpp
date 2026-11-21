#include "explorer/vault/password_vault.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    using namespace explorer::vault;
    namespace fs = std::filesystem;

    fs::path tmp = fs::temp_directory_path() / "explorer_v2_test_vault";
    fs::remove_all(tmp);

    {
        AesGcmPasswordVault v(tmp);
        assert(v.saveOrUpdate("https://github.com/login", "piyush", "S3cr3t!Pass"));
        assert(v.saveOrUpdate("https://example.com/auth", "alice", "alpha123"));

        // Update existing credential cleanly
        assert(v.saveOrUpdate("https://github.com/settings", "piyush", "NewP@ss456"));

        // Security: reject non-web schemes (javascript:, file://, data:) and userinfo (@) spoofing
        assert(!v.saveOrUpdate("javascript:alert(1)", "u", "p"));
        assert(!v.saveOrUpdate("file:///etc/passwd", "u", "p"));
        assert(!v.saveOrUpdate("https://evil.com@bank.com", "u", "p"));

        auto gh = v.findForOrigin("https://github.com/repo");
        assert(gh.size() == 1 && gh[0].username == "piyush" && gh[0].password == "NewP@ss456");
    }

    // Verify encrypted file on disk contains ZERO plaintext password
    {
        std::ifstream in(tmp / "vault.enc", std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(in)), {});
        assert(raw.find("NewP@ss456") == std::string::npos);
        assert(raw.find("alpha123") == std::string::npos);
    }

    // Reload from disk and verify decryption roundtrip + delete
    {
        AesGcmPasswordVault v2(tmp);
        auto all = v2.listAll();
        assert(all.size() == 2);
        auto gh = v2.findForOrigin("https://github.com");
        assert(gh.size() == 1 && gh[0].password == "NewP@ss456");
        assert(v2.remove("https://github.com", "piyush"));
        assert(v2.listAll().size() == 1);
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
        assert(v3.listAll().empty());
    }

    fs::remove_all(tmp);
    std::cout << "[test_vault] ALL PASSED (AES-256-GCM encrypt/update/delete/tamper-check)\n";
    return 0;
}
