// Accounts: their saved sign-ins, which one Codex uses now, switching, and what OpenAI says
// about their limits.
//
// Codex keeps one sign-in, in <codex home>\auth.json, read by the app, the CLI and the editor
// extensions alike. Each account here keeps its own copy of that file in an encrypted vault;
// switching writes the chosen copy back. ChatGPT renews a sign-in by replacing its refresh
// token, and the old one stops working, so the copy in auth.json is always taken back into
// the vault before anything else happens: switching away and back never restores a stale one.
#pragma once
#include "core.h"

struct Usage {
    bool known = false;                    // the server answered at least once
    double five = -1, week = -1;           // percent used; -1 = no such window
    long long fiveReset = 0, weekReset = 0;// unix seconds
    bool limited = false;                  // the server says a limit is reached
    std::wstring plan;                     // "plus", "pro", "team", …
    bool credits = false, unlimited = false;
    std::wstring balance;
    int resetCredits = 0;                  // manual limit resets available
    long long resetExpires = 0;            // unix seconds: when the first of them expires
    long long at = 0;                      // unix ms of the answer
    std::wstring problem;                  // the last attempt failed: why (shown on the card)
    bool signedOut = false;                // OpenAI rejected the sign-in: sign in again
    bool Stale() const;                    // older than half an hour, or past a reset
    double FiveNow() const;                // the figure, or 0 once its window has reset since
    double WeekNow() const;
    // What is left, as Codex's own /status shows it: 100 minus what is used; -1 = no such window.
    double FiveLeft() const { double f = FiveNow(); return f < 0 ? -1 : f >= 100 ? 0 : 100 - f; }
    double WeekLeft() const { double w = WeekNow(); return w < 0 ? -1 : w >= 100 ? 0 : 100 - w; }
};

struct Account {
    std::wstring id;                       // permanent: shortcuts, colours and hotkeys follow it
    std::wstring name, email, plan;
    std::wstring workspace, user;          // ChatGPT account (workspace) and user ids: who it is
    json::Value auth;                      // its auth.json, the way Codex writes it
    long long added = 0, lastUsed = 0, warmedAt = 0;   // unix ms
    long long subscriptionUntil = 0;       // unix ms: the paid plan runs until then, 0 = unknown
    // Codex processes that went on running with this sign-in after a switch away from it (a
    // terminal session, or the app when it wasn't restarted): pid and start time. They renew
    // it themselves, so the switcher doesn't while any of them lives.
    std::vector<std::pair<DWORD, ULONGLONG>> heldBy;
    bool ApiKey() const;
    std::wstring Identity() const;
};

// An account's Codex statistics (GET /backend-api/wham/profiles/me), its limit resets
// (/wham/rate-limit-reset-credits) and its subscription (/accounts/check). -1 = not given.
struct Stats {
    bool known = false;
    std::wstring problem;
    long long lifetime = -1, peakDaily = -1, longestTask = -1, streak = -1, longestStreak = -1;
    long long threads = -1, skills = -1, skillsTotal = -1;
    double fastPercent = -1, effortPercent = -1;
    std::wstring effort;                   // the reasoning effort used most
    std::vector<std::pair<std::wstring, long long>> daily;   // "2026-11-19" and its tokens, oldest first
    struct Top { std::wstring kind, name; long long count = 0; };
    std::vector<Top> top;                  // skills and plugins used most
    long long asOf = 0;                    // unix ms
    struct Reset { std::wstring title, status; long long expires = 0; };
    int resets = 0; long long resetNext = 0; std::vector<Reset> resetList;
    std::wstring plan; long long subscriptionUntil = 0;
    long long at = 0;                      // unix ms of the answer
    long long Since(int days) const;       // tokens in the last `days` days, today included; -1 = unknown
};
Stats ParseStats(const json::Value& profile, long long nowMs);
void ParseResets(const json::Value& v, long long nowMs, int& count, long long& next, std::vector<Stats::Reset>* list);

namespace Accounts {
    // One change at a time, across every copy of the switcher (a shortcut runs a second one).
    // Throws a std::wstring when another copy holds it for too long, rather than going on unlocked.
    struct Lock { Lock(); ~Lock(); HANDLE mutex = nullptr; Lock(const Lock&) = delete; Lock& operator=(const Lock&) = delete; };

    std::vector<Account> Load();           // empty when there is no vault yet
    void Save(const std::vector<Account>& list);
    bool Exists();
    std::wstring Problem();                // the vault was there but couldn't be read

    // Who a sign-in belongs to: "<workspace>|<user>", or "" when it holds none.
    std::wstring IdentityOf(const json::Value& auth);
    Account Describe(const json::Value& auth);             // email, plan and ids from the tokens
    std::wstring DefaultName(const Account& a, const std::vector<Account>& others);
    std::wstring ValidateName(const std::wstring& name, const std::vector<Account>& list, const std::wstring& exceptId);
    bool NeedsRenewal(const json::Value& auth, long long nowSec);

    // auth.json
    json::Value ReadLive();
    bool WriteLive(const json::Value& auth);
    std::wstring ActiveId(const std::vector<Account>& list, const json::Value& live);
    // Takes the tokens Codex renewed in auth.json into the matching account. True if anything changed.
    bool SyncLive(std::vector<Account>& list, const json::Value& live);

    // Everything below locks by itself and may block: call it off the UI thread.
    struct Added { std::wstring id, error; bool existed = false; };
    // A known account just gets the new sign-in. fresh: it comes from signing in just now, so
    // if Codex uses that account, auth.json gets it too (the old one may have been signed out).
    Added Add(const json::Value& auth, const std::wstring& name = L"", bool fresh = false);
    std::wstring Rename(const std::wstring& id, const std::wstring& name); // "" or why not
    void Remove(const std::wstring& id);
    void Restore(const Account& a, int at);   // an account just removed, back where it was with its id
    void Move(const std::wstring& id, int to);

    struct SwitchOptions { bool restartApp = true, openApp = false; };
    struct SwitchResult {
        std::wstring error;
        bool already = false;              // it was in use already
        bool closedApp = false, startedApp = false, appKeepsOld = false;
        bool appFailed = false;            // the app was to be opened (or opened again) and didn't start
        int terminals = 0;                 // Codex sessions in terminals, which keep the old sign-in
        std::wstring keptUnknown;          // Codex was signed in to an account not on the list: it was added, not lost
    };
    SwitchResult Switch(const std::wstring& id, const SwitchOptions& o);

    Usage FetchUsage(const std::wstring& id);
    Stats FetchStats(const std::wstring& id);
    bool Warmup(const std::wstring& id, std::wstring& error);

    // The usage last seen, so cards have figures the moment the window opens.
    std::map<std::wstring, Usage> LoadUsage();
    void SaveUsage(const std::map<std::wstring, Usage>& all);

    // Importing
    json::Value ReadAuthFile(const std::wstring& path, std::wstring& error);
    struct Legacy { std::wstring name; json::Value auth; long long lastUsed = 0; };
    std::wstring LegacyFile();             // Codex Switcher's %USERPROFILE%\.codex-switcher\accounts.json
    std::vector<Legacy> LegacyAccounts(std::wstring& error);
    json::Value LegacyAuth(const json::Value& entry);       // one entry of that file as auth.json
    struct Imported { int added = 0, updated = 0, skipped = 0, logins = 0; std::wstring error; };
    Imported ImportLegacy();               // all of them; names that are emails become the usual short ones

    // Backups of every account, to move them to another PC: encrypted with a password you
    // choose (AES-256-GCM, the key from PBKDF2-SHA256), as a file or as a line of text.
    std::string ExportBackup(const std::wstring& password);          // the file's bytes; throws std::wstring
    std::string BackupText(const std::string& file);                  // "cas1.…", for the clipboard
    enum class BackupKind { None, Ours, CodexSwitcherFile, CodexSwitcherText };
    BackupKind KindOf(const std::string& data);                       // file bytes or pasted text
    // Ours needs the password; Codex Switcher's own exports (.cswf files, "css1." text) don't.
    Imported ImportBackup(const std::string& data, const std::wstring& password);
}

// Usage as the server sends it (GET /backend-api/wham/usage). Kept apart for the tests.
Usage ParseUsage(const json::Value& v, long long nowMs);


// A browser sign-in with ChatGPT, the way the Codex CLI does it: a one-off server on
// localhost:1455 (or 1457) receives the answer, PKCE keeps the code useless to anyone else.
namespace Login {
    struct Result { json::Value auth; std::wstring error; bool cancelled = false; };
    // Opens the port and builds the link; the result arrives on a worker thread.
    bool Begin(std::wstring& url, std::wstring& error, std::function<void(Result)> done);
    void Cancel();
    bool Active();
    std::string AuthorizeUrl(const std::string& redirect, const std::string& challenge, const std::string& state);
}

namespace OpenAI {
    extern const char* ClientId;
    extern const wchar_t* Issuer;
    // Renews a sign-in with its refresh token. revoked: OpenAI refused it for good.
    bool Renew(json::Value& auth, std::wstring& error, bool& revoked);
    json::Value AuthFromTokens(const std::wstring& id, const std::wstring& access, const std::wstring& refresh);
}
