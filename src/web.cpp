#define NOMINMAX
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "web.h"
#include "net.h"
// From app.h, which this file can't include (winsock2 must come before windows.h here).
namespace App {
    std::string StateJson();                                          // safe from any thread; "" = no answer
    int RunRemote(const std::wstring& what, const std::wstring& id);   // the same; 1 done, 0 no such account, -1 no answer
}
#include <thread>
#include <atomic>
#include <vector>

namespace Web {
    namespace {
        std::atomic<bool> running{ false }, stopping{ false };
        std::atomic<int> handling{ 0 };          // connections being answered, each on a thread of its own
        const int MaxConnections = 8;
        std::wstring problem;
        int boundPort = 0;
        std::thread worker;

        // The page: the same list as the window, in Codex's black and white. It reads
        // /api/state every few seconds and posts the buttons' actions.
        const char* Page = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Codex accounts</title>
<style>
:root{color-scheme:light dark;--bg:#f3f3f3;--card:#fff;--line:#e5e5e5;--text:#0d0d0d;--muted:#6b6b6b;--faint:#9a9a9a;--btn:#0d0d0d;--on:#fff;--ok:#0f7b0f;--warn:#b86e00;--bad:#c42b1c;--track:#ececec}
@media(prefers-color-scheme:dark){:root{--bg:#202020;--card:#2b2b2b;--line:#3a3a3a;--text:#fff;--muted:#c8c8c8;--faint:#8f8f8f;--btn:#ececec;--on:#0d0d0d;--ok:#6ccb5f;--warn:#e8a33d;--bad:#ff99a4;--track:#3a3a3a}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 "Segoe UI Variable Text","Segoe UI",system-ui,sans-serif}
main{max-width:760px;margin:0 auto;padding:24px 16px 40px}
header{display:flex;align-items:center;gap:12px;margin-bottom:14px}h1{font:600 22px "Segoe UI Variable Display","Segoe UI",sans-serif;margin:0;flex:1}
.state{color:var(--muted);font-size:13px;margin:-8px 0 14px}
button{font:inherit;border-radius:6px;border:1px solid var(--line);background:var(--card);color:var(--text);padding:6px 12px;cursor:pointer}
button.primary{background:var(--btn);color:var(--on);border-color:var(--btn);font-weight:600}button:disabled{opacity:.5;cursor:default}
.row{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px;margin-bottom:8px;display:grid;grid-template-columns:1fr auto;gap:10px 16px;align-items:center}
.row.active{border-color:var(--ok);box-shadow:inset 3px 0 0 var(--ok)}
.name{font-weight:600;font-size:15px}.plan{font-size:11px;font-weight:600;padding:1px 8px;border-radius:9px;background:var(--track);color:var(--muted);margin-left:6px;vertical-align:1px}
.sub{color:var(--muted);font-size:13px}.sub b{color:var(--ok);font-weight:600}
.limits{display:grid;grid-template-columns:44px 110px 40px 118px;gap:4px 10px;align-items:center;font-size:12px}
.lbl{color:var(--faint)}.bar{height:6px;border-radius:3px;background:var(--track);overflow:hidden}.bar i{display:block;height:100%;border-radius:3px}
.pct{font:600 13px "Cascadia Mono",Consolas,monospace;text-align:right}.when{color:var(--faint)}
.actions{grid-column:1/-1;display:flex;gap:8px;flex-wrap:wrap}
.err{color:var(--bad)}footer{color:var(--faint);font-size:12px;margin-top:16px}
@media(max-width:560px){.row{grid-template-columns:1fr}.limits{grid-template-columns:44px 1fr 40px auto}}
</style></head><body><main>
<header><h1 id="title">Codex accounts</h1><button id="refresh">⟳</button></header>
<div class="state" id="state"></div><div id="list"></div><footer id="foot"></footer>
</main><script>
const key=new URLSearchParams(location.search).get('key')||'';
const T={en:{title:'Codex accounts',inuse:'In use',use:'Use',warm:'Start 5h window',open:'Open %a',h5:'5h',wk:'Week',ns:'not started',back:'back ',most:'Most limit left',appOpen:'%a is open: switching restarts it',appClosed:'%a is closed',confirm:'Switch Codex to %s?',updated:'Updated ',none:'No accounts yet. Add them in the switcher on the PC.',fail:'The switcher on the PC did not answer'},
ru:{title:'Аккаунты Codex',inuse:'Используется',use:'Использовать',warm:'Запустить окно 5 ч',open:'Открыть %a',h5:'5 ч',wk:'Неделя',ns:'не начато',back:'снова ',most:'Больше всего лимита',appOpen:'%a открыт: переключение его перезапустит',appClosed:'%a закрыт',confirm:'Переключить Codex на %s?',updated:'Обновлено ',none:'Аккаунтов пока нет. Добавьте их в переключателе на компьютере.',fail:'Переключатель на компьютере не ответил'}};
let t=T.en;
const el=(tag,cls,text)=>{const e=document.createElement(tag);if(cls)e.className=cls;if(text!=null)e.textContent=text;return e};
const time=s=>{if(!s)return'';const d=new Date(s*1000),n=new Date(),hm=d.toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'});
 if(d-n<20*3600e3&&d.getDate()==n.getDate())return hm;if(d-n<6*864e5)return d.toLocaleDateString([], {weekday:'short'})+' '+hm;return d.toLocaleDateString([], {day:'numeric',month:'short'})};
const color=l=>l>=40?'var(--ok)':l>=15?'var(--warn)':'var(--bad)';
async function call(path){const r=await fetch(path+(path.includes('?')?'&':'?')+'key='+encodeURIComponent(key),{method:'POST'});if(!r.ok)throw 0;return r.json()}
function limit(box,label,left,reset,started){
 box.append(el('span','lbl',label));const bar=el('div','bar'),fill=el('i');if(left>=0){fill.style.width=left+'%';fill.style.background=color(left)}bar.append(fill);box.append(bar);
 const p=el('span','pct',left<0?'—':Math.round(left)+'%');if(left===0)p.style.color='var(--bad)';box.append(p);
 const now=Date.now()/1000;let w='';if(left===0&&reset>now)w=t.back+time(reset);else if(!started)w=t.ns;else if(reset>now)w='↻ '+time(reset);
 const ws=el('span','when',w);if(left===0)ws.style.color='var(--bad)';box.append(ws)}
function render(s){
 t=s.lang=='ru'?T.ru:T.en;document.title=t.title;document.getElementById('title').textContent=t.title;
 const app=s.codex.appName||'ChatGPT';document.getElementById('state').textContent=s.codex.installed?(s.codex.appRunning?t.appOpen:t.appClosed).replace('%a',()=>app):'';
 const list=document.getElementById('list');list.textContent='';
 if(!s.accounts.length)list.append(el('p','sub',t.none));
 for(const a of s.accounts){
  const row=el('div','row'+(a.active?' active':''));const left=el('div');
  const n=el('div','name',a.name);if(a.plan)n.append(el('span','plan',a.plan));left.append(n);
  const sub=el('div','sub');if(a.active){sub.append(el('b',null,t.inuse))}else if(a.best){const b=el('b',null,t.most);sub.append(b)}else sub.append(a.status);
  if(a.email)sub.append(' · '+a.email);left.append(sub);if(a.problem)left.append(el('div','sub err',a.problem));row.append(left);
  const lim=el('div','limits');if(a.limits)for(const l of a.limits)limit(lim,l.week?t.wk:t.h5,l.left,l.reset,l.started);row.append(lim);
  const act=el('div','actions');
  if(!a.active){const b=el('button','primary',t.use);b.disabled=a.busy;b.onclick=async()=>{if(!confirm(t.confirm.replace('%s',()=>a.name)))return;b.disabled=true;try{await call('/api/switch?id='+a.id)}catch(e){alert(t.fail)}setTimeout(load,1500)};act.append(b)}
  else if(s.codex.installed){const b=el('button',null,t.open.replace('%a',()=>app));b.onclick=()=>call('/api/open').catch(()=>alert(t.fail));act.append(b)}
  if(!a.apiKey){const w=el('button',null,t.warm);w.disabled=a.busy;w.onclick=async()=>{w.disabled=true;try{await call('/api/warmup?id='+a.id)}catch(e){alert(t.fail)}setTimeout(load,3000)};act.append(w)}
  row.append(act);list.append(row)}
 document.getElementById('foot').textContent=s.version?('Codex Account Switcher '+s.version):''}
async function load(){try{const r=await fetch('/api/state?key='+encodeURIComponent(key));if(!r.ok)throw 0;render(await r.json())}catch(e){document.getElementById('state').textContent=t.fail}}
document.getElementById('refresh').onclick=async()=>{try{await call('/api/refresh')}catch(e){}setTimeout(load,2500)};
load();setInterval(load,5000);
</script></body></html>)HTML";

        bool SameKey(const std::string& a, const std::wstring& b) {
            auto bb = Util::Utf8(b);
            if (a.size() != bb.size() || bb.empty()) return false;
            unsigned char diff = 0;
            for (size_t i = 0; i < a.size(); i++) diff |= (unsigned char)(a[i] ^ bb[i]);
            return diff == 0;
        }

        void Respond(SOCKET c, int status, const char* type, const std::string& body) {
            const char* text = status == 200 ? "OK" : status == 401 ? "Unauthorized" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed"
                             : status == 503 ? "Service Unavailable" : "Bad Request";
            std::string head = "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\nContent-Type: " + type +
                "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer"
                "\r\nContent-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'"
                "\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
            auto all = head + body;
            send(c, all.data(), (int)all.size(), 0);
            shutdown(c, SD_SEND);
            closesocket(c);
        }

        void Handle(SOCKET c, const std::wstring& key) {
            DWORD t = 300, ts = 2000;
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ts, sizeof ts);
            std::string req; char buf[4096];
            auto until = GetTickCount64() + 1500;
            while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384 && !stopping && GetTickCount64() < until) {
                int n = recv(c, buf, sizeof buf, 0);
                if (n == 0 || (n < 0 && WSAGetLastError() != WSAETIMEDOUT)) break;
                if (n > 0) req.append(buf, n);
            }
            auto sp1 = req.find(' '), sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
            if (sp2 == std::string::npos) { Respond(c, 400, "text/plain", ""); return; }
            auto method = req.substr(0, sp1), target = req.substr(sp1 + 1, sp2 - sp1 - 1);
            auto q = target.find('?');
            auto path = target.substr(0, q), query = q == std::string::npos ? "" : target.substr(q + 1);
            if (!SameKey(Net::QueryParam(query, "key"), key)) {
                Respond(c, 401, "text/plain; charset=utf-8", "Open the link from the switcher's Settings, it carries the key.");
                return;
            }
            if (method == "GET" && (path == "/" || path == "/index.html")) { Respond(c, 200, "text/html; charset=utf-8", Page); return; }
            if (method == "GET" && path == "/api/state") {
                auto state = App::StateJson();
                if (state.empty()) Respond(c, 503, "text/plain", "");
                else Respond(c, 200, "application/json; charset=utf-8", state);
                return;
            }
            if (method != "POST") { Respond(c, path.rfind("/api/", 0) == 0 ? 405 : 404, "text/plain", ""); return; }
            auto id = Util::Wide(Net::QueryParam(query, "id"));
            int done = 1;
            if (path == "/api/switch") done = App::RunRemote(L"switch", id);
            else if (path == "/api/refresh") done = App::RunRemote(L"refresh", L"");
            else if (path == "/api/warmup") done = App::RunRemote(L"warmup", id);
            else if (path == "/api/open") done = App::RunRemote(L"open", L"");
            else { Respond(c, 404, "text/plain", ""); return; }
            Respond(c, done > 0 ? 200 : done == 0 ? 400 : 503, "application/json", done > 0 ? "{\"ok\":true}" : "{\"ok\":false}");
        }

        // Each connection on a thread of its own, so a browser's idle spare connection (or a
        // slow device on the network) doesn't hold up the next request.
        void Serve(SOCKET s, std::wstring key) {
            while (!stopping) {
                fd_set set; FD_ZERO(&set); FD_SET(s, &set);
                timeval tv{ 0, 300000 };
                if (select(0, &set, nullptr, nullptr, &tv) <= 0) continue;
                SOCKET c = accept(s, nullptr, nullptr);
                if (c == INVALID_SOCKET) continue;
                if (handling >= MaxConnections) { closesocket(c); continue; }
                handling++;
                try { std::thread([c, key] { Handle(c, key); handling--; }).detach(); }
                catch (...) { handling--; closesocket(c); }
            }
            closesocket(s);
            running = false;
        }
    }

    HANDLE StopSignal() {
        static HANDLE evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return evt;
    }

    int Port() { int p = Settings::Int(L"WebPort", 3210); return p > 0 && p < 65536 ? p : 3210; }
    bool Running() { return running; }
    std::wstring Problem() { return problem; }

    std::wstring NewKey() { auto k = Util::Wide(Net::Base64Url(Net::Random(18))); Settings::Set(L"WebKey", k); return k; }

    static std::wstring Key() { auto k = Settings::Str(L"WebKey"); return k.size() >= 16 ? k : NewKey(); }

    // This PC's address on the network, for the link other devices use: the adapter with a
    // gateway, which is the one a phone shares. Virtual switches (Hyper-V, WSL) have none.
    static std::wstring LanAddress() {
        ULONG size = 16 * 1024;
        std::vector<BYTE> buf;
        IP_ADAPTER_ADDRESSES* list = nullptr;
        for (int i = 0; i < 3 && !list; i++) {
            buf.assign(size, 0);
            auto r = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                          nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
            if (r == NO_ERROR) list = (IP_ADAPTER_ADDRESSES*)buf.data();
            else if (r != ERROR_BUFFER_OVERFLOW) return L"";
        }
        std::wstring withGateway, any;
        for (auto a = list; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto u = a->FirstUnicastAddress; u; u = u->Next) {
                auto sin = (sockaddr_in*)u->Address.lpSockaddr;
                if (!sin || sin->sin_family != AF_INET) continue;
                char ip[64] = {};
                inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
                if (!strncmp(ip, "127.", 4) || !strncmp(ip, "169.254.", 8)) continue;
                if (a->FirstGatewayAddress && withGateway.empty()) withGateway = Util::Wide(ip);
                if (any.empty()) any = Util::Wide(ip);
            }
        }
        return withGateway.empty() ? any : withGateway;
    }

    std::wstring Url(bool local) {
        std::wstring host = local ? L"localhost" : LanAddress();
        if (host.empty()) host = L"localhost";
        return Util::Format(L"http://%s:%d/?key=%s", host.c_str(), boundPort ? boundPort : Port(), Key().c_str());
    }

    void Stop() {
        if (!worker.joinable()) return;
        stopping = true;
        SetEvent(StopSignal());
        worker.join();
        // Connections end within a few seconds; none of them waits for this (UI) thread now.
        for (int i = 0; i < 80 && handling > 0; i++) Sleep(50);
        ResetEvent(StopSignal());
        stopping = false;
        boundPort = 0;
    }

    void Apply() {
        Stop();
        problem.clear();
        if (!Settings::Flag(L"WebOn", false)) return;
        static bool wsa = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
        if (!wsa) { problem = Tr(L"Windows networking isn't available", L"Сеть Windows недоступна"); return; }
        bool lan = Settings::Flag(L"WebLan", false);
        int port = Port();
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOL exclusive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof exclusive);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
        inet_pton(AF_INET, lan ? "0.0.0.0" : "127.0.0.1", &a.sin_addr);
        if (s == INVALID_SOCKET || bind(s, (sockaddr*)&a, sizeof a) != 0 || listen(s, 16) != 0) {
            if (s != INVALID_SOCKET) closesocket(s);
            problem = Util::Format(Tr(L"Port %d is taken by another program. Choose another one", L"Порт %d занят другой программой. Выберите другой"), port);
            return;
        }
        boundPort = port;
        running = true;
        worker = std::thread(Serve, s, Key());
    }
}
