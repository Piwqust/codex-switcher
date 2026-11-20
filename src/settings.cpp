#include "app.h"
#include "codex.h"
#include "web.h"
#include <shellapi.h>

using namespace Gfx;

namespace {
    const float Pad = 24;

    struct SettingsW : Surface {
        bool recording = false;
        std::wstring hotkeyNote;
        bool hotkeyNoteBad = false;

        void Start() {
            backdrop = 2;
            Create(Title(), WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, 0, 620, 760);
        }

        static const wchar_t* Title() { return Tr(L"Settings — Codex Accounts", L"Настройки — Аккаунты Codex"); }
        float Wd() { return Client().w; }

        // A Windows 11 settings row: title and description on the left, a control on the right.
        float Card(float y, const std::wstring& title, const std::wstring& desc, float controlW, float minH = 68, D2D1_COLOR_F descColor = {}) {
            float w = Wd() - 2 * Pad, textW = w - 32 - controlW - (controlW > 0 ? 16 : 0);
            float dh = desc.empty() ? 0 : Measure(desc, Font(12), textW, true).height;
            float h = std::max(minH, 24 + 20 + (dh ? dh + 2 : 0));
            RectF r{ Pad, y, w, h };
            Fill(r, T.card, 6); Stroke(r, T.cardStroke, 6);
            float ty = y + (h - 20 - (dh ? dh + 2 : 0)) / 2;
            Text(title, { r.x + 16, ty, textW, 20 }, Font(14), T.text);
            if (dh) Text(desc, { r.x + 16, ty + 22, textW, dh }, Font(12), descColor.a > 0 ? descColor : T.text2, Align::Left, false, true);
            return h;
        }

        float Section(float y, const wchar_t* t) { Text(t, { Pad + 2, y + 20, 400, 20 }, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text); return 48; }

        // Each control has a key of its own, so its hover state and the checks' names stay put
        // whatever the language.
        float ToggleRow(float y, const char* key, const std::wstring& title, const std::wstring& desc, bool on, std::function<void()> flip, bool enabled = true) {
            float h = Card(y, title, desc, 40);
            Toggle(Id(key), Wd() - Pad - 16 - 40, y + h / 2 - 10, on, [this, flip] { flip(); Invalidate(); }, enabled);
            return h + 4;
        }

        float ButtonRow(float y, const char* key, const std::wstring& title, const std::wstring& desc, const std::wstring& label, std::function<void()> click, bool enabled = true, bool primary = false) {
            float bw = std::max(130.f, Measure(label, Font(14)).width + 32);
            float h = Card(y, title, desc, bw);
            Button(Id(key), { Wd() - Pad - 16 - bw, y + h / 2 - 16, bw, 32 }, label, primary, click, false, enabled);
            return h + 4;
        }

        // A segmented choice, like the Windows 11 settings pages use for a few fixed values.
        float Segments(float y, const std::wstring& title, const std::wstring& desc, const std::vector<std::wstring>& labels, int current, float segW, std::function<void(int)> pick, const char* key) {
            // Each segment as wide as its label needs, at least segW.
            std::vector<float> widths;
            float total = 8;
            for (auto& l : labels) { widths.push_back(std::max(segW, Measure(l, Font(13)).width + 28)); total += widths.back(); }
            float h = Card(y, title, desc, total);
            RectF seg{ Wd() - Pad - 16 - total, y + h / 2 - 17, total, 34 };
            Fill(seg, T.control, 6); Stroke(seg, T.controlStroke, 6);
            float x = seg.x + 4;
            for (int i = 0; i < (int)labels.size(); i++) {
                RectF b{ x, seg.y + 4, widths[i], 26 };
                x += widths[i];
                UINT id = Id(key, i);
                float t = A(id ^ 0x5E, current == i ? 1.f : 0.f, 140), hh = HotT(id);
                Fill(b, Mix(Mix(D2D1::ColorF(0, 0, 0, 0), T.layerHover, hh), T.accent, t), 4);
                Text(labels[i], b, Font(13), Mix(T.text, T.onAccent, t), Align::Center);
                Hit(id, b, [this, pick, i] { pick(i); Invalidate(); });
                FocusRing(id, b, 4);
            }
            return h + 4;
        }

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

        // A card with a title, a description and up to two buttons on the right.
        float Buttons(float y, const std::wstring& title, const std::wstring& desc, D2D1_COLOR_F descColor,
                      const std::vector<std::pair<std::wstring, std::function<void()>>>& buttons, const char* key) {
            float total = 0;
            std::vector<float> widths;
            for (auto& b : buttons) { float bw = std::max(110.f, Measure(b.first, Font(14)).width + 30); widths.push_back(bw); total += bw + 8; }
            float h = Card(y, title, desc, total > 0 ? total - 8 : 0, 68, descColor);
            float x = Wd() - Pad - 16;
            for (int i = (int)buttons.size() - 1; i >= 0; i--) { x -= widths[i]; Button(Id(key, i), { x, y + h / 2 - 16, widths[i], 32 }, buttons[i].first, false, buttons[i].second); x -= 8; }
            return h + 4;
        }

        void Build() override {
            float y = 16 - scrollY, w = Wd();
            Text(Tr(L"Settings", L"Настройки"), { Pad, y, 300, 40 }, Font(28, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), T.text);
            y += 44;
            auto& s = App::Now();

            // ---------------------------------------------------------------- Codex
            y += Section(y, L"Codex");
            {
                auto app = Codex::App();
                auto name = Codex::AppName();
                std::wstring title = !app.found() ? Util::Format(Tr(L"The %s app isn't installed", L"Приложение %s не установлено"), name.c_str())
                                   : Util::Format(Tr(L"%s app ", L"Приложение %s "), name.c_str()) + (app.version.empty() ? L"" : app.version + L" ") + L"· " + (app.kind == L"Msix" ? L"Microsoft Store" : Tr(L"your path", L"ваш путь"));
                std::wstring desc = !app.found() ? std::wstring(Tr(L"The CLI and editors still switch. The app is in the Microsoft Store", L"CLI и редакторы всё равно переключаются. Приложение есть в Microsoft Store"))
                                  : s.appRunning ? std::wstring(Tr(L"Open now. Switching closes it and opens it again with the new account", L"Сейчас открыто. Переключение закроет его и откроет снова с новым аккаунтом"))
                                  : std::wstring(Tr(L"Closed. It starts with whichever account is in use", L"Закрыто. Оно запустится с тем аккаунтом, который используется"));
                if (app.found() && name == L"ChatGPT") desc += Tr(L". Codex lives inside it", L". Codex находится внутри него");
                if (s.editors) desc += Util::Format(Tr(L". Editors with Codex: %d", L". Редакторов с Codex: %d"), s.editors);
                std::vector<std::pair<std::wstring, std::function<void()>>> b;
                if (app.found()) b.push_back({ s.appRunning ? Tr(L"Restart", L"Перезапустить") : Tr(L"Open", L"Открыть"), [] { if (App::Now().appRunning) App::RestartCodex(); else App::OpenCodex(); } });
                y += Buttons(y, title, desc, {}, b, "codexapp");
            }
            {
                LookCli();
                std::wstring title = cliPath.empty() ? std::wstring(Tr(L"The Codex CLI isn't installed", L"Codex CLI не установлен"))
                                   : L"Codex CLI" + (cliVersion.empty() ? L"" : L" " + cliVersion);
                std::wstring desc = cliPath.empty() ? std::wstring(Tr(L"npm i -g @openai/codex installs it. It's needed to continue conversations the limit stopped", L"Установка: npm i -g @openai/codex. Он нужен, чтобы продолжать разговоры, остановленные лимитом"))
                                  : CliFolder() + (s.terminals ? Util::Format(Tr(L". Running in terminals: %d", L". Запущено в терминалах: %d"), s.terminals) : L"");
                y += Buttons(y, title, desc, {}, {}, "cli");
            }
            {
                auto file = Codex::AuthFile();
                std::wstring desc = s.keyring ? std::wstring(Tr(L"config.toml says cli_auth_credentials_store = \"keyring\" (or \"auto\"), so Codex keeps its sign-in in Windows Credential Manager. Set it to \"file\" to switch accounts",
                                                                 L"В config.toml указано cli_auth_credentials_store = \"keyring\" (или \"auto\"), и Codex хранит вход в диспетчере учётных данных Windows. Поставьте \"file\", чтобы переключать аккаунты"))
                                              : Util::Tilde(file) + Tr(L". Every Codex reads its sign-in from here; switching writes the chosen account's copy to it", L". Отсюда вход читает любой Codex; переключение записывает сюда копию выбранного аккаунта");
                wchar_t env[8];
                if (!s.keyring && GetEnvironmentVariableW(L"CODEX_HOME", env, ARRAYSIZE(env))) desc += Tr(L" (CODEX_HOME)", L" (CODEX_HOME)");
                y += Buttons(y, Tr(L"Sign-in file", L"Файл входа"), desc, s.keyring ? T.danger : D2D1_COLOR_F{},
                             { { Tr(L"Show", L"Показать"), [file] { Util::Open(L"explorer.exe", L"/select,\"" + file + L"\""); } } }, "authfile");
            }

            // ---------------------------------------------------------------- switching
            y += Section(y, Tr(L"Switching", L"Переключение"));
            {
                auto name = Codex::AppName();
                y += ToggleRow(y, "open-after", Util::Format(Tr(L"Open %s when choosing an account", L"Открывать %s при выборе аккаунта"), name.c_str()),
                               Util::Format(Tr(L"A click on an account switches Codex to it and opens %s with it; on the account in use it just opens %s. Without it a click only switches",
                                               L"Щелчок по аккаунту переключает Codex на него и открывает %s с ним, а по используемому — просто открывает %s. Без этого щелчок только переключает"), name.c_str(), name.c_str()),
                               App::OpenAfterSwitch(), [] { App::SetOpenAfterSwitch(!App::OpenAfterSwitch()); }, s.appInstalled);
                y += ToggleRow(y, "restart", Util::Format(Tr(L"Restart %s when switching", L"Перезапускать %s при переключении"), name.c_str()),
                               Util::Format(Tr(L"%s closes and opens again with the new account, and editors such as VS Code reconnect. Without it the app keeps the old account until you restart it",
                                               L"%s закроется и откроется с новым аккаунтом, редакторы вроде VS Code переподключатся. Без этого приложение останется на старом аккаунте до перезапуска"), name.c_str()),
                               App::RestartOnSwitch(), [] { App::SetRestartOnSwitch(!App::RestartOnSwitch()); });
                y += ToggleRow(y, "ask", Util::Format(Tr(L"Ask before closing %s", L"Спрашивать перед закрытием %s"), name.c_str()),
                               Tr(L"Tasks running in the app or in an editor stop when it closes", L"Задачи, запущенные в приложении или редакторе, остановятся при его закрытии"),
                               App::AskBeforeRestart(), [] { App::SetAskBeforeRestart(!App::AskBeforeRestart()); }, App::RestartOnSwitch());
            }
            y += ToggleRow(y, "continue", Tr(L"Continue conversations the limit stopped", L"Продолжать разговоры, остановленные лимитом"),
                           cliPath.empty() ? std::wstring(Tr(L"Needs the Codex CLI (npm i -g @openai/codex)", L"Нужен Codex CLI (npm i -g @openai/codex)"))
                                           : std::wstring(Tr(L"After a switch that restarts the app, conversations of the last 24 hours that stopped at the usage limit get “Continue the previous task.” with the new account. Each gets it once",
                                                             L"После переключения с перезапуском приложения разговоры последних суток, остановленные лимитом, получают «Continue the previous task.» уже с новым аккаунтом. Каждый — один раз")),
                           App::ContinueStopped(), [] { App::SetContinueStopped(!App::ContinueStopped()); }, !cliPath.empty());
            y += ToggleRow(y, "hide-after", Tr(L"Hide the window after switching", L"Прятать окно после переключения"), Tr(L"The list goes away as soon as you choose", L"Список уберётся, как только вы выберете аккаунт"),
                           App::HideAfterSwitch(), [] { App::SetHideAfterSwitch(!App::HideAfterSwitch()); });

            // ---------------------------------------------------------------- limits
            y += Section(y, Tr(L"Limits", L"Лимиты"));
            {
                int values[] = { 0, 5, 15, 60 };
                int cur = 2; for (int i = 0; i < 4; i++) if (App::UsageMinutes() == values[i]) cur = i;
                y += Segments(y, Tr(L"Refresh limits", L"Обновлять лимиты"),
                              Tr(L"Asks OpenAI for each account's 5-hour and weekly limits with that account's own sign-in, in the background or only when the window opens",
                                 L"Спрашивает у OpenAI 5-часовой и недельный лимиты каждого аккаунта с его собственным входом: в фоне или только при открытии окна"),
                              { Tr(L"On open", L"При открытии"), Tr(L"5 min", L"5 мин"), Tr(L"15 min", L"15 мин"), Tr(L"1 h", L"1 ч") }, cur, 66,
                              [values](int i) { App::SetUsageMinutes(values[i]); }, "usage");
            }
            {
                const wchar_t* modes[] = { L"icon", L"percent", L"hidden" };
                int cur = 0; for (int i = 0; i < 3; i++) if (App::TrayMode() == modes[i]) cur = i;
                y += Segments(y, Tr(L"Tray icon", L"Значок в трее"),
                              Tr(L"The switcher's icon, or what is left of the 5-hour window of the account in use, or no icon: Ctrl+Alt+X and the Start menu still open it",
                                 L"Значок переключателя, сколько осталось в 5-часовом окне используемого аккаунта или без значка: Ctrl+Alt+X и меню «Пуск» всё равно его откроют"),
                              { Tr(L"Icon", L"Значок"), Tr(L"% left", L"% остатка"), Tr(L"Hidden", L"Скрыт") }, cur, 80,
                              [](int i) { const wchar_t* m[] = { L"icon", L"percent", L"hidden" }; App::SetTrayMode(m[i]); }, "traymode");
            }
            y += ToggleRow(y, "autowarm", Tr(L"Start 5-hour windows automatically", L"Запускать 5-часовые окна автоматически"),
                           Tr(L"For every account: when its window is idle, one tiny request starts the next one, so it resets sooner. For a few accounts only, use ⋯ on each. Skips accounts whose week is used up",
                              L"Для всех аккаунтов: когда окно простаивает, один крошечный запрос запускает следующее, и оно раньше сбросится. Только для некоторых — через ⋯ у аккаунта. Аккаунты с исчерпанной неделей пропускаются"),
                           App::AutoWarmup(), [] { App::SetAutoWarmup(!App::AutoWarmup()); });
            {
                std::wstring times;
                for (auto& t : App::WarmupTimes()) times += (times.empty() ? L"" : L", ") + t;
                float bw = std::max(150.f, Measure(times, Font(14)).width + 30);
                float h = Card(y, Tr(L"Start them at set times", L"Запускать в заданное время"),
                               Tr(L"Every day at these times every account's window starts, so they reset when you want them. A time the PC slept through is skipped",
                                  L"Каждый день в это время запускаются окна всех аккаунтов, чтобы они сбрасывались, когда вам нужно. Время, проспанное компьютером, пропускается"), bw + 56);
                Toggle(Id("timed"), w - Pad - 16 - 40, y + h / 2 - 10, App::TimedWarmup(), [this] { App::SetTimedWarmup(!App::TimedWarmup()); Invalidate(); });
                Button(Id("times"), { w - Pad - 16 - 56 - bw, y + h / 2 - 16, bw, 32 }, times.empty() ? Tr(L"Set times", L"Задать время") : times, false, [this] {
                    std::wstring text;
                    for (auto& t : App::WarmupTimes()) text += (text.empty() ? L"" : L", ") + t;
                    for (;;) {
                        if (!Dialogs::Prompt(hwnd, Tr(L"Times to start the windows", L"Время запуска окон"), Tr(L"24-hour clock, separated by commas, for example 8:00, 13:00, 18:00", L"В 24-часовом формате, через запятую, например 8:00, 13:00, 18:00"), text, false, false, Tr(L"Save", L"Сохранить"))) break;
                        if (App::SetWarmupTimes(text)) { App::SetTimedWarmup(true); break; }
                        Dialogs::Info(hwnd, Tr(L"That doesn't read as times", L"Это не похоже на время"), Tr(L"Write them like 8:00, 13:30", L"Пишите их так: 8:00, 13:30"));
                    }
                    Invalidate();
                });
                y += h + 4;
            }

            // ---------------------------------------------------------------- accounts
            y += Section(y, Tr(L"Accounts", L"Аккаунты"));
            y += ButtonRow(y, "add", Tr(L"Sign in with ChatGPT", L"Вход через ChatGPT"),
                           Tr(L"Opens the sign-in page in your browser, the same way codex login does. It always asks for the account, so you can add one the browser isn't signed in to",
                              L"Открывает страницу входа в браузере, как codex login. Она всегда спрашивает аккаунт, так что можно добавить не тот, в котором уже открыт браузер"),
                           Tr(L"Add account", L"Добавить"), [] { App::AddAccount(); }, !App::SigningIn(), true);
            y += ButtonRow(y, "authjson", Tr(L"From an auth.json file", L"Из файла auth.json"),
                           Tr(L"A sign-in saved by Codex or with ⋯ → Save auth.json", L"Вход, сохранённый Codex или через ⋯ → Сохранить auth.json"),
                           Tr(L"Choose file…", L"Выбрать файл…"), [this] { App::ImportFile(hwnd); });
            if (s.legacy > 0 || Util::Exists(Accounts::LegacyFile()))
                y += ButtonRow(y, "legacy", Tr(L"From Codex Switcher on this PC", L"Из Codex Switcher на этом компьютере"),
                               s.legacy > 0 ? Util::Format(Tr(L"%s there that aren't here yet. Names come along, no signing in again", L"Там %s, которых здесь ещё нет. Имена перенесутся, входить заново не нужно"),
                                                           Util::Count(s.legacy, L"account", L"accounts", L"аккаунт", L"аккаунта", L"аккаунтов").c_str())
                                            : std::wstring(Tr(L"Every account from it is here already", L"Все его аккаунты уже здесь")),
                               Tr(L"Import", L"Импортировать"), [this] { App::ImportLegacy(hwnd); }, s.legacy > 0);
            y += Buttons(y, Tr(L"Backup", L"Резервная копия"),
                         Tr(L"All accounts with their sign-ins, encrypted with a password, as a file or a line of text: to move them to another PC or keep them safe. Import takes Codex Switcher's exports too",
                            L"Все аккаунты со входами, зашифрованные паролем, файлом или строкой текста: чтобы перенести на другой компьютер или сохранить. Импорт принимает и экспорт Codex Switcher"), {},
                         { { Tr(L"Import…", L"Импорт…"), [this] { App::ImportBackup(hwnd); } }, { Tr(L"Back up…", L"Сохранить…"), [this] { App::ExportAll(hwnd); } } }, "backup");
            y += ToggleRow(y, "mask", Tr(L"Hide emails", L"Скрывать email"),
                           Tr(L"For screen sharing and streams: emails, and names made from them, show as jo•••@g•••.com. One account at a time: ⋯ → Hide its email",
                              L"Для демонстрации экрана и стримов: email и имена из них показываются как jo•••@g•••.com. Для отдельного аккаунта: ⋯ → Скрывать его email"),
                           App::MaskAll(), [] { App::SetMaskAll(!App::MaskAll()); });

            // ---------------------------------------------------------------- browser
            y += Section(y, Tr(L"In a browser", L"В браузере"));
            y += ToggleRow(y, "web", Tr(L"Account list in a browser", L"Список аккаунтов в браузере"),
                           Web::Problem().empty() ? std::wstring(Tr(L"The same list as a web page, to switch from a phone, a tablet or another computer. Anyone with the link can switch your accounts",
                                                                    L"Тот же список веб-страницей, чтобы переключаться с телефона, планшета или другого компьютера. Любой, у кого есть ссылка, может переключать ваши аккаунты"))
                                                  : Web::Problem(),
                           App::WebOn(), [] { App::SetWebOn(!App::WebOn()); });
            if (App::WebOn()) {
                y += ToggleRow(y, "weblan", Tr(L"Allow other devices", L"Пускать другие устройства"),
                               Tr(L"From your network or Tailscale; otherwise only this PC. The link isn't encrypted: allow this only on networks you trust. Windows may ask to let it through the firewall",
                                  L"Из вашей сети или Tailscale; иначе только с этого компьютера. Ссылка не шифруется: включайте только в сетях, которым доверяете. Windows может спросить разрешение для брандмауэра"),
                               App::WebLan(), [] { App::SetWebLan(!App::WebLan()); });
                if (Web::Running()) {
                    auto url = Web::Url(!App::WebLan());
                    y += Buttons(y, Tr(L"Link", L"Ссылка"), url, {},
                                 { { Tr(L"New link", L"Новая ссылка"), [] { Web::NewKey(); Web::Apply(); } },
                                   { Tr(L"Copy", L"Копировать"), [this, url] { Util::CopyText(hwnd, url); } },
                                   { Tr(L"Open", L"Открыть"), [url] { Util::Open(url); } } }, "weblink");
                }
            }

            // ---------------------------------------------------------------- startup and look
            y += Section(y, Tr(L"Startup and look", L"Запуск и вид"));
            y += ToggleRow(y, "autostart", Tr(L"Start with Windows", L"Запускать вместе с Windows"), Tr(L"The switcher waits in the tray, keeps the limits current and saves renewed sign-ins", L"Переключатель ждёт в трее, обновляет лимиты и сохраняет продлённые входы"),
                           App::Autostart(), [] { App::SetAutostart(!App::Autostart()); });
            y += ToggleRow(y, "keep-tray", Tr(L"Stay in the tray when closed", L"Оставаться в трее после закрытия окна"), Tr(L"Keeps the switcher at hand, so it opens instantly", L"Так переключатель всегда под рукой и открывается мгновенно"),
                           App::KeepInTray(), [] { App::SetKeepInTray(!App::KeepInTray()); });
            {
                const wchar_t* values[] = { L"auto", L"light", L"dark" };
                int cur = 0; for (int i = 0; i < 3; i++) if (App::Theme() == values[i]) cur = i;
                y += Segments(y, Tr(L"Theme", L"Тема"), Tr(L"System follows Windows", L"Системная повторяет Windows"),
                              { Tr(L"System", L"Системная"), Tr(L"Light", L"Светлая"), Tr(L"Dark", L"Тёмная") }, cur, 96,
                              [](int i) { const wchar_t* v[] = { L"auto", L"light", L"dark" }; App::SetTheme(v[i]); }, "theme");
            }

            // ---------------------------------------------------------------- quick access
            y += Section(y, Tr(L"Quick access", L"Быстрый доступ"));
            {
                auto hk = App::Hotkey();
                std::wstring desc = recording ? Tr(L"Press a shortcut, for example Ctrl + Alt + X. Esc cancels", L"Нажмите сочетание, например Ctrl + Alt + X. Esc — отмена")
                                  : !hotkeyNote.empty() ? hotkeyNote
                                  : App::HotkeyFailed() ? Tr(L"Another app already uses this shortcut, choose a different one", L"Это сочетание занято другой программой, выберите другое")
                                  : Tr(L"Brings up the account list from any app", L"Вызывает список аккаунтов из любой программы");
                bool bad = !recording && (hotkeyNoteBad || (hotkeyNote.empty() && App::HotkeyFailed()));
                float h = Card(y, Tr(L"Shortcut", L"Горячая клавиша"), desc, 200, 68, bad ? T.danger : D2D1_COLOR_F{});
                std::wstring label = recording ? Tr(L"Press keys…", L"Нажмите сочетание…") : hk.empty() ? Tr(L"Not set", L"Не задана") : Hotkeys::Pretty(hk);
                RectF b{ w - Pad - 16 - (hk.empty() ? 160.f : 196.f), y + h / 2 - 16, 160, 32 };
                Button(Id("hk"), b, label, recording, [this] { recording = true; hotkeyNote.clear(); App::SuspendHotkey(); Invalidate(); });
                if (!hk.empty()) IconButton(Id("hk-clear"), { b.R() + 4, b.y, 32, 32 }, Glyphs::Clear, [this] { App::SetHotkey(L""); hotkeyNote.clear(); Invalidate(); });
                y += h + 4;
            }
            {
                auto taken = App::AccountHotkeysTaken();
                std::wstring desc;
                if (!taken.empty() && App::AccountHotkeys()) {
                    desc = Tr(L"Taken by another app:", L"Заняты другой программой:");
                    for (size_t i = 0; i < taken.size(); i++) desc += (i ? L"," : L"") + Util::Format(L" Ctrl + Shift + Alt + %d", taken[i]);
                } else desc = Tr(L"Ctrl + Shift + Alt + 1 switches to the first account, + 2 to the second, and so on. Works from any app",
                                 L"Ctrl + Shift + Alt + 1 переключает на первый аккаунт, + 2 на второй и так далее. Работает из любой программы");
                float h = Card(y, Tr(L"Ctrl + Shift + Alt + number switches", L"Ctrl + Shift + Alt + цифра переключает"), desc, 40, 68, !taken.empty() && App::AccountHotkeys() ? T.danger : D2D1_COLOR_F{});
                Toggle(Id("acchk"), w - Pad - 16 - 40, y + h / 2 - 10, App::AccountHotkeys(), [this] { App::SetAccountHotkeys(!App::AccountHotkeys()); Invalidate(); });
                y += h + 4;
            }
            {
                float h = Card(y, Tr(L"      Accounts on the taskbar", L"      Аккаунты на панели задач"), Tr(L"      Pin the switcher to the taskbar: right-click it to switch from there", L"      Закрепите переключатель на панели задач: правый клик по нему позволит переключаться оттуда"), 0);
                Glyph(Glyphs::Pin, { Pad + 12, y + h / 2 - 12, 24, 24 }, 16, T.text2);
                y += h + 4;
            }

            // ---------------------------------------------------------------- language
            y += Section(y, Tr(L"Language", L"Язык"));
            {
                const wchar_t* values[] = { L"auto", L"en", L"ru" };
                auto current = L10n::Choice();
                int cur = 0; for (int i = 0; i < 3; i++) if (current == values[i]) cur = i;
                y += Segments(y, Tr(L"Interface language", L"Язык интерфейса"),
                              Tr(L"System follows Windows: Russian when Windows is in Russian, English otherwise", L"Системный повторяет Windows: русский, если Windows на русском, иначе английский"),
                              { Tr(L"System", L"Системный"), L"English", L"Русский" }, cur, 84,
                              [this](int i) { const wchar_t* v[] = { L"auto", L"en", L"ru" }; App::SetLanguage(v[i]); SetWindowTextW(hwnd, Title()); }, "lang");
            }

            // ---------------------------------------------------------------- about
            y += Section(y, Tr(L"About", L"О программе"));
            {
                float h = Card(y, Util::Format(Tr(L"Codex Account Switcher %s", L"Codex Account Switcher %s"), Update::Version()),
                               Tr(L"Saved accounts are kept encrypted for your Windows user in ", L"Сохранённые аккаунты лежат зашифрованными для вашего пользователя Windows в ") + Util::Tilde(Paths::Root), 150);
                Button(Id("folder"), { w - Pad - 16 - 150, y + h / 2 - 16, 150, 32 }, Tr(L"Data folder", L"Папка данных"), false, [] {
                    CreateDirectoryW(Paths::Root.c_str(), nullptr);
                    Util::Open(Paths::Root);
                });
                y += h + 4;
            }
            if (!Paths::Isolated || Update::State() != Update::Phase::Idle) {
                using P = Update::Phase;
                auto st = Update::State();
                std::wstring title, desc;
                D2D1_COLOR_F descColor{};
                switch (st) {
                case P::Checking: title = Tr(L"Checking for updates…", L"Проверяю обновления…"); break;
                case P::Ready:
                    title = Util::Format(Tr(L"Version %s is out", L"Вышла версия %s"), Update::Latest().c_str());
                    desc = Tr(L"Updating takes a few seconds. Codex stays as it is, the switcher comes back by itself", L"Обновление займёт пару секунд. Codex не затронется, переключатель перезапустится сам");
                    break;
                case P::Downloading: title = Util::Format(Tr(L"Downloading %s…", L"Скачиваю %s…"), Update::Latest().c_str()); break;
                case P::Installing: title = Tr(L"Installing the update…", L"Устанавливаю обновление…"); break;
                case P::Failed: title = Tr(L"Couldn't update", L"Не удалось обновить"); desc = Update::Problem(); descColor = T.danger; break;
                case P::Current: {
                    auto when = Util::When(Update::CheckedAt());
                    for (auto word : { Tr(L"Today", L"Сегодня"), Tr(L"Yesterday", L"Вчера") })
                        if (when.rfind(word, 0) == 0) when[0] = towlower(when[0]);
                    title = Tr(L"The switcher is up to date", L"Установлена последняя версия"); desc = Tr(L"Checked ", L"Проверено: ") + when;
                    break;
                }
                default: title = Tr(L"Updates", L"Обновления"); desc = Tr(L"New versions come from the project's GitHub releases", L"Новые версии берутся из релизов проекта на GitHub"); break;
                }
                float h = Card(y, title, desc, 150, 68, descColor);
                RectF b{ w - Pad - 16 - 150, y + h / 2 - 16, 150, 32 };
                bool busy = st == P::Checking || st == P::Downloading || st == P::Installing;
                if (st == P::Ready || st == P::Downloading || st == P::Installing)
                    Button(Id("update"), b, busy ? Tr(L"Updating…", L"Обновляю…") : Tr(L"Update", L"Обновить"), true, [] { Update::Install(); }, false, !busy);
                else
                    Button(Id("update"), b, Tr(L"Check now", L"Проверить"), false, [] { Update::Check(true); }, false, !busy);
                y += h + 4;
                y += ToggleRow(y, "autoupdate", Tr(L"Check for updates automatically", L"Проверять обновления автоматически"),
                               Tr(L"Looks for a new release at start and every 6 hours. Only the version is requested from GitHub, nothing about you or your accounts is sent", L"Ищет новый релиз при запуске и раз в 6 часов. У GitHub запрашивается только номер версии, ничего о вас и аккаунтах не отправляется"),
                               Update::Auto(), [] { Update::SetAuto(!Update::Auto()); });
            }
            y += 20;

            float total = y + scrollY;
            scrollMax = std::max(0.f, total - Client().h);
            if (scrollY > scrollMax) { scrollY = scrollMax; Invalidate(); }
            // Thin scroll indicator, as in Windows 11 when the pointer is over the page.
            if (scrollMax > 0) {
                float ch = Client().h, th = std::max(40.f, ch * ch / total), ty = (ch - th) * scrollY / scrollMax;
                Fill({ Client().w - 6, ty + 2, 3, th - 4 }, T.text3, 1.5f);
            }
        }

        bool OnKey(UINT vk, bool alt) override {
            if (!recording) return false;
            if (vk == VK_ESCAPE) { recording = false; App::ResumeHotkey(); Invalidate(); return true; }
            if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN) return true;
            UINT mods = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
            if (alt || (GetKeyState(VK_MENU) & 0x8000)) mods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
            if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;
            if (!(mods & (MOD_CONTROL | MOD_ALT | MOD_WIN))) {
                hotkeyNote = Tr(L"Add Ctrl, Alt or Win so the shortcut doesn't get in the way of typing", L"Добавьте Ctrl, Alt или Win, чтобы сочетание не мешало набору текста"); hotkeyNoteBad = true;
                Invalidate(); return true;
            }
            recording = false;
            bool ok = App::SetHotkey(Hotkeys::FromKeys(mods, vk));
            hotkeyNote = ok ? L"" : Tr(L"Another app already uses this shortcut, choose a different one", L"Это сочетание занято другой программой, выберите другое");
            hotkeyNoteBad = !ok;
            Invalidate();
            return true;
        }

        void OnDeactivate() override { if (recording) { recording = false; App::ResumeHotkey(); } }
        void OnClose() override { if (recording) { recording = false; App::ResumeHotkey(); } Hide(); }
    };

    SettingsW* win = nullptr;
}

namespace SettingsWin {
    void Show() {
        if (!win) {
            win = new SettingsW(); win->Start();
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
}
