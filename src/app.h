// The running switcher: the account list the windows draw, the tray icon, hotkeys and every
// action the windows offer.
#pragma once
#include "gfx.h"
#include "accounts.h"
#include "recovery.h"

struct Item {
    std::wstring id, name, email, plan, letters;
    COLORREF color = 0;
    int index = 0;                  // 1-based; the digit that switches to it, for the first nine
    bool active = false;            // Codex uses it now
    bool apiKey = false;
    bool switching = false, deleting = false, warming = false, fetching = false;
    long long lastUsed = 0, warmedAt = 0, subscriptionUntil = 0;
    bool masked = false, autoWarm = false;
    std::wstring shownName, shownEmail;   // what the windows show: masked when emails are hidden
    Usage usage;
    std::wstring Status() const;
    bool SignedOut() const { return usage.signedOut; }
    std::wstring PlanLabel() const;  // "Plus", "Pro", "Team", "API key"
};

enum class StartMode { Window, Tray, Startup };

inline const wchar_t* AppTitle() { return Tr(L"Codex Accounts", L"Аккаунты Codex"); }

namespace App {
    extern std::vector<Item> items;
    extern HWND tray;
    extern HICON smallIcon;

    struct State {
        bool appInstalled = false, appRunning = false;
        int terminals = 0, editors = 0;   // Codex sessions in terminals; Codex servers editors run
        std::wstring unknownLogin;  // the email Codex is signed in with, when no account here matches
        bool keyring = false;       // Codex keeps its sign-in where the switcher can't follow
        std::wstring vaultProblem;
        int legacy = 0;             // accounts Codex Switcher has that aren't here yet
    };
    const State& Now();

    void Start(StartMode mode);
    void Refresh();                 // accounts, the sign-in Codex uses and its processes; off the UI thread
    void RefreshUsage(bool all, const std::wstring& only = L"");
    bool FetchingUsage();
    Item* Find(const std::wstring& id);
    Item* Active();
    // The account worth switching to: clearly more limit left than the one in use. Null when
    // there is none, or when the one in use is as good.
    Item* MostLeft();
    double LimitLeft(const Item& it);   // the smaller of the two windows' remainders; -1 = unknown

    void ShowPicker();
    void ClosePicker();
    void ShowSettings();
    void Exit();
    void ThemeChanged();

    // The account someone chose: Codex switches to it, and the app opens with it unless
    // Settings say otherwise (openApp opens it either way). Choosing the account in use just
    // opens the app. By value: callers pass ids out of App::items, which a refresh replaces.
    void Switch(std::wstring id, bool fromPicker, bool openApp = false);

    // An account's statistics, limit resets and subscription, fetched when its details open.
    void ShowDetails(const std::wstring& id);
    void FetchStats(const std::wstring& id);
    const Stats* StatsOf(const std::wstring& id);
    bool StatsLoading(const std::wstring& id);

    // Conversations the limit stopped, continued after a switch (or when asked).
    struct RecoveryState { bool running = false, shown = false; int done = 0, total = 0; Recovery::Report report; };
    const RecoveryState& Recovering();
    void ContinueConversations(bool asked);
    void DismissRecovery();

    // Backups of all accounts, and importing them (Codex Switcher's own exports too).
    void ExportAll(HWND owner);
    void ImportBackup(HWND owner);

    // The browser dashboard's way in; both are safe from any thread.
    std::string StateJson();                                          // "" when the switcher didn't answer
    int RunRemote(const std::wstring& what, const std::wstring& id);   // 1 done, 0 no such account, -1 no answer
    void OpenCodex();               // the app, signed in to whichever account is in use
    void RestartCodex();
    void AddAccount();              // a browser sign-in
    void SignInAgain(const std::wstring& id);
    void CancelSignIn();
    bool SigningIn();
    std::wstring SignInLink();
    void AddCurrentLogin();         // the sign-in Codex uses now
    void ImportFile(HWND owner);
    void ImportLegacy(HWND owner);
    void ExportAuth(const std::wstring& id, HWND owner);
    void BeginRename(const std::wstring& id);
    void CommitRename(const std::wstring& id, const std::wstring& name);
    void Delete(std::wstring id, HWND owner);
    void Warmup(const std::vector<std::wstring>& ids);
    void Move(const std::wstring& id, int delta);
    void ShowMenu(std::wstring id, POINT screen, HWND owner);
    // onClick runs when the balloon itself is clicked.
    void Notify(const std::wstring& title, const std::wstring& text, bool error = false, std::function<void()> onClick = nullptr);
    void OnUi(std::function<void()> f);   // run on the UI thread, from any thread
    // Another copy (a shortcut, the jump list) hands its request to the running switcher, so
    // the same questions and notifications apply. False when none runs.
    bool Forward(const std::wstring& request);

    // settings
    bool Autostart(); void SetAutostart(bool on);
    bool HideAfterSwitch(); void SetHideAfterSwitch(bool on);
    bool KeepInTray(); void SetKeepInTray(bool on);
    bool RestartOnSwitch(); void SetRestartOnSwitch(bool on);
    bool OpenAfterSwitch(); void SetOpenAfterSwitch(bool on);   // choosing an account opens the app with it
    bool AskBeforeRestart(); void SetAskBeforeRestart(bool on);
    int UsageMinutes(); void SetUsageMinutes(int m);   // 0 = only when asked
    bool AutoWarmup(); void SetAutoWarmup(bool on);                  // every account
    void ToggleAutoWarm(const std::wstring& id);                        // one account
    bool TimedWarmup(); void SetTimedWarmup(bool on);
    std::vector<std::wstring> WarmupTimes();
    bool SetWarmupTimes(const std::wstring& text);                     // "8:00, 13:30"; false if it doesn't read
    bool ContinueStopped(); void SetContinueStopped(bool on);
    bool MaskAll(); void SetMaskAll(bool on);
    void ToggleMask(const std::wstring& id);
    std::wstring TrayMode(); void SetTrayMode(const std::wstring& mode);   // L"icon" (the default), L"percent" or L"hidden"
    std::wstring Theme(); void SetTheme(const std::wstring& choice);       // L"auto", L"light" or L"dark"
    bool WebOn(); void SetWebOn(bool on);
    bool WebLan(); void SetWebLan(bool on);
    std::wstring Hotkey(); bool SetHotkey(const std::wstring& combo); bool HotkeyFailed();
    void SuspendHotkey(); void ResumeHotkey();
    bool AccountHotkeys(); void SetAccountHotkeys(bool on); std::vector<int> AccountHotkeysTaken();
    void SetLanguage(const std::wstring& choice);   // L"auto", L"en" or L"ru"; applies at once
}

// Windows
namespace Picker {
    void Show();
    void Close();
    bool Visible();
    bool Active();
    void Rename(const std::wstring& id);
    void Added(const std::wstring& id, bool existed);
    void Restyle();
    void Invalidate();
    HWND Handle();
}
namespace Flyout { void Toggle(); void Hide(); bool Visible(); void Restyle(); void Invalidate(); }
namespace SettingsWin { void Show(); bool Visible(); void Restyle(); void Invalidate(); }
namespace Details { void Show(const std::wstring& id); bool Visible(); void Restyle(); void Invalidate(); void Close(); }
namespace Dialogs {
    bool Ask(HWND owner, const std::wstring& title, const std::wstring& text, const std::wstring& yes, bool danger = false, const std::wstring& no = Tr(L"Cancel", L"Отмена"));
    void Info(HWND owner, const std::wstring& title, const std::wstring& text);
    // Several answers, the first the main one: its index, or -1 when the dialog was closed.
    int Choose(HWND owner, const std::wstring& title, const std::wstring& text, const std::vector<std::wstring>& choices);
    // A line to type: a password (hidden; confirm asks for it twice) or pasted text.
    bool Prompt(HWND owner, const std::wstring& title, const std::wstring& text, std::wstring& value, bool password, bool confirm = false, const std::wstring& yes = L"");
    void RestyleAll();
}

namespace Update {
    enum class Phase { Idle, Checking, Current, Ready, Downloading, Installing, Failed };
    const wchar_t* Version();          // this build's, e.g. L"1.0.0"
    Phase State();
    std::wstring Latest();             // the newer version, once one is found
    std::wstring Problem();
    long long CheckedAt();             // unix ms of the last successful check, 0 = never
    bool Auto(); void SetAuto(bool on);
    void Check(bool manual);
    void Install();
    void Cleanup();
}

namespace Hotkeys {
    bool Parse(const std::wstring& combo, UINT& mods, UINT& vk);
    std::wstring Pretty(const std::wstring& combo);
    std::wstring FromKeys(UINT mods, UINT vk);
}

// Icons, shortcuts, start with Windows, installing and the jump list.
namespace WinShell {
    std::wstring AccountIcon(const Item& it);
    std::wstring AddIcon();
    void RemoveIcons(const std::wstring& id);
    void WriteAppIcon(const std::wstring& path);   // the switcher's own icon, used at build time
    void WriteIconSamples(const std::wstring& dir);
    std::wstring CreateShortcut(const Item& it, const std::wstring& dir);
    std::vector<std::wstring> RemoveShortcuts(const std::wstring& name);
    bool AutostartEnabled();
    void SetAutostart(bool on);
    void Install();
    void UpdateJumpList(const std::vector<Item>& items);
    void DarkMenus(bool dark);         // native popup menus follow the theme
    HICON PercentIcon(int size, int left);   // the tray icon showing what is left, 0…100
    HICON TrayIcon(int size, bool darkTaskbar);   // the switcher's own mark, for the taskbar's colour
    void AllowDark(HWND h, bool dark);
}
