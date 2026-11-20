#include "accounts.h"
#include "codex.h"
#include "net.h"
#include "compat.h"
#include <algorithm>
#include <cmath>
#include <objbase.h>

// ================================================================ usage and accounts

bool Usage::Stale() const {
    long long now = Core::NowMs();
    if (!at || now - at > 30ll * 60 * 1000) return true;
    return false;
}

double Usage::FiveNow() const { return five >= 0 && fiveReset && Core::NowMs() / 1000 >= fiveReset ? 0 : five; }
double Usage::WeekNow() const { return week >= 0 && weekReset && Core::NowMs() / 1000 >= weekReset ? 0 : week; }

bool Account::ApiKey() const { return !auth.Find(L"tokens") && !auth.Str(L"OPENAI_API_KEY").empty(); }
std::wstring Account::Identity() const { return Accounts::IdentityOf(auth); }

Usage ParseUsage(const json::Value& v, long long nowMs) {
    Usage u;
    u.known = true; u.at = nowMs;
    u.plan = v.Str(L"plan_type");
    long long nowSec = nowMs / 1000;
    if (auto rl = v.Find(L"rate_limit")) {
        u.limited = rl->Flag(L"limit_reached", false);
        // The 5-hour window is usually primary and the weekly one secondary, but a plan with
        // only a weekly limit sends that one as primary: the window's length decides.
        const json::Value* windows[] = { rl->Find(L"primary_window"), rl->Find(L"secondary_window") };
        for (int i = 0; i < 2; i++) {
            auto w = windows[i];
            if (!w || w->type != json::Value::Object) continue;
            double pct = w->Num(L"used_percent", -1);
            long long len = (long long)w->Num(L"limit_window_seconds", 0);
            long long reset = (long long)w->Num(L"reset_at", 0);
            if (!reset && w->Find(L"reset_after_seconds")) reset = nowSec + (long long)w->Num(L"reset_after_seconds", 0);
            bool weekly = len ? len >= 24 * 3600 : i == 1;
            if (weekly) { u.week = pct; u.weekReset = reset; }
            else { u.five = pct; u.fiveReset = reset; }
        }
    }
    if (auto c = v.Find(L"credits")) {
        u.credits = c->Flag(L"has_credits", false);
        u.unlimited = c->Flag(L"unlimited", false);
        u.balance = c->Str(L"balance");
    }
    if (auto r = v.Find(L"rate_limit_reset_credits")) u.resetCredits = (int)r->Num(L"available_count", 0);
    return u;
}

void ParseResets(const json::Value& v, long long nowMs, int& count, long long& next, std::vector<Stats::Reset>* list) {
    count = (int)v.Num(L"available_count", count);
    next = 0;
    if (auto credits = v.Find(L"credits"))
        for (auto& c : credits->arr) {
            Stats::Reset r{ c.Str(L"title", c.Str(L"reset_type")), c.Str(L"status"), Core::ParseIso(c.Str(L"expires_at")) / 1000 };
            if (r.status == L"available" && r.expires > nowMs / 1000 && (!next || r.expires < next)) next = r.expires;
            if (list) list->push_back(r);
        }
}

Stats ParseStats(const json::Value& v, long long nowMs) {
    Stats s;
    s.at = nowMs;
    auto st = v.Find(L"stats");
    if (!st || st->type != json::Value::Object) {
        auto meta = v.Find(L"metadata");
        s.problem = meta && !meta->Str(L"stats_error").empty() ? meta->Str(L"stats_error") : std::wstring(Tr(L"No statistics for this account yet", L"Статистики по этому аккаунту пока нет"));
        return s;
    }
    s.known = true;
    auto num = [&](const wchar_t* k) { auto x = st->Find(k); return x && x->type == json::Value::Number ? (long long)x->n : -1ll; };
    auto real = [&](const wchar_t* k) { auto x = st->Find(k); return x && x->type == json::Value::Number ? x->n : -1.0; };
    s.lifetime = num(L"lifetime_tokens"); s.peakDaily = num(L"peak_daily_tokens"); s.longestTask = num(L"longest_running_turn_sec");
    s.streak = num(L"current_streak_days"); s.longestStreak = num(L"longest_streak_days");
    s.threads = num(L"total_threads"); s.skills = num(L"unique_skills_used"); s.skillsTotal = num(L"total_skills_used");
    s.fastPercent = real(L"fast_mode_usage_percentage"); s.effortPercent = real(L"most_used_reasoning_effort_percentage");
    s.effort = st->Str(L"most_used_reasoning_effort");
    if (auto d = st->Find(L"daily_usage_buckets"))
        for (auto& b : d->arr) {
            auto day = b.Str(L"start_date");
            if (day.size() >= 10 && day[4] == L'-' && day[7] == L'-') s.daily.push_back({ day.substr(0, 10), (long long)b.Num(L"tokens", 0) });
        }
    std::sort(s.daily.begin(), s.daily.end());
    if (auto t = st->Find(L"top_invocations"))
        for (auto& i : t->arr) s.top.push_back({ i.Str(L"type", L"unknown"), i.Str(L"skill_name", i.Str(L"plugin_name", L"?")), (long long)i.Num(L"usage_count", 0) });
    if (auto meta = v.Find(L"metadata")) s.asOf = Core::ParseIso(meta->Str(L"stats_as_of", meta->Str(L"generated_at")));
    return s;
}

long long Stats::Since(int days) const {
    if (daily.empty()) return -1;
    SYSTEMTIME t; GetLocalTime(&t);
    FILETIME f; SystemTimeToFileTime(&t, &f);
    ULONGLONG now = ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
    ULONGLONG start = now - (ULONGLONG)(days - 1) * 864000000000ull;
    FILETIME sf{ (DWORD)start, (DWORD)(start >> 32) }; SYSTEMTIME s; FileTimeToSystemTime(&sf, &s);
    auto from = Util::Format(L"%04d-%02d-%02d", s.wYear, s.wMonth, s.wDay);
    long long sum = 0;
    for (auto& d : daily) if (d.first >= from) sum += d.second;
    return sum;
}

namespace OpenAI {
    const char* ClientId = "app_EMoamEEZ73f0CkXaXp7hrann";
    const wchar_t* Issuer = L"https://auth.openai.com";

    json::Value AuthFromTokens(const std::wstring& id, const std::wstring& access, const std::wstring& refresh) {
        auto auth = json::Value::MakeObject();
        auto& t = auth[L"tokens"];
        t = json::Value::MakeObject();
        t[L"id_token"] = id;
        t[L"access_token"] = access;
        t[L"refresh_token"] = refresh;
        auto claims = ParseJwt(id);
        if (!claims.accountId.empty()) t[L"account_id"] = claims.accountId;
        auth[L"last_refresh"] = Core::IsoNow();
        return auth;
    }

    bool Renew(json::Value& auth, std::wstring& error, bool& revoked) {
        revoked = false;
        auto t = auth.Find(L"tokens");
        auto refresh = t ? t->Str(L"refresh_token") : L"";
        if (refresh.empty()) { error = Tr(L"This sign-in can't be renewed. Sign in again", L"Этот вход нельзя продлить. Войдите заново"); revoked = true; return false; }
        if (Paths::Demo) { auth[L"last_refresh"] = Core::IsoNow(); return true; }
        // The same request the Codex CLI makes.
        auto body = "{\"client_id\":\"" + std::string(ClientId) + "\",\"grant_type\":\"refresh_token\",\"refresh_token\":\"" + Util::Utf8(refresh) + "\",\"scope\":\"openid profile email\"}";
        auto r = Net::Request(L"POST", std::wstring(Issuer) + L"/oauth/token", L"Content-Type: application/json\r\nAccept: application/json\r\n", body, 25000);
        if (r.status == 0) { error = Tr(L"Couldn't reach OpenAI. Check your connection", L"Не удалось связаться с OpenAI. Проверьте соединение"); return false; }
        json::Value v; json::Parser::Parse(r.Text(), v);
        if (r.status != 200) {
            std::wstring code = v.Str(L"error");
            if (auto e = v.Find(L"error")) if (e->type == json::Value::Object) code = e->Str(L"code", e->Str(L"type"));
            // 400 and 401 with a refresh-token error are final: expired, used elsewhere or signed out.
            revoked = (r.status == 400 || r.status == 401) && (code.empty() || code.find(L"refresh_token") != std::wstring::npos || code == L"invalid_grant");
            error = revoked ? std::wstring(Tr(L"OpenAI ended this sign-in. Sign in again", L"OpenAI завершил этот вход. Войдите заново"))
                            : Util::Format(Tr(L"OpenAI couldn't renew the sign-in (%lu). Try again later", L"OpenAI не продлил вход (%lu). Повторите позже"), r.status);
            Core::Log(Util::Format(L"Renewing a sign-in: HTTP %lu %s", r.status, code.c_str()));
            return false;
        }
        auto access = v.Str(L"access_token");
        if (access.empty()) { error = Tr(L"OpenAI sent an unexpected answer", L"OpenAI прислал неожиданный ответ"); return false; }
        auto& tokens = auth[L"tokens"];
        tokens[L"access_token"] = access;
        auto id = v.Str(L"id_token");
        if (!id.empty()) {
            tokens[L"id_token"] = id;
            auto claims = ParseJwt(id);
            if (!claims.accountId.empty()) tokens[L"account_id"] = claims.accountId;
        }
        auto next = v.Str(L"refresh_token");
        if (!next.empty()) tokens[L"refresh_token"] = next;
        auth[L"last_refresh"] = Core::IsoNow();
        return true;
    }

    // What the ChatGPT backend expects from Codex.
    static std::wstring Headers(const json::Value& auth) {
        auto t = auth.Find(L"tokens");
        auto access = t ? t->Str(L"access_token") : L"";
        auto workspace = t ? t->Str(L"account_id") : L"";
        if (workspace.empty() && t) workspace = ParseJwt(t->Str(L"id_token")).accountId;
        std::wstring h = L"Authorization: Bearer " + access + L"\r\noriginator: codex_cli_rs\r\nAccept: application/json\r\n";
        if (!workspace.empty()) h += L"ChatGPT-Account-Id: " + workspace + L"\r\n";
        return h;
    }

    static std::wstring Agent() {
        static std::wstring agent = [] {
            typedef LONG(WINAPI * Fn)(OSVERSIONINFOW*);
            OSVERSIONINFOW o{ sizeof o };
            if (auto f = (Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) f(&o);
#if defined(_M_ARM64)
            const wchar_t* arch = L"aarch64";
#else
            const wchar_t* arch = L"x86_64";
#endif
            return Util::Format(L"codex_cli_rs/0.157.1 (Windows %lu.%lu.%lu; %s) CodexAccountSwitcher", o.dwMajorVersion, o.dwMinorVersion, o.dwBuildNumber, arch);
        }();
        return agent;
    }

    static Net::Response Get(const json::Value& auth, const std::wstring& url) {
        return Net::Request(L"GET", url, Headers(auth), "", 20000, 2u << 20, Agent());
    }

    // The smallest model Codex lists, for warm-ups: names with luna, mini or nano first.
    static std::vector<std::wstring> WarmupModels() {
        std::vector<std::wstring> out;
        auto saved = Settings::Str(L"WarmupModel");
        if (!saved.empty()) out.push_back(saved);
        if (std::find(out.begin(), out.end(), L"gpt-6-luna") == out.end()) out.push_back(L"gpt-6-luna");
        json::Value cache;
        if (json::Parser::Parse(Util::ReadText(Codex::Home() + L"\\models_cache.json"), cache))
            if (auto models = cache.Find(L"models")) {
                std::vector<std::pair<int, std::wstring>> ranked;
                for (auto& m : models->arr) {
                    auto slug = m.Str(L"slug");
                    if (slug.empty() || slug.find(L'/') != std::wstring::npos || m.Str(L"visibility", L"list") != L"list") continue;
                    auto l = Util::Lower(slug);
                    int rank = l.find(L"nano") != std::wstring::npos ? 0 : l.find(L"luna") != std::wstring::npos ? 1 : l.find(L"mini") != std::wstring::npos ? 2 : 9;
                    ranked.push_back({ rank * 1000 + (int)m.Num(L"priority", 500), slug });
                }
                std::sort(ranked.begin(), ranked.end());
                for (auto& r : ranked) if (std::find(out.begin(), out.end(), r.second) == out.end()) out.push_back(r.second);
            }
        return out;
    }

    // One tiny request, so the account's 5-hour window starts now.
    static Net::Response Warm(const json::Value& auth, const std::wstring& model) {
        auto body = "{\"model\":\"" + Util::Utf8(model) + "\",\"instructions\":\"You are Codex.\",\"input\":[{\"type\":\"message\",\"role\":\"user\","
                    "\"content\":[{\"type\":\"input_text\",\"text\":\"Hi\"}]}],\"tools\":[],\"tool_choice\":\"auto\",\"parallel_tool_calls\":false,"
                    "\"reasoning\":{\"effort\":\"low\"},\"store\":false,\"stream\":true}";
        auto headers = Headers(auth);
        headers.replace(headers.find(L"Accept: application/json"), 24, L"Accept: text/event-stream");
        headers += L"Content-Type: application/json\r\n";
        return Net::Request(L"POST", L"https://chatgpt.com/backend-api/codex/responses", headers, body, 60000, 2u << 20, Agent());
    }
}

// ================================================================ the vault

namespace Accounts {
    namespace {
        CRITICAL_SECTION* Guard() { static CRITICAL_SECTION cs; static bool init = (InitializeCriticalSection(&cs), true); (void)init; return &cs; }
        std::vector<Account> cache;
        ULONGLONG cacheTime = ~0ull;
        std::wstring problem;

        json::Value ToJson(const Account& a) {
            auto o = json::Value::MakeObject();
            o[L"id"] = a.id; o[L"name"] = a.name; o[L"email"] = a.email; o[L"plan"] = a.plan;
            o[L"workspace"] = a.workspace; o[L"user"] = a.user;
            o[L"added"] = (double)a.added; o[L"lastUsed"] = (double)a.lastUsed; o[L"warmedAt"] = (double)a.warmedAt;
            if (a.subscriptionUntil) o[L"subscriptionUntil"] = (double)a.subscriptionUntil;
            if (!a.heldBy.empty()) {
                auto& held = o[L"heldBy"]; held = json::Value::MakeArray();
                // Start times need all 64 bits, more than a JSON number keeps exactly.
                for (auto& h : a.heldBy) held.arr.push_back(json::Value(Util::Format(L"%lu:%llu", h.first, h.second)));
            }
            o[L"auth"] = a.auth;
            return o;
        }

        Account FromJson(const json::Value& o) {
            Account a;
            a.id = o.Str(L"id"); a.name = o.Str(L"name"); a.email = o.Str(L"email"); a.plan = o.Str(L"plan");
            a.workspace = o.Str(L"workspace"); a.user = o.Str(L"user");
            a.added = (long long)o.Num(L"added", 0); a.lastUsed = (long long)o.Num(L"lastUsed", 0); a.warmedAt = (long long)o.Num(L"warmedAt", 0);
            a.subscriptionUntil = (long long)o.Num(L"subscriptionUntil", 0);
            if (auto auth = o.Find(L"auth")) a.auth = *auth;
            if (auto held = o.Find(L"heldBy"))
                for (auto& h : held->arr) {
                    DWORD pid = 0; unsigned long long at = 0;
                    if (h.type == json::Value::String && swscanf_s(h.s.c_str(), L"%lu:%llu", &pid, &at) == 2) a.heldBy.push_back({ pid, at });
                }
            return a;
        }

        // Prunes processes that have ended; true while one of them still runs.
        bool Held(Account& a, bool& changed) {
            size_t before = a.heldBy.size();
            a.heldBy.erase(std::remove_if(a.heldBy.begin(), a.heldBy.end(), [](const std::pair<DWORD, ULONGLONG>& h) {
                return Util::CreatedAt(h.first) != h.second;
            }), a.heldBy.end());
            changed |= a.heldBy.size() != before;
            return !a.heldBy.empty();
        }

        // May the switcher renew this account's sign-in? Not while a Codex uses it.
        bool Owned(Account& a, bool active, bool& changed) {
            if (Held(a, changed)) return false;
            return !active || !Codex::Scan().anyRunning();
        }

        bool blocked = false;   // an unreadable vault is still in place: never write over it

        std::wstring NewId(const std::vector<Account>& list) {
            for (;;) {
                auto id = Net::Hex(Net::Random(4));
                bool taken = false;
                for (auto& a : list) taken |= _wcsicmp(a.id.c_str(), id.c_str()) == 0;
                if (!taken) return id;
            }
        }

        std::wstring Tokens(const json::Value& auth, const wchar_t* key) { auto t = auth.Find(L"tokens"); return t ? t->Str(key) : L""; }
        long long AccessExp(const json::Value& auth) { return ParseJwt(Tokens(auth, L"access_token")).exp; }
    }

    Lock::Lock() {
        EnterCriticalSection(Guard());
        mutex = CreateMutexW(nullptr, FALSE, Paths::Demo ? L"Local\\CodexAccountSwitcherDemo.Vault" : L"Local\\CodexAccountSwitcher.Vault");
        if (mutex) {
            DWORD r = WaitForSingleObject(mutex, 60000);
            if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) {
                CloseHandle(mutex); mutex = nullptr;
                LeaveCriticalSection(Guard());
                throw std::wstring(Tr(L"Another copy of the switcher is busy with the accounts. Try again in a moment", L"Другая копия переключателя занята аккаунтами. Повторите через минуту"));
            }
        }
    }
    Lock::~Lock() {
        if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
        LeaveCriticalSection(Guard());
    }

    bool Exists() { return Util::Exists(Paths::VaultFile); }
    std::wstring Problem() { EnterCriticalSection(Guard()); auto p = problem; LeaveCriticalSection(Guard()); return p; }

    std::vector<Account> Load() {
        EnterCriticalSection(Guard());
        auto stamp = Util::FileTime(Paths::VaultFile);
        if (stamp != cacheTime) {
            cache.clear(); cacheTime = stamp;
            if (stamp) {
                std::string plain;
                json::Value v;
                if (!Net::Unprotect(Util::ReadBytes(Paths::VaultFile), plain) || !json::Parser::Parse(Util::Wide(plain), v)) {
                    // Another Windows user's or another PC's vault, or a damaged one. It is moved
                    // aside, never overwritten, so nothing is lost by starting over.
                    SYSTEMTIME t; GetLocalTime(&t);
                    auto aside = Paths::VaultFile + Util::Format(L".unreadable-%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
                    if (MoveFileExW(Paths::VaultFile.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING))
                        problem = Util::Format(Tr(L"The saved accounts couldn't be read on this Windows account and were moved to %s", L"Сохранённые аккаунты не читаются под этой учётной записью Windows и перенесены в %s"), aside.c_str());
                    else {
                        blocked = true;
                        problem = Util::Format(Tr(L"The saved accounts in %s can't be read. Nothing is written over them until that file is moved", L"Сохранённые аккаунты в %s не читаются. Поверх них ничего не запишется, пока этот файл на месте"), Paths::VaultFile.c_str());
                    }
                    Core::Log(problem);
                    cacheTime = 0;
                } else {
                    blocked = false;
                    if (auto list = v.Find(L"accounts"))
                        for (auto& o : list->arr) { auto a = FromJson(o); if (!a.id.empty()) cache.push_back(a); }
                }
                SecureZeroMemory(&plain[0], plain.size());
            }
        }
        auto copy = cache;
        LeaveCriticalSection(Guard());
        return copy;
    }

    void Save(const std::vector<Account>& list) {
        EnterCriticalSection(Guard());
        if (blocked && Util::Exists(Paths::VaultFile)) {
            LeaveCriticalSection(Guard());
            throw std::wstring(Tr(L"The saved accounts can't be read, so nothing was written over them", L"Сохранённые аккаунты не читаются, поэтому поверх них ничего не записано"));
        }
        auto v = json::Value::MakeObject();
        v[L"version"] = 1;
        auto& arr = v[L"accounts"]; arr = json::Value::MakeArray();
        for (auto& a : list) arr.arr.push_back(ToJson(a));
        std::wstring text; json::Write(text, v);
        auto sealed = Net::Protect(Util::Utf8(text));
        SecureZeroMemory(&text[0], text.size() * sizeof(wchar_t));
        CreateDirectoryW(Paths::Root.c_str(), nullptr);
        bool ok = !sealed.empty() && Util::WriteBytes(Paths::VaultFile, sealed);
        if (ok) { cache = list; cacheTime = Util::FileTime(Paths::VaultFile); }
        else { cacheTime = ~0ull; Core::Log(L"Couldn't save the accounts to " + Paths::VaultFile); }
        LeaveCriticalSection(Guard());
        if (!ok) throw std::wstring(Tr(L"Couldn't save the accounts", L"Не удалось сохранить аккаунты"));
        // Another running copy (the tray) picks the change up at once.
        if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, Core::ReloadEvent)) { SetEvent(e); CloseHandle(e); }
    }

    // ------------------------------------------------------------ identity

    std::wstring IdentityOf(const json::Value& auth) {
        if (auth.type != json::Value::Object) return L"";
        if (auto t = auth.Find(L"tokens")) {
            if (t->type != json::Value::Object) return L"";
            auto id = ParseJwt(t->Str(L"id_token"));
            auto access = ParseJwt(t->Str(L"access_token"));
            auto workspace = !id.accountId.empty() ? id.accountId : !t->Str(L"account_id").empty() ? t->Str(L"account_id") : access.accountId;
            auto user = !id.userId.empty() ? id.userId : !access.userId.empty() ? access.userId : !id.sub.empty() ? id.sub : Util::Lower(id.email);
            if (workspace.empty() && user.empty()) return L"";
            return Util::Lower(workspace) + L"|" + user;
        }
        auto key = auth.Str(L"OPENAI_API_KEY");
        if (!key.empty()) return L"key|" + Net::Hex(Net::Sha256(Util::Utf8(key))).substr(0, 16);
        return L"";
    }

    Account Describe(const json::Value& auth) {
        Account a;
        a.auth = auth;
        if (auto t = auth.Find(L"tokens")) {
            auto id = ParseJwt(t->Str(L"id_token"));
            auto access = ParseJwt(t->Str(L"access_token"));
            a.email = !id.email.empty() ? id.email : access.email;
            a.plan = !id.plan.empty() ? id.plan : access.plan;
            a.workspace = !id.accountId.empty() ? id.accountId : t->Str(L"account_id");
            a.user = !id.userId.empty() ? id.userId : access.userId;
            a.subscriptionUntil = id.subscriptionUntil;
        } else if (!auth.Str(L"OPENAI_API_KEY").empty()) a.plan = L"api";
        return a;
    }

    std::wstring ValidateName(const std::wstring& name, const std::vector<Account>& list, const std::wstring& exceptId) {
        if (Util::Trim(name).empty()) return Tr(L"The name can't be empty", L"Имя не может быть пустым");
        // Names become shortcut file names, "Codex - <name>.lnk".
        if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return Tr(L"A name can't contain \\ / : * ? \" < > |", L"В имени не может быть \\ / : * ? \" < > |");
        for (wchar_t c : name) if (c < 0x20) return Tr(L"The name contains control characters", L"В имени есть служебные символы");
        if (name[0] == L'.' || name.back() == L'.' || name.back() == L' ') return Tr(L"A name can't start with a dot or end with a dot or a space", L"Имя не может начинаться с точки или заканчиваться точкой или пробелом");
        if (name.size() > 40) return Tr(L"The name is too long, 40 characters at most", L"Имя слишком длинное, максимум 40 символов");
        if (_wcsicmp(name.c_str(), L"Add account") == 0) return Tr(L"The switcher's own shortcut already uses this name", L"Это имя занято ярлыком самого переключателя");
        for (auto& a : list) if (a.id != exceptId && _wcsicmp(a.name.c_str(), name.c_str()) == 0) return Util::Format(Tr(L"There is already an account called %s", L"Аккаунт %s уже есть"), Util::Q(name).c_str());
        return L"";
    }

    // The part of the email before @, which is how most people tell their accounts apart. The
    // same email in a second workspace gets the plan added: "john (Team)".
    std::wstring DefaultName(const Account& a, const std::vector<Account>& others) {
        std::wstring base = a.email.substr(0, a.email.find(L'@'));
        for (auto& c : base) if (wcschr(L"\\/:*?\"<>|", c) || c < 0x20) c = L'_';
        base = Util::Trim(base);
        while (!base.empty() && (base.back() == L'.' || base.back() == L' ')) base.pop_back();
        while (!base.empty() && base[0] == L'.') base.erase(0, 1);
        if (base.size() > 32) base.resize(32);
        if (base.empty()) base = a.ApiKey() ? L"API key" : L"Account";
        for (auto& o : others)
            if (!a.email.empty() && _wcsicmp(o.email.c_str(), a.email.c_str()) == 0 && !a.plan.empty()) {
                auto plan = a.plan; plan[0] = towupper(plan[0]);
                base += L" (" + plan + L")";
                break;
            }
        auto name = base;
        for (int n = 2; !ValidateName(name, others, L"").empty() && n < 100; n++) name = Util::Format(L"%s %d", base.c_str(), n);
        return name;
    }

    bool NeedsRenewal(const json::Value& auth, long long nowSec) {
        auto exp = AccessExp(auth);
        return exp && exp - nowSec < 120;
    }

    // ------------------------------------------------------------ auth.json

    json::Value ReadLive() {
        json::Value v;
        auto text = Util::ReadText(Codex::AuthFile());
        if (text.empty() || !json::Parser::Parse(text, v) || v.type != json::Value::Object) return json::Value();
        return v;
    }

    bool WriteLive(const json::Value& auth) {
        CreateDirectoryW(Codex::Home().c_str(), nullptr);
        std::wstring text; json::Write(text, auth);
        auto bytes = Util::Utf8(text);
        bool ok = Util::WriteBytes(Codex::AuthFile(), bytes);
        SecureZeroMemory(&bytes[0], bytes.size());
        SecureZeroMemory(&text[0], text.size() * sizeof(wchar_t));
        return ok;
    }

    std::wstring ActiveId(const std::vector<Account>& list, const json::Value& live) {
        auto who = IdentityOf(live);
        if (who.empty()) return L"";
        for (auto& a : list) if (a.Identity() == who) return a.id;
        return L"";
    }

    bool SyncLive(std::vector<Account>& list, const json::Value& live) {
        auto who = IdentityOf(live);
        if (who.empty()) return false;
        for (auto& a : list) {
            if (a.Identity() != who) continue;
            // Codex's copy is the reference: the same tokens with another last_refresh or a field
            // Codex added count as a change too, so switching back restores exactly what it wrote.
            std::wstring mineText, liveText;
            json::Write(mineText, a.auth); json::Write(liveText, live);
            bool same = mineText == liveText;
            SecureZeroMemory(&mineText[0], mineText.size() * sizeof(wchar_t));
            SecureZeroMemory(&liveText[0], liveText.size() * sizeof(wchar_t));
            if (same) return false;
            // An old copy written back (by a Codex that still ran with it) must not replace a newer one.
            auto mine = AccessExp(a.auth), theirs = AccessExp(live);
            if (mine && theirs && theirs < mine) return false;
            auto d = Describe(live);
            a.auth = live;
            if (!d.email.empty()) a.email = d.email;
            if (!d.plan.empty()) a.plan = d.plan;
            if (!d.workspace.empty()) a.workspace = d.workspace;
            if (!d.user.empty()) a.user = d.user;
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------ changes

    Added Add(const json::Value& auth, const std::wstring& name, bool fresh) {
        Added r;
        auto who = IdentityOf(auth);
        if (who.empty()) { r.error = Tr(L"This file has no Codex sign-in in it", L"В этом файле нет входа Codex"); return r; }
        Lock lock;
        auto list = Load();
        auto d = Describe(auth);
        for (auto& a : list) {
            if (a.Identity() != who) continue;
            r.id = a.id; r.existed = true;
            // The same person and workspace: the new sign-in replaces the saved one, unless it
            // is an older copy (an old auth.json, or Codex Switcher's file after renewals here).
            auto mine = AccessExp(a.auth), theirs = AccessExp(auth);
            if (mine && theirs && theirs < mine) return r;
            a.auth = auth;
            if (!d.email.empty()) a.email = d.email;
            if (!d.plan.empty()) a.plan = d.plan;
            try { Save(list); } catch (const std::wstring& e) { r.error = e; }
            if (fresh && IdentityOf(ReadLive()) == who) WriteLive(auth);
            return r;
        }
        d.id = NewId(list);
        d.name = name.empty() || !ValidateName(name, list, L"").empty() ? DefaultName(d, list) : name;
        d.added = Core::NowMs();
        list.push_back(d);
        r.id = d.id;
        try { Save(list); } catch (const std::wstring& e) { r.error = e; }
        return r;
    }

    std::wstring Rename(const std::wstring& id, const std::wstring& raw) {
        auto name = Util::Trim(raw);
        Lock lock;
        auto list = Load();
        auto err = ValidateName(name, list, id);
        if (!err.empty()) return err;
        for (auto& a : list) if (a.id == id) {
            a.name = name;
            try { Save(list); } catch (const std::wstring& e) { return e; }
            return L"";
        }
        return Tr(L"This account is gone", L"Этого аккаунта уже нет");
    }

    void Remove(const std::wstring& id) {
        Lock lock;
        auto list = Load();
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Account& a) { return a.id == id; }), list.end());
        Save(list);
    }

    void Move(const std::wstring& id, int to) {
        Lock lock;
        auto list = Load();
        auto it = std::find_if(list.begin(), list.end(), [&](const Account& a) { return a.id == id; });
        if (it == list.end()) return;
        Account a = *it; list.erase(it);
        to = std::max(0, std::min(to, (int)list.size()));
        list.insert(list.begin() + to, a);
        Save(list);
    }

    // ------------------------------------------------------------ switching

    SwitchResult Switch(const std::wstring& id, const SwitchOptions& o) {
        SwitchResult r;
        if (!Paths::Demo && Codex::KeyringStore()) {
            r.error = Tr(L"Codex keeps its sign-in in Windows Credential Manager (cli_auth_credentials_store = \"keyring\" in config.toml). Set it to \"file\" so accounts can be switched",
                         L"Codex хранит вход в диспетчере учётных данных Windows (cli_auth_credentials_store = \"keyring\" в config.toml). Поставьте \"file\", чтобы аккаунты переключались");
            return r;
        }
        Lock lock;
        auto list = Load();
        auto live = ReadLive();
        bool dirty = SyncLive(list, live);
        auto find = [&]() -> Account* { for (auto& a : list) if (a.id == id) return &a; return nullptr; };
        if (!find()) { r.error = Tr(L"This account is gone", L"Этого аккаунта уже нет"); return r; }
        // A sign-in Codex has that isn't on the list (a codex login of its own) would be lost by
        // writing over it, so it is added first.
        auto who = IdentityOf(live);
        if (!who.empty() && ActiveId(list, live).empty()) {
            auto d = Describe(live);
            d.id = NewId(list); d.name = DefaultName(d, list); d.added = d.lastUsed = Core::NowMs();
            list.push_back(d);
            r.keptUnknown = !d.email.empty() ? d.email : d.name;
            dirty = true;
        }
        auto previous = ActiveId(list, live);

        if (ActiveId(list, live) == id) {
            r.already = true;
            if (dirty) Save(list);
            if (o.openApp && !Codex::ShowApp()) { r.startedApp = Codex::StartApp(); r.appFailed = !r.startedApp; }
            return r;
        }

        // Checked before anything is closed: an account OpenAI signed out must not cost the
        // running Codex its session for nothing. The account isn't in use, so its tokens are ours.
        bool held = Held(*find(), dirty);
        if (!held && NeedsRenewal(find()->auth, Core::NowMs() / 1000)) {
            std::wstring err; bool revoked = false;
            if (OpenAI::Renew(find()->auth, err, revoked)) dirty = true;
            else if (revoked) {
                if (dirty) Save(list);
                r.error = Util::Format(Tr(L"%s needs to sign in again: ⋯ → Sign in again", L"%s нужно войти заново: ⋯ → Войти заново"), Util::Q(find()->name).c_str());
                return r;
            }
            // Offline: Codex renews it itself once it can.
        }
        if (dirty) Save(list);

        auto procs = Codex::Scan();
        r.terminals = (int)procs.cli.size();
        bool reopen = false;
        if (o.restartApp) {
            if (procs.appRunning()) {
                // Open again even when it only waited in the tray: closing it is ours to undo.
                reopen = true;
                r.closedApp = Codex::CloseApp(procs);
                if (!r.closedApp) { r.error = Util::Format(Tr(L"Couldn't close %s. Close it yourself and try again", L"Не удалось закрыть %s. Закройте его сами и повторите"), Codex::AppName().c_str()); return r; }
                // Codex may have renewed and saved its sign-in on the way out.
                if (SyncLive(list, ReadLive())) Save(list);
            }
            // Editors restart their Codex server by themselves and pick up the new sign-in.
            Codex::EndHelpers(procs);
        } else r.appKeepsOld = procs.appRunning();

        // Read once more right before writing: a Codex (an editor's server, a terminal) may have
        // renewed the sign-in a moment ago, and that copy must not be written over unsaved.
        SyncLive(list, ReadLive());
        auto target = find();
        target->lastUsed = Core::NowMs();
        if (!WriteLive(target->auth)) {
            r.error = Util::Format(Tr(L"Couldn't write %s", L"Не удалось записать %s"), Codex::AuthFile().c_str());
            if (reopen) Codex::StartApp();
            return r;
        }
        // Whatever Codex goes on running keeps the previous account and renews it by itself.
        for (auto& a : list) if (a.id == previous && previous != id) {
            auto keep = [&](DWORD pid) { if (auto at = Util::CreatedAt(pid)) a.heldBy.push_back({ pid, at }); };
            for (DWORD pid : procs.cli) keep(pid);
            if (!o.restartApp) { for (DWORD pid : procs.app) keep(pid); for (DWORD pid : procs.helpers) keep(pid); }
            if (procs.daemon && (!o.restartApp || !procs.cli.empty())) keep(procs.daemon);
        }
        try { Save(list); } catch (const std::wstring&) {}
        if (reopen || o.openApp) {
            // Still open with the old account (restarting is off): bring it forward instead.
            if (r.appKeepsOld && Codex::ShowApp()) return r;
            r.startedApp = Codex::StartApp();
            r.appFailed = !r.startedApp;
        }
        return r;
    }

    // ------------------------------------------------------------ limits

    // A GET to the ChatGPT backend with an account's own sign-in. Renewed first when it is ours
    // to renew and about to expire, and once more if the server answers 401 and nobody else
    // renewed it meanwhile. status 0 with `problem` set when it couldn't be asked at all.
    static Net::Response Authed(const std::wstring& id, const std::wstring& url, std::wstring& problem, bool& signedOut) {
        Net::Response none;
        json::Value auth;
        bool owned = true, active = false;
        {
            Lock lock;
            auto list = Load();
            auto live = ReadLive();
            bool dirty = SyncLive(list, live);
            Account* acc = nullptr;
            for (auto& a : list) if (a.id == id) acc = &a;
            if (!acc) { if (dirty) Save(list); problem = Tr(L"This account is gone", L"Этого аккаунта уже нет"); return none; }
            if (acc->ApiKey()) { if (dirty) Save(list); problem = Tr(L"API keys have no plan limits", L"У API-ключей нет лимитов подписки"); return none; }
            active = ActiveId(list, live) == id;
            // Codex renews the sign-in it uses by itself; renewing it here too would make one
            // of the two copies stale. Only when no Codex runs with it is it ours to renew.
            owned = Owned(*acc, active, dirty);
            if (owned && NeedsRenewal(acc->auth, Core::NowMs() / 1000)) {
                std::wstring err; bool revoked = false;
                if (OpenAI::Renew(acc->auth, err, revoked)) { dirty = true; if (active) WriteLive(acc->auth); }
                else { if (dirty) Save(list); problem = err; signedOut = revoked; return none; }
            }
            if (dirty) Save(list);
            auth = acc->auth;
        }
        auto r = OpenAI::Get(auth, url);
        if (r.status == 401) {
            Lock lock;
            auto list = Load();
            auto live = ReadLive();
            bool dirty = SyncLive(list, live);
            Account* acc = nullptr;
            for (auto& a : list) if (a.id == id) acc = &a;
            if (acc && Tokens(acc->auth, L"access_token") == Tokens(auth, L"access_token")) {
                if (!owned) { if (dirty) Save(list); problem = Tr(L"Codex is renewing this sign-in. Use Codex, then refresh", L"Codex сейчас продлевает этот вход. Поработайте в Codex и обновите"); return none; }
                std::wstring err; bool revoked = false;
                if (!OpenAI::Renew(acc->auth, err, revoked)) { if (dirty) Save(list); problem = err; signedOut = revoked; return none; }
                dirty = true;
                if (active) WriteLive(acc->auth);
            }
            if (dirty) Save(list);
            if (acc) r = OpenAI::Get(acc->auth, url);
        }
        if (r.status == 401) { problem = Tr(L"OpenAI didn't accept this sign-in. Sign in again", L"OpenAI не принял этот вход. Войдите заново"); signedOut = true; }
        else if (r.status == 403) problem = Tr(L"OpenAI refused the request for now. Try again later", L"OpenAI пока отклоняет запрос. Повторите позже");
        else if (r.status == 0) problem = Tr(L"No connection to OpenAI", L"Нет связи с OpenAI");
        else if (r.status != 200) problem = Util::Format(Tr(L"OpenAI answered with an error (%lu)", L"OpenAI ответил ошибкой (%lu)"), r.status);
        return r;
    }

    static const wchar_t* UsageUrl = L"https://chatgpt.com/backend-api/wham/usage";
    static const wchar_t* ResetsUrl = L"https://chatgpt.com/backend-api/wham/rate-limit-reset-credits";
    static const wchar_t* ProfileUrl = L"https://chatgpt.com/backend-api/wham/profiles/me";
    static const wchar_t* CheckUrl = L"https://chatgpt.com/backend-api/accounts/check/v4-2023-04-27";

    Usage FetchUsage(const std::wstring& id) {
        Usage u;
        if (Paths::Demo) {
            // The demo's figures are the ones tools/demo.ps1 wrote; they just stay current.
            auto all = LoadUsage();
            auto it = all.find(id);
            if (it != all.end()) { u = it->second; u.at = Core::NowMs(); u.problem.clear(); }
            return u;
        }
        std::wstring problem; bool signedOut = false;
        auto r = Authed(id, UsageUrl, problem, signedOut);
        json::Value v;
        if (!problem.empty() || !json::Parser::Parse(r.Text(), v) || v.type != json::Value::Object) {
            u.problem = problem.empty() ? std::wstring(Tr(L"OpenAI sent an unexpected answer", L"OpenAI прислал неожиданный ответ")) : problem;
            u.signedOut = signedOut;
            return u;
        }
        u = ParseUsage(v, Core::NowMs());
        // When a reset is available, the day the first one expires is worth seeing.
        if (u.resetCredits > 0) {
            std::wstring p2; bool s2 = false; json::Value rv;
            auto rr = Authed(id, ResetsUrl, p2, s2);
            if (p2.empty() && json::Parser::Parse(rr.Text(), rv)) ParseResets(rv, Core::NowMs(), u.resetCredits, u.resetExpires, nullptr);
        }
        if (!u.plan.empty()) {
            // The plan changes with the subscription; keep the card's badge current.
            Lock lock;
            auto list = Load();
            for (auto& a : list) if (a.id == id && a.plan != u.plan) { a.plan = u.plan; try { Save(list); } catch (...) {} break; }
        }
        return u;
    }

    // Made-up figures for the demo's statistics window.
    static Stats DemoStats(const std::wstring& id) {
        Stats s; s.known = true; s.at = s.asOf = Core::NowMs();
        unsigned seed = 0; for (wchar_t c : id) seed = seed * 31 + c;
        auto rnd = [&](unsigned lo, unsigned hi) { seed = seed * 1103515245 + 12345; return lo + (seed >> 8) % (hi - lo + 1); };
        long long now = Core::NowMs();
        for (int d = 29; d >= 0; d--) {
            ULONGLONG ft = (ULONGLONG)(now - d * 86400000ll) * 10000 + 116444736000000000ull;
            FILETIME f{ (DWORD)ft, (DWORD)(ft >> 32) }; SYSTEMTIME t; FileTimeToSystemTime(&f, &t);
            long long tokens = d % 7 == 5 ? 0 : (long long)rnd(200, 4200) * 1000;
            s.daily.push_back({ Util::Format(L"%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay), tokens });
        }
        s.lifetime = (long long)rnd(80, 900) * 1000000; s.peakDaily = 6800000; s.longestTask = rnd(900, 5400);
        s.streak = rnd(2, 12); s.longestStreak = s.streak + rnd(3, 20); s.threads = rnd(40, 400); s.skills = rnd(4, 18); s.skillsTotal = s.skills * 7;
        s.fastPercent = rnd(5, 40); s.effort = L"medium"; s.effortPercent = rnd(40, 80);
        s.top = { { L"skill", L"code-review", 128 }, { L"plugin", L"GitHub", 74 }, { L"skill", L"frontend-design", 41 }, { L"plugin", L"Linear", 17 } };
        s.resets = 1; s.resetNext = now / 1000 + 9 * 86400; s.resetList = { { L"Limit reset", L"available", s.resetNext } };
        s.subscriptionUntil = now + 23ll * 86400000;
        return s;
    }

    Stats FetchStats(const std::wstring& id) {
        if (Paths::Demo) return DemoStats(id);
        Stats s;
        std::wstring problem; bool signedOut = false;
        json::Value v;
        auto r = Authed(id, ProfileUrl, problem, signedOut);
        if (problem.empty() && json::Parser::Parse(r.Text(), v)) s = ParseStats(v, Core::NowMs());
        else s.problem = problem.empty() ? std::wstring(Tr(L"OpenAI sent an unexpected answer", L"OpenAI прислал неожиданный ответ")) : problem;
        if (signedOut) return s;
        // Resets and the subscription can be there even when the statistics aren't.
        std::wstring p2; bool s2 = false; json::Value rv;
        auto rr = Authed(id, ResetsUrl, p2, s2);
        if (p2.empty() && json::Parser::Parse(rr.Text(), rv)) ParseResets(rv, Core::NowMs(), s.resets, s.resetNext, &s.resetList);
        std::wstring p3; bool s3 = false; json::Value cv;
        auto rc = Authed(id, CheckUrl, p3, s3);
        if (p3.empty() && json::Parser::Parse(rc.Text(), cv)) {
            // { "accounts": { "<workspace>": { "account": { "plan_type" }, "entitlement": { "expires_at" } } } }
            const json::Value* entry = nullptr;
            std::wstring workspace;
            for (auto& a : Load()) if (a.id == id) workspace = a.workspace;
            if (auto accounts = cv.Find(L"accounts")) {
                entry = accounts->Find(workspace);
                if (!entry) entry = accounts->Find(L"default");
                if (!entry && !accounts->obj.empty()) entry = &accounts->obj[0].second;
            }
            if (entry) {
                if (auto acct = entry->Find(L"account")) s.plan = acct->Str(L"plan_type");
                if (auto ent = entry->Find(L"entitlement")) s.subscriptionUntil = Core::ParseIso(ent->Str(L"expires_at"));
            }
            if (!s.plan.empty() || s.subscriptionUntil) {
                Lock lock;
                auto list = Load();
                for (auto& a : list) if (a.id == id) {
                    if (!s.plan.empty()) a.plan = s.plan;
                    if (s.subscriptionUntil) a.subscriptionUntil = s.subscriptionUntil;
                    try { Save(list); } catch (...) {}
                }
            }
        }
        return s;
    }

    bool Warmup(const std::wstring& id, std::wstring& error) {
        if (Paths::Demo) {
            Lock lock; auto list = Load();
            for (auto& a : list) if (a.id == id) a.warmedAt = Core::NowMs();
            Save(list); return true;
        }
        json::Value auth;
        {
            Lock lock;
            auto list = Load();
            auto live = ReadLive();
            bool dirty = SyncLive(list, live);
            Account* acc = nullptr;
            for (auto& a : list) if (a.id == id) acc = &a;
            if (!acc) { error = Tr(L"This account is gone", L"Этого аккаунта уже нет"); return false; }
            bool active = ActiveId(list, live) == id;
            bool owned = Owned(*acc, active, dirty);
            if (owned && NeedsRenewal(acc->auth, Core::NowMs() / 1000)) {
                bool revoked = false;
                if (!OpenAI::Renew(acc->auth, error, revoked)) { if (dirty) Save(list); return false; }
                dirty = true;
                if (active) WriteLive(acc->auth);
            }
            if (dirty) Save(list);
            auth = acc->auth;
        }
        auto models = OpenAI::WarmupModels();
        Net::Response r;
        for (size_t i = 0; i < models.size() && i < 4; i++) {
            r = OpenAI::Warm(auth, models[i]);
            if (r.status == 200) {
                if (Settings::Str(L"WarmupModel") != models[i]) Settings::Set(L"WarmupModel", models[i]);
                Lock lock;
                auto list = Load();
                for (auto& a : list) if (a.id == id) a.warmedAt = Core::NowMs();
                try { Save(list); } catch (...) {}
                return true;
            }
            // A model this account can't use: try the next one. Anything else won't change.
            if (r.status != 400 && r.status != 404) break;
        }
        error = r.status == 0 ? std::wstring(Tr(L"No connection to OpenAI", L"Нет связи с OpenAI"))
              : r.status == 429 ? std::wstring(Tr(L"The limit is used up, so the window can't be started", L"Лимит исчерпан, окно не запустить"))
              : r.status == 401 ? std::wstring(Tr(L"OpenAI didn't accept this sign-in. Sign in again", L"OpenAI не принял этот вход. Войдите заново"))
              : Util::Format(Tr(L"OpenAI answered with an error (%lu)", L"OpenAI ответил ошибкой (%lu)"), r.status);
        Core::Log(Util::Format(L"Warm-up: HTTP %lu", r.status));
        return false;
    }

    // ------------------------------------------------------------ usage cache

    std::map<std::wstring, Usage> LoadUsage() {
        std::map<std::wstring, Usage> out;
        json::Value v;
        if (!json::Parser::Parse(Util::ReadText(Paths::UsageFile), v) || v.type != json::Value::Object) return out;
        for (auto& kv : v.obj) {
            auto& o = kv.second; Usage u;
            u.known = o.Flag(L"known", false);
            u.five = o.Num(L"five", -1); u.week = o.Num(L"week", -1);
            u.fiveReset = (long long)o.Num(L"fiveReset", 0); u.weekReset = (long long)o.Num(L"weekReset", 0);
            u.limited = o.Flag(L"limited", false); u.plan = o.Str(L"plan");
            u.credits = o.Flag(L"credits", false); u.unlimited = o.Flag(L"unlimited", false); u.balance = o.Str(L"balance");
            u.resetCredits = (int)o.Num(L"resetCredits", 0); u.resetExpires = (long long)o.Num(L"resetExpires", 0); u.at = (long long)o.Num(L"at", 0);
            u.problem = o.Str(L"problem"); u.signedOut = o.Flag(L"signedOut", false);
            out[kv.first] = u;
        }
        return out;
    }

    void SaveUsage(const std::map<std::wstring, Usage>& all) {
        auto v = json::Value::MakeObject();
        for (auto& kv : all) {
            auto& u = kv.second; auto o = json::Value::MakeObject();
            o[L"known"] = u.known; o[L"five"] = u.five; o[L"week"] = u.week;
            o[L"fiveReset"] = (double)u.fiveReset; o[L"weekReset"] = (double)u.weekReset;
            o[L"limited"] = u.limited; o[L"plan"] = u.plan; o[L"credits"] = u.credits; o[L"unlimited"] = u.unlimited; o[L"balance"] = u.balance;
            o[L"resetCredits"] = u.resetCredits; o[L"resetExpires"] = (double)u.resetExpires; o[L"at"] = (double)u.at; o[L"problem"] = u.problem; o[L"signedOut"] = u.signedOut;
            v[kv.first] = o;
        }
        std::wstring text; json::Write(text, v);
        CreateDirectoryW(Paths::Root.c_str(), nullptr);
        Util::WriteText(Paths::UsageFile, text, false);
    }

    // ------------------------------------------------------------ importing

    json::Value ReadAuthFile(const std::wstring& path, std::wstring& error) {
        json::Value v;
        auto text = Util::ReadText(path);
        if (text.empty()) { error = Tr(L"The file is empty or can't be read", L"Файл пуст или не читается"); return json::Value(); }
        if (!json::Parser::Parse(text, v) || v.type != json::Value::Object) { error = Tr(L"This isn't an auth.json file", L"Это не файл auth.json"); return json::Value(); }
        if (IdentityOf(v).empty()) { error = Tr(L"This file has no Codex sign-in in it", L"В этом файле нет входа Codex"); return json::Value(); }
        return v;
    }

    std::wstring LegacyFile() { return Paths::Demo ? Paths::Root + L"\\.codex-switcher\\accounts.json" : Paths::UserProfile() + L"\\.codex-switcher\\accounts.json"; }

    // An entry of Codex Switcher's accounts.json: { name, auth_data: { type, id_token, … } }.
    json::Value LegacyAuth(const json::Value& entry) {
        auto data = entry.Find(L"auth_data");
        if (!data || data->type != json::Value::Object) return json::Value();
        auto type = Util::Lower(data->Str(L"type"));
        if (type == L"api_key") {
            auto key = data->Str(L"key");
            if (key.empty()) return json::Value();
            auto auth = json::Value::MakeObject(); auth[L"OPENAI_API_KEY"] = key;
            return auth;
        }
        auto id = data->Str(L"id_token"), access = data->Str(L"access_token"), refresh = data->Str(L"refresh_token");
        if (access.empty() || refresh.empty()) return json::Value();
        auto auth = OpenAI::AuthFromTokens(id, access, refresh);
        auto workspace = data->Str(L"account_id");
        if (!workspace.empty()) auth[L"tokens"][L"account_id"] = workspace;
        return auth;
    }

    Imported ImportLegacy() {
        Imported r;
        for (auto& l : LegacyAccounts(r.error)) {
            // Codex Switcher names an account after its email when given no name.
            auto a = Add(l.auth, l.name.find(L'@') != std::wstring::npos ? L"" : l.name);
            if (!a.error.empty()) { r.skipped++; continue; }
            (a.existed ? r.updated : r.added)++;
            if (!l.lastUsed) continue;
            Lock lock;
            auto list = Load();
            for (auto& x : list) if (x.id == a.id && x.lastUsed < l.lastUsed) { x.lastUsed = l.lastUsed; try { Save(list); } catch (...) {} break; }
        }
        return r;
    }

    // ---- backups

    static const unsigned BackupIterations = 600000;

    std::string ExportBackup(const std::wstring& password) {
        if (password.size() < 8) throw std::wstring(Tr(L"Use a password of at least 8 characters", L"Пароль должен быть не короче 8 символов"));
        auto v = json::Value::MakeObject();
        v[L"format"] = L"codex-account-switcher-backup";
        v[L"version"] = 1;
        v[L"exported"] = (double)Core::NowMs();
        auto& arr = v[L"accounts"]; arr = json::Value::MakeArray();
        {
            Lock lock;
            auto list = Load();
            if (SyncLive(list, ReadLive())) { try { Save(list); } catch (...) {} }
            for (auto& a : list) {
                auto o = json::Value::MakeObject();
                o[L"name"] = a.name; o[L"lastUsed"] = (double)a.lastUsed; o[L"auth"] = a.auth;
                arr.arr.push_back(o);
            }
        }
        std::wstring text; json::Write(text, v);
        auto plain = Util::Utf8(text);
        SecureZeroMemory(&text[0], text.size() * sizeof(wchar_t));
        auto salt = Net::Random(16), nonce = Net::Random(12);
        auto pw8 = Util::Utf8(password);
        auto key = Net::Pbkdf2(pw8, salt, BackupIterations, 32);
        if (!pw8.empty()) SecureZeroMemory(&pw8[0], pw8.size());
        auto sealed = key.empty() ? std::string() : Net::AesGcmSeal(key, nonce, plain);
        SecureZeroMemory(&plain[0], plain.size());
        if (!key.empty()) SecureZeroMemory(&key[0], key.size());
        if (sealed.empty()) throw std::wstring(Tr(L"Couldn't encrypt the backup", L"Не удалось зашифровать резервную копию"));
        std::string out = "CASB";
        out += (char)1;
        for (int i = 0; i < 4; i++) out += (char)((BackupIterations >> (8 * i)) & 0xFF);
        return out + salt + nonce + sealed;
    }

    std::string BackupText(const std::string& file) { return "cas1." + Net::Base64Url(file); }

    static std::string Trimmed(const std::string& data) {
        size_t a = 0, b = data.size();
        while (a < b && isspace((unsigned char)data[a])) a++;
        while (b > a && isspace((unsigned char)data[b - 1])) b--;
        return data.substr(a, b - a);
    }

    BackupKind KindOf(const std::string& raw) {
        if (raw.compare(0, 4, "CASB") == 0) return BackupKind::Ours;
        if (raw.compare(0, 4, "CSWF") == 0) return BackupKind::CodexSwitcherFile;
        auto t = Trimmed(raw);
        if (t.compare(0, 5, "cas1.") == 0) return BackupKind::Ours;
        if (t.compare(0, 5, "css1.") == 0) return BackupKind::CodexSwitcherText;
        return BackupKind::None;
    }

    // Adds one account from a backup, keeping its name when it is free and valid.
    static void AddFrom(Imported& r, const json::Value& auth, std::wstring name, long long lastUsed) {
        if (name.find(L'@') != std::wstring::npos) name.clear();
        auto a = Add(auth, name);
        if (!a.error.empty()) { r.skipped++; return; }
        (a.existed ? r.updated : r.added)++;
        if (!lastUsed) return;
        Lock lock;
        auto list = Load();
        for (auto& x : list) if (x.id == a.id && x.lastUsed < lastUsed) { x.lastUsed = lastUsed; try { Save(list); } catch (...) {} break; }
    }

    Imported ImportBackup(const std::string& raw, const std::wstring& password) {
        Imported r;
        auto kind = KindOf(raw);
        std::string json8;
        if (kind == BackupKind::Ours) {
            std::string text;
            if (raw.compare(0, 4, "CASB") != 0) for (char c : Trimmed(raw).substr(5)) if (!isspace((unsigned char)c)) text += c;
            auto file = raw.compare(0, 4, "CASB") == 0 ? raw : Net::Base64UrlDecode(text);
            if (file.size() < 4 + 1 + 4 + 16 + 12 + 16 || file.compare(0, 4, "CASB") != 0 || file[4] != 1) { r.error = Tr(L"This backup is damaged or from a newer version", L"Резервная копия повреждена или из более новой версии"); return r; }
            unsigned iterations = 0;
            for (int i = 0; i < 4; i++) iterations |= (unsigned)(unsigned char)file[5 + i] << (8 * i);
            if (iterations < 10000 || iterations > 10000000) { r.error = Tr(L"This backup is damaged", L"Резервная копия повреждена"); return r; }
            auto salt = file.substr(9, 16), nonce = file.substr(25, 12), sealed = file.substr(37);
            auto pw8 = Util::Utf8(password);
            auto key = Net::Pbkdf2(pw8, salt, iterations, 32);
            if (!pw8.empty()) SecureZeroMemory(&pw8[0], pw8.size());
            bool opened = !key.empty() && Net::AesGcmOpen(key, nonce, sealed, json8);
            if (!key.empty()) SecureZeroMemory(&key[0], key.size());
            if (!opened) { r.error = Tr(L"Wrong password, or the backup is damaged", L"Неверный пароль, или копия повреждена"); return r; }
        } else if (kind == BackupKind::CodexSwitcherFile) {
            if (!Compat::OpenCodexSwitcherFile(raw, json8)) { r.error = Tr(L"This Codex Switcher backup can't be read", L"Эту копию Codex Switcher не прочитать"); return r; }
        } else if (kind == BackupKind::CodexSwitcherText) {
            std::string text;
            for (char c : Trimmed(raw)) if (!isspace((unsigned char)c)) text += c;
            if (!Compat::OpenCodexSwitcherText(text, json8)) { r.error = Tr(L"This Codex Switcher text can't be read", L"Этот текст Codex Switcher не прочитать"); return r; }
        } else { r.error = Tr(L"This isn't a backup of accounts", L"Это не резервная копия аккаунтов"); return r; }

        json::Value v;
        auto wide = Util::Wide(json8);
        bool parsed = json::Parser::Parse(wide, v);
        if (!json8.empty()) SecureZeroMemory(&json8[0], json8.size());
        if (!wide.empty()) SecureZeroMemory(&wide[0], wide.size() * sizeof(wchar_t));
        if (!parsed || v.type != json::Value::Object) { r.error = Tr(L"The backup's contents can't be read", L"Содержимое копии не читается"); return r; }

        std::wstring stopped;
        try {
        if (kind == BackupKind::Ours) {
            if (auto list = v.Find(L"accounts"))
                for (auto& a : list->arr) {
                    auto auth = a.Find(L"auth");
                    if (!auth || IdentityOf(*auth).empty()) { r.skipped++; continue; }
                    AddFrom(r, *auth, a.Str(L"name"), (long long)a.Num(L"lastUsed", 0));
                }
        } else if (kind == BackupKind::CodexSwitcherFile) {
            // Its own accounts.json, encrypted.
            if (auto list = v.Find(L"accounts"))
                for (auto& e : list->arr) {
                    auto auth = LegacyAuth(e);
                    if (auth.IsNull()) { r.skipped++; continue; }
                    AddFrom(r, auth, Util::Trim(e.Str(L"name")), Core::ParseIso(e.Str(L"last_used_at")));
                }
        } else {
            // { "v": 1, "c": [ { "n": name, "t": 1, "r": refresh token } or { "t": 0, "k": key } ] }:
            // only refresh tokens, so each sign-in is renewed to get the rest.
            if ((int)v.Num(L"v", 0) != 1) { r.error = Tr(L"This Codex Switcher export is from a version this switcher doesn't know", L"Этот экспорт Codex Switcher из неизвестной версии"); return r; }
            if (auto list = v.Find(L"c"))
                for (auto& e : list->arr) {
                    json::Value auth;
                    if ((int)e.Num(L"t", -1) == 0 && !e.Str(L"k").empty()) { auth = json::Value::MakeObject(); auth[L"OPENAI_API_KEY"] = e.Str(L"k"); }
                    else if ((int)e.Num(L"t", -1) == 1 && !e.Str(L"r").empty()) {
                        auth = json::Value::MakeObject();
                        auth[L"tokens"] = json::Value::MakeObject();
                        auth[L"tokens"][L"refresh_token"] = e.Str(L"r");
                        std::wstring err; bool revoked = false;
                        if (!OpenAI::Renew(auth, err, revoked) || IdentityOf(auth).empty()) { r.skipped++; if (r.error.empty()) r.error = err; continue; }
                    } else { r.skipped++; continue; }
                    AddFrom(r, auth, Util::Trim(e.Str(L"n")), 0);
                }
        }
        } catch (const std::wstring& e) { stopped = e; }
        if (r.added + r.updated > 0) r.error.clear();
        if (!stopped.empty()) r.error = stopped;
        return r;
    }

    std::vector<Legacy> LegacyAccounts(std::wstring& error) {
        std::vector<Legacy> out;
        json::Value v;
        auto text = Util::ReadText(LegacyFile());
        if (text.empty()) return out;
        if (!json::Parser::Parse(text, v)) { error = Tr(L"Codex Switcher's accounts.json can't be read", L"Файл accounts.json от Codex Switcher не читается"); return out; }
        auto list = v.Find(L"accounts");
        if (!list) return out;
        for (auto& e : list->arr) {
            Legacy l;
            l.auth = LegacyAuth(e);
            if (l.auth.IsNull()) continue;
            l.name = Util::Trim(e.Str(L"name"));
            l.lastUsed = Core::ParseIso(e.Str(L"last_used_at"));
            out.push_back(l);
        }
        return out;
    }
}
