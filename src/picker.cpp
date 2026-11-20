#include "app.h"
#include "codex.h"
#include <cmath>
#include <shellapi.h>

using namespace Gfx;

namespace {
    // A list rather than a grid of cards: what matters with Codex is how much each account has
    // left and when it comes back, which needs width, and names that are emails need it too.
    const float W = 720, Pad = 20, RowH = 76, RowGap = 8, AddH = 52, MaxRows = 7;
    const float LimitsW = 262, MenuW = 40;   // the limits column and the ⋯ at the right of a row

    enum class Banner { None, SigningIn, Recovery, Added, Problem, Unknown, Legacy };

    // One line of a row's limits: "5h  ▓▓▓▓▓░░  82%  ↻ 10:25".
    struct LimitLine { const wchar_t* label; double left; long long reset; bool started; };

    struct PickerWin : Surface {
        std::wstring added; bool addedExisted = false;
        bool legacyDismissed = false;
        std::wstring renaming;     // id whose name is being edited in place
        ULONGLONG copiedAt = 0;
        float listTop = 0, listH = 0;   // the visible part of the list, for clipping and scrolling

        void Start() {
            sizeToContent = true;
            backdrop = 2;
            Create(AppTitle(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 0, W, 420);
        }

        Banner Current() {
            auto& s = App::Now();
            if (App::SigningIn()) return Banner::SigningIn;
            if (App::Recovering().running || App::Recovering().shown) return Banner::Recovery;
            if (!added.empty() && App::Find(added)) return Banner::Added;
            if (s.keyring || !s.vaultProblem.empty()) return Banner::Problem;
            if (!s.unknownLogin.empty()) return Banner::Unknown;
            if (s.legacy > 0 && !legacyDismissed && !Settings::Flag(L"LegacyDismissed", false)) return Banner::Legacy;
            return Banner::None;
        }

        // ------------------------------------------------------------ geometry

        Item* ItemByRow(UINT id) { for (auto& i : App::items) if (Id("row", i.index) == id) return &i; return nullptr; }

        // The menu runs its own loop; whatever the pointer did meanwhile went unseen, so hover
        // is re-read from where the pointer is now.
        void Menu(const std::wstring& id, POINT at) { App::ShowMenu(id, at, hwnd); RefreshHover(); }

        RectF RowRect(int index) { return { Pad, listTop + (index - 1) * (RowH + RowGap) - scrollY, W - 2 * Pad, RowH }; }
        static float TextX(RectF r) { return r.x + 98; }
        static RectF NameField(RectF r) { return { TextX(r) - 5, r.y + 13, 220, 26 }; }

        POINT Screen(float x, float y) { POINT p{ (LONG)(x * scale), (LONG)(y * scale) }; ClientToScreen(hwnd, &p); return p; }

        // ------------------------------------------------------------ the top

        // "ChatGPT is open: choosing an account restarts it with that one · 2 sessions in terminals"
        std::wstring CodexState() {
            auto& s = App::Now();
            auto app = Codex::AppName();
            bool opens = App::OpenAfterSwitch() && s.appInstalled;
            std::wstring t;
            if (s.appRunning) t = App::RestartOnSwitch() ? Util::Format(Tr(L"%s is open: choosing an account restarts it with that one", L"%s открыт: выбор аккаунта перезапустит его с ним"), app.c_str())
                                                         : Util::Format(Tr(L"%s is open and keeps its account until restarted", L"%s открыт и останется на своём аккаунте до перезапуска"), app.c_str());
            else if (opens) t = Util::Format(Tr(L"Choose an account to open %s with it", L"Выберите аккаунт, чтобы открыть %s с ним"), app.c_str());
            else if (s.appInstalled) t = Util::Format(Tr(L"%s is closed", L"%s закрыт"), app.c_str());
            else t = Tr(L"Switches the Codex CLI and editors", L"Переключает Codex CLI и редакторы");
            if (s.terminals) t += L" · " + Util::Count(s.terminals, L"terminal session", L"terminal sessions", L"сеанс в терминале", L"сеанса в терминале", L"сеансов в терминале");
            return t;
        }

        struct Action { std::wstring label; std::function<void()> click; bool primary; };

        void BannerUi(float y, Banner b) {
            RectF r{ Pad, y, W - 2 * Pad, 64 };
            bool warn = b == Banner::Problem || (b == Banner::Recovery && !App::Recovering().running && !App::Recovering().report.failed.empty());
            Fill(r, warn ? Mix(T.layer, Rgba(T.dark ? 0xFCE100 : 0x9D5D00, 1), .12f) : T.accentTint, 8);
            Stroke(r, T.layerStroke, 8);
            std::wstring title, text;
            std::vector<Action> actions;
            const wchar_t* glyph = Glyphs::Contact;
            auto& s = App::Now();
            switch (b) {
            case Banner::SigningIn:
                title = Tr(L"Sign in in the browser", L"Войдите в браузере");
                text = GetTickCount64() - copiedAt < 2500 ? std::wstring(Tr(L"Link copied: paste it into a private window to use another browser session", L"Ссылка скопирована: вставьте её в приватное окно, чтобы войти другим сеансом"))
                                                         : std::wstring(Tr(L"A ChatGPT sign-in page opened. Use the account you want to add", L"Открылась страница входа ChatGPT. Войдите в аккаунт, который нужно добавить"));
                actions.push_back({ Tr(L"Copy link", L"Ссылка"), [this] { Util::CopyText(hwnd, App::SignInLink()); copiedAt = GetTickCount64(); Invalidate(); }, false });
                actions.push_back({ Tr(L"Cancel", L"Отмена"), [] { App::CancelSignIn(); }, false });
                Spinner(r.x + 24, r.y + 24, 9);
                glyph = nullptr;
                break;
            case Banner::Recovery: {
                auto& rc = App::Recovering();
                if (rc.running) {
                    title = Tr(L"Continuing conversations the limit stopped", L"Продолжаю разговоры, остановленные лимитом");
                    text = rc.total ? Util::Format(Tr(L"%d of %d. %s opens each one so its message goes out", L"%d из %d. %s открывает каждый, чтобы сообщение ушло"), std::min(rc.done + 1, rc.total), rc.total, Codex::AppName().c_str())
                                    : std::wstring(Tr(L"Looking through the last 24 hours…", L"Просматриваю последние сутки…"));
                    Spinner(r.x + 24, r.y + 24, 9);
                    glyph = nullptr;
                } else {
                    auto& rep = rc.report;
                    title = Util::Format(Tr(L"Continued %d of %d conversations", L"Продолжено разговоров: %d из %d"), rep.resumed, rep.total - rep.skipped);
                    text = rep.failed.empty() ? std::wstring(Tr(L"They carry on with the new account", L"Они продолжаются с новым аккаунтом")) : rep.failed[0].reason;
                    glyph = rep.failed.empty() ? Glyphs::Check : Glyphs::Warning;
                    if (!rep.failed.empty()) { auto first = rep.failed[0].id; actions.push_back({ Tr(L"Open", L"Открыть"), [first] { Recovery::Open(first); }, true }); }
                    actions.push_back({ Tr(L"Done", L"Готово"), [] { App::DismissRecovery(); }, false });
                }
                break;
            }
            case Banner::Added: {
                auto it = App::Find(added);
                title = addedExisted ? Util::Format(Tr(L"Sign-in updated for %s", L"Вход %s обновлён"), Util::Q(it ? it->shownName : added).c_str())
                                     : Util::Format(Tr(L"Added %s", L"Добавлен %s"), it ? it->shownEmail.c_str() : added.c_str());
                text = addedExisted ? std::wstring(Tr(L"It was already on the list, so it just got the new sign-in", L"Он уже был в списке, поэтому просто получил новый вход"))
                                    : Util::Format(Tr(L"The name %s is selected: type your own, like “Work”", L"Имя %s уже выделено: впишите своё, например «Работа»"), Util::Q(it ? it->shownName : L"").c_str());
                actions.push_back({ Tr(L"Done", L"Готово"), [this] { added.clear(); Invalidate(); }, false });
                break;
            }
            case Banner::Problem:
                glyph = Glyphs::Warning;
                title = s.keyring ? Tr(L"Codex keeps its sign-in elsewhere", L"Codex хранит вход в другом месте") : Tr(L"The saved accounts couldn't be read", L"Сохранённые аккаунты не прочитались");
                text = s.keyring ? Tr(L"config.toml says cli_auth_credentials_store = \"keyring\". Change it to \"file\" to switch accounts", L"В config.toml указано cli_auth_credentials_store = \"keyring\". Поставьте \"file\", чтобы переключать аккаунты") : s.vaultProblem;
                break;
            case Banner::Unknown:
                title = Util::Format(Tr(L"Codex uses %s", L"Codex использует %s"), s.unknownLogin.c_str());
                text = Tr(L"That account isn't on the list yet. Add it to switch back to it later", L"Этого аккаунта ещё нет в списке. Добавьте его, чтобы потом возвращаться к нему");
                actions.push_back({ Tr(L"Add it", L"Добавить"), [] { App::AddCurrentLogin(); }, true });
                break;
            case Banner::Legacy:
                glyph = Glyphs::Import;
                title = Util::Format(Tr(L"Codex Switcher has %s", L"В Codex Switcher есть %s"), Util::Count(s.legacy, L"more account", L"more accounts", L"ещё аккаунт", L"ещё аккаунта", L"ещё аккаунтов").c_str());
                text = Tr(L"Bring them over with their names, no signing in again", L"Перенесите их вместе с именами, без повторного входа");
                actions.push_back({ Tr(L"Import", L"Импорт"), [this] { App::ImportLegacy(hwnd); }, true });
                actions.push_back({ Tr(L"Hide", L"Скрыть"), [this] { legacyDismissed = true; Settings::Set(L"LegacyDismissed", true); Invalidate(); }, false });
                break;
            default: break;
            }
            if (glyph) Glyph(glyph, { r.x + 12, r.y + 12, 24, 24 }, 16, warn ? (T.dark ? Rgba(0xFCE100) : Rgba(0x9D5D00)) : T.text2);
            float bx = r.R() - 12;
            for (int i = (int)actions.size() - 1; i >= 0; i--) {
                float bw = Measure(actions[i].label, Font(14, actions[i].primary ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL)).width + 26;
                bx -= bw;
                Button(Id("banner", i), { bx, r.y + 16, bw, 32 }, actions[i].label, actions[i].primary, actions[i].click);
                bx -= 8;
            }
            float tw = bx - (r.x + 44) - 4;
            Text(title, { r.x + 44, r.y + 12, tw, 20 }, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text);
            Text(text, { r.x + 44, r.y + 33, tw, 18 }, Font(12), T.text2);
            if (b == Banner::SigningIn && GetTickCount64() - copiedAt < 2600) Animate();
        }

        // ------------------------------------------------------------ rows

        // "5h  ▓▓▓▓▓░░  82%  ↻ 10:25", the way Codex's /status shows a limit.
        void Limit(float x, float y, const LimitLine& l, bool stale) {
            long long now = Core::NowMs() / 1000;
            bool out = l.left >= 0 && l.left <= 0 && l.reset > now;
            Text(l.label, { x, y, 40, 16 }, Font(11.5f), T.text3);
            Meter({ x + 42, y + 5, 84, 6 }, l.left, stale);
            auto pc = out ? T.danger : stale ? T.text3 : T.text;
            Text(l.left < 0 ? L"—" : Util::Format(L"%d%%", (int)std::lround(l.left)), { x + 128, y - 1, 44, 18 }, Font(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), pc, Align::Right);
            std::wstring when; auto wc = T.text3;
            if (out) { when = Tr(L"back ", L"снова ") + Core::ResetTime(l.reset); wc = T.danger; }
            else if (!l.started) when = Tr(L"not started", L"не начато");
            else if (l.reset > now) when = L"↻ " + Core::ResetTime(l.reset);
            Text(when, { x + 180, y, LimitsW - 180, 16 }, Font(11.5f), wc);
        }

        void Row(Item& it, RectF r, bool best) {
            UINT id = Id("row", it.index);
            bool visible = r.B() > listTop && r.y < listTop + listH;
            if (!visible) return;
            bool over = Over(r) && my >= listTop && my < listTop + listH;
            float h = A(id ^ 0x51, over ? 1.f : 0.f), p = PressT(id);
            float act = A(id ^ 0xC4, it.active ? 1.f : 0.f, 160);
            Fill(r, Mix(Mix(T.layer, T.layerHover, h), T.layerPress, p), 8);
            auto strokeHover = T.dark ? Rgba(0xFFFFFF, .16f) : Rgba(0x000000, .14f);
            Stroke(r, Mix(Mix(T.layerStroke, strokeHover, h), T.success, act * .55f), 8, 1 + act * .5f);
            // Handlers look the account up again by id: the list may be rebuilt before a click lands.
            auto pid = it.id;
            RectF hit = r;
            if (hit.y < listTop) { hit.h -= listTop - hit.y; hit.y = listTop; }
            if (hit.B() > listTop + listH) hit.h = listTop + listH - hit.y;
            Hit(id, hit, [pid] { App::Switch(pid, true); }, [this, pid] { POINT pt; GetCursorPos(&pt); Menu(pid, pt); });
            FocusRing(id, r, 8);

            // The digit that switches to it, and its mark.
            if (it.index <= 9) {
                RectF k{ r.x + 14, r.y + r.h / 2 - 11, 22, 22 };
                Fill(k, T.badge, 5);
                Text(std::to_wstring(it.index), k, Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text2, Align::Center);
            }
            Avatar(r.x + 66, r.y + r.h / 2, 40, it.letters, it.color, it.active, T.dark ? Rgba(0x2B2B2B) : Rgba(0xFBFBFB));
            if (it.switching || it.deleting || it.warming) Spinner(r.x + 66, r.y + r.h / 2, 23);

            // Name and plan; then whether Codex uses it, and its email.
            float tx = TextX(r), limitsX = r.R() - MenuW - LimitsW, tw = limitsX - tx - 16;
            auto plan = it.PlanLabel();
            float pw = plan.empty() ? 0 : Measure(plan, Font(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)).width + 14;
            if (Editing() && renaming == it.id) {
                // The text box sits here; this is its field, drawn in the Windows 11 style.
                RectF f = NameField(r);
                Fill(f.Inset(-1), T.menu, 4);
                Stroke(f.Inset(-1), T.controlStroke, 4);
                Fill({ f.x - 1, f.B() - 1, f.w + 2, 2 }, T.text, 1);
            } else {
                float nw = std::min(Measure(it.shownName, Font(15, DWRITE_FONT_WEIGHT_SEMI_BOLD)).width + 2, tw - (pw ? pw + 8 : 0));
                Text(it.shownName, { tx, r.y + 14, nw, 22 }, Font(15, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text);
                if (pw) Pill({ tx + nw + 8, r.y + 16, pw, 18 }, plan, plan == L"Free" ? T.badge : T.accentTint, T.text2);
            }
            std::wstring status = it.Status();
            auto sc = it.switching || it.deleting || it.warming ? T.text : it.SignedOut() ? T.danger : it.active ? T.success : T.text2;
            if (best && !it.switching && !it.deleting && !it.warming) { status = Tr(L"Most limit left", L"Больше всего лимита"); sc = T.success; }
            float sw = Measure(status, Font(12.5f)).width;
            Text(status, { tx, r.y + 40, tw, 18 }, Font(12.5f, it.active || best ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL), sc);
            auto email = it.shownEmail.empty() || _wcsicmp(it.shownEmail.c_str(), it.shownName.c_str()) == 0 ? std::wstring() : it.shownEmail;
            if (!email.empty() && sw + 16 < tw) Text(L"·  " + email, { tx + sw + 7, r.y + 40, tw - sw - 7, 18 }, Font(12.5f), T.text3);

            // What is left, on the right.
            auto& u = it.usage;
            if (it.apiKey) Text(Tr(L"API key: no plan limits", L"API-ключ: без лимитов подписки"), { limitsX, r.y, LimitsW, r.h }, Font(12), T.text3);
            else if (u.known) {
                long long now = Core::NowMs() / 1000;
                std::vector<LimitLine> lines;
                // A window that hasn't started has its reset a full five hours away, moving with the clock.
                bool fiveStarted = u.fiveReset > now && (u.FiveNow() > 0 || u.fiveReset - now < 5 * 3600 - 180);
                if (u.five >= 0) lines.push_back({ Tr(L"5h", L"5 ч"), u.FiveLeft(), u.fiveReset, fiveStarted || u.FiveLeft() <= 0 });
                if (u.week >= 0) lines.push_back({ Tr(L"Week", L"Неделя"), u.WeekLeft(), u.weekReset, true });
                bool stale = u.Stale() || !u.problem.empty();
                float ly = lines.size() > 1 ? r.y + 15 : r.y + r.h / 2 - 8;
                for (auto& l : lines) { Limit(limitsX, ly, l, stale); ly += 24; }
                if (!u.problem.empty() && !u.signedOut)
                    Text(Tr(L"not updated ", L"не обновлено ") + Core::Age(u.at), { limitsX + 42, r.y + r.h - 18, LimitsW - 42, 14 }, Font(10.5f), T.text3);
            } else if (u.signedOut) {
                float bw = Measure(Tr(L"Sign in again", L"Войти заново"), Font(14)).width + 32;
                Button(Id("signin", it.index), { limitsX, r.y + r.h / 2 - 16, bw, 32 }, Tr(L"Sign in again", L"Войти заново"), false, [pid] { App::SignInAgain(pid); });
            } else {
                auto msg = it.fetching ? std::wstring(Tr(L"Getting limits…", L"Получаю лимиты…")) : u.problem;
                Text(msg, { limitsX, r.y + 8, LimitsW, r.h - 16 }, Font(12), T.text3, Align::Left, true, true);
            }

            // The menu, on hover or keyboard focus.
            float showMore = A(id ^ 0x6D, (over || (keyboardCues && focus == id)) ? 1.f : 0.f, 120);
            if (showMore > 0.01f) {
                PushOpacity(showMore);
                UINT mid = Id("more", it.index);
                RectF m{ r.R() - 38, r.y + r.h / 2 - 15, 30, 30 };
                Fill(m, Mix(D2D1::ColorF(0, 0, 0, 0), T.layerHover, HotT(mid)), 4);
                Glyph(Glyphs::More, m, 14, T.text2);
                Hit(mid, m, [this, pid, m] { Menu(pid, Screen(m.x, m.B())); }, nullptr, false);
                PopOpacity();
            }
        }

        void AddRow(RectF r) {
            UINT id = Id("addrow");
            float h = HotT(id), p = PressT(id);
            Fill(r, Mix(D2D1::ColorF(0, 0, 0, 0), T.layer, h), 8);
            auto s = T.controlStrong; s.a *= .5f;
            DashedStroke(p > 0 ? r.Inset(1) : r, s, 8);
            Glyph(Glyphs::Add, { r.x + 46, r.y, 40, r.h }, 15, T.text2);
            Text(Tr(L"Add account", L"Добавить аккаунт"), { r.x + 98, r.y, 200, r.h }, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text);
            float aw = Measure(Tr(L"Add account", L"Добавить аккаунт"), Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD)).width;
            Text(Tr(L"Sign in with ChatGPT in the browser", L"Вход через ChatGPT в браузере"), { r.x + 98 + aw + 12, r.y, 300, r.h }, Font(12.5f), T.text3);
            Text(L"Ctrl+N", { r.R() - 120, r.y, 104, r.h }, Font(12.5f), T.text3, Align::Right);
            Hit(id, r, [] { App::AddAccount(); });
            FocusRing(id, r, 8);
        }

        void Build() override {
            Text(Tr(L"Codex accounts", L"Аккаунты Codex"), { Pad, 12, W - 2 * Pad - 90, 30 }, Font(20, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), T.text);
            Text(CodexState(), { Pad, 41, W - 2 * Pad - 90, 18 }, Font(12.5f), App::Now().appRunning ? T.text2 : T.text3);
            if (App::FetchingUsage()) Spinner(W - Pad - 52, 30, 8);
            else IconButton(Id("refresh"), { W - Pad - 68, 14, 32, 32 }, Glyphs::Refresh, [] { App::RefreshUsage(true); });
            IconButton(Id("settings"), { W - Pad - 32, 14, 32, 32 }, Glyphs::Settings, [] { App::ShowSettings(); });

            float y = 72;
            auto b = Current();
            if (b != Banner::None) { BannerUi(y, b); y += 64 + 12; }

            // The list scrolls once there are more accounts than fit comfortably.
            int n = (int)App::items.size();
            float full = n * (RowH + RowGap);
            listTop = y;
            listH = std::min(full, MaxRows * (RowH + RowGap));
            scrollMax = std::max(0.f, full - listH);
            if (scrollY > scrollMax) scrollY = scrollMax;
            // Keyboard focus on a row out of view brings it into view.
            if (keyboardCues) if (auto f = ItemByRow(focus)) {
                float top = (f->index - 1) * (RowH + RowGap);
                if (top < scrollY) scrollY = top;
                if (top + RowH > scrollY + listH) scrollY = top + RowH - listH + RowGap;
            }
            auto best = App::MostLeft();
            if (n) {
                RT()->PushAxisAlignedClip(D2D1::RectF(0, listTop - 4, W, listTop + listH), D2D1_ANTIALIAS_MODE_ALIASED);
                for (auto& it : App::items) Row(it, RowRect(it.index), &it == best);
                RT()->PopAxisAlignedClip();
                if (scrollMax > 0) {
                    float th = std::max(30.f, listH * listH / full), ty = listTop + (listH - th) * scrollY / scrollMax;
                    Fill({ W - 8, ty + 2, 3, th - 4 }, T.text3, 1.5f);
                }
            }
            y = listTop + listH;
            AddRow({ Pad, y, W - 2 * Pad, AddH });
            y += AddH + 20;

            // As in Claude Profile Switcher: starting with Windows on the left, the keys on the right.
            UINT at = Id("autostart");
            auto flip = [this] { App::SetAutostart(!App::Autostart()); Invalidate(); };
            Toggle(at, Pad, y, App::Autostart(), flip);
            auto label = Tr(L"Start with Windows", L"Запускать вместе с Windows");
            float lw = Measure(label, Font(14)).width;
            Text(label, { Pad + 52, y - 4, lw + 4, 28 }, Font(14), T.text);
            Hit(at, { Pad + 48, y - 6, lw + 12, 32 }, flip, nullptr, false);
            bool opens = App::OpenAfterSwitch() && App::Now().appInstalled;
            auto hint = opens ? Tr(L"A digit opens · F2 renames · F5 refreshes", L"Цифра — открыть · F2 — имя · F5 — обновить")
                              : Tr(L"A digit switches · F2 renames · F5 refreshes", L"Цифра — переключить · F2 — имя · F5 — обновить");
            float hx = Pad + 52 + lw + 24;
            Text(hint, { hx, y - 4, W - Pad - hx, 28 }, Font(12.5f), T.text3, Align::Right);
            contentHeight = y + 20 + 20;
        }

        // ------------------------------------------------------------ input

        bool OnKey(UINT vk, bool alt) override {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (vk >= '1' && vk <= '9' && !ctrl && !alt) { int d = vk - '0'; for (auto& i : App::items) if (i.index == d) { App::Switch(i.id, true); break; } return true; }
            if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9 && !ctrl) { int d = vk - VK_NUMPAD0; for (auto& i : App::items) if (i.index == d) { App::Switch(i.id, true); break; } return true; }
            if (ctrl && vk == 'N') { App::AddAccount(); return true; }
            if (ctrl && vk == VK_OEM_COMMA) { App::ShowSettings(); return true; }
            auto focused = ItemByRow(focus);
            if (vk == VK_F5) { if (focused && keyboardCues) App::RefreshUsage(true, focused->id); else App::RefreshUsage(true); return true; }
            if (vk == VK_ESCAPE) { if (App::SigningIn()) App::CancelSignIn(); else OnClose(); return true; }
            if (!focused) return false;
            auto id = focused->id;
            if (vk == VK_F2) { App::BeginRename(id); return true; }
            if (vk == 'I' && !ctrl && !alt) { App::ShowDetails(id); return true; }
            if (vk == VK_DELETE) { App::Delete(id, hwnd); return true; }
            if (ctrl && (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT)) {
                int delta = vk == VK_UP || vk == VK_LEFT ? -1 : 1, to = focused->index + delta;
                App::Move(id, delta);
                if (to >= 1 && to <= (int)App::items.size()) focus = Id("row", to);
                return true;
            }
            if (vk == VK_APPS || (vk == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000))) {
                auto r = RowRect(focused->index);
                Menu(id, Screen(r.R() - 38, r.y + r.h / 2 + 15));
                return true;
            }
            return false;
        }

        void OnClose() override { App::ClosePicker(); }

        void FocusLikely() {
            Item* best = nullptr;
            for (auto& i : App::items) if (!best || (i.active && !best->active)) best = &i;
            if (best) focus = Id("row", best->index);
            keyboardCues = false;
        }

        void BeginRename(Item& it) {
            auto id = it.id;
            renaming = id;
            ShowEdit(NameField(RowRect(it.index)), it.name, [this, id](bool commit, const std::wstring& text) {
                renaming.clear();
                if (commit) App::CommitRename(id, text);
            });
            Invalidate();
        }
    };

    PickerWin* win = nullptr;
    PickerWin& Get() { if (!win) { win = new PickerWin(); win->Start(); } return *win; }
}

namespace Picker {
    void Show() {
        auto& w = Get();
        if (!w.Visible()) {
            // Centred on the monitor the pointer is on, where the person is looking.
            POINT pt; GetCursorPos(&pt);
            MONITORINFO mi{ sizeof mi }; GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
            // Measured before centring, and again after the move in case that monitor has another scale.
            for (int pass = 0; pass < 2; pass++) {
                w.LayoutNow();
                RECT wr; GetWindowRect(w.hwnd, &wr);
                int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
                SetWindowPos(w.hwnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2,
                             mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2 - 40, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            w.FocusLikely();
        }
        if (IsIconic(w.hwnd)) ShowWindow(w.hwnd, SW_RESTORE);
        w.Show(true);
    }
    void Close() { if (win) { win->added.clear(); win->Hide(); } }
    bool Visible() { return win && win->Visible(); }
    bool Active() { return win && GetForegroundWindow() == win->hwnd; }
    void Added(const std::wstring& id, bool existed) { auto& w = Get(); w.added = id; w.addedExisted = existed; w.Invalidate(); }
    void Rename(const std::wstring& id) {
        auto& w = Get();
        if (auto it = App::Find(id)) {
            if (!w.Visible()) Show();
            if (!Offscreen()) SetForegroundWindow(w.hwnd);
            UpdateWindow(w.hwnd);
            w.BeginRename(*it);
        }
    }
    void Restyle() { if (win) win->Restyle(); }
    void Invalidate() { if (win) win->Invalidate(); }
    HWND Handle() { return win ? win->hwnd : nullptr; }
}
