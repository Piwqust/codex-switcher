// Unit tests for the parts that decide what happens to sign-ins: encoding, tokens, who an
// account is, taking renewed tokens back, switching, limits and importing. Built with
// `build.cmd x64 build\x64\Tests.exe test`; no network, and nothing outside a temporary folder.
#include "../src/app.h"
#include "../src/codex.h"
#include "../src/net.h"
#include "../src/recovery.h"
#include <shellapi.h>
#include <winhttp.h>
#include <atomic>
#include <iostream>
#include <stdexcept>

static int checks = 0;
static void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
    checks++;
}

static std::wstring Jwt(const std::wstring& payload) {
    return L"eyJhbGciOiJub25lIn0." + Util::Wide(Net::Base64Url(Util::Utf8(payload))) + L".sig";
}

static std::wstring IdToken(const std::wstring& email, const std::wstring& workspace, const std::wstring& user, const std::wstring& plan, long long exp) {
    return Jwt(Util::Format(L"{\"email\":\"%s\",\"sub\":\"auth0|%s\",\"exp\":%lld,\"https://api.openai.com/auth\":{\"chatgpt_account_id\":\"%s\",\"chatgpt_user_id\":\"%s\",\"chatgpt_plan_type\":\"%s\"}}",
                            email.c_str(), user.c_str(), exp, workspace.c_str(), user.c_str(), plan.c_str()));
}

static std::wstring Access(long long exp, const std::wstring& tag) {
    return Jwt(Util::Format(L"{\"exp\":%lld,\"jti\":\"%s\"}", exp, tag.c_str()));
}

static json::Value Auth(const std::wstring& email, const std::wstring& workspace, const std::wstring& user, long long exp, const std::wstring& refresh) {
    auto a = OpenAI::AuthFromTokens(IdToken(email, workspace, user, L"plus", exp), Access(exp, refresh), refresh);
    return a;
}

static std::wstring Refresh(const json::Value& auth) { auto t = auth.Find(L"tokens"); return t ? t->Str(L"refresh_token") : L""; }

static void Encoding() {
    Check(Net::Base64Url("") == "", "base64url empty");
    Check(Net::Base64Url("f") == "Zg" && Net::Base64Url("fo") == "Zm8" && Net::Base64Url("foo") == "Zm9v", "base64url vectors");
    Check(Net::Base64UrlDecode("Zm9vYmE") == "fooba" && Net::Base64UrlDecode("Zm9vYmFy") == "foobar", "base64url decode");
    Check(Net::Base64UrlDecode("-_8") == std::string("\xfb\xff"), "base64url alphabet");
    Check(Net::Hex(Net::Sha256("abc")) == L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha-256 of abc");
    // RFC 7636, appendix B.
    Check(Net::Base64Url(Net::Sha256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")) == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM", "PKCE S256 challenge");
    Check(Net::UrlEncode("http://localhost:1455/auth/callback") == "http%3A%2F%2Flocalhost%3A1455%2Fauth%2Fcallback", "url encode");
    Check(Net::QueryParam("code=ab%2Bc&state=x%20y", "code") == "ab+c" && Net::QueryParam("code=1&state=x+y", "state") == "x y", "query parameters");
    Check(Net::QueryParam("a=1", "b").empty(), "missing parameter");
    Check(Net::Random(32).size() == 32 && Net::Random(32) != Net::Random(32), "random bytes");
    std::string plain;
    Paths::Demo = false;
    auto sealed = Net::Protect("secret");
    Check(!sealed.empty() && sealed.find("secret") == std::string::npos && Net::Unprotect(sealed, plain) && plain == "secret", "DPAPI round trip");
    Check(!Net::Unprotect("not a blob", plain), "DPAPI refuses garbage");
}

static void Crypto() {
    // RFC 6070-style vector for PBKDF2-HMAC-SHA256 ("password", "salt", 1 iteration).
    Check(Net::Hex(Net::Pbkdf2("password", "salt", 1, 32)) == L"120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b", "PBKDF2-SHA256");
    auto key = Net::Random(32), nonce = Net::Random(12);
    auto sealed = Net::AesGcmSeal(key, nonce, "the sign-ins");
    std::string plain;
    Check(sealed.size() == 12 + 16 && Net::AesGcmOpen(key, nonce, sealed, plain) && plain == "the sign-ins", "AES-GCM round trip");
    sealed[3] ^= 1;
    Check(!Net::AesGcmOpen(key, nonce, sealed, plain), "AES-GCM refuses a changed byte");
}

static void Stopped() {
    auto line = [](const std::string& type, const std::string& payload) { return "{\"type\":\"" + type + "\",\"payload\":" + payload + "}"; };
    std::string limit = line("event_msg", "{\"type\":\"error\",\"codex_error_info\":\"usage_limit_exceeded\",\"message\":\"x\"}");
    std::string failedTurn = line("event_msg", "{\"type\":\"task_complete\",\"error\":{\"message\":\"You've hit your usage limit. Upgrade\"}}");
    std::string started = line("event_msg", "{\"type\":\"task_started\"}");
    std::string user = line("response_item", "{\"type\":\"message\",\"role\":\"user\",\"content\":[]}");
    std::string quoted = line("response_item", "{\"type\":\"function_call_output\",\"output\":\"You've hit your usage limit.\"}");
    Check(Recovery::StoppedByLimit({ started, limit }), "stopped by the limit");
    Check(Recovery::StoppedByLimit({ started, failedTurn }), "a turn that failed on the limit");
    Check(!Recovery::StoppedByLimit({ limit, user }), "continued since");
    Check(!Recovery::StoppedByLimit({ started, quoted }), "the error text quoted in output doesn't count");
    Check(!Recovery::StoppedByLimit({ limit, "{\"type\":\"event_msg\",\"payl" }), "a broken last record means unknown");
    Check(!Recovery::StoppedByLimit({ started, line("event_msg", "{\"type\":\"task_complete\"}") }), "finished normally");
    Check(Recovery::StoppedByLimit({ started + "\r", limit + "\r" }), "records ending in CRLF");
    Check(Recovery::StoppedByLimit({ started, limit, line("event_msg", "{\"type\":\"token_count\",\"info\":{}}") }), "bookkeeping after the error changes nothing");
    auto meta = [](const std::string& extra) { return "{\"type\":\"session_meta\",\"payload\":{\"id\":\"01a1f4f9-4ced-78f0-a500-b4687f59df4d\"" + extra + "}}"; };
    Check(Recovery::ThreadId(meta("")) == L"01a1f4f9-4ced-78f0-a500-b4687f59df4d", "conversation id");
    Check(Recovery::ThreadId(meta(",\"source\":{\"subagent\":{}}")).empty() && Recovery::ThreadId(meta(",\"agent_path\":\"/root/x\"")).empty(), "subagents are left to their parent");
    Check(Recovery::ThreadId("{\"type\":\"turn_context\"}").empty(), "not a session header");
}

static void Statistics() {
    json::Value v;
    Check(json::Parser::Parse(L"{\"stats\":{\"lifetime_tokens\":123456789,\"peak_daily_tokens\":5000,\"longest_running_turn_sec\":3700,\"current_streak_days\":4,"
                              L"\"longest_streak_days\":9,\"total_threads\":40,\"unique_skills_used\":3,\"total_skills_used\":21,\"fast_mode_usage_percentage\":12.5,"
                              L"\"most_used_reasoning_effort\":\"high\",\"most_used_reasoning_effort_percentage\":70,"
                              L"\"daily_usage_buckets\":[{\"start_date\":\"2026-11-19\",\"tokens\":10},{\"start_date\":\"2026-11-18\",\"tokens\":5}],"
                              L"\"top_invocations\":[{\"type\":\"skill\",\"skill_name\":\"review\",\"usage_count\":7}]},\"metadata\":{\"stats_as_of\":\"2026-11-20T01:00:00Z\"}}", v), "statistics parse");
    auto s = ParseStats(v, Core::NowMs());
    Check(s.known && s.lifetime == 123456789 && s.longestTask == 3700 && s.streak == 4 && s.effort == L"high" && s.fastPercent == 12.5, "statistics fields");
    Check(s.daily.size() == 2 && s.daily[0].first == L"2026-11-18" && s.top.size() == 1 && s.top[0].name == L"review" && s.top[0].count == 7, "days in order, top skills");
    Check(s.asOf > 0, "as of");
    json::Value none; json::Parser::Parse(L"{\"metadata\":{\"stats_error\":\"Not yet\"}}", none);
    auto n = ParseStats(none, 0);
    Check(!n.known && n.problem == L"Not yet", "no statistics");
    json::Value r;
    json::Parser::Parse(L"{\"available_count\":2,\"credits\":[{\"title\":\"Reset\",\"status\":\"available\",\"expires_at\":\"2099-01-02T00:00:00Z\"},"
                        L"{\"title\":\"Old\",\"status\":\"available\",\"expires_at\":\"2001-01-01T00:00:00Z\"},{\"status\":\"redeemed\",\"expires_at\":\"2098-01-01T00:00:00Z\"}]}", r);
    int count = 0; long long next = 0; std::vector<Stats::Reset> list;
    ParseResets(r, Core::NowMs(), count, next, &list);
    Check(count == 2 && next == Core::ParseIso(L"2099-01-02T00:00:00Z") / 1000 && list.size() == 3, "limit resets: the first available one still to expire");
    Check(Core::ShortNumber(1234567) == (L10n::ru ? L"1,2 млн" : L"1.2M") && Core::ShortNumber(999) == L"999", "short numbers");
}

static void Tokens() {
    auto c = ParseJwt(IdToken(L"a@x.com", L"ws-1", L"user-1", L"pro", 2000000000));
    Check(c.ok && c.email == L"a@x.com" && c.accountId == L"ws-1" && c.userId == L"user-1" && c.plan == L"pro" && c.exp == 2000000000, "id token claims");
    Check(!ParseJwt(L"nonsense").ok && !ParseJwt(L"a.b").ok, "bad tokens");
    auto profile = ParseJwt(Jwt(L"{\"https://api.openai.com/profile\":{\"email\":\"p@x.com\"}}"));
    Check(profile.email == L"p@x.com", "access token profile email");

    auto a = Auth(L"a@x.com", L"ws-1", L"user-1", 2000000000, L"r1");
    Check(Accounts::IdentityOf(a) == L"ws-1|user-1", "identity from claims");
    Check(Accounts::IdentityOf(Auth(L"a@x.com", L"ws-2", L"user-1", 2000000000, L"r2")) != Accounts::IdentityOf(a), "another workspace is another account");
    Check(Accounts::IdentityOf(Auth(L"a@x.com", L"WS-1", L"user-1", 1, L"r3")) == Accounts::IdentityOf(a), "workspace ids compare case-insensitively");
    auto key = json::Value::MakeObject(); key[L"OPENAI_API_KEY"] = L"sk-test";
    Check(Accounts::IdentityOf(key).rfind(L"key|", 0) == 0 && Accounts::IdentityOf(key).find(L"sk-test") == std::wstring::npos, "API key identity without the key");
    Check(Accounts::IdentityOf(json::Value::MakeObject()).empty() && Accounts::IdentityOf(json::Value()).empty(), "no sign-in");
    auto d = Accounts::Describe(a);
    Check(d.email == L"a@x.com" && d.plan == L"plus" && d.workspace == L"ws-1" && d.user == L"user-1", "describe");
    Check(a.Find(L"tokens")->Str(L"account_id") == L"ws-1" && !a.Str(L"last_refresh").empty(), "auth.json shape");

    long long now = 1800000000;
    Check(Accounts::NeedsRenewal(Auth(L"a@x.com", L"w", L"u", now + 60, L"r"), now), "renew when about to expire");
    Check(!Accounts::NeedsRenewal(Auth(L"a@x.com", L"w", L"u", now + 3600, L"r"), now), "keep a valid token");

    auto iso = Core::IsoNow();
    Check(iso.size() == 30 && iso.back() == L'Z' && iso[10] == L'T' && Core::ParseIso(iso) > 0, "timestamps the way Codex writes them");
    Check(Core::ParseIso(L"2026-04-23T05:03:38+00:00") == 1776920618000ll, "ISO with offset");
    Check(Core::ParseIso(L"2026-04-23T08:03:38+03:00") == 1776920618000ll, "ISO with positive offset");
}

static void Sync() {
    long long now = Core::NowMs() / 1000;
    Account a; a.id = L"a"; a.auth = Auth(L"a@x.com", L"ws-a", L"u-a", now + 3600, L"a-old"); a.email = L"a@x.com";
    Account b; b.id = L"b"; b.auth = Auth(L"b@x.com", L"ws-b", L"u-b", now + 3600, L"b-old");
    std::vector<Account> list{ a, b };
    auto live = Auth(L"a@x.com", L"ws-a", L"u-a", now + 7200, L"a-new");
    Check(Accounts::ActiveId(list, live) == L"a", "active account by identity");
    Check(Accounts::SyncLive(list, live) && Refresh(list[0].auth) == L"a-new" && Refresh(list[1].auth) == L"b-old", "renewed tokens are taken back");
    Check(!Accounts::SyncLive(list, live), "nothing new, nothing changed");
    auto stale = Auth(L"a@x.com", L"ws-a", L"u-a", now + 60, L"a-older");
    Check(!Accounts::SyncLive(list, stale) && Refresh(list[0].auth) == L"a-new", "an older copy never replaces a newer one");
    auto other = Auth(L"c@x.com", L"ws-c", L"u-c", now + 7200, L"c");
    Check(!Accounts::SyncLive(list, other) && Accounts::ActiveId(list, other).empty(), "an unknown sign-in touches nothing");
    auto rewritten = live; rewritten[L"last_refresh"] = L"2026-01-01T00:00:00.000000000Z";
    Check(Accounts::SyncLive(list, rewritten) && list[0].auth.Str(L"last_refresh") == L"2026-01-01T00:00:00.000000000Z", "Codex's own copy wins when the tokens are the same");
    // Unknown fields Codex adds to auth.json come along unchanged.
    live[L"future_field"] = L"kept";
    live[L"tokens"][L"access_token"] = Access(now + 9000, L"a-newest");
    Check(Accounts::SyncLive(list, live) && list[0].auth.Str(L"future_field") == L"kept", "auth.json kept whole");
}

static void Names() {
    std::vector<Account> list;
    Account a; a.id = L"1"; a.name = L"work"; a.email = L"work@x.com"; list.push_back(a);
    Check(!Accounts::ValidateName(L"", list, L"").empty() && !Accounts::ValidateName(L"a/b", list, L"").empty(), "bad names");
    Check(!Accounts::ValidateName(L"Work", list, L"").empty() && Accounts::ValidateName(L"Work", list, L"1").empty(), "duplicate names, except itself");
    Check(!Accounts::ValidateName(std::wstring(41, L'x'), list, L"").empty() && !Accounts::ValidateName(L"x.", list, L"").empty(), "length and trailing dot");
    Account n; n.email = L"john.doe@corp.example"; n.plan = L"team";
    Check(Accounts::DefaultName(n, list) == L"john.doe", "name from the email");
    Account same; same.email = L"work@x.com"; same.plan = L"team";
    Check(Accounts::DefaultName(same, list) == L"work (Team)", "same email, another workspace");
    Account odd; odd.email = L"a:b*c@x.com";
    Check(Accounts::DefaultName(odd, list) == L"a_b_c", "file-name characters replaced");
    Check(Core::Initials(L"john.doe") == L"JD" && Core::Initials(L"Work") == L"W" && Core::Initials(L"рабочий профиль") == L"РП", "initials");
}

static void Limits() {
    long long now = 1795142728000ll;
    json::Value v;
    Check(json::Parser::Parse(L"{\"plan_type\":\"plus\",\"rate_limit\":{\"allowed\":true,\"limit_reached\":false,"
                              L"\"primary_window\":{\"used_percent\":12,\"limit_window_seconds\":18000,\"reset_after_seconds\":16808,\"reset_at\":1795159536},"
                              L"\"secondary_window\":{\"used_percent\":82,\"limit_window_seconds\":604800,\"reset_after_seconds\":293269,\"reset_at\":1795435998}},"
                              L"\"credits\":{\"has_credits\":false,\"unlimited\":false,\"balance\":\"0\"},\"rate_limit_reset_credits\":{\"available_count\":1}}", v), "usage sample parses");
    auto u = ParseUsage(v, now);
    Check(u.known && u.plan == L"plus" && u.five == 12 && u.week == 82 && u.fiveReset == 1795159536 && u.weekReset == 1795435998, "both windows");
    Check(u.resetCredits == 1 && u.balance == L"0" && !u.limited, "credits and resets");
    json::Value weekly;
    json::Parser::Parse(L"{\"plan_type\":\"free\",\"rate_limit\":{\"limit_reached\":true,\"primary_window\":{\"used_percent\":100,\"limit_window_seconds\":604800,\"reset_after_seconds\":60},\"secondary_window\":null}}", weekly);
    auto w = ParseUsage(weekly, now);
    Check(w.five < 0 && w.week == 100 && w.weekReset == now / 1000 + 60 && w.limited, "a weekly-only plan sends its week as primary");
    Usage past; past.five = 70; past.fiveReset = Core::NowMs() / 1000 - 5; past.week = 40; past.weekReset = Core::NowMs() / 1000 + 100;
    Check(past.FiveNow() == 0 && past.WeekNow() == 40, "a reset window reads as empty");
}

static void Legacy() {
    json::Value e;
    json::Parser::Parse(L"{\"name\":\"Main\",\"auth_data\":{\"type\":\"chat_g_pt\",\"id_token\":\"" + IdToken(L"m@x.com", L"ws-m", L"u-m", L"plus", 2000000000) +
                        L"\",\"access_token\":\"acc\",\"refresh_token\":\"ref\",\"account_id\":\"ws-m\"}}", e);
    auto auth = Accounts::LegacyAuth(e);
    Check(Refresh(auth) == L"ref" && auth.Find(L"tokens")->Str(L"account_id") == L"ws-m" && Accounts::IdentityOf(auth) == L"ws-m|u-m", "Codex Switcher account");
    json::Value k;
    json::Parser::Parse(L"{\"name\":\"Key\",\"auth_data\":{\"type\":\"api_key\",\"key\":\"sk-1\"}}", k);
    Check(Accounts::LegacyAuth(k).Str(L"OPENAI_API_KEY") == L"sk-1", "Codex Switcher API key");
    json::Value broken;
    json::Parser::Parse(L"{\"name\":\"x\",\"auth_data\":{\"type\":\"chat_g_pt\",\"id_token\":\"a\"}}", broken);
    Check(Accounts::LegacyAuth(broken).IsNull(), "incomplete entries are skipped");
}

static void LoginUrl() {
    auto url = Login::AuthorizeUrl("http://localhost:1455/auth/callback", "CHALLENGE", "STATE");
    for (auto part : { "https://auth.openai.com/oauth/authorize?", "response_type=code", "client_id=app_EMoamEEZ73f0CkXaXp7hrann",
                       "redirect_uri=http%3A%2F%2Flocalhost%3A1455%2Fauth%2Fcallback", "scope=openid%20profile%20email%20offline_access",
                       "code_challenge=CHALLENGE", "code_challenge_method=S256", "state=STATE", "originator=codex_cli_rs", "prompt=login",
                       "codex_cli_simplified_flow=true", "id_token_add_organizations=true" })
        Check(url.find(part) != std::string::npos, part);
}

static DWORD LocalGet(int port, const std::wstring& path) {
    DWORD status = 0, size = sizeof status;
    HINTERNET s = WinHttpOpen(L"tests", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, L"127.0.0.1", (INTERNET_PORT)port, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr))
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); if (s) WinHttpCloseHandle(s);
    return status;
}

// The local sign-in server, without a browser or OpenAI: only an answer with the right
// state could ever be exchanged, anything else is turned away.
static void LoginServer() {
    std::atomic<bool> done{ false }; Login::Result result;
    std::wstring url, err;
    bool ok = Login::Begin(url, err, [&](Login::Result r) { result = r; done = true; });
    if (!ok && err.find(L"1455") != std::wstring::npos) { std::wcout << L"(sign-in server skipped: ports 1455 and 1457 are in use)\n"; return; }
    Check(ok && Login::Active(), "sign-in server listens");
    std::wstring again, err2;
    Check(!Login::Begin(again, err2, [](Login::Result) {}), "one sign-in at a time");
    int port = url.find(L"localhost%3A1457") != std::wstring::npos ? 1457 : 1455;
    Check(url.find(Util::Format(L"localhost%%3A%d%%2Fauth%%2Fcallback", port)) != std::wstring::npos, "redirect on the listening port");
    Check(LocalGet(port, L"/auth/callback?code=abc&state=forged") == 400, "a forged state is refused");
    Check(LocalGet(port, L"/favicon.ico") == 404, "other paths are not served");
    Check(!done, "still waiting for the real answer");
    Check(LocalGet(port, L"/cancel") == 200, "cancel answered");
    for (int i = 0; i < 100 && !done; i++) Sleep(30);
    Check(done && result.cancelled && result.auth.IsNull() && !Login::Active(), "cancelled cleanly");
}

// The whole switch, in a demo folder: no Codex is touched and nothing goes to the network.
static void Switching(const std::wstring& dir) {
    Paths::Demo = true;
    Paths::Root = dir; Paths::Home = dir + L"\\.switcher"; Paths::SettingsFile = dir + L"\\settings.json";
    Paths::VaultFile = dir + L"\\accounts.dat"; Paths::UsageFile = dir + L"\\usage.json"; Paths::Log = dir + L"\\log.txt";
    CreateDirectoryW(dir.c_str(), nullptr);
    long long now = Core::NowMs() / 1000;
    auto a = Accounts::Add(Auth(L"a@x.com", L"ws-a", L"u-a", now + 3600, L"a1"));
    auto b = Accounts::Add(Auth(L"b@x.com", L"ws-b", L"u-b", now + 3600, L"b1"), L"Second");
    Check(a.error.empty() && b.error.empty() && !a.existed && a.id != b.id, "two accounts added");
    auto again = Accounts::Add(Auth(L"a@x.com", L"ws-a", L"u-a", now + 7200, L"a-relogin"));
    Check(again.existed && again.id == a.id && Accounts::Load().size() == 2, "signing in again updates, not duplicates");
    auto older = Accounts::Add(Auth(L"a@x.com", L"ws-a", L"u-a", now + 60, L"a-stale"));
    Check(older.existed && Refresh(Accounts::Load()[0].auth) == L"a-relogin", "an older copy of a sign-in doesn't replace a newer one");
    Check(Accounts::Load()[1].name == L"Second" && Accounts::Load()[0].name == L"a", "names");
    Check(!Accounts::Rename(a.id, L"Second").empty() && Accounts::Rename(a.id, L"First").empty() && Accounts::Load()[0].name == L"First", "rename");

    // Codex uses A and renews its sign-in while it runs.
    CreateDirectoryW(Codex::Home().c_str(), nullptr);
    Check(Accounts::WriteLive(Auth(L"a@x.com", L"ws-a", L"u-a", now + 9000, L"a-rotated-by-codex")), "auth.json written");
    auto r = Accounts::Switch(b.id, { true, false });
    Check(r.error.empty() && !r.already, "switched");
    auto live = Accounts::ReadLive();
    Check(Refresh(live) == L"b1" && Accounts::ActiveId(Accounts::Load(), live) == b.id, "auth.json holds the new account");
    for (auto& x : Accounts::Load()) if (x.id == a.id) Check(Refresh(x.auth) == L"a-rotated-by-codex", "the renewed tokens of the previous account were saved first");
    Check(Accounts::Switch(b.id, { true, false }).already, "switching to the account in use does nothing");
    r = Accounts::Switch(a.id, { true, false });
    Check(r.error.empty() && Refresh(Accounts::ReadLive()) == L"a-rotated-by-codex", "switching back restores the newest copy");

    // A sign-in Codex has that isn't on the list is added before it is written over.
    Accounts::WriteLive(Auth(L"stranger@x.com", L"ws-s", L"u-s", now + 3600, L"s1"));
    r = Accounts::Switch(b.id, { true, false });
    Check(r.error.empty() && r.keptUnknown == L"stranger@x.com" && Accounts::Load().size() == 3, "an unknown sign-in is kept, not lost");
    for (auto& x : Accounts::Load()) if (x.email == L"stranger@x.com") { Check(Refresh(x.auth) == L"s1", "with its tokens"); Accounts::Remove(x.id); }

    // While a Codex that kept running with an account lives, the switcher doesn't renew it.
    {
        auto list = Accounts::Load();
        for (auto& x : list) if (x.id == a.id) { x.auth = Auth(L"a@x.com", L"ws-a", L"u-a", now + 30, L"a-held"); x.auth[L"last_refresh"] = L"held"; x.heldBy.push_back({ GetCurrentProcessId(), Util::CreatedAt(GetCurrentProcessId()) }); }
        Accounts::Save(list);
    }
    r = Accounts::Switch(a.id, { true, false });
    Check(r.error.empty() && Accounts::ReadLive().Str(L"last_refresh") == L"held", "a sign-in another Codex holds isn't renewed");
    for (auto& x : Accounts::Load()) if (x.id == a.id) Check(x.heldBy.size() == 1, "the holder is remembered while it runs");
    {
        auto list = Accounts::Load();
        for (auto& x : list) if (x.id == a.id) x.heldBy = { { GetCurrentProcessId(), 1 } };   // a process that has ended
        Accounts::Save(list);
    }
    Accounts::Switch(b.id, { true, false });
    r = Accounts::Switch(a.id, { true, false });
    Check(r.error.empty() && Accounts::ReadLive().Str(L"last_refresh") != L"held", "once the holder is gone, it is renewed as usual");
    for (auto& x : Accounts::Load()) if (x.id == a.id) Check(x.heldBy.empty(), "ended holders are forgotten");

    // A backup carries every account to another vault; the wrong password opens nothing.
    {
        auto file = Accounts::ExportBackup(L"correct horse");
        Check(file.compare(0, 4, "CASB") == 0 && Accounts::KindOf(file) == Accounts::BackupKind::Ours, "backup file");
        Check(file.find("a-rotated") == std::string::npos && file.find("a@x.com") == std::string::npos, "nothing readable in the backup");
        auto text = Accounts::BackupText(file);
        Check(text.compare(0, 5, "cas1.") == 0 && Accounts::KindOf("  " + text + "\r\n") == Accounts::BackupKind::Ours, "backup as text");
        Check(!Accounts::ImportBackup(file, L"wrong password").error.empty(), "the wrong password is refused");
        bool threw = false; try { Accounts::ExportBackup(L"short"); } catch (const std::wstring&) { threw = true; }
        Check(threw, "a short password is refused");
        auto saved = Accounts::Load();
        Accounts::Save({});
        auto r = Accounts::ImportBackup(text, L"correct horse");
        Check(r.error.empty() && r.added == (int)saved.size() && Accounts::Load().size() == saved.size(), "every account comes back from the text");
        for (auto& x : Accounts::Load()) for (auto& y : saved) if (x.Identity() == y.Identity()) Check(Refresh(x.auth) == Refresh(y.auth) && x.name == y.name, "with its sign-in and name");
        std::string wrapped;
        for (size_t i = 0; i < text.size(); i++) { wrapped += text[i]; if (i % 40 == 39) wrapped += "\r\n  "; }
        Check(Accounts::ImportBackup(wrapped, L"correct horse").error.empty(), "the text survives a chat wrapping it");
        auto again = Accounts::ImportBackup(file, L"correct horse");
        Check(again.updated == (int)saved.size() && again.added == 0, "importing twice only updates");
        Accounts::Save(saved);   // the ids the rest of the test uses
        Check(Accounts::KindOf("CSWF....") == Accounts::BackupKind::CodexSwitcherFile && Accounts::KindOf("css1.abc") == Accounts::BackupKind::CodexSwitcherText && Accounts::KindOf("hello") == Accounts::BackupKind::None, "kinds of backups");
    }

    Accounts::Move(b.id, 0);
    Check(Accounts::Load()[0].id == b.id, "move");
    Accounts::Remove(b.id);
    Check(Accounts::Load().size() == 1 && Accounts::Load()[0].id == a.id, "remove");

    std::wstring err;
    Check(Accounts::ReadAuthFile(dir + L"\\missing.json", err).IsNull() && !err.empty(), "missing auth.json");
    Util::WriteText(dir + L"\\other.json", L"{\"tokens\":{\"id_token\":\"" + IdToken(L"c@x.com", L"ws-c", L"u-c", L"pro", now + 3600) + L"\",\"access_token\":\"x\",\"refresh_token\":\"y\"}}", false);
    err.clear();
    Check(!Accounts::ReadAuthFile(dir + L"\\other.json", err).IsNull() && err.empty(), "auth.json from a file");

    // Only a top-level setting moves the sign-in out of auth.json.
    Paths::Demo = false;
    SetEnvironmentVariableW(L"CODEX_HOME", (dir + L"\\home").c_str());
    CreateDirectoryW((dir + L"\\home").c_str(), nullptr);
    Util::WriteText(dir + L"\\home\\config.toml", L"model = \"x\"\n[profiles.k]\ncli_auth_credentials_store = \"keyring\"\n", false);
    Check(!Codex::KeyringStore(), "keyring inside a profile doesn't count");
    Util::WriteText(dir + L"\\home\\config.toml", L"cli_auth_credentials_store = \"keyring\"\n", false);
    Check(Codex::KeyringStore() && Codex::AuthFile() == dir + L"\\home\\auth.json", "keyring at the top level; CODEX_HOME is followed");
    SetEnvironmentVariableW(L"CODEX_HOME", nullptr);

    // The real vault is encrypted.
    Paths::VaultFile = dir + L"\\sealed.dat";
    Accounts::Save(Accounts::Load());
    auto bytes = Util::ReadBytes(Paths::VaultFile);
    Check(!bytes.empty() && bytes.find("a-rotated-by-codex") == std::string::npos && bytes.find("a@x.com") == std::string::npos, "nothing readable in the vault file");
}

int wmain() {
    Paths::Init();
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    auto dir = std::wstring(tmp) + Util::Format(L"cas-tests-%lu", GetCurrentProcessId());
    int code = 0;
    try {
        Encoding(); Crypto(); Tokens(); Sync(); Names(); Limits(); Statistics(); Stopped(); Legacy(); LoginUrl(); LoginServer(); Switching(dir);
        std::wcout << L"All " << checks << L" checks passed\n";
    } catch (const std::exception& e) {
        std::cout << "FAILED after " << checks << " checks: " << e.what() << "\n";
        code = 1;
    } catch (const std::wstring& e) {
        std::wcout << L"FAILED after " << checks << L" checks: " << e << L"\n";
        code = 1;
    }
    SHFILEOPSTRUCTW op{}; std::wstring from = dir + L'\0';
    op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
    return code;
}
