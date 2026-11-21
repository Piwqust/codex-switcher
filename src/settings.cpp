#include "app.h"
#include "codex.h"
#include "web.h"
#include <shellapi.h>

using namespace Gfx;

namespace {
    // Windows 11 Settings in small: pages on the left, a few groups of rows on each page, so
    // nothing has to be hunted for in one long list.
    const float NavW = 236, NavItemH = 36, NavStep = 40, NavTop = 64, MaxContentW = 780;

    struct PageInfo { const wchar_t* glyph; const wchar_t* en; const wchar_t* ru; };
    const PageInfo Pages[] = {
        { L"", L"General", L"Общие" },
        { L"", L"Switching", L"Переключение" },
        { L"", L"Limits", L"Лимиты" },
        { L"", L"Accounts", L"Аккаунты" },
        { L"", L"About", L"О программе" },
    };
    const int PageCount = ARRAYSIZE(Pages);

    // One row of a group: title and description on the left, its control on the right.
    struct Row {
        std::wstring title, desc;
        float controlW = 0;
        std::function<void(RectF)> control;    // draws the control inside the row
        std::function<void()> click;           // a switch's row flips it wherever it's clicked
        UINT clickId = 0;
        bool sub = false;                      // depends on the row above, shown indented
        bool enabled = true;
        D2D1_COLOR_F descColor{};
    };

    struct Btn {
        std::wstring label; std::function<void()> click;
        bool primary = false, enabled = true;
        const char* key = nullptr;             // its own name; otherwise the row's, numbered
    };

    struct SettingsW : Surface {
        int page = 0;
        bool recording = false, placed = false;
        std::wstring hotkeyNote;
        bool hotkeyNoteBad = false;
        float x0 = NavW, cw = 600;             // the content column: left edge and card width

        void Start() {
            backdrop = 2;
            Create(Title(), WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, 0, 900, 680);
        }

        static const wchar_t* Title() { return Tr(L"Settings — Codex Accounts", L"Настройки — Аккаунты Codex"); }

        void StopRecording() { if (recording) { recording = false; App::ResumeHotkey(); } }

        void Go(int p) {
            StopRecording();
            page = (p + PageCount) % PageCount;
            scrollY = 0;
            Test::Trace(Util::Format(L"settings-page|%d", page));
            Invalidate();
        }

        // ------------------------------------------------------------ rows and groups

        float TextLeft(const Row& r) const { return x0 + 16 + (r.sub ? 28.f : 0.f); }
        float TextWidth(const Row& r) const { return std::max(140.f, x0 + cw - 16 - r.controlW - (r.controlW > 0 ? 16 : 0) - TextLeft(r)); }
        float DescHeight(const Row& r) { return r.desc.empty() ? 0 : Measure(r.desc, Font(12.5f), TextWidth(r), true).height; }
        float RowHeight(const Row& r) { float dh = DescHeight(r); return std::max(dh ? 62.f : 50.f, 26 + 20 + (dh ? dh + 2 : 0)); }

        // An optional heading and note, then the rows in one card with thin lines between them.
        float Group(float y, const std::wstring& heading, const std::vector<Row>& rows, const std::wstring& note = L"") {
            float top = y;
            if (!heading.empty()) {
                Text(heading, { x0 + 2, y + 16, cw, 20 }, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text);
                y += 42;
                if (!note.empty()) {
                    float nh = Measure(note, Font(12.5f), cw - 4, true).height;
                    Text(note, { x0 + 2, y - 4, cw - 4, nh }, Font(12.5f), T.text2, Align::Left, false, true);
                    y += nh + 6;
                }
            } else y += 12;
            std::vector<float> hs;
            float total = 0;
            for (auto& r : rows) { hs.push_back(RowHeight(r)); total += hs.back(); }
            RectF card{ x0, y, cw, total };
            Fill(card, T.card, 8); Stroke(card, T.cardStroke, 8);
            for (size_t i = 0; i < rows.size(); i++) {
                auto& r = rows[i];
                RectF rr{ x0, y, cw, hs[i] };
                if (i) Fill({ x0 + 1, y, cw - 2, 1 }, T.layerStroke);
                if (r.click && r.enabled) {
                    float h = A(r.clickId ^ 0x51, Over(rr) ? 1.f : 0.f), p = PressT(r.clickId);
                    if (h > 0.01f || p > 0.01f) Fill(rr.Inset(4), Veil(h * .7f, p), 4);   // 4 in + radius 4 = the card's 8
                    Hit(r.clickId, rr, [this, f = r.click] { f(); Invalidate(); }, nullptr, false, false);
                }
                float tx = TextLeft(r), tw = TextWidth(r), dh = DescHeight(r);
                float ty = y + (hs[i] - 20 - (dh ? dh + 2 : 0)) / 2;
                Text(r.title, { tx, ty, tw, 20 }, Font(14), r.enabled ? T.text : T.text3);
                if (dh) Text(r.desc, { tx, ty + 22, tw, dh }, Font(12.5f), r.descColor.a > 0 ? r.descColor : r.enabled ? T.text2 : T.text3, Align::Left, false, true);
                if (r.control) r.control(rr);
                y += hs[i];
            }
            return y - top;
        }

        // Each control has a key of its own, so its hover state and the checks' names stay put
        // whatever the language and the page.
        Row Switch(const char* key, std::wstring title, std::wstring desc, bool on, std::function<void()> flip, bool enabled = true, bool sub = false) {
            Row r;
            r.title = std::move(title); r.desc = std::move(desc); r.controlW = 40; r.enabled = enabled; r.sub = sub;
            UINT id = Id(key);
            auto name = r.title;
            r.control = [this, id, on, flip, enabled, name](RectF rr) { Toggle(id, rr.R() - 16 - 40, rr.y + rr.h / 2 - 10, on, [this, flip] { flip(); Invalidate(); }, enabled, name); };
            r.click = flip;
            r.clickId = Id((std::string(key) + "-row").c_str());
            return r;
        }

        Row Buttons(const char* key, std::wstring title, std::wstring desc, std::vector<Btn> bs, bool sub = false, D2D1_COLOR_F descColor = {}) {
            Row r;
            r.title = std::move(title); r.desc = std::move(desc); r.sub = sub; r.descColor = descColor;
            std::vector<float> ws;
            for (auto& b : bs) { ws.push_back(std::max(96.f, Measure(b.label, Font(14, b.primary ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL)).width + 32)); r.controlW += ws.back() + 8; }
            if (!bs.empty()) r.controlW -= 8;
            r.control = [this, key, bs, ws](RectF rr) {
                float x = rr.R() - 16;
                for (int i = (int)bs.size() - 1; i >= 0; i--) {
                    x -= ws[i];
                    UINT id = bs[i].key ? Id(bs[i].key) : Id(key, i);
                    Button(id, { x, rr.y + rr.h / 2 - 16, ws[i], 32 }, bs[i].label, bs[i].primary, bs[i].click, false, bs[i].enabled);
                    x -= 8;
                }
            };
            return r;
        }

        // A segmented choice, as the Windows 11 settings pages use for a few fixed values.
        // Its segments as wide as their labels need, at least segW.
        std::vector<float> SegmentWidths(const std::vector<std::wstring>& labels, float segW) {
            std::vector<float> widths;
            for (auto& l : labels) widths.push_back(std::max(segW, Measure(l, Font(12.5f)).width + 28));
            return widths;
        }
        float ChoiceWidth(const std::vector<std::wstring>& labels, float segW) { float w = 8; for (float s : SegmentWidths(labels, segW)) w += s; return w; }

        // width: the whole control's, so choices stacked in one card line up; the extra room
        // goes to the segments evenly.
        Row Choice(const char* key, std::wstring title, std::wstring desc, std::vector<std::wstring> labels, int current, float segW, std::function<void(int)> pick, float width = 0) {
            Row r;
            r.title = std::move(title); r.desc = std::move(desc);
            auto what = r.title;
            auto widths = SegmentWidths(labels, segW);
            float total = 8;
            for (float s : widths) total += s;
            if (width > total) { for (auto& s : widths) s += (width - total) / widths.size(); total = width; }
            r.controlW = total;
            r.control = [this, key, labels, widths, total, current, pick, what](RectF rr) {
                RectF seg{ rr.R() - 16 - total, rr.y + rr.h / 2 - 17, total, 34 };
                Fill(seg, T.control, 8); Stroke(seg, T.controlStroke, 8);   // the segments' 4 + the 4 around them
                float x = seg.x + 4;
                for (int i = 0; i < (int)labels.size(); i++) {
                    RectF b{ x, seg.y + 4, widths[i], 26 };
                    x += widths[i];
                    UINT id = Id(key, i);
                    float t = A(id ^ 0x5E, current == i ? 1.f : 0.f, 140), hh = HotT(id);
                    Fill(b, Mix(Veil(hh), T.accent, t), 4);
                    Text(labels[i], b, Font(12.5f), Mix(T.text, T.onAccent, t), Align::Center);
                    Hit(id, b, [this, pick, i] { pick(i); Invalidate(); });
                    Describe(what + L": " + labels[i], Role::Radio, current == i ? 1 : 0);
                    FocusRing(id, b, 4);
                }
            };
            return r;
        }

        // ------------------------------------------------------------ what the pages need

        // The Codex CLI the switcher would use, and its version, looked up at most every 10 s.
        std::wstring cliPath, cliVersion; ULONGLONG cliAt = 0;
        void LookCli() {
            if (cliAt && GetTickCount64() - cliAt < 10000) return;
            cliAt = GetTickCount64();
            cliPath = Recovery::Cli(); cliVersion.clear();
            wchar_t b[MAX_PATH * 2];
            if (!cliPath.empty() && GetEnvironmentVariableW(L"APPDATA", b, ARRAYSIZE(b))) {
                json::Value v;
                if (json::Parser::Parse(Util::ReadText(std::wstring(b) + L"\\npm\\node_modules\\@openai\\codex\\package.json"), v)) cliVersion = v.Str(L"version");
            }
        }

        // Where the CLI is installed: the npm package rather than the binary deep inside it.
        std::wstring CliFolder() {
            if (Util::Lower(cliPath).find(L"\\windowsapps\\") != std::wstring::npos)
                return Util::Format(Tr(L"The copy inside the %s app. npm i -g @openai/codex installs one for terminals", L"Копия внутри приложения %s. Для терминалов установите: npm i -g @openai/codex"), Codex::AppName().c_str());
            auto at = Util::Lower(cliPath).find(L"\\node_modules\\@openai\\codex\\");
            return Util::Tilde(at == std::wstring::npos ? cliPath : cliPath.substr(0, at + 27));
        }

        // ------------------------------------------------------------ pages

        void General(float& y) {
            y += Group(y, L"", {
                Switch("autostart", Tr(L"Start with Windows", L"Запускать вместе с Windows"),
                       Tr(L"Waits in the tray, keeps the limits current and saves renewed sign-ins", L"Ждёт в трее, обновляет лимиты и сохраняет продлённые входы"),
                       App::Autostart(), [] { App::SetAutostart(!App::Autostart()); }),
                Switch("keep-tray", Tr(L"Stay in the tray when the window closes", L"Оставаться в трее после закрытия окна"),
                       Tr(L"The list then opens instantly", L"Тогда список открывается мгновенно"),
                       App::KeepInTray(), [] { App::SetKeepInTray(!App::KeepInTray()); }),
            });

            std::vector<Row> keys;
            {
                auto hk = App::Hotkey();
                Row r;
                r.title = Tr(L"Shortcut for the account list", L"Сочетание для списка аккаунтов");
                r.desc = recording ? Tr(L"Press a shortcut, for example Ctrl + Alt + X. Esc cancels", L"Нажмите сочетание, например Ctrl + Alt + X. Esc — отмена")
                       : !hotkeyNote.empty() ? hotkeyNote
                       : App::HotkeyFailed() ? Tr(L"Another app already uses it, choose a different one", L"Оно занято другой программой, выберите другое")
                       : Tr(L"Opens the list from any app", L"Открывает список из любой программы");
                if (!recording && (hotkeyNoteBad || (hotkeyNote.empty() && App::HotkeyFailed()))) r.descColor = T.danger;
                r.controlW = hk.empty() ? 160.f : 196.f;
                r.control = [this, hk](RectF rr) {
                    std::wstring label = recording ? Tr(L"Press keys…", L"Нажмите сочетание…") : hk.empty() ? Tr(L"Not set", L"Не задано") : Hotkeys::Pretty(hk);
                    RectF b{ rr.R() - 16 - (hk.empty() ? 160.f : 196.f), rr.y + rr.h / 2 - 16, 160, 32 };
                    Button(Id("hk"), b, label, recording, [this] { recording = true; hotkeyNote.clear(); App::SuspendHotkey(); Invalidate(); });
                    if (!hk.empty()) IconButton(Id("hk-clear"), { b.R() + 4, b.y, 32, 32 }, Glyphs::Clear, Tr(L"Remove the shortcut", L"Убрать сочетание"), [this] { App::SetHotkey(L""); hotkeyNote.clear(); Invalidate(); });
                };
                keys.push_back(r);
            }
            {
                auto taken = App::AccountHotkeysTaken();
                bool bad = !taken.empty() && App::AccountHotkeys();
                std::wstring desc = Tr(L"Switches to the account with that number, from any app", L"Переключает на аккаунт с этим номером из любой программы");
                if (bad) {
                    desc = Tr(L"Taken by another app:", L"Заняты другой программой:");
                    for (size_t i = 0; i < taken.size(); i++) desc += (i ? L"," : L"") + Util::Format(L" Ctrl + Shift + Alt + %d", taken[i]);
                }
                auto r = Switch("acchk", Tr(L"Ctrl + Shift + Alt + digit", L"Ctrl + Shift + Alt + цифра"), desc,
                                App::AccountHotkeys(), [] { App::SetAccountHotkeys(!App::AccountHotkeys()); });
                if (bad) r.descColor = T.danger;
                keys.push_back(r);
            }
            y += Group(y, Tr(L"Keyboard", L"Клавиатура"), keys);

            const wchar_t* themes[] = { L"auto", L"light", L"dark" };
            int theme = 0; for (int i = 0; i < 3; i++) if (App::Theme() == themes[i]) theme = i;
            const wchar_t* langs[] = { L"auto", L"en", L"ru" };
            auto choice = L10n::Choice();
            int lang = 0; for (int i = 0; i < 3; i++) if (choice == langs[i]) lang = i;
            std::vector<std::wstring> themeLabels = { Tr(L"System", L"Системная"), Tr(L"Light", L"Светлая"), Tr(L"Dark", L"Тёмная") },
                                      langLabels = { Tr(L"System", L"Системный"), L"English", L"Русский" };
            float w = std::max(ChoiceWidth(themeLabels, 84), ChoiceWidth(langLabels, 84));
            y += Group(y, Tr(L"Appearance", L"Оформление"), {
                Choice("theme", Tr(L"Theme", L"Тема"), L"", themeLabels, theme, 84,
                       [](int i) { const wchar_t* v[] = { L"auto", L"light", L"dark" }; App::SetTheme(v[i]); }, w),
                Choice("lang", Tr(L"Language", L"Язык"), L"", langLabels, lang, 84,
                       [this](int i) { const wchar_t* v[] = { L"auto", L"en", L"ru" }; App::SetLanguage(v[i]); SetWindowTextW(hwnd, Title()); }, w),
            });
        }

        void Switching(float& y) {
            auto& s = App::Now();
            auto name = Codex::AppName();
            std::vector<Row> choose;
            choose.push_back(Switch("open-after", Util::Format(Tr(L"Open %s with it", L"Открывать %s с ним"), name.c_str()),
                                    s.appInstalled ? std::wstring(Tr(L"Otherwise a click only switches Codex", L"Иначе щелчок только переключает Codex"))
                                                   : Util::Format(Tr(L"%s isn't installed", L"%s не установлен"), name.c_str()),
                                    App::OpenAfterSwitch(), [] { App::SetOpenAfterSwitch(!App::OpenAfterSwitch()); }, s.appInstalled));
            choose.push_back(Switch("restart", Util::Format(Tr(L"Restart %s if it's open", L"Перезапускать %s, если он открыт"), name.c_str()),
                                    Tr(L"Otherwise it keeps the old account until restarted. Editors such as VS Code reconnect", L"Иначе он останется на старом аккаунте до перезапуска. Редакторы вроде VS Code переподключатся"),
                                    App::RestartOnSwitch(), [] { App::SetRestartOnSwitch(!App::RestartOnSwitch()); }));
            if (App::RestartOnSwitch())
                choose.push_back(Switch("ask", Tr(L"Ask first", L"Сначала спрашивать"),
                                        Tr(L"Tasks running in it or in an editor stop when it closes", L"Задачи, запущенные в нём или в редакторе, при закрытии остановятся"),
                                        App::AskBeforeRestart(), [] { App::SetAskBeforeRestart(!App::AskBeforeRestart()); }, true, true));
            choose.push_back(Switch("hide-after", Tr(L"Hide the account list", L"Прятать список аккаунтов"), L"",
                                    App::HideAfterSwitch(), [] { App::SetHideAfterSwitch(!App::HideAfterSwitch()); }));
            y += Group(y, Tr(L"When you choose an account", L"Когда вы выбираете аккаунт"), choose);

            LookCli();
            y += Group(y, Tr(L"When an account hits its limit", L"Когда аккаунт упирается в лимит"), {
                Switch("continue", Tr(L"Continue the stopped conversations", L"Продолжать остановленные разговоры"),
                       cliPath.empty() ? std::wstring(Tr(L"Needs the Codex CLI: npm i -g @openai/codex", L"Нужен Codex CLI: npm i -g @openai/codex"))
                                       : Util::Format(Tr(L"After a switch that reopens %s, conversations of the last 24 hours that hit the limit get “Continue the previous task.” once",
                                                         L"После переключения с перезапуском %s разговоры последних суток, упёршиеся в лимит, один раз получат «Continue the previous task.»"), name.c_str()),
                       App::ContinueStopped(), [] { App::SetContinueStopped(!App::ContinueStopped()); }, !cliPath.empty()),
            });

            std::vector<Row> web;
            web.push_back(Switch("web", Tr(L"Account list in a browser", L"Список аккаунтов в браузере"),
                                 Web::Problem().empty() ? std::wstring(Tr(L"To switch from a phone or another computer. Anyone with the link can switch your accounts",
                                                                          L"Чтобы переключать с телефона или другого компьютера. Переключать может любой, у кого есть ссылка"))
                                                        : Web::Problem(),
                                 App::WebOn(), [] { App::SetWebOn(!App::WebOn()); }));
            if (App::WebOn()) {
                web.push_back(Switch("weblan", Tr(L"Let other devices in", L"Пускать другие устройства"),
                                     Tr(L"From your network or Tailscale. The link isn't encrypted: trusted networks only", L"Из вашей сети или Tailscale. Ссылка не шифруется: только в доверенных сетях"),
                                     App::WebLan(), [] { App::SetWebLan(!App::WebLan()); }, true, true));
                if (Web::Running()) {
                    auto url = Web::Url(!App::WebLan());
                    web.push_back(Buttons("weblink", Tr(L"Link", L"Ссылка"), url, {
                        { Tr(L"New link", L"Новая"), [] { Web::NewKey(); Web::Apply(); } },
                        { Tr(L"Copy", L"Копировать"), [this, url] { if (Util::CopyText(hwnd, url)) Toast(Tr(L"Link copied", L"Ссылка скопирована")); } },
                        { Tr(L"Open", L"Открыть"), [url] { Util::Open(url); } } }, true));
                }
            }
            y += Group(y, Tr(L"From other devices", L"С других устройств"), web);
        }

        void Limits(float& y) {
            int minutes[] = { 0, 5, 15, 60 };
            int usage = 2; for (int i = 0; i < 4; i++) if (App::UsageMinutes() == minutes[i]) usage = i;
            const wchar_t* modes[] = { L"icon", L"percent", L"hidden" };
            int tray = 0; for (int i = 0; i < 3; i++) if (App::TrayMode() == modes[i]) tray = i;
            std::vector<std::wstring> usageLabels = { Tr(L"On open", L"При открытии"), Tr(L"5 min", L"5 мин"), Tr(L"15 min", L"15 мин"), Tr(L"1 h", L"1 ч") },
                                      trayLabels = { Tr(L"Icon", L"Значок"), Tr(L"% left", L"% остатка"), Tr(L"None", L"Нет") };
            float w = std::max(ChoiceWidth(usageLabels, 62), ChoiceWidth(trayLabels, 72));
            y += Group(y, L"", {
                Choice("usage", Tr(L"Refresh limits", L"Обновлять лимиты"), Tr(L"How often to ask OpenAI for every account's limits", L"Как часто спрашивать у OpenAI лимиты всех аккаунтов"),
                       usageLabels, usage, 62, [](int i) { int v[] = { 0, 5, 15, 60 }; App::SetUsageMinutes(v[i]); }, w),
                Choice("traymode", Tr(L"Tray icon", L"Значок в трее"),
                       Tr(L"“% left” is the 5-hour window of the account in use. Without an icon, the shortcut opens the list",
                          L"«% остатка» — 5-часовое окно текущего аккаунта. Без значка список открывается сочетанием клавиш"),
                       trayLabels, tray, 72, [](int i) { const wchar_t* m[] = { L"icon", L"percent", L"hidden" }; App::SetTrayMode(m[i]); }, w),
            });

            std::vector<Row> warm;
            warm.push_back(Switch("autowarm", Tr(L"When a window is idle", L"Когда окно простаивает"),
                                  Tr(L"For every account. For just some, use ⋯ next to each", L"Для всех аккаунтов. Для отдельных — ⋯ у аккаунта"),
                                  App::AutoWarmup(), [] { App::SetAutoWarmup(!App::AutoWarmup()); }));
            {
                auto r = Switch("timed", Tr(L"Every day at set times", L"Каждый день в заданное время"),
                                Tr(L"So the windows reset when you need them. A time the PC slept through is skipped", L"Чтобы окна сбрасывались, когда нужно. Время, проспанное компьютером, пропускается"),
                                App::TimedWarmup(), [] { App::SetTimedWarmup(!App::TimedWarmup()); });
                std::wstring times;
                for (auto& t : App::WarmupTimes()) times += (times.empty() ? L"" : L", ") + t;
                float bw = std::max(130.f, Measure(times, Font(14)).width + 30);
                auto toggle = r.control;
                r.controlW = bw + 56;
                r.control = [this, toggle, times, bw](RectF rr) {
                    Button(Id("times"), { rr.R() - 16 - 56 - bw, rr.y + rr.h / 2 - 16, bw, 32 }, times.empty() ? Tr(L"Set times", L"Задать время") : times, false, [this] { AskTimes(); });
                    toggle(rr);
                };
                warm.push_back(r);
            }
            y += Group(y, Tr(L"Start 5-hour windows early", L"Запускать 5-часовые окна заранее"), warm,
                       Tr(L"A tiny request starts an account's window, so it resets sooner. Accounts whose week is used up are skipped",
                          L"Крошечный запрос запускает окно аккаунта, и оно раньше сбросится. Аккаунты с исчерпанной неделей пропускаются"));
        }

        void AskTimes() {
            std::wstring text;
            for (auto& t : App::WarmupTimes()) text += (text.empty() ? L"" : L", ") + t;
            for (;;) {
                if (!Dialogs::Prompt(hwnd, Tr(L"Times to start the windows", L"Время запуска окон"), Tr(L"24-hour clock, separated by commas, for example 8:00, 13:00, 18:00", L"В 24-часовом формате, через запятую, например 8:00, 13:00, 18:00"), text, false, false, Tr(L"Save", L"Сохранить"))) break;
                if (App::SetWarmupTimes(text)) { App::SetTimedWarmup(true); break; }
                Dialogs::Info(hwnd, Tr(L"That doesn't read as times", L"Это не похоже на время"), Tr(L"Write them like 8:00, 13:30", L"Пишите их так: 8:00, 13:30"));
            }
            Invalidate();
        }

        void AccountsPage(float& y) {
            auto& s = App::Now();
            y += Group(y, L"", {
                Buttons("manage-row", Tr(L"Account manager", L"Управление аккаунтами"),
                        Tr(L"Passwords, 2FA codes, orders and when each paid month ends. Takes a shop's message and walks a bought account through to ready",
                           L"Пароли, коды 2FA, заказы и сроки подписок. Принимает сообщение магазина и проводит купленный аккаунт по шагам до готовности"),
                        { { Tr(L"Open", L"Открыть"), [] { App::ShowManager(); }, true, true, "manage" } }),
            });
            std::vector<Row> add;
            add.push_back(Buttons("add-row", Tr(L"Add an account", L"Добавить аккаунт"),
                                  Tr(L"Sign in with ChatGPT in the browser, or take a saved auth.json", L"Вход через ChatGPT в браузере или сохранённый auth.json"), {
                                      { Tr(L"From file…", L"Из файла…"), [this] { App::ImportFile(hwnd); }, false, true, "authjson" },
                                      { Tr(L"Sign in", L"Войти"), [] { App::AddAccount(); }, true, !App::SigningIn(), "add" } }));
            if (s.legacy > 0 || Util::Exists(Accounts::LegacyFile()))
                add.push_back(Buttons("legacy", Tr(L"From Codex Switcher", L"Из Codex Switcher"),
                                      s.legacy > 0 ? Util::Format(Tr(L"%s there that aren't here yet, with their names", L"Там %s, которых здесь ещё нет; имена перенесутся"),
                                                                  Util::Count(s.legacy, L"account", L"accounts", L"аккаунт", L"аккаунта", L"аккаунтов").c_str())
                                                   : std::wstring(Tr(L"Every account from it is here already", L"Все его аккаунты уже здесь")),
                                      { { Tr(L"Import", L"Импортировать"), [this] { App::ImportLegacy(hwnd); }, false, s.legacy > 0 } }));
            y += Group(y, L"", add);
            y += Group(y, L"", {
                Buttons("backup", Tr(L"Backup", L"Резервная копия"),
                        Tr(L"Every account under one password, as a file or a line of text: to move them to another PC. Codex Switcher's exports import too",
                           L"Все аккаунты под одним паролем, файлом или строкой текста: чтобы перенести на другой компьютер. Импорт принимает и экспорт Codex Switcher"),
                        { { Tr(L"Import…", L"Импорт…"), [this] { App::ImportBackup(hwnd); } }, { Tr(L"Back up…", L"Сохранить…"), [this] { App::ExportAll(hwnd); } } }),
                Switch("mask", Tr(L"Hide emails", L"Скрывать email"),
                       Tr(L"For screen sharing: jo•••@g•••.com. For one account: ⋯ → Hide its email", L"Для показа экрана: jo•••@g•••.com. Для одного аккаунта: ⋯ → Скрывать его email"),
                       App::MaskAll(), [] { App::SetMaskAll(!App::MaskAll()); }),
            });
        }

        void About(float& y) {
            using P = Update::Phase;
            auto st = Update::State();
            std::vector<Row> top;
            {
                Row r;
                r.title = Util::Format(L"Codex Account Switcher %s", Update::Version());
                switch (st) {
                case P::Checking: r.desc = Tr(L"Checking for updates…", L"Проверяю обновления…"); break;
                case P::Ready: r.desc = Util::Format(Tr(L"Version %s is out. Updating takes a few seconds, Codex isn't touched", L"Вышла версия %s. Обновление займёт пару секунд, Codex не затронется"), Update::Latest().c_str()); break;
                case P::Downloading: r.desc = Util::Format(Tr(L"Downloading %s…", L"Скачиваю %s…"), Update::Latest().c_str()); break;
                case P::Installing: r.desc = Tr(L"Installing the update…", L"Устанавливаю обновление…"); break;
                case P::Failed: r.desc = Tr(L"Couldn't update: ", L"Не удалось обновить: ") + Update::Problem(); r.descColor = T.danger; break;
                case P::Current: {
                    auto when = Util::When(Update::CheckedAt());
                    for (auto word : { Tr(L"Today", L"Сегодня"), Tr(L"Yesterday", L"Вчера") })
                        if (when.rfind(word, 0) == 0) when[0] = towlower(when[0]);
                    r.desc = Tr(L"The latest version · checked ", L"Последняя версия · проверено ") + when;
                    break;
                }
                default: r.desc = Tr(L"New versions come from the project's GitHub releases", L"Новые версии приходят из релизов проекта на GitHub"); break;
                }
                // The demo has nothing to update; it shows the version alone.
                if (!Paths::Isolated || st != P::Idle) {
                    bool busy = st == P::Checking || st == P::Downloading || st == P::Installing, install = st == P::Ready || st == P::Downloading || st == P::Installing;
                    r.controlW = 150;
                    r.control = [this, busy, install](RectF rr) {
                        RectF b{ rr.R() - 16 - 150, rr.y + rr.h / 2 - 16, 150, 32 };
                        if (install) Button(Id("update"), b, busy ? Tr(L"Updating…", L"Обновляю…") : Tr(L"Update", L"Обновить"), true, [] { Update::Install(); }, false, !busy);
                        else Button(Id("update"), b, Tr(L"Check now", L"Проверить"), false, [] { Update::Check(true); }, false, !busy);
                    };
                }
                top.push_back(r);
                if (!Paths::Isolated || st != P::Idle)
                    top.push_back(Switch("autoupdate", Tr(L"Check for updates automatically", L"Проверять обновления автоматически"),
                                         Tr(L"At start and every 6 hours. Only the version number is asked from GitHub", L"При запуске и раз в 6 часов. У GitHub запрашивается только номер версии"),
                                         Update::Auto(), [] { Update::SetAuto(!Update::Auto()); }));
            }
            y += Group(y, L"", top);

            auto& s = App::Now();
            std::vector<Row> codex;
            {
                auto app = Codex::App();
                auto name = Codex::AppName();
                std::wstring title = !app.found() ? Util::Format(Tr(L"The %s app isn't installed", L"Приложение %s не установлено"), name.c_str())
                                   : Util::Format(Tr(L"%s app ", L"Приложение %s "), name.c_str()) + (app.version.empty() ? L"" : app.version + L" ") + L"· " + (app.kind == L"Msix" ? L"Microsoft Store" : Tr(L"your path", L"ваш путь"));
                std::wstring desc = !app.found() ? std::wstring(Tr(L"The CLI and editors still switch. The app is in the Microsoft Store", L"CLI и редакторы всё равно переключаются. Приложение есть в Microsoft Store"))
                                  : s.appRunning ? std::wstring(Tr(L"Open", L"Открыто")) : std::wstring(Tr(L"Closed", L"Закрыто"));
                if (app.found() && name == L"ChatGPT") desc += Tr(L". Codex is built into it", L". Codex встроен в него");
                if (s.editors) desc += Util::Format(Tr(L". Editors with Codex: %d", L". Редакторов с Codex: %d"), s.editors);
                std::vector<Btn> b;
                if (app.found()) b.push_back({ s.appRunning ? Tr(L"Restart", L"Перезапустить") : Tr(L"Open", L"Открыть"), [] { if (App::Now().appRunning) App::RestartCodex(); else App::OpenCodex(); } });
                codex.push_back(Buttons("codexapp", title, desc, b));
            }
            {
                LookCli();
                std::wstring title = cliPath.empty() ? std::wstring(Tr(L"The Codex CLI isn't installed", L"Codex CLI не установлен"))
                                   : L"Codex CLI" + (cliVersion.empty() ? L"" : L" " + cliVersion);
                std::wstring desc = cliPath.empty() ? std::wstring(Tr(L"npm i -g @openai/codex installs it. Continuing stopped conversations needs it", L"Установка: npm i -g @openai/codex. Он нужен, чтобы продолжать остановленные разговоры"))
                                  : CliFolder() + (s.terminals ? Util::Format(Tr(L". Running in terminals: %d", L". Запущено в терминалах: %d"), s.terminals) : L"");
                codex.push_back(Buttons("cli", title, desc, {}));
            }
            {
                auto file = Codex::AuthFile();
                std::wstring desc = s.keyring ? std::wstring(Tr(L"config.toml says cli_auth_credentials_store = \"keyring\" (or \"auto\"), so Codex keeps its sign-in in Windows Credential Manager. Set it to \"file\" to switch accounts",
                                                                 L"В config.toml указано cli_auth_credentials_store = \"keyring\" (или \"auto\"), и Codex хранит вход в диспетчере учётных данных Windows. Поставьте \"file\", чтобы переключать аккаунты"))
                                              : Util::Tilde(file) + Tr(L". Every Codex reads its sign-in from here", L". Отсюда вход читает любой Codex");
                wchar_t env[8];
                if (!s.keyring && GetEnvironmentVariableW(L"CODEX_HOME", env, ARRAYSIZE(env))) desc += L" (CODEX_HOME)";
                codex.push_back(Buttons("authfile", Tr(L"Sign-in file", L"Файл входа"), desc,
                                        { { Tr(L"Show", L"Показать"), [file] { Util::Open(L"explorer.exe", L"/select,\"" + file + L"\""); } } }, false, s.keyring ? T.danger : D2D1_COLOR_F{}));
            }
            codex.push_back(Buttons("folder", Tr(L"Saved accounts", L"Сохранённые аккаунты"),
                                    Tr(L"Encrypted for your Windows user, in ", L"Зашифрованы для вашего пользователя Windows, в ") + Util::Tilde(Paths::Root),
                                    { { Tr(L"Open folder", L"Открыть папку"), [] { CreateDirectoryW(Paths::Root.c_str(), nullptr); Util::Open(Paths::Root); }, false, true, "folder" } }));
            y += Group(y, Tr(L"Codex on this PC", L"Codex на этом компьютере"), codex);
        }

        // ------------------------------------------------------------ the window

        void NavItem(int i, RectF r) {
            UINT id = Id("nav", i);
            float h = HotT(id), p = PressT(id), sel = A(id ^ 0x5E, page == i ? 1.f : 0.f, 140);
            auto hover = T.dark ? Rgba(0xFFFFFF, .04f) : Rgba(0x000000, .035f), chosen = T.dark ? Rgba(0xFFFFFF, .07f) : Rgba(0x000000, .05f);
            auto bg = Mix(Mix(D2D1::ColorF(0, 0, 0, 0), hover, h), chosen, sel);
            if (p > 0) bg = Mix(bg, T.layerPress, p);
            if (bg.a > 0.002f) Fill(r, bg, 4);
            Glyph(Pages[i].glyph, { r.x + 10, r.y, 24, r.h }, 16, T.text);
            Text(Tr(Pages[i].en, Pages[i].ru), { r.x + 44, r.y, r.w - 52, r.h }, Font(14), T.text);
            Hit(id, r, [this, i] { Go(i); });
            Describe(Tr(Pages[i].en, Pages[i].ru), Role::Tab, page == i ? 1 : 0);
            FocusRing(id, r, 4);
        }

        void Build() override {
            auto c = Client();
            x0 = NavW;
            cw = std::min(MaxContentW, c.w - NavW - 28);

            Text(Tr(L"Settings", L"Настройки"), { 24, 14, NavW - 36, 40 }, Font(20, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), T.text);
            for (int i = 0; i < PageCount; i++) NavItem(i, { 12, NavTop + i * NavStep, NavW - 28, NavItemH });
            // The selection mark glides to the chosen page, as in Windows 11.
            float mark = A(Id("nav-mark"), NavTop + page * NavStep + 10, 150);
            Fill({ 12, mark, 3, 16 }, T.accent, 1.5f);

            BeginScroll({ NavW - 8, 0, c.w - NavW + 8, c.h });
            float y = 14 - scrollY;
            Text(Tr(Pages[page].en, Pages[page].ru), { x0 + 2, y, cw, 44 }, Font(28, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), T.text);
            y += 40;
            switch (page) {
            case 0: General(y); break;
            case 1: Switching(y); break;
            case 2: Limits(y); break;
            case 3: AccountsPage(y); break;
            default: About(y); break;
            }
            y += 28;

            EndScroll(y + scrollY);
        }

        bool OnKey(UINT vk, bool alt) override {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (!recording) {
                // Ctrl+Tab and Ctrl+PgDn go to the next page, as in tabbed windows.
                if (ctrl && (vk == VK_TAB || vk == VK_NEXT || vk == VK_PRIOR)) { Go(page + (vk == VK_PRIOR || (vk == VK_TAB && shift) ? -1 : 1)); return true; }
                return false;
            }
            if (vk == VK_ESCAPE) { StopRecording(); Invalidate(); return true; }
            if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN) return true;
            UINT mods = 0;
            if (ctrl) mods |= MOD_CONTROL;
            if (alt || (GetKeyState(VK_MENU) & 0x8000)) mods |= MOD_ALT;
            if (shift) mods |= MOD_SHIFT;
            if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;
            if (!(mods & (MOD_CONTROL | MOD_ALT | MOD_WIN))) {
                hotkeyNote = Tr(L"Add Ctrl, Alt or Win so the shortcut doesn't get in the way of typing", L"Добавьте Ctrl, Alt или Win, чтобы сочетание не мешало набору текста"); hotkeyNoteBad = true;
                Invalidate(); return true;
            }
            recording = false;
            bool ok = App::SetHotkey(Hotkeys::FromKeys(mods, vk));
            hotkeyNote = ok ? L"" : Tr(L"Another app already uses it, choose a different one", L"Оно занято другой программой, выберите другое");
            hotkeyNoteBad = !ok;
            Invalidate();
            return true;
        }

        // Narrower than this, the page's text and its controls would run into each other.
        LRESULT Extra(UINT m, WPARAM, LPARAM l, bool& handled) override {
            if (m != WM_GETMINMAXINFO || !hwnd) return 0;
            auto mm = (MINMAXINFO*)l;
            mm->ptMinTrackSize.x = (LONG)(780 * scale);
            mm->ptMinTrackSize.y = (LONG)(460 * scale);
            handled = true;
            return 0;
        }

        void OnDeactivate() override { StopRecording(); }
        void OnClose() override { StopRecording(); Hide(); }
    };

    SettingsW* win = nullptr;
}

namespace SettingsWin {
    void Show() {
        if (!win) {
            win = new SettingsW(); win->Start();
        }
        // Where it was left; the first time, in the middle of the screen the pointer is on.
        if (!win->Visible() && !win->RestorePlace(L"settings") && !win->placed) {
            win->placed = true;
            POINT pt; GetCursorPos(&pt);
            MONITORINFO mi{ sizeof mi }; GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
            RECT wr; GetWindowRect(win->hwnd, &wr);
            int ww = std::min<int>(wr.right - wr.left, mi.rcWork.right - mi.rcWork.left - 40), wh = std::min<int>(wr.bottom - wr.top, mi.rcWork.bottom - mi.rcWork.top - 40);
            SetWindowPos(win->hwnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2,
                         mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (IsIconic(win->hwnd)) ShowWindow(win->hwnd, SW_RESTORE);
        win->Show(true);
    }
    bool Visible() { return win && win->Visible(); }
    void Restyle() { if (win) win->Restyle(); }
    void Invalidate() { if (win) win->Invalidate(); }
}
