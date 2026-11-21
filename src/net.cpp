#include "net.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include "version.h"

namespace Net {
    namespace {
        struct Http {
            HINTERNET h;
            explicit Http(HINTERNET value) : h(value) {}
            ~Http() { if (h) WinHttpCloseHandle(h); }
        };
    }

    Response Request(const wchar_t* method, const std::wstring& url, const std::wstring& headers, const std::string& body, DWORD timeoutMs, size_t limit, const std::wstring& agent) {
        Response r;
        wchar_t host[256] = {}, path[8192] = {};
        URL_COMPONENTS u{ sizeof u };
        u.lpszHostName = host; u.dwHostNameLength = ARRAYSIZE(host);
        u.lpszUrlPath = path; u.dwUrlPathLength = ARRAYSIZE(path);
        // The path includes the query: WinHTTP leaves lpszExtraInfo empty unless asked for it.
        wchar_t extra[8192] = {}; u.lpszExtraInfo = extra; u.dwExtraInfoLength = ARRAYSIZE(extra);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u) || u.nScheme != INTERNET_SCHEME_HTTPS) return r;
        std::wstring object = std::wstring(path) + extra;
        Http session(WinHttpOpen(agent.empty() ? L"CodexAccountSwitcher/" CAS_VERSION_TEXT : agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session.h) return r;
        WinHttpSetTimeouts(session.h, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;   // Windows 10 before 1903 has no TLS 1.3 here
            WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        }
        Http connection(WinHttpConnect(session.h, host, u.nPort, 0));
        Http request(connection.h ? WinHttpOpenRequest(connection.h, method, object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr);
        if (!request.h) return r;
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never);
        DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(request.h, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);
        BOOL sent = WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : (DWORD)-1L,
                                       body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
        if (!sent || !WinHttpReceiveResponse(request.h, nullptr)) return r;
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        char buffer[16384]; DWORD got = 0;
        do {
            if (!WinHttpReadData(request.h, buffer, sizeof buffer, &got)) break;
            r.body.append(buffer, got);
            if (r.body.size() > limit) break;
        } while (got);
        r.status = status;
        return r;
    }

    std::string Random(size_t n) {
        std::string out(n, '\0');
        if (n && !BCRYPT_SUCCESS(BCryptGenRandom(nullptr, (PUCHAR)&out[0], (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            throw std::wstring(L"Windows' random number generator failed");
        return out;
    }

    std::string Sha256(const std::string& data) {
        std::string out(32, '\0');
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), (PUCHAR)&out[0], 32);
        return out;
    }

    std::string HmacSha1(const std::string& key, const std::string& data) {
        std::string out(20, '\0');
        if (BCryptHash(BCRYPT_HMAC_SHA1_ALG_HANDLE, (PUCHAR)key.data(), (ULONG)key.size(), (PUCHAR)data.data(), (ULONG)data.size(), (PUCHAR)&out[0], 20) < 0) return "";
        return out;
    }

    std::wstring Hex(const std::string& bytes) {
        std::wstring out;
        for (unsigned char b : bytes) out += Util::Format(L"%02x", b);
        return out;
    }

    static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string Base64Url(const std::string& in) {
        std::string out;
        size_t i = 0;
        for (; i + 2 < in.size(); i += 3) {
            unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
            out += B64[(v >> 18) & 63]; out += B64[(v >> 12) & 63]; out += B64[(v >> 6) & 63]; out += B64[v & 63];
        }
        if (i + 1 == in.size()) {
            unsigned v = (unsigned char)in[i] << 16;
            out += B64[(v >> 18) & 63]; out += B64[(v >> 12) & 63];
        } else if (i + 2 == in.size()) {
            unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
            out += B64[(v >> 18) & 63]; out += B64[(v >> 12) & 63]; out += B64[(v >> 6) & 63];
        }
        return out;
    }

    // Takes both alphabets and ignores padding, so plain base64 decodes too.
    std::string Base64UrlDecode(const std::string& in) {
        std::string out; unsigned v = 0; int bits = 0;
        for (char c : in) {
            int d;
            if (c >= 'A' && c <= 'Z') d = c - 'A';
            else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
            else if (c >= '0' && c <= '9') d = c - '0' + 52;
            else if (c == '-' || c == '+') d = 62;
            else if (c == '_' || c == '/') d = 63;
            else if (c == '=') break;
            else return "";
            v = (v << 6) | (unsigned)d; bits += 6;
            if (bits >= 8) { bits -= 8; out += (char)((v >> bits) & 0xFF); }
        }
        return out;
    }

    std::string UrlEncode(const std::string& s) {
        std::string out;
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
            else { char b[4]; sprintf_s(b, "%%%02X", c); out += b; }
        }
        return out;
    }

    std::string UrlDecode(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '+') out += ' ';
            else if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
                out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2;
            } else out += s[i];
        }
        return out;
    }

    std::string QueryParam(const std::string& query, const std::string& name) {
        size_t s = 0;
        while (s <= query.size()) {
            auto e = query.find('&', s);
            auto part = query.substr(s, e == std::string::npos ? std::string::npos : e - s);
            auto eq = part.find('=');
            if (UrlDecode(part.substr(0, eq)) == name) return eq == std::string::npos ? "" : UrlDecode(part.substr(eq + 1));
            if (e == std::string::npos) break;
            s = e + 1;
        }
        return "";
    }

    // The entropy ties the blob to this program: another app running as the same user can't
    // decrypt it by simply calling CryptUnprotectData on the file.
    static DATA_BLOB Entropy() {
        static const char salt[] = "CodexAccountSwitcher/vault/v1";
        return { (DWORD)(sizeof salt - 1), (BYTE*)salt };
    }

    std::string Protect(const std::string& plain) {
        if (Paths::Demo) return plain;   // the demo's stand-in accounts stay readable for tools/demo.ps1
        DATA_BLOB in{ (DWORD)plain.size(), (BYTE*)plain.data() }, out{}, ent = Entropy();
        if (!CryptProtectData(&in, L"Codex accounts", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
        std::string sealed((char*)out.pbData, out.cbData);
        LocalFree(out.pbData);
        return sealed;
    }

    bool Unprotect(const std::string& sealed, std::string& plain) {
        if (Paths::Demo) { plain = sealed; return true; }
        DATA_BLOB in{ (DWORD)sealed.size(), (BYTE*)sealed.data() }, out{}, ent = Entropy();
        if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
        plain.assign((char*)out.pbData, out.cbData);
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return true;
    }

    std::string Pbkdf2(const std::string& password, const std::string& salt, unsigned iterations, size_t length) {
        std::string key(length, '\0');
        BCRYPT_ALG_HANDLE alg = nullptr;
        bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) >= 0 &&
                  BCryptDeriveKeyPBKDF2(alg, (PUCHAR)password.data(), (ULONG)password.size(), (PUCHAR)salt.data(), (ULONG)salt.size(),
                                        iterations, (PUCHAR)&key[0], (ULONG)length, 0) >= 0;
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        return ok ? key : std::string();
    }

    // AES-256-GCM through BCrypt; encrypt and decrypt share everything but the call.
    static bool Gcm(bool seal, const std::string& key, const std::string& nonce, const std::string& in, std::string& out) {
        if (key.size() != 32 || nonce.size() != 12 || (!seal && in.size() < 16)) return false;
        BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE h = nullptr;
        bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0 &&
                  BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) >= 0 &&
                  BCryptGenerateSymmetricKey(alg, &h, nullptr, 0, (PUCHAR)key.data(), 32, 0) >= 0;
        if (ok) {
            size_t n = seal ? in.size() : in.size() - 16;
            std::string tag = seal ? std::string(16, '\0') : in.substr(n);
            BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info; BCRYPT_INIT_AUTH_MODE_INFO(info);
            info.pbNonce = (PUCHAR)nonce.data(); info.cbNonce = 12;
            info.pbTag = (PUCHAR)&tag[0]; info.cbTag = 16;
            std::string buf(n, '\0'); ULONG got = 0;
            auto src = (PUCHAR)in.data();
            auto dst = n ? (PUCHAR)&buf[0] : nullptr;
            ok = (seal ? BCryptEncrypt(h, src, (ULONG)n, &info, nullptr, 0, dst, (ULONG)n, &got, 0)
                       : BCryptDecrypt(h, src, (ULONG)n, &info, nullptr, 0, dst, (ULONG)n, &got, 0)) >= 0;
            if (ok) out = seal ? buf + tag : buf;
            SecureZeroMemory(&buf[0], buf.size());
        }
        if (h) BCryptDestroyKey(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        return ok;
    }

    std::string AesGcmSeal(const std::string& key, const std::string& nonce, const std::string& plain) {
        std::string out; return Gcm(true, key, nonce, plain, out) ? out : std::string();
    }
    bool AesGcmOpen(const std::string& key, const std::string& nonce, const std::string& sealed, std::string& plain) { return Gcm(false, key, nonce, sealed, plain); }
}

Claims ParseJwt(const std::wstring& jwt) {
    Claims c;
    auto a = jwt.find(L'.');
    auto b = a == std::wstring::npos ? a : jwt.find(L'.', a + 1);
    if (b == std::wstring::npos) return c;
    auto payload = Net::Base64UrlDecode(Util::Utf8(jwt.substr(a + 1, b - a - 1)));
    json::Value v;
    if (payload.empty() || !json::Parser::Parse(Util::Wide(payload), v) || v.type != json::Value::Object) return c;
    c.ok = true;
    c.email = v.Str(L"email");
    c.name = v.Str(L"name");
    c.sub = v.Str(L"sub");
    c.exp = (long long)v.Num(L"exp", 0);
    // The ChatGPT claims sit under one namespaced key; access tokens carry the email in a second one.
    if (auto auth = v.Find(L"https://api.openai.com/auth")) {
        c.plan = auth->Str(L"chatgpt_plan_type");
        c.accountId = auth->Str(L"chatgpt_account_id");
        c.userId = auth->Str(L"chatgpt_user_id", auth->Str(L"user_id"));
        c.subscriptionUntil = Core::ParseIso(auth->Str(L"chatgpt_subscription_active_until"));
    }
    if (c.email.empty()) if (auto profile = v.Find(L"https://api.openai.com/profile")) c.email = profile->Str(L"email");
    return c;
}
