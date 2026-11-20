/*
    CodexAccountSwitcher.exe                        open the picker (installs itself on first run)
    CodexAccountSwitcher.exe --tray                 start in the notification area only
    CodexAccountSwitcher.exe --startup              what Windows runs at sign-in (the tray)
    CodexAccountSwitcher.exe --switch Work          switch Codex to an account, then exit
    CodexAccountSwitcher.exe --switch Work --open   the same, then open the Codex app
    CodexAccountSwitcher.exe --add-account          sign in to another account (in the running switcher)
    CodexAccountSwitcher.exe --list                 print the accounts, the one in use marked
    CodexAccountSwitcher.exe --diagnose             print what the switcher sees of Codex here (changes nothing)
    CodexAccountSwitcher.exe --install              install or refresh the install, then open the picker
                             (--tray, --settings and --updated pass on to the installed copy)
    CodexAccountSwitcher.exe --codex-path <exe>     point at the Codex app if it isn't the Store one (remembered)

    CAS_DEMO=<folder>   environment variable: a separate switcher over stand-in accounts (see core.cpp)
*/
#include "app.h"
#include "codex.h"
#include <shellapi.h>
#include <objbase.h>
#include <cstdio>

// Output redirected to a pipe or a file: a script runs us, and wants text, not a window.
static bool Redirected() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return false;
    auto type = GetFileType(out);
    return type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK;
}

static void Print(const std::wstring& s);

static void Fail(const std::wstring& what) {
    Core::Log(what);
    if (Redirected() || GetEnvironmentVariableW(L"CAS_OFFSCREEN", nullptr, 0)) { Print(what); return; }
    MessageBoxW(nullptr, (what + Tr(L"\n\nDetails were written to ", L"\n\nПодробности записаны в ") + Paths::Log).c_str(), AppTitle(), MB_OK | MB_ICONERROR);
}

// An account by its number in the list, its id, its name or its email, whichever was given.
static bool Resolve(const std::wstring& key, Account& out) {
    auto list = Accounts::Load();
    if (!key.empty() && key.size() <= 2 && key.find_first_not_of(L"0123456789") == std::wstring::npos) {
        int n = _wtoi(key.c_str());
        if (n >= 1 && n <= (int)list.size()) { out = list[n - 1]; return true; }
    }
    for (auto& a : list) if (_wcsicmp(a.id.c_str(), key.c_str()) == 0) { out = a; return true; }
    for (auto& a : list) if (_wcsicmp(a.name.c_str(), key.c_str()) == 0) { out = a; return true; }
    for (auto& a : list) if (!a.email.empty() && _wcsicmp(a.email.c_str(), key.c_str()) == 0) { out = a; return true; }
    return false;
}

// A line on the console or into a pipe, for --list and the imports.
static void Print(const std::wstring& s) {
    auto line = s + L"\r\n";
    DWORD w = 0;
    // Started with its output redirected, a GUI program has the pipe as its standard output.
    if (Redirected()) { auto b = Util::Utf8(line); WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), b.data(), (DWORD)b.size(), &w, nullptr); return; }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!WriteConsoleW(out, line.c_str(), (DWORD)line.size(), &w, nullptr)) { auto b = Util::Utf8(line); WriteFile(out, b.data(), (DWORD)b.size(), &w, nullptr); }
    FreeConsole();
}

static int Run() {
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    struct Free { LPWSTR* p; ~Free() { LocalFree(p); } } freeArgs{ argv };
    if (!argv) argc = 0;
    auto flag = [&](const wchar_t* n) { for (int i = 1; i < argc; i++) if (_wcsicmp(argv[i], n) == 0) return true; return false; };
    auto arg = [&](const wchar_t* n) -> std::wstring { for (int i = 1; i + 1 < argc; i++) if (_wcsicmp(argv[i], n) == 0) return argv[i + 1]; return L""; };

    auto appIcon = arg(L"--make-app-icon");
    if (!appIcon.empty()) { Gfx::Init(); WinShell::WriteAppIcon(appIcon); return 0; }
    auto samples = arg(L"--icon-samples");
    if (!samples.empty()) { Gfx::Init(); WinShell::WriteIconSamples(samples); return 0; }

    auto codexPath = arg(L"--codex-path");
    if (!codexPath.empty()) {
        if (!Util::Exists(codexPath)) throw Util::Format(Tr(L"--codex-path points to %s, which doesn't exist", L"--codex-path указывает на %s, такого файла нет"), Util::Q(codexPath).c_str());
        Settings::Set(L"CodexPath", codexPath);
    }

    // What the switcher sees of Codex on this PC, for reports: nothing is changed or sent, and
    // no sign-in or email is printed.
    if (flag(L"--diagnose")) {
        Codex::Detect();
        auto app = Codex::App();
        auto p = Codex::Scan();
        Print(Util::Format(L"Switcher %s, data in %s%s", Update::Version(), Paths::Root.c_str(), Paths::Isolated ? L" (separate copy)" : L""));
        Print(app.found() ? Util::Format(L"App: %s, %s %s, %s%s", Codex::AppName().c_str(), app.kind.c_str(), app.version.c_str(), app.exe.c_str(), app.aumid.empty() ? L"" : (L", " + app.aumid).c_str())
                          : std::wstring(L"App: not found"));
        std::wstring pids;
        for (DWORD pid : p.app) pids += Util::Format(L" %lu", pid);
        Print(Util::Format(L"App running: %s%s%s", p.appRunning() ? L"yes, pid" : L"no", pids.c_str(), p.appRunning() ? (p.appVisible ? L", window shown" : L", no window (tray)") : L""));
        Print(Util::Format(L"Codex in terminals: %d, editor servers: %d, background server: %s", (int)p.cli.size(), (int)p.helpers.size(), p.daemon ? L"yes" : L"no"));
        Print(L"Sign-in file: " + Codex::AuthFile() + (Util::Exists(Codex::AuthFile()) ? L"" : L" (missing)") + (Codex::KeyringStore() ? L", but config.toml keeps the sign-in in Credential Manager" : L""));
        auto cli = Recovery::Cli();
        Print(L"Codex CLI: " + (cli.empty() ? std::wstring(L"not found") : cli));
        Print(Util::Format(L"Settings: open the app when choosing %s, restart it when switching %s, ask first %s", App::OpenAfterSwitch() ? L"on" : L"off", App::RestartOnSwitch() ? L"on" : L"off", App::AskBeforeRestart() ? L"on" : L"off"));
        return 0;
    }

    if (flag(L"--list")) {
        auto list = Accounts::Load();
        auto active = Accounts::ActiveId(list, Accounts::ReadLive());
        int n = 0;
        for (auto& a : list) Print(Util::Format(L"%c %d  %s  %s  %s", a.id == active ? L'*' : L' ', ++n, a.name.c_str(), a.email.c_str(), a.plan.c_str()));
        if (list.empty()) Print(Tr(L"No accounts yet", L"Аккаунтов пока нет"));
        return 0;
    }

    // Adding accounts without the window: an auth.json, or everything Codex Switcher has.
    auto importFile = arg(L"--import");
    if (!importFile.empty() || flag(L"--import-codex-switcher")) {
        std::wstring err;
        int added = 0, updated = 0;
        if (!importFile.empty()) {
            auto auth = Accounts::ReadAuthFile(importFile, err);
            if (auth.IsNull()) throw err;
            auto r = Accounts::Add(auth);
            if (!r.error.empty()) throw r.error;
            (r.existed ? updated : added)++;
        } else {
            auto r = Accounts::ImportLegacy();
            if (!r.error.empty()) throw r.error;
            added = r.added; updated = r.updated;
        }
        Print(Util::Format(Tr(L"Added %d, updated %d", L"Добавлено: %d, обновлено: %d"), added, updated));
        return 0;
    }

    auto target = arg(L"--switch");
    if (!target.empty()) {
        // Shortcuts and the jump list land here: no window, straight to the switch. The tray
        // copy hears about it through the vault's change event and redraws.
        Account a;
        if (!Resolve(target, a)) throw Util::Format(Tr(L"There is no account called %s", L"Аккаунта %s нет"), Util::Q(target).c_str());
        // The running switcher does it when there is one: its question before closing Codex
        // and its notification apply. Scripts (output redirected) get the result here instead.
        if (!Redirected() && !Paths::Isolated && App::Forward((flag(L"--open") ? L"open " : L"switch ") + a.id)) return 0;
        Codex::Detect();
        Accounts::SwitchOptions o;
        o.restartApp = Settings::Flag(L"RestartCodex", true);
        o.openApp = flag(L"--open");
        auto r = Accounts::Switch(a.id, o);
        if (!r.error.empty()) throw r.error;
        if (Redirected()) Print(r.already ? Util::Format(Tr(L"%s is in use already", L"%s уже используется"), a.name.c_str()) : Util::Format(Tr(L"Now using %s", L"Теперь используется %s"), a.name.c_str()));
        return 0;
    }

    // Starts 5-hour windows; for Task Scheduler, to have them reset at hours of your choosing.
    auto warm = arg(L"--warmup");
    if (!warm.empty()) {
        std::vector<Account> which;
        if (_wcsicmp(warm.c_str(), L"all") == 0) { for (auto& a : Accounts::Load()) if (!a.ApiKey()) which.push_back(a); }
        else { Account a; if (!Resolve(warm, a)) throw Util::Format(Tr(L"There is no account called %s", L"Аккаунта %s нет"), Util::Q(warm).c_str()); which.push_back(a); }
        std::wstring failed;
        for (auto& a : which) {
            std::wstring err;
            if (Accounts::Warmup(a.id, err)) Print(Util::Format(Tr(L"%s: the 5-hour window is running", L"%s: 5-часовое окно запущено"), a.name.c_str()));
            else { Print(a.name + L": " + err); failed = a.name + L": " + err; }
        }
        if (!failed.empty() && !Redirected()) throw failed;
        return failed.empty() ? 0 : 2;
    }

    if (flag(L"--add-account")) {
        // The running switcher does it, so the new card can be named straight away.
        if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, Core::AddEvent)) { AllowSetForegroundWindow(ASFW_ANY); SetEvent(e); CloseHandle(e); return 0; }
    }

    Gfx::Init();

    // Run from anywhere other than its install folder (a fresh download, a build output),
    // the exe installs itself and hands over to the installed copy.
    bool installed = Paths::Isolated || Paths::Same(Paths::Self, Paths::InstalledExe);
    if (flag(L"--install") || (!installed && argc <= 1)) {
        WinShell::Install();
        std::wstring pass;
        for (auto f : { L"--tray", L"--settings", L"--updated" }) if (flag(f)) pass += (pass.empty() ? L"" : L" ") + std::wstring(f);
        AllowSetForegroundWindow(ASFW_ANY);
        ShellExecuteW(nullptr, L"open", Paths::InstalledExe.c_str(), pass.empty() ? nullptr : pass.c_str(), Paths::Home.c_str(), SW_SHOWNORMAL);
        return 0;
    }

    auto mode = flag(L"--startup") || flag(L"--tray") ? StartMode::Tray : StartMode::Window;

    // One switcher at a time. A second copy asks the first to show itself and leaves.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, Core::MutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mode == StartMode::Window) {
            if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, Core::ShowEvent)) { AllowSetForegroundWindow(ASFW_ANY); SetEvent(e); CloseHandle(e); }
        }
        CloseHandle(mutex);
        return 0;
    }

    Codex::Detect();
    App::Start(mode);
    if (flag(L"--settings")) App::ShowSettings();
    if (flag(L"--add-account")) { App::ShowPicker(); App::AddAccount(); }
    if (flag(L"--updated")) App::Notify(Util::Format(Tr(L"Switcher updated to %s", L"Переключатель обновлён до %s"), Update::Version()), Tr(L"Everything is where you left it", L"Всё на своих местах"));
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    ReleaseMutex(mutex); CloseHandle(mutex);
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Paths::Init();
    L10n::Init();
    int code = 1;
    try { code = Run(); }
    catch (const std::wstring& e) { Fail(e); }
    catch (const wchar_t* e) { Fail(e); }
    catch (const std::exception& e) { Fail(Util::Format(L"%S", e.what())); }
    CoUninitialize();
    return code;
}
