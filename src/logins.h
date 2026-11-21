// Sign-in details for accounts: the email, password and 2FA key a shop (or anyone) handed
// over, what has been done to make the account yours (a new password, a new 2FA key, the
// phone), and when its paid month and the seller's warranty end. Kept encrypted next to the
// accounts, in logins.dat, readable only by this Windows user.
#pragma once
#include "core.h"

// 2FA codes from a key, the way authenticator apps make them (RFC 6238: HMAC-SHA1, 30 s, 6 digits).
namespace Totp {
    // The key in what people paste: base32 in any case, with spaces or dashes; an otpauth://
    // link; or a link that ends in the key (2fa.live/tok/<key>). "" when there is none.
    std::wstring Clean(const std::wstring& text);
    std::string Decode(const std::wstring& base32);    // the raw secret; "" when it isn't base32
    std::wstring Code(const std::wstring& key, long long unixSec);   // "" for a bad key
    std::wstring Hotp(const std::string& secret, long long counter, int digits = 6);
    int SecondsLeft(long long unixSec);                // until the code changes: 1…30
    std::wstring Spaced(const std::wstring& code);     // "123 456"
}

struct Cred {
    std::wstring id;
    std::wstring email, password, key;                 // key: base32, as Totp::Clean gives it
    std::wstring newPassword, newKey;                  // made or pasted here, not yet confirmed as set on the site
    std::wstring oldPassword, oldKey;                  // what they replaced, in case the change didn't take
    std::wstring order, product, note;
    long long received = 0;                            // unix ms
    long long warrantyUntil = 0, paidUntil = 0;        // unix ms, from the product's "W24H" and "1M"; 0 = unknown
    bool passwordDone = false, keyDone = false, phoneDone = false;
    std::wstring accountId;                            // the switcher's account, once signed in
    bool warnedWarranty = false, warnedPaid = false;
};

namespace Creds {
    std::wstring File();
    std::vector<Cred> Load();
    void Save(const std::vector<Cred>& list);          // throws std::wstring
    json::Value ToJson(const Cred& c);
    Cred FromJson(const json::Value& v);

    // A shop's message ("Заказ: …", "Товар: ChatGPT plus | Apple Pay | 1M | W24H", lines of
    // email|password|2fa) or bare lines. Fields may also be split by tabs, ';' or ':'.
    struct Parsed {
        std::vector<Cred> creds;
        std::wstring order, product;
        int months = 0; long long warrantyMs = 0;
        int bad = 0;                                   // lines that looked like an account but weren't one
    };
    Parsed Parse(const std::wstring& text, long long nowMs);
    // New emails are added; one already there gets only what it lacked, so a change made
    // since (a new password, a new key) is never undone.
    struct Merged { int added = 0, updated = 0; std::vector<std::wstring> ids; };
    Merged Merge(std::vector<Cred>& list, const Parsed& p);
    int MergeJson(const json::Value& arr);             // from a backup; returns how many were new
    std::wstring NewPassword();                        // 20 characters: letters, digits and a few symbols

    enum class Stage { New, Securing, SignIn, Ready, Expired };
    Stage StageOf(const Cred& c, bool signedIn, long long paidUntil, long long nowMs);
    int StepsDone(const Cred& c, bool signedIn);       // of 4: password, 2FA, phone, signed in here
}

// A browser profile of its own for each account: signing in to one never touches the others
// or your everyday browser, and the site stays signed in for that account next time.
namespace Browser {
    std::wstring Profile(const std::wstring& credId);
    std::wstring Name();                               // "Microsoft Edge", "Google Chrome", … for the interface
    bool Open(const std::wstring& credId, const std::wstring& url);
    void Forget(const std::wstring& credId);           // removes its profile
}
