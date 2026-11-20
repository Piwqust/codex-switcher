// One account up close: its limits, limit resets and subscription, and what Codex's own
// statistics say about its use (tokens by day, streaks, the longest task, skills and plugins).
#include "app.h"
#include "codex.h"
#include <cmath>

using namespace Gfx;

namespace {
    const float Pad = 24;

    std::wstring Tokens(long long n) { return Core::ShortNumber(n); }

    std::wstring Duration(long long sec) {
        if (sec < 0) return L"—";
        if (sec < 60) return Util::Format(Tr(L"%lld s", L"%lld с"), sec);
        long long m = sec / 60, h = m / 60;
        if (h == 0) return Util::Format(Tr(L"%lld min", L"%lld мин"), m);
        return m % 60 ? Util::Format(Tr(L"%lld h %lld min", L"%lld ч %lld мин"), h, m % 60) : Util::Format(Tr(L"%lld h", L"%lld ч"), h);
    }

    std::wstring Days(long long d) { return d < 0 ? L"—" : Util::Count((int)d, L"day", L"days", L"день", L"дня", L"дней"); }

    std::wstring Date(long long ms) {
        if (ms <= 0) return L"";
        static const wchar_t* en[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
        static const wchar_t* ru[] = { L"янв", L"фев", L"мар", L"апр", L"мая", L"июн", L"июл", L"авг", L"сен", L"окт", L"ноя", L"дек" };
        ULONGLONG ft = (ULONGLONG)ms * 10000 + 116444736000000000ull;
        FILETIME f{ (DWORD)ft, (DWORD)(ft >> 32) }; SYSTEMTIME u, t; FileTimeToSystemTime(&f, &u); SystemTimeToTzSpecificLocalTime(nullptr, &u, &t);
        return L10n::ru ? Util::Format(L"%d %s %d", t.wDay, ru[t.wMonth - 1], t.wYear) : Util::Format(L"%s %d, %d", en[t.wMonth - 1], t.wDay, t.wYear);
    }

    struct DetailsWin : Surface {
        std::wstring id, title;

        void Start() {
            backdrop = 2;
            Create(AppTitle(), WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, 0, 640, 760);
        }

        float Wd() { return Client().w; }

        float Section(float y, const wchar_t* t) { Text(t, { Pad + 2, y + 18, 400, 20 }, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text); return 46; }

        RectF Card(float y, float h) { RectF r{ Pad, y, Wd() - 2 * Pad, h }; Fill(r, T.card, 8); Stroke(r, T.cardStroke, 8); return r; }

        // "5h  ▓▓▓▓▓▓▓▓▓░░░░  82% left  ↻ today 10:25"
        void Limit(RectF r, float y, const wchar_t* label, double left, long long reset, bool started) {
            long long now = Core::NowMs() / 1000;
            bool out = left == 0 && reset > now;
            Text(label, { r.x + 16, y, 70, 20 }, Font(13), T.text2);
            Meter({ r.x + 90, y + 7, r.w - 90 - 250, 7 }, left);
            Text(left < 0 ? L"—" : Util::Format(Tr(L"%d%% left", L"осталось %d%%"), (int)std::lround(left)), { r.R() - 244, y - 1, 110, 22 }, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), out ? T.danger : T.text, Align::Right);
            std::wstring when;
            if (out) when = Tr(L"back ", L"снова ") + Core::ResetTime(reset);
            else if (!started) when = Tr(L"not started", L"не начато");
            else if (reset > now) when = L"↻ " + Core::ResetTime(reset);
            Text(when, { r.R() - 124, y, 112, 20 }, Font(12.5f), out ? T.danger : T.text3);
        }

        void Tile(RectF r, const std::wstring& value, const std::wstring& label) {
            Fill(r, T.layer, 6); Stroke(r, T.layerStroke, 6);
            Text(value, { r.x + 12, r.y + 10, r.w - 24, 26 }, Font(20, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), T.text);
            Text(label, { r.x + 12, r.y + 38, r.w - 24, 18 }, Font(12), T.text2);
        }

        void Chart(RectF r, const Stats& s) {
            // The last 30 days, one bar each; a day without use is a stub, so gaps still read as days.
            std::vector<std::pair<std::wstring, long long>> days(s.daily.end() - std::min<size_t>(30, s.daily.size()), s.daily.end());
            if (days.empty()) return;
            long long top = 1; for (auto& d : days) top = std::max(top, d.second);
            float plot = r.h - 22, bw = (r.w - 32) / days.size();
            for (size_t i = 0; i < days.size(); i++) {
                float h = std::max(2.f, (float)(plot * days[i].second / (double)top));
                bool today = i + 1 == days.size();
                auto c = today ? T.success : T.text3;
                if (!days[i].second) c.a *= .4f;
                Fill({ r.x + 16 + i * bw + 1, r.y + plot - h, std::max(2.f, bw - 3), h }, c, 1.5f);
            }
            // "Oct 22" / "22 окт"
            auto monthDay = [](const std::wstring& d) {
                static const wchar_t* en[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
                static const wchar_t* ru[] = { L"янв", L"фев", L"мар", L"апр", L"мая", L"июн", L"июл", L"авг", L"сен", L"окт", L"ноя", L"дек" };
                if (d.size() < 10) return d;
                int m = _wtoi(d.substr(5, 2).c_str()), day = _wtoi(d.substr(8, 2).c_str());
                if (m < 1 || m > 12) return d;
                return L10n::ru ? Util::Format(L"%d %s", day, ru[m - 1]) : Util::Format(L"%s %d", en[m - 1], day);
            };
            Text(monthDay(days.front().first), { r.x + 16, r.B() - 18, 80, 16 }, Font(11), T.text3);
            Text(monthDay(days.back().first), { r.R() - 96, r.B() - 18, 80, 16 }, Font(11), T.text3, Align::Right);
            Text(Tr(L"max ", L"макс. ") + Tokens(top), { r.R() - 156, r.y - 21, 140, 16 }, Font(11), T.text3, Align::Right);
        }

        void Build() override {
            auto it = App::Find(id);
            if (!it) { Text(Tr(L"This account is gone", L"Этого аккаунта уже нет"), { Pad, 20, 400, 30 }, Font(16), T.text2); return; }
            // The title follows the name as shown: hiding emails can change it while open.
            auto want = it->shownName + L" — " + AppTitle();
            if (want != title) { title = want; SetWindowTextW(hwnd, title.c_str()); }
            const Stats* st = App::StatsOf(id);
            bool loading = App::StatsLoading(id);
            float y = 18 - scrollY, w = Wd();
            auto pid = it->id;

            // Who it is, and the way to switch to it.
            Avatar(Pad + 26, y + 28, 52, it->letters, it->color, it->active, T.dark ? Rgba(0x202020) : Rgba(0xF3F3F3));
            // The same as a click on it in the list: open the app with it, or just switch to it.
            bool opens = App::OpenAfterSwitch() && App::Now().appInstalled, showUse = opens || !it->active;
            auto use = opens ? Util::Format(Tr(L"Open %s", L"Открыть %s"), Codex::AppName().c_str()) : std::wstring(Tr(L"Use this account", L"Использовать"));
            float tx = Pad + 66, bw = showUse ? Measure(use, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD)).width + 30 : 0.f;
            float tw = w - Pad - tx - bw - 52;
            auto plan = it->PlanLabel();
            float nw = std::min(Measure(it->shownName, Font(20, DWRITE_FONT_WEIGHT_SEMI_BOLD, true)).width + 2, tw - 70);
            Text(it->shownName, { tx, y + 2, nw, 30 }, Font(20, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), T.text);
            if (!plan.empty()) { float pw = Measure(plan, Font(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)).width + 14; Pill({ tx + nw + 8, y + 9, pw, 18 }, plan, T.accentTint, T.text2); }
            auto line = (it->active ? std::wstring(Tr(L"In use", L"Используется")) : it->Status()) + (it->shownEmail.empty() ? L"" : L"  ·  " + it->shownEmail);
            Text(line, { tx, y + 32, tw, 20 }, Font(13), it->active ? T.success : T.text2);
            if (showUse) Button(Id("use"), { w - Pad - 44 - bw, y + 12, bw, 32 }, use, true, [pid] { App::Switch(pid, false); });
            if (loading || App::FetchingUsage()) Spinner(w - Pad - 16, y + 28, 8);
            else IconButton(Id("refresh"), { w - Pad - 32, y + 12, 32, 32 }, Glyphs::Refresh, [pid] { App::FetchStats(pid); App::RefreshUsage(true, pid); });
            y += 64;

            // ---------------------------------------------------------------- limits
            y += Section(y, Tr(L"Limits", L"Лимиты"));
            {
                auto& u = it->usage;
                long long now = Core::NowMs() / 1000;
                std::vector<std::wstring> notes;
                if (u.credits || u.unlimited) notes.push_back(u.unlimited ? std::wstring(Tr(L"Credits: unlimited", L"Кредиты: без ограничений")) : Tr(L"Credits: ", L"Кредиты: ") + (u.balance.empty() ? L"—" : u.balance));
                int resets = st && st->resets ? st->resets : u.resetCredits;
                long long resetNext = st && st->resetNext ? st->resetNext : u.resetExpires;
                auto sub = st && st->subscriptionUntil ? st->subscriptionUntil : it->subscriptionUntil;
                int lines = (u.five >= 0) + (u.week >= 0);
                float h = 20 + lines * 30 + (resets ? 26.f : 0) + (sub ? 26.f : 0) + (notes.empty() ? 0 : 26.f) + 8;
                if (!u.known) h = 64;
                RectF r = Card(y, h);
                float ly = y + 16;
                if (!u.known) Text(u.problem.empty() ? (it->fetching ? std::wstring(Tr(L"Getting limits…", L"Получаю лимиты…")) : std::wstring(Tr(L"No limits yet", L"Лимитов пока нет"))) : u.problem, { r.x + 16, r.y, r.w - 32, r.h }, Font(13), u.signedOut ? T.danger : T.text3);
                else {
                    bool started = u.fiveReset > now && (u.FiveNow() > 0 || u.fiveReset - now < 5 * 3600 - 180);
                    if (u.five >= 0) { Limit(r, ly, Tr(L"5 hours", L"5 часов"), u.FiveLeft(), u.fiveReset, started || u.FiveLeft() <= 0); ly += 30; }
                    if (u.week >= 0) { Limit(r, ly, Tr(L"Week", L"Неделя"), u.WeekLeft(), u.weekReset, true); ly += 30; }
                    ly += 4;
                    if (resets) {
                        long long left = resetNext ? (resetNext - now) / 86400 : -1;
                        auto c = left >= 0 && left <= 3 ? T.danger : left >= 0 && left <= 10 ? (T.dark ? Rgba(0xE8A33D) : Rgba(0xB86E00)) : T.text2;
                        auto t = Util::Format(Tr(L"Limit resets available: %d", L"Доступно сбросов лимита: %d"), resets);
                        if (resetNext) t += Tr(L" · the first expires ", L" · первый сгорает ") + Date(resetNext * 1000);
                        Text(t, { r.x + 16, ly, r.w - 32, 20 }, Font(13), c); ly += 26;
                    }
                    if (sub) {
                        bool ended = sub < Core::NowMs();
                        auto t = ended ? Tr(L"The subscription ended ", L"Подписка закончилась ") + Date(sub) : Util::Format(Tr(L"%s until ", L"%s до "), plan.c_str()) + Date(sub);
                        Text(t, { r.x + 16, ly, r.w - 32, 20 }, Font(13), ended ? T.danger : T.text2); ly += 26;
                    }
                    for (auto& n : notes) { Text(n, { r.x + 16, ly, r.w - 32, 20 }, Font(13), T.text2); ly += 26; }
                }
                y += h + 4;
            }

            // ---------------------------------------------------------------- statistics
            y += Section(y, Tr(L"Codex usage", L"Использование Codex"));
            if (it->apiKey) { Text(Tr(L"API keys have no Codex statistics", L"У API-ключей нет статистики Codex"), { Pad + 2, y, 400, 20 }, Font(13), T.text3); y += 30; }
            else if (!st || !st->known) {
                RectF r = Card(y, 64);
                auto msg = loading ? std::wstring(Tr(L"Getting statistics…", L"Получаю статистику…")) : st && !st->problem.empty() ? st->problem : std::wstring(Tr(L"No statistics yet", L"Статистики пока нет"));
                if (loading) Spinner(r.x + 28, r.y + 32, 8);
                Text(msg, { r.x + (loading ? 48.f : 16.f), r.y, r.w - 64, r.h }, Font(13), T.text3);
                y += 68;
            } else {
                float gw = w - 2 * Pad, tw4 = (gw - 3 * 8) / 4, th = 64;
                std::pair<std::wstring, std::wstring> tiles[] = {
                    { Tokens(st->Since(1)), Tr(L"Tokens today", L"Токенов сегодня") },
                    { Tokens(st->Since(7)), Tr(L"Last 7 days", L"За 7 дней") },
                    { Tokens(st->Since(30)), Tr(L"Last 30 days", L"За 30 дней") },
                    { Tokens(st->lifetime), Tr(L"All time", L"За всё время") },
                    { Tokens(st->peakDaily), Tr(L"Best day", L"Лучший день") },
                    { Days(st->streak), Tr(L"Current streak", L"Серия сейчас") },
                    { Days(st->longestStreak), Tr(L"Longest streak", L"Рекорд серии") },
                    { Duration(st->longestTask), Tr(L"Longest task", L"Самая долгая задача") },
                };
                for (int i = 0; i < 8; i++) Tile({ Pad + (i % 4) * (tw4 + 8), y + (i / 4) * (th + 8), tw4, th }, tiles[i].first, tiles[i].second);
                y += 2 * (th + 8) + 4;
                if (!st->daily.empty()) {
                    RectF r = Card(y, 150);
                    Text(Tr(L"Tokens by day", L"Токены по дням"), { r.x + 16, r.y + 12, 300, 18 }, Font(12.5f), T.text2);
                    Chart({ r.x, r.y + 34, r.w, r.h - 42 }, *st);
                    y += 154;
                }
                std::vector<std::wstring> facts;
                if (st->threads >= 0) facts.push_back(Util::Count((int)st->threads, L"conversation", L"conversations", L"разговор", L"разговора", L"разговоров"));
                if (st->skills >= 0) facts.push_back(Util::Format(Tr(L"%lld skills used %lld times", L"навыков: %lld, использований: %lld"), st->skills, std::max(0ll, st->skillsTotal)));
                if (st->fastPercent >= 0) facts.push_back(Util::Format(Tr(L"fast mode %d%%", L"быстрый режим %d%%"), (int)std::lround(st->fastPercent)));
                if (!st->effort.empty()) {
                    // Codex's names for reasoning effort, in the interface's language.
                    std::wstring e = st->effort;
                    if (L10n::ru) {
                        static const std::pair<const wchar_t*, const wchar_t*> names[] = { { L"minimal", L"минимальный" }, { L"low", L"низкий" }, { L"medium", L"средний" }, { L"high", L"высокий" }, { L"xhigh", L"очень высокий" } };
                        for (auto& n : names) if (e == n.first) e = n.second;
                    }
                    facts.push_back(Util::Format(Tr(L"mostly %s reasoning (%d%%)", L"уровень рассуждений чаще всего %s (%d%%)"), e.c_str(), (int)std::lround(std::max(0.0, st->effortPercent))));
                }
                if (!facts.empty()) {
                    std::wstring t; for (auto& f : facts) t += (t.empty() ? L"" : L"  ·  ") + f;
                    float h = Measure(t, Font(13), w - 2 * Pad - 32, true).height + 24;
                    RectF r = Card(y, h);
                    Text(t, { r.x + 16, r.y + 12, r.w - 32, h - 24 }, Font(13), T.text2, Align::Left, false, true);
                    y += h + 4;
                }
                if (!st->top.empty()) {
                    size_t n = std::min<size_t>(6, st->top.size());
                    RectF r = Card(y, 40 + n * 28.f);
                    Text(Tr(L"Skills and plugins used most", L"Чаще всего навыки и плагины"), { r.x + 16, r.y + 12, 400, 18 }, Font(12.5f), T.text2);
                    for (size_t i = 0; i < n; i++) {
                        auto& t = st->top[i];
                        float ry = r.y + 36 + i * 28;
                        Text(t.name, { r.x + 16, ry, r.w - 200, 22 }, Font(13.5f), T.text);
                        auto kind = t.kind == L"plugin" ? std::wstring(Tr(L"plugin", L"плагин")) : t.kind == L"skill" ? std::wstring(Tr(L"skill", L"навык")) : t.kind;
                        float kw = Measure(kind, Font(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)).width + 14;
                        Pill({ r.R() - 90 - kw, ry + 2, kw, 18 }, kind, T.badge, T.text2);
                        Text(std::to_wstring(t.count), { r.R() - 80, ry, 64, 22 }, Font(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text, Align::Right);
                    }
                    y += 40 + n * 28.f + 4;
                }
                auto asOf = st->asOf ? st->asOf : st->at;
                Text(Tr(L"Figures from OpenAI, as of ", L"Данные OpenAI на ") + Core::Age(asOf) + (st->problem.empty() ? L"" : L"  ·  " + st->problem), { Pad + 2, y + 6, w - 2 * Pad, 18 }, Font(11.5f), T.text3);
                y += 30;
            }
            y += 16;
            float total = y + scrollY;
            scrollMax = std::max(0.f, total - Client().h);
            if (scrollY > scrollMax) { scrollY = scrollMax; Invalidate(); }
            if (scrollMax > 0) {
                float ch = Client().h, sh = std::max(40.f, ch * ch / total), sy = (ch - sh) * scrollY / scrollMax;
                Fill({ Client().w - 6, sy + 2, 3, sh - 4 }, T.text3, 1.5f);
            }
        }

        bool OnKey(UINT vk, bool) override {
            if (vk == VK_F5) { App::FetchStats(id); App::RefreshUsage(true, id); return true; }
            return false;
        }
    };

    DetailsWin* win = nullptr;
}

namespace Details {
    void Show(const std::wstring& id) {
        bool fresh = !win;
        if (fresh) win = new DetailsWin(), win->Start();
        win->id = id; win->scrollY = 0;
        if (auto it = App::Find(id)) SetWindowTextW(win->hwnd, (it->shownName + L" — " + AppTitle()).c_str());
        if (fresh || !win->Visible()) {
            POINT pt; GetCursorPos(&pt);
            MONITORINFO mi{ sizeof mi }; GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
            RECT wr; GetWindowRect(win->hwnd, &wr);
            int ww = wr.right - wr.left, wh = std::min<int>(wr.bottom - wr.top, mi.rcWork.bottom - mi.rcWork.top - 40);
            SetWindowPos(win->hwnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2,
                         mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (IsIconic(win->hwnd)) ShowWindow(win->hwnd, SW_RESTORE);
        win->Show(true);
    }
    bool Visible() { return win && win->Visible(); }
    void Restyle() { if (win) win->Restyle(); }
    void Invalidate() { if (win) win->Invalidate(); }
    void Close() { if (win) win->Hide(); }
}
