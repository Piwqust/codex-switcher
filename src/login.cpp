// Signing in with ChatGPT in the browser. The same flow as `codex login`: the browser goes to
// auth.openai.com with a PKCE challenge, and comes back to http://localhost:1455/auth/callback
// with a one-time code, which is exchanged here for the tokens Codex keeps in auth.json.
// OpenAI accepts only ports 1455 and 1457 for this client.
#define NOMINMAX
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "accounts.h"
#include "net.h"
#include <thread>
#include <atomic>

namespace Login {
    namespace {
        std::atomic<bool> active{ false }, cancelled{ false };
        SOCKET listener = INVALID_SOCKET;
        CRITICAL_SECTION* Guard() { static CRITICAL_SECTION cs; static bool init = (InitializeCriticalSection(&cs), true); (void)init; return &cs; }

        SOCKET Listen(int port) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) return s;
            BOOL exclusive = TRUE;
            setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof exclusive);
            sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
            inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
            if (bind(s, (sockaddr*)&a, sizeof a) != 0 || listen(s, 8) != 0) { closesocket(s); return INVALID_SOCKET; }
            return s;
        }

        // A `codex login` left waiting holds the port; its server stops when asked to.
        void CancelOtherLogin(int port) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) return;
            DWORD t = 1000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof t);
            sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
            inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
            if (connect(s, (sockaddr*)&a, sizeof a) == 0) {
                const char req[] = "GET /cancel HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
                send(s, req, sizeof req - 1, 0);
                char buf[256]; recv(s, buf, sizeof buf, 0);
            }
            closesocket(s);
        }

        std::string Page(bool ok, const std::wstring& detail) {
            auto title = ok ? Tr(L"Signed in", L"Вход выполнен") : Tr(L"Sign-in didn't finish", L"Вход не завершён");
            auto text = ok ? Tr(L"You can close this tab and go back to Codex Account Switcher.", L"Эту вкладку можно закрыть и вернуться в переключатель аккаунтов Codex.") : detail.c_str();
            std::wstring esc;
            for (wchar_t c : std::wstring(text)) { if (c == L'<') esc += L"&lt;"; else if (c == L'>') esc += L"&gt;"; else if (c == L'&') esc += L"&amp;"; else esc += c; }
            std::wstring html = L"<!doctype html><html lang=\"" + std::wstring(L10n::ru ? L"ru" : L"en") + L"\"><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
                L"<title>" + std::wstring(title) + L"</title><style>"
                L":root{color-scheme:light dark;--bg:#f3f3f3;--card:#fff;--text:#0d0d0d;--muted:#5d5d5d;--accent:" + std::wstring(ok ? L"#0d0d0d" : L"#c42b1c") + L";--on:#fff}"
                L"@media(prefers-color-scheme:dark){:root{--bg:#202020;--card:#2b2b2b;--text:#fff;--muted:#c8c8c8;--accent:" + std::wstring(ok ? L"#ececec" : L"#ff99a4") + L";--on:#0d0d0d}}"
                L"body{margin:0;min-height:100vh;display:grid;place-items:center;background:var(--bg);font:15px/1.5 'Segoe UI Variable Text','Segoe UI',system-ui,sans-serif;color:var(--text)}"
                L"main{background:var(--card);border-radius:12px;padding:36px 40px;max-width:420px;box-shadow:0 8px 32px rgba(0,0,0,.12);text-align:center}"
                L".mark{width:56px;height:56px;border-radius:50%;margin:0 auto 18px;display:grid;place-items:center;background:var(--accent);color:var(--on);font-size:28px}"
                L"h1{font:600 22px 'Segoe UI Variable Display','Segoe UI',sans-serif;margin:0 0 8px}p{margin:0;color:var(--muted)}</style>"
                L"<main><div class=\"mark\">" + std::wstring(ok ? L"&#10003;" : L"!") + L"</div><h1>" + std::wstring(title) + L"</h1><p>" + esc + L"</p></main></html>";
            return Util::Utf8(html);
        }

        void Respond(SOCKET c, int status, const std::string& body) {
            auto head = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : status == 404 ? " Not Found" : " Bad Request") +
                        "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
            auto all = head + body;
            send(c, all.data(), (int)all.size(), 0);
            shutdown(c, SD_SEND);
            closesocket(c);
        }

        // Exchanges the one-time code for tokens.
        json::Value Exchange(const std::string& code, const std::string& redirect, const std::string& verifier, std::wstring& error) {
            auto body = "grant_type=authorization_code&code=" + Net::UrlEncode(code) + "&redirect_uri=" + Net::UrlEncode(redirect) +
                        "&client_id=" + Net::UrlEncode(OpenAI::ClientId) + "&code_verifier=" + Net::UrlEncode(verifier);
            auto r = Net::Request(L"POST", std::wstring(OpenAI::Issuer) + L"/oauth/token", L"Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n", body, 30000);
            if (r.status == 0) { error = Tr(L"Couldn't reach OpenAI to finish signing in. Check your connection", L"Не удалось связаться с OpenAI, чтобы завершить вход. Проверьте соединение"); return json::Value(); }
            json::Value v;
            if (r.status != 200 || !json::Parser::Parse(r.Text(), v)) {
                error = Util::Format(Tr(L"OpenAI didn't finish the sign-in (%lu). Try again", L"OpenAI не завершил вход (%lu). Попробуйте ещё раз"), r.status);
                Core::Log(Util::Format(L"Sign-in: token exchange answered HTTP %lu", r.status));
                return json::Value();
            }
            auto id = v.Str(L"id_token"), access = v.Str(L"access_token"), refresh = v.Str(L"refresh_token");
            if (id.empty() || access.empty() || refresh.empty()) { error = Tr(L"OpenAI sent an incomplete sign-in. Try again", L"OpenAI прислал неполный вход. Попробуйте ещё раз"); return json::Value(); }
            return OpenAI::AuthFromTokens(id, access, refresh);
        }

        void Serve(SOCKET server, std::string redirect, std::string verifier, std::string state, std::function<void(Result)> done) {
            Result result;
            auto deadline = GetTickCount64() + 15 * 60 * 1000;
            while (!cancelled && GetTickCount64() < deadline) {
                fd_set set; FD_ZERO(&set); FD_SET(server, &set);
                timeval tv{ 0, 300000 };
                if (select(0, &set, nullptr, nullptr, &tv) <= 0) continue;
                SOCKET c = accept(server, nullptr, nullptr);
                if (c == INVALID_SOCKET) continue;
                // A request gets three seconds in all, so a client trickling bytes can't hold the
                // server, and Cancel is heard between reads.
                DWORD t = 500; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
                std::string req; char buf[4096];
                auto until = GetTickCount64() + 3000;
                while (req.find("\r\n\r\n") == std::string::npos && req.size() < 32768 && !cancelled && GetTickCount64() < until) {
                    int n = recv(c, buf, sizeof buf, 0);
                    if (n == 0 || (n < 0 && WSAGetLastError() != WSAETIMEDOUT)) break;
                    if (n > 0) req.append(buf, n);
                }
                // "GET /auth/callback?code=…&state=… HTTP/1.1"
                auto sp1 = req.find(' '), sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
                if (req.compare(0, 4, "GET ") != 0 || sp2 == std::string::npos) { Respond(c, 400, ""); continue; }
                auto target = req.substr(sp1 + 1, sp2 - sp1 - 1);
                auto q = target.find('?');
                auto path = target.substr(0, q), query = q == std::string::npos ? "" : target.substr(q + 1);
                if (path == "/cancel") { Respond(c, 200, Page(false, Tr(L"Sign-in cancelled", L"Вход отменён"))); result.cancelled = true; break; }
                if (path != "/auth/callback") { Respond(c, 404, ""); continue; }
                // Only an answer to this very request counts; anything else on the port is ignored.
                if (Net::QueryParam(query, "state") != state) { Respond(c, 400, Page(false, Tr(L"This sign-in link is out of date. Start again from the switcher", L"Эта ссылка входа устарела. Начните заново из переключателя"))); continue; }
                auto err = Net::QueryParam(query, "error");
                if (!err.empty()) {
                    auto desc = Net::QueryParam(query, "error_description");
                    result.error = Util::Wide(desc.empty() ? err : desc);
                    Respond(c, 200, Page(false, result.error));
                    break;
                }
                auto code = Net::QueryParam(query, "code");
                if (code.empty()) { Respond(c, 400, Page(false, Tr(L"OpenAI sent no sign-in code", L"OpenAI не прислал код входа"))); continue; }
                result.auth = Exchange(code, redirect, verifier, result.error);
                Respond(c, 200, Page(result.error.empty(), result.error));
                break;
            }
            if (!result.cancelled && result.auth.IsNull() && result.error.empty()) {
                if (cancelled) result.cancelled = true;
                else result.error = Tr(L"The sign-in took too long. Start again", L"Вход занял слишком много времени. Начните заново");
            }
            EnterCriticalSection(Guard());
            closesocket(server); listener = INVALID_SOCKET;
            LeaveCriticalSection(Guard());
            SecureZeroMemory(&verifier[0], verifier.size());
            active = false;
            done(result);
        }
    }

    std::string AuthorizeUrl(const std::string& redirect, const std::string& challenge, const std::string& state) {
        // prompt=login: always the sign-in form, so a browser already signed in to one
        // account can add another instead of quietly handing back the same one.
        return Util::Utf8(OpenAI::Issuer) + "/oauth/authorize?response_type=code&client_id=" + OpenAI::ClientId +
               "&redirect_uri=" + Net::UrlEncode(redirect) +
               "&scope=" + Net::UrlEncode("openid profile email offline_access") +
               "&code_challenge=" + challenge + "&code_challenge_method=S256" +
               "&id_token_add_organizations=true&codex_cli_simplified_flow=true" +
               "&state=" + state + "&originator=codex_cli_rs&prompt=login";
    }

    // The demo signs in without OpenAI or the ports a real `codex login` may be using: the
    // checks write an email to test-login.txt, and a made-up sign-in for it comes back.
    static void DemoServe(std::function<void(Result)> done) {
        Result result;
        auto deadline = GetTickCount64() + 15 * 60 * 1000;
        while (!cancelled && GetTickCount64() < deadline) {
            auto email = Test::Take(L"test-login.txt");
            if (email.empty()) { Sleep(150); continue; }
            auto claims = json::Value::MakeObject();
            claims[L"email"] = email; claims[L"sub"] = L"auth0|demo-" + email;
            claims[L"exp"] = (double)(Core::NowMs() / 1000 + 864000);
            auto& a = claims[L"https://api.openai.com/auth"]; a = json::Value::MakeObject();
            a[L"chatgpt_account_id"] = L"ws-" + email; a[L"chatgpt_user_id"] = L"user-" + email; a[L"chatgpt_plan_type"] = L"plus";
            std::wstring text; json::Write(text, claims);
            auto jwt = "eyJhbGciOiJub25lIn0." + Net::Base64Url(Util::Utf8(text)) + ".demo";
            auto access = "eyJhbGciOiJub25lIn0." + Net::Base64Url(Util::Utf8(Util::Format(L"{\"exp\":%lld}", Core::NowMs() / 1000 + 864000))) + ".demo";
            result.auth = OpenAI::AuthFromTokens(Util::Wide(jwt), Util::Wide(access), L"demo-refresh-" + email);
            break;
        }
        if (result.auth.IsNull()) { if (cancelled) result.cancelled = true; else result.error = Tr(L"The sign-in took too long. Start again", L"Вход занял слишком много времени. Начните заново"); }
        active = false;
        done(result);
    }

    bool Begin(std::wstring& url, std::wstring& error, std::function<void(Result)> done) {
        if (active) { error = Tr(L"A sign-in is already waiting in the browser", L"Вход уже ожидается в браузере"); return false; }
        if (Paths::Demo) {
            url = L"https://auth.openai.com/oauth/authorize?demo=1";
            cancelled = false; active = true;
            std::thread(DemoServe, std::move(done)).detach();
            return true;
        }
        static bool wsa = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
        if (!wsa) { error = Tr(L"Windows networking isn't available", L"Сеть Windows недоступна"); return false; }
        int port = 0;
        SOCKET s = INVALID_SOCKET;
        for (int attempt = 0; attempt < 2 && s == INVALID_SOCKET; attempt++) {
            for (int p : { 1455, 1457 }) { s = Listen(p); if (s != INVALID_SOCKET) { port = p; break; } }
            if (s == INVALID_SOCKET && attempt == 0) { CancelOtherLogin(1455); Sleep(400); }
        }
        if (s == INVALID_SOCKET) {
            error = Tr(L"Ports 1455 and 1457 are busy: another Codex sign-in is waiting. Finish or close it and try again",
                       L"Порты 1455 и 1457 заняты: ждёт другой вход в Codex. Завершите или закройте его и повторите");
            return false;
        }
        auto verifier = Net::Base64Url(Net::Random(64));
        auto challenge = Net::Base64Url(Net::Sha256(verifier));
        auto state = Net::Base64Url(Net::Random(32));
        auto redirect = "http://localhost:" + std::to_string(port) + "/auth/callback";
        url = Util::Wide(AuthorizeUrl(redirect, challenge, state));
        cancelled = false; active = true;
        EnterCriticalSection(Guard()); listener = s; LeaveCriticalSection(Guard());
        std::thread(Serve, s, redirect, verifier, state, std::move(done)).detach();
        return true;
    }

    void Cancel() { cancelled = true; }
    bool Active() { return active; }
}
