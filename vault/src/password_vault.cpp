#include "explorer/vault/password_vault.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>

namespace explorer::vault {

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[4] = {'E', 'X', 'V', '1'};
constexpr size_t kKeyLen = 32;
constexpr size_t kIvLen = 12;
constexpr size_t kTagLen = 16;

std::string serializeEntries(const std::vector<Credential>& entries) {
    // Length-prefixed binary-safe framing: count\n followed by len:str tuples.
    std::ostringstream out;
    out << entries.size() << "\n";
    for (const auto& c : entries) {
        out << c.origin.size() << ":" << c.origin << "\n";
        out << c.username.size() << ":" << c.username << "\n";
        out << c.password.size() << ":" << c.password << "\n";
    }
    return out.str();
}

bool readLenPrefixed(const std::string& buf, size_t& pos, std::string& out) {
    auto colon = buf.find(':', pos);
    if (colon == std::string::npos) return false;
    size_t len = 0;
    try {
        len = std::stoull(buf.substr(pos, colon - pos));
    } catch (...) {
        return false;
    }
    pos = colon + 1;
    if (pos + len > buf.size()) return false;
    out = buf.substr(pos, len);
    pos += len;
    if (pos < buf.size() && buf[pos] == '\n') ++pos;
    return true;
}

bool deserializeEntries(const std::string& plain, std::vector<Credential>& out) {
    out.clear();
    auto nl = plain.find('\n');
    if (nl == std::string::npos) return false;
    size_t count = 0;
    try {
        count = std::stoull(plain.substr(0, nl));
    } catch (...) {
        return false;
    }
    if (count > 10000) return false;
    size_t pos = nl + 1;
    for (size_t i = 0; i < count; ++i) {
        Credential c;
        if (!readLenPrefixed(plain, pos, c.origin) ||
            !readLenPrefixed(plain, pos, c.username) ||
            !readLenPrefixed(plain, pos, c.password)) {
            return false;
        }
        out.push_back(std::move(c));
    }
    return true;
}

}  // namespace

std::string AesGcmPasswordVault::normalizeOrigin(const std::string& raw_url) {
    std::string s = raw_url;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.empty()) return "";

    auto scheme_pos = s.find("://");
    std::string scheme = "https";
    size_t host_start = 0;
    if (scheme_pos != std::string::npos) {
        scheme = s.substr(0, scheme_pos);
        host_start = scheme_pos + 3;
    }
    auto path_pos = s.find_first_of("/?#", host_start);
    std::string authority = (path_pos == std::string::npos)
                                ? s.substr(host_start)
                                : s.substr(host_start, path_pos - host_start);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), ::tolower);
    std::transform(authority.begin(), authority.end(), authority.begin(), ::tolower);
    if (authority.empty()) return "";
    return scheme + "://" + authority;
}

AesGcmPasswordVault::AesGcmPasswordVault(fs::path storage_dir)
    : key_path_(storage_dir / "vault.key"),
      enc_path_(storage_dir / "vault.enc") {
    std::error_code ec;
    fs::create_directories(storage_dir, ec);
    if (ensureKey()) {
        loadFromDisk();
    }
}

bool AesGcmPasswordVault::ensureKey() {
    std::error_code ec;
    if (fs::exists(key_path_, ec)) {
        std::ifstream in(key_path_, std::ios::binary);
        key_.resize(kKeyLen);
        if (in.read(reinterpret_cast<char*>(key_.data()), kKeyLen) &&
            static_cast<size_t>(in.gcount()) == kKeyLen) {
            return true;
        }
    }

    key_.resize(kKeyLen);
    if (RAND_bytes(key_.data(), static_cast<int>(kKeyLen)) != 1) {
        key_.clear();
        return false;
    }

    fs::path tmp = key_path_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write(reinterpret_cast<const char*>(key_.data()), kKeyLen);
        out.flush();
    }
    ::chmod(tmp.c_str(), 0600);
    fs::rename(tmp, key_path_, ec);
    ::chmod(key_path_.c_str(), 0600);
    return !ec;
}

bool AesGcmPasswordVault::loadFromDisk() {
    std::error_code ec;
    if (!fs::exists(enc_path_, ec) || key_.size() != kKeyLen) return false;

    std::ifstream in(enc_path_, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    auto sz = in.tellg();
    if (sz < static_cast<std::streamoff>(sizeof(kMagic) + kIvLen + kTagLen)) return false;
    in.seekg(0, std::ios::beg);
    std::vector<unsigned char> blob(static_cast<size_t>(sz));
    if (!in.read(reinterpret_cast<char*>(blob.data()), sz)) return false;

    if (std::memcmp(blob.data(), kMagic, sizeof(kMagic)) != 0) return false;

    const unsigned char* iv = blob.data() + sizeof(kMagic);
    const unsigned char* tag = iv + kIvLen;
    const unsigned char* ct = tag + kTagLen;
    size_t ct_len = blob.size() - sizeof(kMagic) - kIvLen - kTagLen;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    std::string plain(ct_len, '\0');
    int out_len1 = 0, out_len2 = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kIvLen), nullptr) == 1 &&
              EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_.data(), iv) == 1 &&
              EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(plain.data()), &out_len1,
                                ct, static_cast<int>(ct_len)) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagLen),
                                  const_cast<unsigned char*>(tag)) == 1 &&
              EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plain.data()) + out_len1,
                                  &out_len2) == 1;
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        entries_.clear();
        return false;
    }
    plain.resize(static_cast<size_t>(out_len1 + out_len2));
    return deserializeEntries(plain, entries_);
}

bool AesGcmPasswordVault::persistToDisk() const {
    if (key_.size() != kKeyLen) return false;

    std::string plain = serializeEntries(entries_);
    unsigned char iv[kIvLen];
    if (RAND_bytes(iv, static_cast<int>(kIvLen)) != 1) return false;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    std::vector<unsigned char> ct(plain.size() + 16);
    unsigned char tag[kTagLen];
    int out_len1 = 0, out_len2 = 0;

    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kIvLen), nullptr) == 1 &&
              EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_.data(), iv) == 1 &&
              EVP_EncryptUpdate(ctx, ct.data(), &out_len1,
                                reinterpret_cast<const unsigned char*>(plain.data()),
                                static_cast<int>(plain.size())) == 1 &&
              EVP_EncryptFinal_ex(ctx, ct.data() + out_len1, &out_len2) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagLen), tag) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;

    size_t ct_len = static_cast<size_t>(out_len1 + out_len2);
    fs::path tmp = enc_path_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write(kMagic, sizeof(kMagic));
        out.write(reinterpret_cast<const char*>(iv), kIvLen);
        out.write(reinterpret_cast<const char*>(tag), kTagLen);
        out.write(reinterpret_cast<const char*>(ct.data()), static_cast<std::streamsize>(ct_len));
        out.flush();
    }
    ::chmod(tmp.c_str(), 0600);
    std::error_code ec;
    fs::rename(tmp, enc_path_, ec);
    ::chmod(enc_path_.c_str(), 0600);
    return !ec;
}

bool AesGcmPasswordVault::saveOrUpdate(const std::string& origin,
                                       const std::string& username,
                                       const std::string& password) {
    std::string norm = normalizeOrigin(origin);
    if (norm.empty() || username.empty() || password.empty()) return false;

    for (auto& e : entries_) {
        if (e.origin == norm && e.username == username) {
            e.password = password;
            return persistToDisk();
        }
    }
    entries_.push_back(Credential{norm, username, password});
    return persistToDisk();
}

bool AesGcmPasswordVault::remove(const std::string& origin, const std::string& username) {
    std::string norm = normalizeOrigin(origin);
    auto before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const Credential& c) {
                                      return c.origin == norm && c.username == username;
                                  }),
                   entries_.end());
    if (entries_.size() == before) return false;
    return persistToDisk();
}

std::vector<Credential> AesGcmPasswordVault::findForOrigin(const std::string& origin) const {
    std::string norm = normalizeOrigin(origin);
    std::vector<Credential> res;
    for (const auto& e : entries_) {
        if (e.origin == norm) res.push_back(e);
    }
    return res;
}

std::vector<Credential> AesGcmPasswordVault::listAll() const {
    return entries_;
}

}  // namespace explorer::vault
