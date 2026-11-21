#include "app.h"
#include "codex.h"
#include <cmath>
#include <shellapi.h>
#include <windowsx.h>

using namespace Gfx;

namespace {
    // A list rather than a grid of cards: what matters with Codex is how much each account has
    // left and when it comes back, which needs width, and names that are emails need it too.
    const float W = 720, Pad = 20, RowH = 76, RowGap = 8, AddH = 52, MaxRows = 7;
    const float LimitsW = 262, MenuW = 40;   // the limits column and the ⋯ at the right of a row

    enum class Banner { None, Recovery, Added, Problem, Unknown, Legacy };

    // One line of a row's limits: "5h  ▓▓▓▓▓░░  82%  ↻ 10:25".
    struct LimitLine { const wchar_t* label; double left; long long reset; bool started; };

    struct PickerWin : Surface {
        std::wstring added; bool addedExisted = false;
        bool legacyDismissed = false;
        std::wstring renaming;     // id whose name is being edited in place
        // A row being dragged to another place: which one, where it was taken and where it would land.
        int grabbed = 0, liftTo = 0; bool lifting = false; float grabY = 0, liftDy = 0; std::wstring grabId;
        float listTop = 0, listH = 0;   // the visible part of the list, for clipping and scrolling

        void Start() {
            sizeToContent = true;
            backdrop = 2;
            Create(AppTitle(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 0, W, 420);
            AcceptFiles();
        }

        Banner Current() {
            auto& s = App::Now();
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
            Text(text, { r.x + 44, r.y + 33, tw, 18 }, Font(12.5f), T.text2);
        }

        // ------------------------------------------------------------ rows

        // "5h  ▓▓▓▓▓░░  82%  ↻ 10:25", the way Codex's /status shows a limit.
        void Limit(float x, float y, const LimitLine& l, bool stale) {
            long long now = Core::NowMs() / 1000;
            bool out = l.left >= 0 && l.left <= 0 && l.reset > now;
            Text(l.label, { x, y, 40, 16 }, Font(11), T.text3);
            Meter({ x + 42, y + 5, 84, 6 }, l.left, stale);
            auto pc = out ? T.danger : stale ? T.text3 : T.text;
            Text(l.left < 0 ? L"—" : Util::Format(L"%d%%", (int)std::lround(l.left)), { x + 128, y - 1, 44, 18 }, Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), pc, Align::Right);
            std::wstring when; auto wc = T.text3;
            if (out) { when = Tr(L"back ", L"снова ") + Core::ResetTime(l.reset); wc = T.danger; }
            else if (!l.started) when = Tr(L"not started", L"не начато");
            else if (l.reset > now) when = L"↻ " + Core::ResetTime(l.reset);
            Text(when, { x + 180, y, LimitsW - 180, 16 }, Font(11), wc);
        }

        // A sign-in waiting in the browser, shown where it belongs: its window again, the link
        // for finishing it in another browser, and a way out.
        void Waiting(float x, float cy, int n) {
            auto again = Tr(L"Open again", L"Открыть снова"), cancel = Tr(L"Cancel", L"Отмена");
            float aw = Measure(again, Font(14)).width + 26, cw = Measure(cancel, Font(14)).width + 26;
            Button(Id("signin-open", n), { x, cy - 16, aw, 32 }, again, false, [] { App::ReopenSignIn(); });
            IconButton(Id("signin-link", n), { x + aw + 6, cy - 16, 32, 32 }, Glyphs::Copy,
                       Tr(L"Copy the sign-in link, to finish it in another browser", L"Скопировать ссылку входа, чтобы закончить его в другом браузере"),
                       [this] { if (Util::CopyText(hwnd, App::SignInLink())) Toast(Tr(L"Link copied: paste it into any browser", L"Ссылка скопирована: вставьте её в любой браузер")); });
            Button(Id("signin-cancel", n), { x + aw + 44, cy - 16, cw, 32 }, cancel, false, [] { App::CancelSignIn(); });
        }

        // The account being added: it has no name yet, only a sign-in waiting in the browser.
        void NewRow(RectF r) {
            Fill(r, T.layer, 8);
            Stroke(r, T.layerStroke, 8);
            float cx = r.x + 66, cy = r.y + r.h / 2;
            Circle(cx, cy, 20, T.badge);
            Glyph(Glyphs::Contact, { cx - 20, cy - 20, 40, 40 }, 16, T.text2);
            Spinner(cx, cy, 23);
            float tx = TextX(r), limitsX = r.R() - MenuW - LimitsW;
            Text(Tr(L"New account", L"Новый аккаунт"), { tx, r.y + 14, limitsX - tx - 16, 22 }, Font(16, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text);
            Text(Tr(L"Waiting for the sign-in in the browser", L"Ждёт входа в браузере"), { tx, r.y + 40, limitsX - tx - 16, 18 }, Font(12.5f), T.text2);
            Waiting(limitsX, cy, 0);
        }

        static UINT ShiftId(const Item& it) { return Id("shift", (int)std::hash<std::wstring>()(it.id)); }
        // How far a row steps aside for the one being dragged past it.
        float Shift(int index) const {
            if (!lifting) return 0;
            if (grabbed < index && index <= liftTo) return -(RowH + RowGap);
            if (liftTo <= index && index < grabbed) return RowH + RowGap;
            return 0;
        }

        void Row(Item& it, RectF r, bool best, bool lifted = false) {
            UINT id = Id("row", it.index);
            bool waiting = App::SigningInAccount() == it.id;
            bool visible = r.B() > listTop && r.y < listTop + listH;
            // Handlers look the account up again by id: the list may be rebuilt before a click lands.
            auto pid = it.id;
            // A row scrolled out of view still takes keyboard focus, which scrolls it back in.
            RectF hit = r;
            if (hit.y < listTop) { hit.h -= listTop - hit.y; hit.y = listTop; }
            if (hit.B() > listTop + listH) hit.h = listTop + listH - hit.y;
            if (!visible) hit.h = 0;
            Hit(id, hit, [pid] { App::Switch(pid, true); }, [this, pid] { POINT pt; GetCursorPos(&pt); Menu(pid, pt); });
            Bounds(r);
            Describe(it.Spoken(best), Role::Row, it.active ? 1 : 0);
            if (!visible) return;
            bool over = Over(r) && my >= listTop && my < listTop + listH;
            float h = A(id ^ 0x51, over ? 1.f : 0.f), p = PressT(id);
            float act = A(id ^ 0xC4, it.active ? 1.f : 0.f, 160);
            Fill(r, Mix(Mix(T.layer, T.layerHover, h), T.layerPress, p), 8);
            auto strokeHover = T.dark ? Rgba(0xFFFFFF, .16f) : Rgba(0x000000, .14f);
            Stroke(r, Mix(Mix(T.layerStroke, strokeHover, h), T.success, act * .55f), 8, 1 + act * .5f);
            if (lifted) { Fill(r, Veil(1), 8); Stroke(r, T.text2, 8, 1.5f); }   // in the hand: raised above the rest
            FocusRing(id, r, 8);

            // The digit that switches to it, and its mark.
            if (it.index <= 9) {
                RectF k{ r.x + 14, r.y + r.h / 2 - 11, 22, 22 };
                Fill(k, T.badge, 5);
                Text(std::to_wstring(it.index), k, Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text2, Align::Center);
            }
            Avatar(r.x + 66, r.y + r.h / 2, 40, it.letters, it.color, it.active, T.dark ? Rgba(0x2B2B2B) : Rgba(0xFBFBFB));
            if (it.switching || it.deleting || it.warming || waiting) Spinner(r.x + 66, r.y + r.h / 2, 23);

            // Name and plan; then whether Codex uses it, and its email.
            float tx = TextX(r), limitsX = r.R() - MenuW - LimitsW, tw = limitsX - tx - 16;
            auto plan = it.PlanLabel();
            float pw = plan.empty() ? 0 : Measure(plan, Font(11, DWRITE_FONT_WEIGHT_SEMI_BOLD)).width + 14;
            if (Editing() && renaming == it.id) {
                // The text box sits here; this is its field, drawn in the Windows 11 style.
                RectF f = NameField(r);
                Fill(f.Inset(-1), T.menu, 4);
                Stroke(f.Inset(-1), T.controlStroke, 4);
                FieldLine(f.Inset(-1), T.text, 4);
            } else {
                float nw = std::min(Measure(it.shownName, Font(16, DWRITE_FONT_WEIGHT_SEMI_BOLD)).width + 2, tw - (pw ? pw + 8 : 0));
                Text(it.shownName, { tx, r.y + 14, nw, 22 }, Font(16, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text);
                if (pw) Pill({ tx + nw + 8, r.y + 16, pw, 18 }, plan, plan == L"Free" ? T.badge : T.accentTint, T.text2);
            }
            std::wstring status = it.Status();
            auto sc = it.switching || it.deleting || it.warming ? T.text : it.SignedOut() ? T.danger : it.active ? T.success : T.text2;
            if (best && !it.switching && !it.deleting && !it.warming) { status = Tr(L"Most limit left", L"Больше всего лимита"); sc = T.success; }
            if (waiting) { status = Tr(L"Waiting for the sign-in in the browser", L"Ждёт входа в браузере"); sc = T.text; }
            // Measured in the weight it is drawn in, so the email keeps its distance from a bold status.
            auto sf = Font(12.5f, it.active || best ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
            float sw = Measure(status, sf).width;
            Text(status, { tx, r.y + 40, tw, 18 }, sf, sc);
            auto email = it.shownEmail.empty() || _wcsicmp(it.shownEmail.c_str(), it.shownName.c_str()) == 0 ? std::wstring() : it.shownEmail;
            // A name that is the address's own first half isn't said twice: only "@example.com" follows.
            auto at = email.find(L'@');
            if (at != std::wstring::npos && at > 0 && _wcsicmp(email.substr(0, at).c_str(), it.shownName.c_str()) == 0) email = email.substr(at);
            if (!email.empty() && sw + 16 < tw) Text(L"·  " + email, { tx + sw + 7, r.y + 40, tw - sw - 7, 18 }, Font(12.5f), T.text3);

            // What is left, on the right.
            auto& u = it.usage;
            if (waiting) Waiting(limitsX, r.y + r.h / 2, it.index);
            else if (it.apiKey) Text(Tr(L"API key: no plan limits", L"API-ключ: без лимитов подписки"), { limitsX, r.y, LimitsW, r.h }, Font(12.5f), T.text3);
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
                    Text(Tr(L"not updated ", L"не обновлено ") + Core::Age(u.at), { limitsX + 42, r.y + r.h - 18, LimitsW - 42, 14 }, Font(11), T.text3);
            } else if (u.signedOut) {
                float bw = Measure(Tr(L"Sign in again", L"Войти заново"), Font(14)).width + 32;
                Button(Id("signin", it.index), { limitsX, r.y + r.h / 2 - 16, bw, 32 }, Tr(L"Sign in again", L"Войти заново"), false, [pid] { App::SignInAgain(pid); });
            } else {
                auto msg = it.fetching ? std::wstring(Tr(L"Getting limits…", L"Получаю лимиты…")) : u.problem;
                Text(msg, { limitsX, r.y + 8, LimitsW, r.h - 16 }, Font(12.5f), T.text3, Align::Left, true, true);
            }

            // The menu, on hover or keyboard focus.
            float showMore = A(id ^ 0x6D, (over || (keyboardCues && focus == id)) ? 1.f : 0.f, 120);
            if (showMore > 0.01f) {
                PushOpacity(showMore);
                UINT mid = Id("more", it.index);
                RectF m{ r.R() - 40, r.y + r.h / 2 - 16, 32, 32 };
                float mh = HotT(mid);
                Fill(m, Veil(mh, PressT(mid)), 4);
                Glyph(Glyphs::More, m, 14, Mix(T.text2, T.text, mh));
                Hit(mid, m, [this, pid, m] { Menu(pid, Screen(m.x, m.B())); }, nullptr, false);
                Describe(Util::Format(Tr(L"More for %s", L"Ещё для %s"), it.shownName.c_str()));
                Tip(Tr(L"Rename, details, delete and more", L"Имя, подробности, удаление и другое"));
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
            Text(L"Ctrl+N", { r.R() - 160, r.y, 104, r.h }, Font(12.5f), T.text3, Align::Right);
            Hit(id, r, [] { App::AddAccount(); });
            Describe(Tr(L"Add account", L"Добавить аккаунт"));
            FocusRing(id, r, 8);
            RectF more{ r.R() - 40, r.y + r.h / 2 - 16, 32, 32 };   // in one column with the rows' ⋯
            IconButton(Id("addmore"), more, Glyphs::ChevronDown, OtherWays(), [this, more] { App::AddMenu(Screen(more.x, more.B() + 2), hwnd); RefreshHover(); });
        }

        static const wchar_t* OtherWays() {
            return Tr(L"Other ways to add: a shop's order, the sign-in Codex uses now, a file, a backup",
                      L"Другие способы добавить: заказ из магазина, вход, который сейчас у Codex, файл, резервная копия");
        }

        // Nothing on the list yet: what the list is for and the one thing to do.
        float Empty(float y) {
            RectF e{ Pad, y, W - 2 * Pad, 196 };
            auto s = T.controlStrong; s.a *= .5f;
            DashedStroke(e, s, 8);
            Glyph(Glyphs::Contact, { e.x, e.y + 22, e.w, 28 }, 22, T.text3);
            Text(Tr(L"No accounts yet", L"Аккаунтов пока нет"), { e.x, e.y + 56, e.w, 24 }, Font(16, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text, Align::Center);
            Text(Tr(L"Add the first one: a ChatGPT sign-in page opens in the browser. Add the others the same way, and one click here switches Codex between them",
                    L"Добавьте первый: в браузере откроется вход ChatGPT. Остальные добавляются так же, и Codex переключается между ними одним щелчком"),
                 { e.x + 60, e.y + 84, e.w - 120, 40 }, Font(12.5f), T.text2, Align::Center, false, true);
            auto label = Tr(L"Add account", L"Добавить аккаунт");
            auto other = Tr(L"Other ways…", L"Другие способы…");
            float bw = Measure(label, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD)).width + 40, ow = Measure(other, Font(14)).width + 32;
            float bx = e.x + (e.w - bw - 8 - ow) / 2;
            Button(Id("addrow"), { bx, e.y + 138, bw, 32 }, label, true, [] { App::AddAccount(); });
            RectF more{ bx + bw + 8, e.y + 138, ow, 32 };
            Button(Id("addmore"), more, other, false, [this, more] { App::AddMenu(Screen(more.x, more.B() + 2), hwnd); RefreshHover(); });
            Tip(OtherWays());
            return e.h;
        }

        void Build() override {
            Text(Tr(L"Codex accounts", L"Аккаунты Codex"), { Pad, 12, W - 2 * Pad - 126, 30 }, Font(20, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), T.text);
            Text(CodexState(), { Pad, 41, W - 2 * Pad - 126, 18 }, Font(12.5f), App::Now().appRunning ? T.text2 : T.text3);
            IconButton(Id("manage"), { W - Pad - 104, 14, 32, 32 }, Glyphs::People, Tr(L"Account manager (Ctrl+M)", L"Управление аккаунтами (Ctrl+M)"), [] { App::ShowManager(); });
            if (App::FetchingUsage()) Spinner(W - Pad - 52, 30, 8);
            else IconButton(Id("refresh"), { W - Pad - 68, 14, 32, 32 }, Glyphs::Refresh, Tr(L"Refresh limits (F5)", L"Обновить лимиты (F5)"), [] { App::RefreshUsage(true); });
            IconButton(Id("settings"), { W - Pad - 32, 14, 32, 32 }, Glyphs::Settings, Tr(L"Settings (Ctrl+,)", L"Настройки (Ctrl+,)"), [] { App::ShowSettings(); });

            float y = 72;
            auto b = Current();
            if (b != Banner::None) { BannerUi(y, b); y += 64 + 12; }

            // The list scrolls once there are more accounts than fit comfortably.
            int n = (int)App::items.size();
            float full = n * (RowH + RowGap);
            if (n) {
                // What the two figures of a row are, said once above them.
                float lx = W - Pad - MenuW - LimitsW;
                Text(Tr(L"left", L"осталось"), { lx + 92, y - 6, 80, 14 }, Font(11), T.text3, Align::Right);
                Text(Tr(L"resets", L"сброс"), { lx + 180, y - 6, LimitsW - 180, 14 }, Font(11), T.text3);
                y += 12;
            }
            listTop = y;
            listH = std::min(full, MaxRows * (RowH + RowGap));
            scrollMax = std::max(0.f, full - listH);
            if (scrollY > scrollMax) scrollY = scrollMax;
            auto best = App::MostLeft();
            bool adding = App::SigningInNew();
            if (grabbed && !App::Find(grabId)) { grabbed = 0; lifting = false; }
            if (n || adding) {
                BeginScroll({ 0, listTop, W, listH });
                RT()->PushAxisAlignedClip(D2D1::RectF(0, listTop - 4, W, listTop + listH), D2D1_ANTIALIAS_MODE_ALIASED);
                // The rows step aside for the one being dragged, which is drawn last, on top.
                Item* inHand = nullptr;
                for (auto& it : App::items) {
                    if (lifting && it.index == grabbed) { inHand = &it; continue; }
                    auto rr = RowRect(it.index);
                    rr.y += A(ShiftId(it), Shift(it.index), 140);
                    Row(it, rr, &it == best);
                }
                if (inHand) {
                    auto rr = RowRect(inHand->index);
                    rr.y = std::max(listTop - 6, std::min(listTop + listH - RowH - RowGap + 6, rr.y + liftDy));
                    Row(*inHand, rr, inHand == best, true);
                }
                RT()->PopAxisAlignedClip();
                EndScroll(full);
                y = listTop + listH;
                if (adding) { NewRow({ Pad, y, W - 2 * Pad, RowH }); y += RowH + RowGap; }
                AddRow({ Pad, y, W - 2 * Pad, AddH });
                y += AddH + 20;
            } else {
                scrollMax = 0;
                y += Empty(y) + 20;
            }

            // As in Claude Profile Switcher: starting with Windows on the left, the keys on the right.
            UINT at = Id("autostart");
            auto flip = [this] { App::SetAutostart(!App::Autostart()); Invalidate(); };
            auto label = Tr(L"Start with Windows", L"Запускать вместе с Windows");
            Toggle(at, Pad, y, App::Autostart(), flip, true, label);
            float lw = Measure(label, Font(14)).width;
            Text(label, { Pad + 52, y - 4, lw + 4, 28 }, Font(14), T.text);
            Hit(at, { Pad + 48, y - 6, lw + 12, 32 }, flip, nullptr, false);
            bool opens = App::OpenAfterSwitch() && App::Now().appInstalled;
            auto hint = opens ? Tr(L"A digit opens · F2 renames · F5 refreshes", L"Цифра — открыть · F2 — имя · F5 — обновить")
                              : Tr(L"A digit switches · F2 renames · F5 refreshes", L"Цифра — переключить · F2 — имя · F5 — обновить");
            float hx = Pad + 52 + lw + 24;
            if (n) Text(hint, { hx, y - 4, W - Pad - hx, 28 }, Font(12.5f), T.text3, Align::Right);
            contentHeight = y + 20 + 20;
        }

        // ------------------------------------------------------------ input

        bool OnKey(UINT vk, bool alt) override {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (vk >= '1' && vk <= '9' && !ctrl && !alt) { int d = vk - '0'; for (auto& i : App::items) if (i.index == d) { App::Switch(i.id, true); break; } return true; }
            if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9 && !ctrl) { int d = vk - VK_NUMPAD0; for (auto& i : App::items) if (i.index == d) { App::Switch(i.id, true); break; } return true; }
            if (ctrl && vk == 'N') { App::AddAccount(); return true; }
            if (ctrl && vk == 'V') { App::PasteAccounts(hwnd); return true; }      // whatever account the clipboard holds
            if (ctrl && vk == 'Z' && App::CanUndoDelete()) { App::UndoDelete(); return true; }
            if (ctrl && vk == VK_OEM_COMMA) { App::ShowSettings(); return true; }
            if (ctrl && vk == 'M') { App::ShowManager(); return true; }
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
                if (to >= 1 && to <= (int)App::items.size()) FocusOn(Id("row", to));
                return true;
            }
            if (vk == VK_APPS || (vk == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000))) {
                auto r = RowRect(focused->index);
                Menu(id, Screen(r.R() - 40, r.y + r.h / 2 + 16));
                return true;
            }
            return false;
        }

        void OnClose() override { App::ClosePicker(); }
        void OnDrop(const std::vector<std::wstring>& files) override { App::AddFiles(files, hwnd); }

        // Dragging a row to another place. The press is remembered when it lands on a row
        // itself (not on a button in it); it becomes a drag once the pointer has moved.
        void OnMouseDown() override {
            grabbed = 0; lifting = false;
            if (Editing() || App::items.size() < 2) return;
            const Region* top = nullptr;
            for (auto& g : Regions()) if (g.r.Has(mx, my)) top = &g;
            if (!top) return;
            for (auto& it : App::items) if (top->id == Id("row", it.index)) { grabbed = it.index; grabId = it.id; grabY = my; liftTo = it.index; liftDy = 0; }
        }

        LRESULT Extra(UINT m, WPARAM w, LPARAM l, bool& handled) override {
            if (m == WM_MOUSEMOVE && grabbed) {
                if (!(w & MK_LBUTTON)) { grabbed = 0; lifting = false; return 0; }
                float y = GET_Y_LPARAM(l) / scale;
                if (!lifting && std::fabs(y - grabY) < 6) return 0;
                lifting = true; liftDy = y - grabY;
                const float step = RowH + RowGap;
                liftTo = std::max(1, std::min((int)App::items.size(), (int)std::floor(((grabbed - 1) * step + liftDy + RowH / 2) / step) + 1));
                handled = true; Invalidate();
                return 0;
            }
            if (m == WM_LBUTTONUP && grabbed) {
                int from = grabbed, to = liftTo; bool was = lifting; auto id = grabId;
                grabbed = 0; lifting = false;
                if (!was) return 0;                // it never moved: an ordinary click
                handled = true;
                ReleaseCapture(); pressed = 0;
                for (auto& it : App::items) Snap(ShiftId(it), 0);
                if (to != from && App::Find(id)) App::Move(id, to - from);
                RefreshHover();
                return 0;
            }
            if (m == WM_CAPTURECHANGED && lifting && (HWND)l != hwnd) { grabbed = 0; lifting = false; Invalidate(); }
            return 0;
        }

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
