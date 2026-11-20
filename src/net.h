// HTTPS through WinHTTP, and the bits of crypto and encoding sign-in needs.
#pragma once
#include "core.h"

namespace Net {
    struct Response {
        DWORD status = 0;          // 0: no answer at all (offline, DNS, TLS, timeout)
        std::string body;
        std::wstring Text() const { return Util::Wide(body); }
    };

    // headers: "Name: value\r\n" lines. Never follows a redirect: a token must not travel on.
    Response Request(const wchar_t* method, const std::wstring& url, const std::wstring& headers,
                     const std::string& body = "", DWORD timeoutMs = 20000, size_t limit = 4u << 20,
                     const std::wstring& agent = L"");

    std::string Random(size_t n);                          // cryptographically random bytes
    std::string Sha256(const std::string& data);           // raw 32 bytes
    std::wstring Hex(const std::string& bytes);
    std::string Base64Url(const std::string& bytes);       // no padding
    std::string Base64UrlDecode(const std::string& text);
    std::string UrlEncode(const std::string& s);            // RFC 3986 unreserved kept
    std::string UrlDecode(const std::string& s);
    std::string QueryParam(const std::string& query, const std::string& name);

    // Windows' own protection for the current user (DPAPI): only this Windows account on
    // this PC can read what it writes.
    std::string Protect(const std::string& plain);
    bool Unprotect(const std::string& sealed, std::string& plain);

    // For backups that travel to another PC: a key from a password (PBKDF2-HMAC-SHA256) and
    // AES-256-GCM. Seal returns ciphertext followed by the 16-byte tag; Open checks the tag.
    std::string Pbkdf2(const std::string& password, const std::string& salt, unsigned iterations, size_t length);
    std::string AesGcmSeal(const std::string& key, const std::string& nonce, const std::string& plain);
    bool AesGcmOpen(const std::string& key, const std::string& nonce, const std::string& sealed, std::string& plain);
}

// What a ChatGPT sign-in says about itself. Read from the JWTs without checking their
// signature: they are used to tell accounts apart and to know when to renew, never to
// grant anything.
struct Claims {
    std::wstring email, name, plan, accountId, userId, sub;
    long long exp = 0;               // unix seconds
    long long subscriptionUntil = 0; // unix ms
    bool ok = false;
};
Claims ParseJwt(const std::wstring& jwt);
