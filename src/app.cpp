#include "app.h"
#include "logins.h"
#include "codex.h"
#include "net.h"
#include "web.h"
#include <shellapi.h>
#include <commdlg.h>
#include <objbase.h>
#include <thread>
#include <atomic>
#include <algorithm>
#include <set>
#include <memory>

std::wstring Item::Status() const {
    if (deleting) return Tr(L"Deleting…", L"Удаляется…");
    if (switching) return active ? Tr(L"Opening…", L"Открывается…") : Tr(L"Switching…", L"Переключается…");
    if (warming) return Tr(L"Starting the window…", L"Запускаю окно…");
    if (usage.signedOut) return Tr(L"Signed out", L"Сеанс завершён");
    if (active) return Tr(L"In use", L"Используется");
    if (lastUsed <= 0) return Util::When(lastUsed);
    // A bare date says nothing: "Used today, 10:09", "Выбран 14 октября".
    auto when = Util::When(lastUsed);
    if (!when.empty() && (L10n::ru || when.rfind(L"Today", 0) == 0 || when.rfind(L"Yesterday", 0) == 0)) CharLowerBuffW(&when[0], 1);
    return Tr(L"Used ", L"Выбран ") + when;
}

std::wstring Item::Spoken(bool best) const {
    std::wstring s = shownName;
    auto p = PlanLabel();
    if (!p.empty()) s += L", " + p;
    s += L", " + (best ? std::wstring(Tr(L"most limit left", L"больше всего лимита")) : Status());
    if (usage.known && !apiKey) {
        if (usage.five >= 0) s += Util::Format(Tr(L", 5 hours: %d%% left", L", 5 часов: осталось %d%%"), (int)std::lround(usage.FiveLeft()));
        if (usage.week >= 0) s += Util::Format(Tr(L", week: %d%% left", L", неделя: осталось %d%%"), (int)std::lround(usage.WeekLeft()));
    }
    return s;
}

std::wstring Item::PlanLabel() const {
    if (apiKey) return Tr(L"API key", L"API-ключ");
    auto p = !usage.plan.empty() ? usage.plan : plan;
    if (p.empty()) return L"";
    static const std::pair<const wchar_t*, const wchar_t*> known[] = { { L"free", L"Free" }, { L"go", L"Go" }, { L"plus", L"Plus" }, { L"pro", L"Pro" },
        { L"prolite", L"Pro Lite" }, { L"pro_lite", L"Pro Lite" }, { L"team", L"Team" }, { L"business", L"Business" }, { L"enterprise", L"Enterprise" },
        { L"edu", L"Edu" }, { L"education", L"Edu" } };
    for (auto& k : known) if (_wcsicmp(p.c_str(), k.first) == 0) return k.second;
    p[0] = towupper(p[0]);
    for (auto& c : p) if (c == L'_') c = L' ';
    return p;
}

namespace App {
    std::vector<Item> items;
    HWND tray = nullptr;
    HICON smallIcon = nullptr;

    enum : UINT { WM_TRAY = WM_APP + 1, WM_REFRESHED, WM_SWITCHED, WM_USAGE, WM_USAGE_DONE, WM_SIGNAL, WM_RENAME, WM_RUN, WM_LOGIN, WM_ADDED, WM_WARMED,
                  WM_STATS, WM_RECOVERY, WM_SYNC };
    enum : UINT_PTR { TimerRefresh = 1, TimerSoon = 2, TimerHide = 3, TimerUsage = 4, TimerUpdate = 5, TimerRecovery = 6, TimerUndo = 7 };
    const int HotkeyPicker = 1, HotkeyAccount = 10;   // 11..19 for Ctrl+Shift+Alt+1..9

    static UINT taskbarCreated;
    static bool refreshing = false, refreshAgain = false, iconAdded = false, hotkeyFailed = false;
    static bool fetching = false, fetchAgain = false;
    static std::set<std::wstring> fetchLater;
    static std::atomic<bool> recoveryAlive{ false };   // the thread continuing conversations runs
    static std::map<std::wstring, long long> usageTried, statsTried;   // when each was last asked, answered or not
    static std::vector<int> accountTaken;
    static std::wstring pendingRename, signInLink, signInFor, signInCred;
    static void ForgetDeleted();
    static HANDLE showEvt, exitEvt, addEvt, reloadEvt;
    static std::map<std::wstring, Usage> usage;
    static State state;
    static int quietTicks = 0;
    static std::map<std::wstring, Stats> stats;
    static std::set<std::wstring> statsBusy;
    static RecoveryState recovery;
    static HICON trayIcon = nullptr; static int trayValue = -2;
    static void RedrawAll();
    static void UpdateTip();
    static void UpdateTrayIcon();
    static void DoSwitch(std::wstring id, bool fromPicker, bool openApp, bool ask);

    const State& Now() { return state; }
    Item* Find(const std::wstring& id) { for (auto& i : items) if (i.id == id) return &i; return nullptr; }
    Item* Active() { for (auto& i : items) if (i.active) return &i; return nullptr; }

    double LimitLeft(const Item& i) {
        if (i.apiKey || !i.usage.known || i.usage.signedOut || i.deleting) return -1;
        double f = i.usage.FiveLeft(), w = i.usage.WeekLeft();
        if (f < 0 && w < 0) return -1;
        return std::min(f < 0 ? 100.0 : f, w < 0 ? 100.0 : w);
    }

    Item* MostLeft() {
        Item* best = nullptr; double score = -1;
        for (auto& i : items) { double s = LimitLeft(i); if (s > score) { score = s; best = &i; } }
        if (!best || best->active || score < 10) return nullptr;
        if (auto a = Active()) { double s = LimitLeft(*a); if (s >= 0 && score < s + 10) return nullptr; }
        return best;
    }
    bool FetchingUsage() { return fetching; }
    static void RedrawAll() { Picker::Invalidate(); Flyout::Invalidate(); SettingsWin::Invalidate(); Details::Invalidate(); Manager::Invalidate(); }
    static HWND Owner(HWND w) { return w && IsWindowVisible(w) ? w : Picker::Visible() ? Picker::Handle() : nullptr; }

    // ------------------------------------------------------------ settings

    bool Autostart() { return WinShell::AutostartEnabled(); }
    void SetAutostart(bool on) { WinShell::SetAutostart(on); }
    bool HideAfterSwitch() { return Settings::Flag(L"HideAfterSwitch", true); }
    void SetHideAfterSwitch(bool on) { Settings::Set(L"HideAfterSwitch", on); }
    bool KeepInTray() { return Settings::Flag(L"KeepInTray", true); }
    void SetKeepInTray(bool on) { Settings::Set(L"KeepInTray", on); }
    bool RestartOnSwitch() { return Settings::Flag(L"RestartCodex", true); }
    void SetRestartOnSwitch(bool on) { Settings::Set(L"RestartCodex", on); }
    bool OpenAfterSwitch() { return Settings::Flag(L"OpenAfterSwitch", true); }
    void SetOpenAfterSwitch(bool on) { Settings::Set(L"OpenAfterSwitch", on); }
    bool AskBeforeRestart() { return Settings::Flag(L"AskBeforeRestart", true); }
    void SetAskBeforeRestart(bool on) { Settings::Set(L"AskBeforeRestart", on); }
    int UsageMinutes() { return Settings::Int(L"UsageMinutes", 15); }
    void SetUsageMinutes(int m) { Settings::Set(L"UsageMinutes", m); }
    bool AutoWarmup() { return Settings::Flag(L"AutoWarmup", false); }
    void SetAutoWarmup(bool on) { Settings::Set(L"AutoWarmup", on); if (on) RefreshUsage(true); }

    static std::vector<std::wstring> List(const wchar_t* key) {
        std::vector<std::wstring> out;
        auto all = Settings::Load();
        if (auto a = all.Find(key)) for (auto& v : a->arr) if (v.type == json::Value::String) out.push_back(v.s);
        return out;
    }
    static void SetList(const wchar_t* key, const std::vector<std::wstring>& list) {
        auto a = json::Value::MakeArray();
        for (auto& s : list) a.arr.push_back(json::Value(s));
        Settings::Set(key, a);
    }
    static void Toggle(const wchar_t* key, const std::wstring& id) {
        auto list = List(key);
        auto it = std::find(list.begin(), list.end(), id);
        if (it != list.end()) list.erase(it); else list.push_back(id);
        SetList(key, list);
    }

    void ToggleAutoWarm(const std::wstring& id) { Toggle(L"AutoWarmupIds", id); if (auto it = Find(id)) it->autoWarm = !it->autoWarm; RefreshUsage(true, id); RedrawAll(); }
    bool TimedWarmup() { return Settings::Flag(L"TimedWarmup", false); }
    void SetTimedWarmup(bool on) { Settings::Set(L"TimedWarmup", on); }
    std::vector<std::wstring> WarmupTimes() { return Settings::Has(L"WarmupTimes") ? List(L"WarmupTimes") : std::vector<std::wstring>{ L"08:00", L"13:00", L"18:00" }; }

    bool SetWarmupTimes(const std::wstring& text) {
        std::vector<std::wstring> times;
        size_t s = 0;
        while (s <= text.size()) {
            auto e = text.find_first_of(L",; ", s);
            auto part = Util::Trim(text.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s));
            if (!part.empty()) {
                int h = -1, m = -1; wchar_t extra = 0;
                if (swscanf_s(part.c_str(), L"%d:%d%c", &h, &m, &extra, 1) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return false;
                auto t = Util::Format(L"%02d:%02d", h, m);
                if (std::find(times.begin(), times.end(), t) == times.end()) times.push_back(t);
            }
            if (e == std::wstring::npos) break;
            s = e + 1;
        }
        std::sort(times.begin(), times.end());
        SetList(L"WarmupTimes", times);
        return true;
    }

    bool ContinueStopped() { return Settings::Flag(L"ContinueStopped", false); }
    void SetContinueStopped(bool on) { Settings::Set(L"ContinueStopped", on); }

    static void Remask();
    bool MaskAll() { return Settings::Flag(L"MaskAll", false); }
    void SetMaskAll(bool on) { Settings::Set(L"MaskAll", on); Remask(); }
    void ToggleMask(const std::wstring& id) { Toggle(L"Masked", id); Remask(); }

    // The switcher's own icon unless asked otherwise: it is what people look for in the tray.
    std::wstring TrayMode() { auto m = Settings::Str(L"TrayIcon", L"icon"); return m == L"percent" || m == L"hidden" ? m : L"icon"; }
    void SetTrayMode(const std::wstring& mode) { Settings::Set(L"TrayIcon", mode); trayValue = -2; UpdateTrayIcon(); }
    std::wstring Theme() { auto t = Settings::Str(L"Theme", L"auto"); return t == L"light" || t == L"dark" ? t : L"auto"; }
    void SetTheme(const std::wstring& choice) { Settings::Set(L"Theme", choice); ThemeChanged(); }
    bool WebOn() { return Settings::Flag(L"WebOn", false); }
    void SetWebOn(bool on) { Settings::Set(L"WebOn", on); Web::Apply(); }
    bool WebLan() { return Settings::Flag(L"WebLan", false); }
    void SetWebLan(bool on) { Settings::Set(L"WebLan", on); Web::Apply(); }

    // "jo•••@g•••.com": enough to tell accounts apart on a shared screen, not to read them.
    static std::wstring MaskText(const std::wstring& s);
    std::wstring Masked(const std::wstring& text) { return MaskText(text); }

    static std::wstring MaskText(const std::wstring& s) {
        if (s.empty()) return s;
        auto at = s.find(L'@');
        auto local = s.substr(0, at);
        std::wstring out = local.substr(0, std::min<size_t>(2, local.size())) + L"•••";
        if (at != std::wstring::npos) {
            auto domain = s.substr(at + 1);
            auto dot = domain.rfind(L'.');
            out += L"@" + domain.substr(0, 1) + L"•••" + (dot != std::wstring::npos ? domain.substr(dot) : L"");
        }
        return out;
    }

    // A name made from the email, as the default ones are ("john", "john 2", "john (Team)",
    // cut at 32 characters), would give it away just the same.
    static bool FromEmail(const std::wstring& name, const std::wstring& email) {
        if (email.empty()) return false;
        if (name.find(L'@') != std::wstring::npos) return true;
        auto local = email.substr(0, email.find(L'@'));
        size_t n = std::min<size_t>(local.size(), 32);
        return n >= 2 && name.size() >= n && _wcsnicmp(name.c_str(), local.c_str(), n) == 0;
    }

    static void Shown(Item& it, const std::vector<std::wstring>& masked) {
        it.masked = MaskAll() || std::find(masked.begin(), masked.end(), it.id) != masked.end();
        it.shownEmail = it.masked ? MaskText(it.email) : it.email;
        it.shownName = it.masked && FromEmail(it.name, it.email) ? MaskText(it.name) : it.name;
    }

    static void Remask() {
        auto masked = List(L"Masked");
        for (auto& it : items) Shown(it, masked);
        UpdateTip();
        WinShell::UpdateJumpList(items);
        RedrawAll();
    }

    // Absent means never set, so the default applies; empty means turned off.
    std::wstring Hotkey() { return Settings::Has(L"Hotkey") ? Settings::Str(L"Hotkey") : L"Ctrl+Alt+X"; }
    bool HotkeyFailed() { return hotkeyFailed; }

    static bool RegisterPicker() {
        UnregisterHotKey(tray, HotkeyPicker);
        UINT mods, vk;
        if (Paths::Isolated || !Hotkeys::Parse(Hotkey(), mods, vk)) { hotkeyFailed = false; return true; }
        hotkeyFailed = !RegisterHotKey(tray, HotkeyPicker, mods | MOD_NOREPEAT, vk);
        return !hotkeyFailed;
    }
    bool SetHotkey(const std::wstring& combo) { Settings::Set(L"Hotkey", combo); return RegisterPicker(); }
    void SuspendHotkey() { UnregisterHotKey(tray, HotkeyPicker); }
    void ResumeHotkey() { RegisterPicker(); }

    // Ctrl+Shift+Alt rather than Ctrl+Alt: Claude Profile Switcher already takes Ctrl+Alt+1…9.
    bool AccountHotkeys() { return Settings::Flag(L"AccountHotkeys", true); }
    std::vector<int> AccountHotkeysTaken() { return accountTaken; }
    static void RegisterAccounts() {
        accountTaken.clear();
        for (int d = 1; d <= 9; d++) UnregisterHotKey(tray, HotkeyAccount + d);
        if (!AccountHotkeys() || Paths::Isolated) return;
        for (int d = 1; d <= 9; d++)
            if (!RegisterHotKey(tray, HotkeyAccount + d, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, '0' + d)) accountTaken.push_back(d);
    }
    void SetAccountHotkeys(bool on) { Settings::Set(L"AccountHotkeys", on); RegisterAccounts(); }

    // ------------------------------------------------------------ tray

    static void AddIcon() {
        if (Gfx::Offscreen() || TrayMode() == L"hidden") return;
        NOTIFYICONDATAW d{ sizeof d };
        d.hWnd = tray; d.uID = 1; d.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        d.uCallbackMessage = WM_TRAY; d.hIcon = smallIcon;
        wcscpy_s(d.szTip, AppTitle());
        iconAdded = Shell_NotifyIconW(NIM_ADD, &d) != 0;
        // No taskbar yet (Explorer starting or restarting): TaskbarCreated brings it back.
        if (!iconAdded) return;
        d.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &d);
        trayValue = -2;
        UpdateTip();
        UpdateTrayIcon();
    }

    // Windows' own setting for the taskbar: dark unless "SystemUsesLightTheme" says light.
    static bool DarkTaskbar() {
        DWORD v = 0, n = sizeof v;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n) != ERROR_SUCCESS) return true;
        return v == 0;
    }

    // The tray shows the switcher's icon, drawn for the taskbar's colour. "% left" shows
    // what is left of the 5-hour window of the account in use instead (its week when it has
    // none); "Hidden" leaves the tray altogether: the shortcut and the Start menu still open it.
    static void UpdateTrayIcon() {
        if (Gfx::Offscreen()) return;
        auto mode = TrayMode();
        if (mode == L"hidden") {
            if (iconAdded) { NOTIFYICONDATAW d{ sizeof d }; d.hWnd = tray; d.uID = 1; Shell_NotifyIconW(NIM_DELETE, &d); iconAdded = false; }
            return;
        }
        if (!iconAdded) { AddIcon(); return; }
        int value = -1;
        if (mode == L"percent") if (auto a = Active()) if (a->usage.known && !a->usage.signedOut) {
            double left = a->usage.FiveLeft() >= 0 ? a->usage.FiveLeft() : a->usage.WeekLeft();
            if (left >= 0) value = (int)std::lround(left);
        }
        int key = value >= 0 ? value : DarkTaskbar() ? 1001 : 1000;   // 0…100, or the icon for a light or dark taskbar
        if (key == trayValue) return;
        trayValue = key;
        HICON icon = smallIcon;
        int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
        if (HICON made = key <= 100 ? WinShell::PercentIcon(size, key) : WinShell::TrayIcon(size, key == 1001)) { if (trayIcon) DestroyIcon(trayIcon); trayIcon = made; icon = made; }
        NOTIFYICONDATAW d{ sizeof d };
        d.hWnd = tray; d.uID = 1; d.uFlags = NIF_ICON; d.hIcon = icon;
        Shell_NotifyIconW(NIM_MODIFY, &d);
    }

    // "Codex Accounts · Work · 5 h 34%": which account Codex uses, at a glance.
    static void UpdateTip() {
        if (!iconAdded) return;
        std::wstring tip = AppTitle();
        if (auto a = Active()) {
            tip += L"\n" + a->shownName;
            std::wstring left;
            if (a->usage.known && a->usage.FiveLeft() >= 0) left += Util::Format(Tr(L"5h %d%%", L"5 ч %d%%"), (int)a->usage.FiveLeft());
            if (a->usage.known && a->usage.WeekLeft() >= 0) left += (left.empty() ? L"" : L", ") + Util::Format(Tr(L"week %d%%", L"неделя %d%%"), (int)a->usage.WeekLeft());
            if (!left.empty()) tip += Tr(L"\nLeft: ", L"\nОсталось: ") + left;
        }
        NOTIFYICONDATAW d{ sizeof d };
        d.hWnd = tray; d.uID = 1; d.uFlags = NIF_TIP | NIF_SHOWTIP;
        wcsncpy_s(d.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &d);
    }

    static std::function<void()> balloonClick;

    void OnUi(std::function<void()> f) {
        auto p = new std::function<void()>(std::move(f));
        if (!PostMessageW(tray, WM_RUN, 0, (LPARAM)p)) delete p;
    }

    void Notify(const std::wstring& title, const std::wstring& text, bool error, std::function<void()> onClick) {
        Test::Trace(std::wstring(error ? L"notify-error|" : L"notify|") + title + L"|" + text);
        if (!iconAdded) return;
        balloonClick = std::move(onClick);
        NOTIFYICONDATAW d{ sizeof d };
        d.hWnd = tray; d.uID = 1; d.uFlags = NIF_INFO;
        wcsncpy_s(d.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(d.szInfo, text.c_str(), _TRUNCATE);
        d.dwInfoFlags = (error ? NIIF_ERROR : NIIF_INFO) | NIIF_NOSOUND;
        Shell_NotifyIconW(NIM_MODIFY, &d);
    }

    // Everything drawn picks the language up on its next frame; what Windows holds on to
    // (window titles, the tray tip, the jump list) is updated here.
    void SetLanguage(const std::wstring& choice) {
        L10n::Choose(choice);
        UpdateTip();
        if (HWND h = Picker::Handle()) SetWindowTextW(h, AppTitle());
        WinShell::UpdateJumpList(items);
        RedrawAll();
    }

    // A native menu, and its choice. In the demo's checks it never shows: its items go to the
    // trace, and the check picks one by name through test-menu.txt.
    static int RunMenu(HMENU m, UINT flags, POINT at, HWND owner);
    int PopupMenu(HMENU m, POINT at, HWND owner) { return RunMenu(m, TPM_RIGHTBUTTON, at, owner); }

    static int RunMenu(HMENU m, UINT flags, POINT at, HWND owner) {
        if (!Test::On()) return TrackPopupMenuEx(m, flags | TPM_RETURNCMD, at.x, at.y, owner, nullptr);
        auto pick = Test::Take(L"test-menu.txt");
        std::wstring all; int chosen = 0;
        for (int i = 0, n = GetMenuItemCount(m); i < n; i++) {
            wchar_t text[256] = {};
            MENUITEMINFOW mi{ sizeof mi };
            mi.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE | MIIM_FTYPE; mi.dwTypeData = text; mi.cch = ARRAYSIZE(text) - 1;
            GetMenuItemInfoW(m, i, TRUE, &mi);
            if (mi.fType & MFT_SEPARATOR) { all += L"|-"; continue; }
            std::wstring label = text;
            if (auto tab = label.find(L'\t'); tab != std::wstring::npos) label.resize(tab);
            bool off = (mi.fState & MFS_GRAYED) != 0, checked = (mi.fState & MFS_CHECKED) != 0;
            all += L"|" + label + (off ? L" [off]" : L"") + (checked ? L" [x]" : L"");
            if (!pick.empty() && !off && !chosen && label.find(pick) != std::wstring::npos) chosen = (int)mi.wID;
        }
        Test::Trace(L"menu" + all);
        if (!pick.empty()) Test::Trace(L"menu-pick|" + pick + (chosen ? L"" : L"|not there"));
        return chosen;
    }

    // A file to open or save: the system's dialog, or in the demo's checks the path in test-file.txt.
    static bool PickFile(OPENFILENAMEW& of, bool save) {
        if (!Test::On()) return save ? GetSaveFileNameW(&of) != FALSE : GetOpenFileNameW(&of) != FALSE;
        auto path = Test::Take(L"test-file.txt");
        Test::Trace(std::wstring(save ? L"file-save|" : L"file-open|") + (path.empty() ? L"cancelled" : path));
        if (path.empty() || path.size() >= of.nMaxFile) return false;
        wcscpy_s(of.lpstrFile, of.nMaxFile, path.c_str());
        return true;
    }

    // The tray's right-click menu: native, so it looks and behaves like every other one.
    static void TrayMenu() {
        // Native menus keep dispatching refresh messages. Bind every command to the account
        // shown when the menu opened, rather than a live list index.
        const auto menuItems = items;
        WinShell::DarkMenus(Gfx::T.dark);
        HMENU m = CreatePopupMenu();
        int n = 0;
        for (auto& i : menuItems) {
            auto label = i.shownName + (i.index <= 9 && AccountHotkeys() ? Util::Format(L"\tCtrl+Shift+Alt+%d", i.index) : L"");
            AppendMenuW(m, MF_STRING | (i.active ? MF_CHECKED : 0) | (i.deleting ? MF_GRAYED : 0), 100 + n++, label.c_str());
        }
        if (!menuItems.empty()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        auto best = MostLeft();
        std::wstring bestId = best ? best->id : L"";
        if (best) AppendMenuW(m, MF_STRING, 10, Util::Format(Tr(L"Switch to the one with the most left: %s", L"Переключить на самый свободный: %s"), best->shownName.c_str()).c_str());
        auto appName = Codex::AppName();
        AppendMenuW(m, MF_STRING | (state.appInstalled ? 0 : MF_GRAYED), 5, Util::Format(Tr(L"Open %s", L"Открыть %s"), appName.c_str()).c_str());
        AppendMenuW(m, MF_STRING | (state.appRunning ? 0 : MF_GRAYED), 6, Util::Format(Tr(L"Restart %s", L"Перезапустить %s"), appName.c_str()).c_str());
        AppendMenuW(m, MF_STRING | (fetching || items.empty() ? MF_GRAYED : 0), 7, Tr(L"Refresh limits", L"Обновить лимиты"));
        AppendMenuW(m, MF_STRING | (items.empty() ? MF_GRAYED : 0), 9, Tr(L"Start 5-hour windows for all", L"Запустить 5-часовые окна у всех"));
        AppendMenuW(m, MF_STRING | (recovery.running ? MF_GRAYED : 0), 11, Tr(L"Continue conversations stopped by the limit", L"Продолжить разговоры, остановленные лимитом"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        auto up = Update::State();
        if (up == Update::Phase::Ready) AppendMenuW(m, MF_STRING, 8, Util::Format(Tr(L"Update to %s", L"Обновить до %s"), Update::Latest().c_str()).c_str());
        else if (up == Update::Phase::Downloading || up == Update::Phase::Installing) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, Tr(L"Updating…", L"Обновляется…"));
        AppendMenuW(m, MF_STRING, 1, Tr(L"Show accounts", L"Открыть окно выбора"));
        AppendMenuW(m, MF_STRING, 2, Tr(L"Add account…", L"Добавить аккаунт…"));
        AppendMenuW(m, MF_STRING, 12, Tr(L"Account manager…", L"Управление аккаунтами…"));
        AppendMenuW(m, MF_STRING, 3, Tr(L"Settings", L"Настройки"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 4, Tr(L"Quit switcher", L"Закрыть переключатель"));
        SetMenuDefaultItem(m, 1, FALSE);
        POINT pt; GetCursorPos(&pt);
        if (!Gfx::Offscreen()) SetForegroundWindow(tray);
        int cmd = RunMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt, tray);
        PostMessageW(tray, WM_NULL, 0, 0);
        DestroyMenu(m);
        if (cmd >= 100 && cmd - 100 < (int)menuItems.size()) { auto id = menuItems[cmd - 100].id; if (Find(id)) Switch(id, false); }
        else if (cmd == 10) { if (Find(bestId)) Switch(bestId, false); }
        else if (cmd == 5) OpenCodex();
        else if (cmd == 6) RestartCodex();
        else if (cmd == 7) RefreshUsage(true);
        else if (cmd == 9) { std::vector<std::wstring> ids; for (auto& i : items) if (!i.apiKey) ids.push_back(i.id); Warmup(ids); }
        else if (cmd == 11) ContinueConversations(true);
        else if (cmd == 8) Update::Install();
        else if (cmd == 1) ShowPicker();
        else if (cmd == 2) AddAccount();
        else if (cmd == 3) ShowSettings();
        else if (cmd == 12) ShowManager();
        else if (cmd == 4) Exit();
    }

    // ------------------------------------------------------------ refresh

    struct Snapshot {
        std::vector<Account> accounts;   // tokens stripped: the UI thread never needs them
        std::wstring activeId, unknown;
        Codex::Processes procs;
        bool installed = false, keyring = false;
        std::wstring vaultProblem;
        int legacy = 0;
    };

    // Accounts Codex Switcher has that aren't here yet. Its file is read again only when it changes.
    static int LegacyCount(const std::vector<Account>& list) {
        static ULONGLONG stamp = ~0ull; static std::vector<std::wstring> who;
        auto t = Util::FileTime(Accounts::LegacyFile());
        if (t != stamp) {
            stamp = t; who.clear();
            std::wstring err;
            for (auto& l : Accounts::LegacyAccounts(err)) who.push_back(Accounts::IdentityOf(l.auth));
        }
        int n = 0;
        for (auto& w : who) {
            bool have = false;
            for (auto& a : list) have |= a.Identity() == w;
            n += !have && !w.empty();
        }
        return n;
    }

    static void Merge(Snapshot& s) {
        bool fresh = false;   // an account appeared (added here or by another copy): it needs its limits
        for (auto& a : s.accounts) fresh |= !Find(a.id);
        bool first = items.empty();
        auto masked = List(L"Masked"), warmIds = List(L"AutoWarmupIds");
        std::vector<Item> next;
        for (size_t k = 0; k < s.accounts.size(); k++) {
            auto& a = s.accounts[k];
            Item it;
            if (auto old = Find(a.id)) it = *old;
            it.id = a.id; it.name = a.name; it.email = a.email; it.plan = a.plan;
            it.letters = Core::Initials(a.name);
            it.color = Core::AccountColor(a.id);
            it.index = (int)k + 1;
            it.active = a.id == s.activeId;
            it.apiKey = a.plan == L"api";
            it.lastUsed = a.lastUsed; it.warmedAt = a.warmedAt; it.subscriptionUntil = a.subscriptionUntil;
            it.autoWarm = std::find(warmIds.begin(), warmIds.end(), a.id) != warmIds.end();
            Shown(it, masked);
            auto u = usage.find(a.id);
            it.usage = u != usage.end() ? u->second : Usage();
            next.push_back(it);
        }
        items.swap(next);
        state.appInstalled = s.installed;
        state.appRunning = s.procs.appRunning();
        state.terminals = (int)s.procs.cli.size();
        state.editors = (int)s.procs.helpers.size();
        state.unknownLogin = s.unknown;
        state.keyring = s.keyring;
        state.vaultProblem = s.vaultProblem;
        state.legacy = s.legacy;
        WinShell::UpdateJumpList(items);
        UpdateTip();
        UpdateTrayIcon();
        RedrawAll();
        if (fresh && !first) RefreshUsage(false);
    }

    // The account list, the sign-in Codex uses and its processes, off the UI thread. Each look
    // also takes back any tokens Codex renewed, so a switch never restores a stale copy.
    void Refresh() {
        if (refreshing) { refreshAgain = true; return; }
        refreshing = true;
        std::thread([] {
            auto s = new Snapshot();
            try {
                Codex::Detect();
                // The demo has its own made-up app, whatever is installed on this PC.
                s->installed = Paths::Demo ? !Settings::Flag(L"DemoNoApp", false) : Codex::App().found();
                s->keyring = !Paths::Demo && Codex::KeyringStore();
                json::Value live;
                {
                    Accounts::Lock lock;
                    s->accounts = Accounts::Load();
                    live = Accounts::ReadLive();
                    if (Accounts::SyncLive(s->accounts, live)) { try { Accounts::Save(s->accounts); } catch (...) {} }
                }
                s->activeId = Accounts::ActiveId(s->accounts, live);
                if (s->activeId.empty() && !Accounts::IdentityOf(live).empty()) {
                    auto d = Accounts::Describe(live);
                    s->unknown = !d.email.empty() ? d.email : d.ApiKey() ? std::wstring(Tr(L"an API key", L"API-ключ")) : std::wstring(Tr(L"another account", L"другой аккаунт"));
                }
                s->legacy = LegacyCount(s->accounts);
                for (auto& a : s->accounts) a.auth = json::Value();
                s->procs = Codex::Scan();
                s->vaultProblem = Accounts::Problem();
            } catch (...) {}
            if (!PostMessageW(tray, WM_REFRESHED, 0, (LPARAM)s)) delete s;
        }).detach();
    }

    // ------------------------------------------------------------ limits

    static void MaybeWarm(const Item& it);

    // One account after another on a worker thread; each answer goes to its card as it comes.
    void RefreshUsage(bool all, const std::wstring& only) {
        Test::Trace(L"refresh|" + (!only.empty() ? only : all ? L"all" : L"due"));
        if (fetching) { if (!only.empty()) fetchLater.insert(only); else fetchAgain |= all; return; }
        std::vector<std::wstring> ids;
        long long now = Core::NowMs();
        for (auto& i : items) {
            if (i.apiKey || i.deleting) continue;
            long long every = (long long)std::max(2, UsageMinutes()) * 60000, last = std::max(i.usage.at, usageTried[i.id]);
            if (!only.empty() ? i.id == only : all || now - last > every) ids.push_back(i.id);
        }
        for (auto& id : ids) usageTried[id] = now;
        if (ids.empty()) return;
        fetching = true;
        for (auto& id : ids) if (auto it = Find(id)) it->fetching = true;
        RedrawAll();
        std::thread([ids] {
            for (auto& id : ids) {
                auto u = new std::pair<std::wstring, Usage>(id, Usage());
                try { u->second = Accounts::FetchUsage(id); } catch (...) { u->second.problem = Tr(L"Something went wrong", L"Что-то пошло не так"); }
                if (!PostMessageW(tray, WM_USAGE, 0, (LPARAM)u)) delete u;
            }
            PostMessageW(tray, WM_USAGE_DONE, 0, 0);
        }).detach();
    }

    static void TakeUsage(const std::wstring& id, Usage u) {
        auto& old = usage[id];
        if (!u.known && old.known && !u.signedOut) {
            // A failed attempt keeps the last figures; the card shows them as old.
            auto kept = old; kept.problem = u.problem; u = kept;
        }
        old = u;
        if (auto it = Find(id)) { it->usage = u; it->fetching = false; MaybeWarm(*it); }
        UpdateTip();
        UpdateTrayIcon();
        RedrawAll();
    }

    // The next 5-hour window starts at the first request after the last one ended. With
    // automatic warm-ups on, that request comes from here as soon as an account's window is
    // idle, unless its week is used up. Never more than once in ten minutes per account.
    static void MaybeWarm(const Item& it) {
        if (!(AutoWarmup() || it.autoWarm) || it.apiKey || it.warming || it.usage.signedOut || !it.usage.known || Paths::Demo) return;
        auto& u = it.usage;
        long long now = Core::NowMs() / 1000;
        if (u.WeekNow() >= 100 || now * 1000 - it.warmedAt < 10ll * 60 * 1000 || now * 1000 - u.at > 5ll * 60 * 1000) return;
        bool idle = u.five < 0 || (u.fiveReset && now >= u.fiveReset) || (u.five == 0 && (!u.fiveReset || u.fiveReset - now >= 5 * 3600 - 180));
        if (idle) Warmup({ it.id });
    }

    void Warmup(const std::vector<std::wstring>& all) {
        std::vector<std::wstring> ids;
        for (auto& id : all) Test::Trace(L"warmup|" + id);
        for (auto& id : all) if (auto it = Find(id)) if (!it->warming && !it->apiKey && !it->deleting) { it->warming = true; ids.push_back(id); }
        if (ids.empty()) return;
        RedrawAll();
        std::thread([ids] {
            for (auto& id : ids) {
                auto r = new std::pair<std::wstring, std::wstring>(id, L"");
                bool ok = false;
                try { ok = Accounts::Warmup(id, r->second); } catch (...) { r->second = Tr(L"Something went wrong", L"Что-то пошло не так"); }
                if (ok) r->second.clear(); else if (r->second.empty()) r->second = Tr(L"Something went wrong", L"Что-то пошло не так");
                if (!PostMessageW(tray, WM_WARMED, 0, (LPARAM)r)) delete r;
            }
        }).detach();
    }

    // ------------------------------------------------------------ windows

    void ShowPicker() {
        Test::Trace(L"window|picker");
        // Opened again right after a choice: the hiding that choice set up no longer applies.
        KillTimer(tray, TimerHide);
        Flyout::Hide();
        Picker::Show();
        Refresh();
        RefreshUsage(false);
    }

    void ClosePicker() {
        Test::Trace(L"window|picker|closed");
        Picker::Close();
        if (!KeepInTray()) Exit();
    }

    void ShowSettings() { Test::Trace(L"window|settings"); Flyout::Hide(); SettingsWin::Show(); Refresh(); }
    void ShowManager() { Flyout::Hide(); Manager::Show(); Refresh(); }

    void ThemeChanged() {
        Gfx::ApplyTheme();
        WinShell::DarkMenus(Gfx::T.dark);
        Picker::Restyle(); Flyout::Restyle(); SettingsWin::Restyle(); Details::Restyle(); Manager::Restyle(); Dialogs::RestyleAll();
        trayValue = -2; UpdateTrayIcon();   // the taskbar may have changed colour too
    }

    void Exit() {
        Test::Trace(L"exit");
        ForgetDeleted();
        Login::Cancel();
        Web::Stop();
        // Continuing conversations stops at its next step; it never leaves a message half sent.
        Recovery::Cancel();
        for (int i = 0; i < 60 && recoveryAlive; i++) Sleep(50);
        Details::Close();
        // A renewal or a switch half done would lose a refresh token or leave Codex closed: take
        // the vault's lock, which waits for them, and keep it until the process ends.
        Picker::Close(); Flyout::Hide();
        try { new Accounts::Lock(); } catch (...) {}
        if (iconAdded) { NOTIFYICONDATAW d{ sizeof d }; d.hWnd = tray; d.uID = 1; Shell_NotifyIconW(NIM_DELETE, &d); iconAdded = false; }
        PostQuitMessage(0);
    }

    // ------------------------------------------------------------ actions

    struct SwitchDone { std::wstring id; Accounts::SwitchResult r; bool quiet = false; };

    // The app opens with the chosen account unless Settings turned that off, and only when it
    // is installed: without it the CLI and editors still switch, and no error comes of it.
    void Switch(std::wstring id, bool fromPicker, bool openApp) {
        DoSwitch(std::move(id), fromPicker, openApp || (OpenAfterSwitch() && state.appInstalled), true);
    }

    static void DoSwitch(std::wstring id, bool fromPicker, bool openApp, bool ask) {
        auto it = Find(id);
        if (!it || it->deleting || it->switching) return;
        Test::Trace(Util::Format(L"switch|%s|%s", id.c_str(), openApp ? L"open" : L"stay"));
        if (it->active && !openApp) {
            if (fromPicker && HideAfterSwitch()) SetTimer(tray, TimerHide, 250, nullptr);
            return;
        }
        if (ask && !it->active && RestartOnSwitch() && AskBeforeRestart() && (state.appRunning || state.editors)) {
            auto name = it->shownName;
            auto text = state.appRunning
                ? Util::Format(Tr(L"%s closes and opens again with this account. Tasks running in it stop, and unsent text is lost. Settings can turn this question off",
                                  L"%s закроется и откроется с этим аккаунтом. Запущенные в нём задачи остановятся, неотправленный текст пропадёт. Этот вопрос можно отключить в настройках"), Codex::AppName().c_str())
                : std::wstring(Tr(L"Codex in your editor reconnects with this account; a task running there stops. Settings can turn this question off",
                                  L"Codex в редакторе переподключится с этим аккаунтом, запущенная там задача остановится. Этот вопрос можно отключить в настройках"));
            if (!Dialogs::Ask(Owner(nullptr), Util::Format(Tr(L"Switch Codex to %s?", L"Переключить Codex на %s?"), Util::Q(name).c_str()), text, Tr(L"Switch", L"Переключить"))) return;
            it = Find(id);
            if (!it || it->deleting || it->switching) return;
        }
        it->switching = true;
        // Conversations being continued would reopen Codex in the middle of this switch.
        if (recovery.running) Recovery::Cancel();
        RedrawAll();
        bool restart = RestartOnSwitch(), quiet = fromPicker && Picker::Visible() && !HideAfterSwitch();
        std::thread([id, restart, openApp, quiet] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            auto d = new SwitchDone{ id };
            d->quiet = quiet;
            try { d->r = Accounts::Switch(id, { restart, openApp }); }
            catch (const std::wstring& e) { d->r.error = e; }
            catch (...) { d->r.error = Tr(L"Something went wrong", L"Что-то пошло не так"); }
            CoUninitialize();
            if (!PostMessageW(tray, WM_SWITCHED, 0, (LPARAM)d)) delete d;
        }).detach();
        if (fromPicker && HideAfterSwitch()) SetTimer(tray, TimerHide, 450, nullptr);
    }

    // "Couldn't open ChatGPT": in a dialog over the window when it shows, as a notification otherwise.
    static void AppDidntOpen(const std::wstring& title) {
        auto text = state.appInstalled
            ? Util::Format(Tr(L"Windows didn't start it. Open %s from the Start menu; it uses the account chosen here",
                              L"Windows его не запустила. Откройте %s из меню «Пуск»: он будет в выбранном здесь аккаунте"), Codex::AppName().c_str())
            : Util::Format(Tr(L"%s isn't installed on this PC. The CLI and editors switch without it",
                              L"%s не установлен на этом компьютере. CLI и редакторы переключаются и без него"), Codex::AppName().c_str());
        if (Picker::Visible()) Dialogs::Info(Picker::Handle(), title, text);
        else Notify(title, text, true);
    }

    void OpenCodex() {
        if (auto a = Active()) { Switch(a->id, false, true); return; }
        std::thread([] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            bool ok = Codex::ShowApp() || Codex::StartApp();
            CoUninitialize();
            if (!ok) OnUi([] { AppDidntOpen(Util::Format(Tr(L"Couldn't open %s", L"Не удалось открыть %s"), Codex::AppName().c_str())); });
        }).detach();
    }

    void RestartCodex() {
        std::thread([] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            auto p = Codex::Scan();
            bool ok = Codex::CloseApp(p);
            if (ok) Codex::EndHelpers(p);
            ok = ok && Codex::StartApp();
            CoUninitialize();
            OnUi([ok] {
                if (!ok) Notify(Util::Format(Tr(L"Couldn't restart %s", L"Не удалось перезапустить %s"), Codex::AppName().c_str()), Tr(L"Close it yourself and open it again", L"Закройте его сами и откройте снова"), true);
                Refresh();
            });
        }).detach();
    }

    // ---- adding

    // credId: the manager's card the sign-in was started from.
    struct AddDone { Accounts::Added a; std::wstring forId; bool fromLogin = false; int imported = 0, skipped = 0; std::wstring credId; };

    static void AddAuth(const json::Value& auth, const std::wstring& forId, bool fromLogin, const std::wstring& credId = L"") {
        std::thread([auth, forId, fromLogin, credId] {
            auto d = new AddDone{ {}, forId, fromLogin, 0, 0, credId };
            try { d->a = Accounts::Add(auth, L"", fromLogin); } catch (const std::wstring& e) { d->a.error = e; } catch (...) { d->a.error = Tr(L"Something went wrong", L"Что-то пошло не так"); }
            if (!PostMessageW(tray, WM_ADDED, 0, (LPARAM)d)) delete d;
        }).detach();
    }

    // open: the page goes to the default browser, as any link would. Without it the sign-in
    // only waits, and the link is taken to another browser by hand.
    static void BeginLogin(const std::wstring& forId, const std::wstring& credId = L"", bool open = true) {
        if (Login::Active()) { if (credId.empty()) ShowPicker(); else Dialogs::Info(Manager::Handle(), Tr(L"Another sign-in is waiting", L"Уже ждёт другой вход"), Tr(L"Finish it in the browser or cancel it first", L"Завершите его в браузере или сначала отмените")); return; }
        std::wstring url, err;
        bool ok = Login::Begin(url, err, [](Login::Result r) {
            auto p = new Login::Result(std::move(r));
            if (!PostMessageW(tray, WM_LOGIN, 0, (LPARAM)p)) delete p;
        });
        if (!ok) {
            Dialogs::Info(Owner(nullptr), Tr(L"Couldn't start signing in", L"Не удалось начать вход"), err);
            return;
        }
        signInLink = url; signInFor = forId; signInCred = credId;
        Test::Trace(L"login|begin|" + forId + (credId.empty() ? L"" : L"|card:" + credId));
        if (credId.empty()) ShowPicker();
        if (open) Util::Open(url);
        RedrawAll();
    }

    void AddAccount() { BeginLogin(L""); }
    void AddAccountByLink() {
        if (Login::Active()) { ShowPicker(); return; }
        BeginLogin(L"", L"", false);
        if (signInLink.empty()) return;
        Test::Trace(L"login|link");
        auto where = Surface::From(Picker::Handle());
        if (Util::CopyText(Picker::Handle(), signInLink) && where) where->Toast(Tr(L"Link copied: paste it into any browser", L"Ссылка скопирована: вставьте её в любой браузер"));
    }
    void SignInWith(const std::wstring& credId) { BeginLogin(L"", credId); }
    std::wstring SigningInFor() { return Login::Active() ? signInCred : L""; }
    void SignInAgain(const std::wstring& id) { BeginLogin(id); }
    void CancelSignIn() { Test::Trace(L"login|cancel"); Login::Cancel(); signInLink.clear(); signInCred.clear(); RedrawAll(); }
    bool SigningIn() { return Login::Active(); }
    std::wstring SignInLink() { return signInLink; }
    std::wstring SigningInAccount() { return Login::Active() && !signInLink.empty() ? signInFor : L""; }
    bool SigningInNew() { return Login::Active() && !signInLink.empty() && signInFor.empty() && signInCred.empty(); }
    void ReopenSignIn() {
        if (!Login::Active() || signInLink.empty()) return;
        Test::Trace(L"login|reopen");
        Util::Open(signInLink);
    }

    void AddCurrentLogin() {
        auto live = Accounts::ReadLive();
        if (Accounts::IdentityOf(live).empty()) { Dialogs::Info(Owner(nullptr), Tr(L"Codex isn't signed in", L"Codex не выполнил вход"), Tr(L"Sign in to Codex first, or add an account here with Add", L"Сначала войдите в Codex или добавьте аккаунт кнопкой «Добавить»")); return; }
        AddAuth(live, L"", false);
    }

    void ImportFile(HWND owner) {
        wchar_t file[MAX_PATH * 2] = {};
        OPENFILENAMEW of{ sizeof of };
        of.hwndOwner = Owner(owner);
        of.lpstrFilter = Tr(L"Codex sign-in (auth.json)\0*.json\0All files\0*.*\0", L"Вход Codex (auth.json)\0*.json\0Все файлы\0*.*\0");
        of.lpstrFile = file; of.nMaxFile = ARRAYSIZE(file);
        of.lpstrTitle = Tr(L"Choose an auth.json", L"Выберите auth.json"); of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!PickFile(of, false)) return;
        std::wstring err;
        auto auth = Accounts::ReadAuthFile(file, err);
        if (auth.IsNull()) { Dialogs::Info(Owner(owner), Tr(L"Couldn't add the account", L"Не удалось добавить аккаунт"), err); return; }
        AddAuth(auth, L"", false);
    }

    void ImportLegacy(HWND owner) {
        std::wstring err;
        auto legacy = Accounts::LegacyAccounts(err);
        if (legacy.empty()) { Dialogs::Info(Owner(owner), Tr(L"Nothing to import", L"Импортировать нечего"), err.empty() ? Util::Format(Tr(L"There are no accounts in %s", L"В %s нет аккаунтов"), Util::Tilde(Accounts::LegacyFile()).c_str()) : err); return; }
        if (!Dialogs::Ask(Owner(owner), Util::Format(Tr(L"Import %s from Codex Switcher?", L"Импортировать %s из Codex Switcher?"), Util::Count((int)legacy.size(), L"account", L"accounts", L"аккаунт", L"аккаунта", L"аккаунтов").c_str()),
                          Tr(L"Names come along; accounts already here get the newer sign-in. Afterwards use one switcher, not both: ChatGPT replaces a sign-in's key whenever it is renewed, so two programs renewing the same account sign each other out",
                             L"Имена перенесутся, у уже добавленных аккаунтов обновится вход. После этого пользуйтесь одним переключателем, а не обоими: ChatGPT меняет ключ входа при каждом продлении, и две программы, продлевающие один аккаунт, выкидывают друг друга"),
                          Tr(L"Import", L"Импортировать"))) return;
        std::thread([] {
            auto d = new AddDone();
            Accounts::Imported r;
            try { r = Accounts::ImportLegacy(); } catch (const std::wstring& e) { r.error = e; } catch (...) {}
            d->imported = r.added + r.updated; d->skipped = r.skipped; d->a.error = r.error;
            d->forId = L"*legacy";
            if (!PostMessageW(tray, WM_ADDED, 0, (LPARAM)d)) delete d;
        }).detach();
    }

    void ExportAuth(const std::wstring& id, HWND owner) {
        auto it = Find(id);
        if (!it) return;
        auto name = it->shownName;
        if (!Dialogs::Ask(Owner(owner), Util::Format(Tr(L"Save the sign-in of %s?", L"Сохранить вход %s в файл?"), Util::Q(name).c_str()),
                          Tr(L"The file works like a password: whoever has it can use this account. Keep it to yourself, and don't use the same sign-in on two PCs at once: renewing it on one signs the other out",
                             L"Этот файл — как пароль: у кого он есть, тот может пользоваться аккаунтом. Не передавайте его и не используйте один вход на двух компьютерах сразу: продление на одном выкинет другой"),
                          Tr(L"Choose where", L"Выбрать место"))) return;
        wchar_t file[MAX_PATH * 2] = L"auth.json";
        OPENFILENAMEW of{ sizeof of };
        of.hwndOwner = Owner(owner);
        of.lpstrFilter = L"auth.json\0*.json\0";
        of.lpstrFile = file; of.nMaxFile = ARRAYSIZE(file); of.lpstrDefExt = L"json";
        of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!PickFile(of, true)) return;
        std::wstring path = file;
        std::thread([id, path] {
            bool ok = false;
            try {
                json::Value auth;
                {
                    Accounts::Lock lock;
                    auto list = Accounts::Load();
                    if (Accounts::SyncLive(list, Accounts::ReadLive())) { try { Accounts::Save(list); } catch (...) {} }
                    for (auto& a : list) if (a.id == id) auth = a.auth;
                }
                std::wstring text; json::Write(text, auth);
                ok = !auth.IsNull() && Util::WriteText(path, text, false);
                SecureZeroMemory(&text[0], text.size() * sizeof(wchar_t));
            } catch (...) {}
            if (!ok) OnUi([path] { Dialogs::Info(Owner(nullptr), Tr(L"Couldn't save the file", L"Не удалось сохранить файл"), path); });
        }).detach();
    }

    // ---- renaming, deleting, moving

    // Started from the message loop rather than right away: a menu that has just closed
    // hands focus back to its window a moment later, which would end the edit at once.
    void BeginRename(const std::wstring& id) { Test::Trace(L"rename-begin|" + id); pendingRename = id; PostMessageW(tray, WM_RENAME, 0, 0); }

    void CommitRename(const std::wstring& id, const std::wstring& raw) {
        auto it = Find(id);
        if (!it) return;
        auto name = Util::Trim(raw);
        if (name.empty() || name == it->name) return;
        Test::Trace(L"rename|" + id + L"|" + name);
        // Checked here for an answer at once; the vault checks again when it saves.
        std::vector<Account> names;
        for (auto& i : items) { Account a; a.id = i.id; a.name = i.name; names.push_back(a); }
        auto err = Accounts::ValidateName(name, names, id);
        if (!err.empty()) { Dialogs::Info(Owner(nullptr), Tr(L"Couldn't rename", L"Не удалось переименовать"), err); return; }
        auto old = it->name;
        it->name = name; it->letters = Core::Initials(name);
        RedrawAll();
        std::thread([id, name, old] {
            std::wstring error;
            try { error = Accounts::Rename(id, name); } catch (const std::wstring& e) { error = e; } catch (...) { error = Tr(L"Something went wrong", L"Что-то пошло не так"); }
            OnUi([id, name, old, error] {
                if (!error.empty()) { Dialogs::Info(Owner(nullptr), Tr(L"Couldn't rename", L"Не удалось переименовать"), error); Refresh(); return; }
                // Shortcuts are named after the account, so put back any that were there.
                if (auto x = Find(id)) for (auto& lnk : WinShell::RemoveShortcuts(old)) WinShell::CreateShortcut(*x, Util::Parent(lnk));
                WinShell::UpdateJumpList(items);
                Refresh();
            });
        }).detach();
    }

    // ---- deleting, with a way back

    // The account just deleted, kept whole for a few seconds: Undo puts it back as it was.
    struct Deleted { Account account; int at = 0; std::wstring name; bool valid = false; };
    static Deleted undone;

    // What an account leaves behind, removed once there is no way back.
    static void Cleanup(const std::wstring& id, const std::wstring& name) {
        WinShell::RemoveShortcuts(name); WinShell::RemoveIcons(id);
        usage.erase(id); Accounts::SaveUsage(usage); Core::ForgetColor(id);
        Manager::AccountGone(id);
    }

    static void ForgetDeleted() {
        if (!undone.valid) return;
        KillTimer(tray, TimerUndo);
        auto d = undone; undone = {};
        Cleanup(d.account.id, d.name);
    }

    bool CanUndoDelete() { return undone.valid; }

    void UndoDelete() {
        if (!undone.valid) return;
        KillTimer(tray, TimerUndo);
        auto d = undone; undone = {};
        Test::Trace(L"restore|" + d.account.id);
        std::thread([d] {
            std::wstring error;
            try { Accounts::Restore(d.account, d.at); } catch (const std::wstring& e) { error = e; } catch (...) { error = Tr(L"Something went wrong", L"Что-то пошло не так"); }
            OnUi([error] {
                if (!error.empty()) Dialogs::Info(Owner(nullptr), Tr(L"Couldn't bring it back", L"Не удалось вернуть"), error);
                Refresh();
            });
        }).detach();
    }

    void Delete(std::wstring id, HWND owner, bool undo) {
        auto it = Find(id);
        if (!it || it->deleting) return;
        auto name = it->shownName;
        Test::Trace(L"delete|" + id);
        ForgetDeleted();                       // one way back at a time
        it->deleting = true;
        RedrawAll();
        std::thread([id, name, undo, owner] {
            std::wstring error;
            Deleted d; d.name = name;
            try {
                auto list = Accounts::Load();
                for (size_t i = 0; i < list.size(); i++) if (list[i].id == id) { d.account = list[i]; d.at = (int)i; d.valid = true; }
                Accounts::Remove(id);
            } catch (const std::wstring& e) { error = e; } catch (...) { error = Tr(L"Something went wrong", L"Что-то пошло не так"); }
            OnUi([id, name, error, d, undo, owner] {
                if (!error.empty()) { if (auto x = Find(id)) x->deleting = false; Dialogs::Info(Owner(nullptr), Tr(L"Couldn't delete", L"Не удалось удалить"), error); }
                else if (undo && d.valid) {
                    // Nothing was asked, so nothing is final yet: its shortcuts, limits and
                    // browser profile wait until the way back closes.
                    undone = d;
                    SetTimer(tray, TimerUndo, 9500, nullptr);
                    auto where = Surface::From(owner);
                    if (!where) where = Surface::From(Picker::Handle());
                    auto text = Util::Format(Tr(L"%s deleted", L"%s удалён"), Util::Q(name).c_str());
                    if (where && where->Visible()) where->Toast(text, Tr(L"Undo", L"Отменить"), [] { UndoDelete(); });
                }
                else Cleanup(id, name);
                Refresh();
            });
        }).detach();
    }

    void Move(const std::wstring& id, int delta) {
        auto it = Find(id);
        if (!it) return;
        Test::Trace(Util::Format(L"move|%s|%d", id.c_str(), delta));
        int to = it->index - 1 + delta;
        if (to < 0 || to >= (int)items.size()) return;
        // Moved on screen at once; the vault follows.
        auto moved = *it;
        items.erase(items.begin() + (it->index - 1));
        items.insert(items.begin() + to, moved);
        for (size_t k = 0; k < items.size(); k++) items[k].index = (int)k + 1;
        RedrawAll();
        std::thread([id, to] { try { Accounts::Move(id, to); } catch (...) {} OnUi([] { Refresh(); }); }).detach();
    }

    // The ⋯ menu of an account card. Native, so it has the system's look, animation and
    // keyboard behaviour; uxtheme's dark mode makes it follow the theme.
    void ShowMenu(std::wstring id, POINT at, HWND owner) {
        // Own the id across the menu's nested message loop: a refresh replaces the list.
        auto it = Find(id);
        if (!it || it->deleting) return;
        WinShell::DarkMenus(Gfx::T.dark);
        WinShell::AllowDark(owner, Gfx::T.dark);
        HMENU m = CreatePopupMenu();
        if (!it->email.empty()) { AppendMenuW(m, MF_STRING | MF_GRAYED, 0, it->shownEmail.c_str()); AppendMenuW(m, MF_SEPARATOR, 0, nullptr); }
        auto key = it->index <= 9 ? Util::Format(L"\t%d", it->index) : L"";
        // The digit goes with whatever a click does: open the app with it, or only switch.
        bool clickOpens = OpenAfterSwitch() && state.appInstalled;
        AppendMenuW(m, MF_STRING | (state.appInstalled ? 0 : MF_GRAYED), 2, (Util::Format(Tr(L"Open %s with it", L"Открыть %s с ним"), Codex::AppName().c_str()) + (clickOpens ? key : L"")).c_str());
        AppendMenuW(m, MF_STRING | (it->active ? MF_GRAYED : 0), 1, (std::wstring(clickOpens ? Tr(L"Switch without opening the app", L"Переключить, не открывая приложение") : Tr(L"Use this account", L"Использовать этот аккаунт")) + (clickOpens ? L"" : key)).c_str());
        AppendMenuW(m, MF_STRING | (it->apiKey ? MF_GRAYED : 0), 12, Tr(L"Details and statistics…\tI", L"Подробности и статистика…\tI"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (it->apiKey || it->fetching ? MF_GRAYED : 0), 3, Tr(L"Refresh limits\tF5", L"Обновить лимиты\tF5"));
        AppendMenuW(m, MF_STRING | (it->apiKey || it->warming ? MF_GRAYED : 0), 4, Tr(L"Start the 5-hour window now", L"Запустить 5-часовое окно сейчас"));
        AppendMenuW(m, MF_STRING | (it->apiKey || AutoWarmup() ? MF_GRAYED : 0) | (it->autoWarm || AutoWarmup() ? MF_CHECKED : 0), 13, Tr(L"Start its 5-hour windows automatically", L"Автоматически запускать его 5-часовые окна"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 5, Tr(L"Rename\tF2", L"Переименовать\tF2"));
        AppendMenuW(m, MF_STRING | (it->index > 1 ? 0 : MF_GRAYED), 6, Tr(L"Move up\tCtrl+↑", L"Поднять\tCtrl+↑"));
        AppendMenuW(m, MF_STRING | (it->index < (int)items.size() ? 0 : MF_GRAYED), 7, Tr(L"Move down\tCtrl+↓", L"Опустить\tCtrl+↓"));
        AppendMenuW(m, MF_STRING, 8, Tr(L"Desktop shortcut", L"Ярлык на рабочий стол"));
        AppendMenuW(m, MF_STRING | (it->masked ? MF_CHECKED : 0) | (MaskAll() ? MF_GRAYED : 0), 14, Tr(L"Hide its email", L"Скрывать его email"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (it->apiKey ? MF_GRAYED : 0), 9, Tr(L"Sign in again…", L"Войти заново…"));
        AppendMenuW(m, MF_STRING | (it->apiKey ? MF_GRAYED : 0), 16, Tr(L"Password, 2FA and order…", L"Пароль, 2FA и заказ…"));
        AppendMenuW(m, MF_STRING, 10, Tr(L"Save auth.json…", L"Сохранить auth.json…"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        // No ellipsis: nothing is asked, and Undo is there for a slip.
        AppendMenuW(m, MF_STRING, 11, Tr(L"Delete\tDel", L"Удалить\tDel"));
        SetMenuDefaultItem(m, clickOpens ? 2 : 1, FALSE);
        int cmd = RunMenu(m, TPM_RIGHTBUTTON, at, owner);
        DestroyMenu(m);
        it = Find(id);
        if (!it) return;
        switch (cmd) {
        case 1: DoSwitch(id, true, false, true); break;
        case 2: Switch(id, true, true); break;
        case 3: RefreshUsage(true, id); break;
        case 4: Warmup({ id }); break;
        case 5: BeginRename(id); break;
        case 16: Manager::ShowAccount(id); break;
        case 6: Move(id, -1); break;
        case 7: Move(id, 1); break;
        case 8: {
            auto lnk = WinShell::CreateShortcut(*it, Paths::Desktop());
            auto leaf = Util::Leaf(lnk);
            if (leaf.size() > 4) Notify(Tr(L"Shortcut created", L"Ярлык готов"), Util::Format(Tr(L"%s is on the desktop", L"%s на рабочем столе"), Util::Q(leaf.substr(0, leaf.size() - 4)).c_str()));
            else Notify(Tr(L"Couldn't create the shortcut", L"Не удалось создать ярлык"), Paths::Desktop(), true);
            break;
        }
        case 9: SignInAgain(id); break;
        case 10: ExportAuth(id, owner); break;
        case 11: Delete(id, owner); break;
        case 12: ShowDetails(id); break;
        case 13: ToggleAutoWarm(id); break;
        case 14: ToggleMask(id); break;
        }
    }

    // ------------------------------------------------------------ timed warm-ups

    // At each set time (within two minutes of it, once a day) every account with week left
    // starts its window. A time missed while the PC slept is skipped, not caught up late.
    static void CheckTimedWarmups() {
        if (!TimedWarmup() || items.empty()) return;
        SYSTEMTIME t; GetLocalTime(&t);
        int now = t.wHour * 60 + t.wMinute;
        auto today = Util::Format(L"%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
        auto fired = Settings::Load();
        auto last = fired.Find(L"WarmupFired");
        for (auto& time : WarmupTimes()) {
            int h = 0, m = 0;
            if (swscanf_s(time.c_str(), L"%d:%d", &h, &m) != 2) continue;
            int at = h * 60 + m;
            if (now < at || now - at > 2) continue;
            if (last && last->Str(time) == today) continue;
            Settings::Update([&](json::Value& all) { all[L"WarmupFired"][time] = today; });
            std::vector<std::wstring> ids;
            for (auto& i : items) if (!i.apiKey && !i.usage.signedOut && i.usage.WeekLeft() != 0) ids.push_back(i.id);
            Warmup(ids);
        }
    }

    // ------------------------------------------------------------ statistics

    const Stats* StatsOf(const std::wstring& id) { auto it = stats.find(id); return it == stats.end() ? nullptr : &it->second; }
    bool StatsLoading(const std::wstring& id) { return statsBusy.count(id) > 0; }

    void FetchStats(const std::wstring& id) {
        auto it = Find(id);
        if (!it || it->apiKey || statsBusy.count(id)) return;
        statsBusy.insert(id);
        statsTried[id] = Core::NowMs();
        RedrawAll();
        std::thread([id] {
            auto r = new std::pair<std::wstring, Stats>(id, Stats());
            try { r->second = Accounts::FetchStats(id); } catch (const std::wstring& e) { r->second.problem = e; } catch (...) { r->second.problem = Tr(L"Something went wrong", L"Что-то пошло не так"); }
            if (!PostMessageW(tray, WM_STATS, 0, (LPARAM)r)) delete r;
        }).detach();
    }

    void ShowDetails(const std::wstring& id) {
        if (!Find(id)) return;
        Test::Trace(L"window|details|" + id);
        Details::Show(id);
        auto st = stats.find(id);
        long long now = Core::NowMs();
        if ((st == stats.end() || now - st->second.at > 5ll * 60 * 1000) && now - statsTried[id] > 60 * 1000) FetchStats(id);
        RefreshUsage(true, id);
    }

    // ------------------------------------------------------------ conversations stopped by the limit

    const RecoveryState& Recovering() { return recovery; }
    void DismissRecovery() { recovery.shown = false; RedrawAll(); }

    static void OnRecovered(const Recovery::Report& r, bool asked) {
        recovery.running = false;
        recovery.report = r;
        recovery.done = r.total;
        if (r.total == 0) {
            recovery.shown = false;
            if (asked) Notify(Tr(L"Nothing to continue", L"Продолжать нечего"), Tr(L"No conversation of the last 24 hours stopped at the limit", L"За последние сутки ни один разговор не остановился на лимите"));
            RedrawAll();
            return;
        }
        recovery.shown = true;
        auto title = Util::Format(Tr(L"Continued %d of %d conversations", L"Продолжено разговоров: %d из %d"), r.resumed, r.total - r.skipped);
        std::wstring text = r.failed.empty() ? std::wstring(Tr(L"They carry on with the new account", L"Они продолжаются с новым аккаунтом"))
                                             : r.failed[0].reason;
        if (!Picker::Visible()) {
            std::wstring first = r.failed.empty() ? L"" : r.failed[0].id;
            Notify(title, text, !r.failed.empty(), first.empty() ? std::function<void()>() : [first] { Recovery::Open(first); });
        }
        RedrawAll();
    }

    void ContinueConversations(bool asked) {
        if (recovery.running) return;
        Test::Trace(L"continue");
        recovery = RecoveryState(); recovery.running = true;
        RedrawAll();
        Recovery::Cancel(false);
        recoveryAlive = true;
        std::thread([asked] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            auto report = new Recovery::Report();
            try {
                auto sessions = Recovery::FindStopped();
                if (!sessions.empty()) {
                    PostMessageW(tray, WM_RECOVERY, 0, 0);
                    *report = Recovery::Continue(sessions, [](int done, int total) {
                        OnUi([done, total] { recovery.done = done; recovery.total = total; RedrawAll(); });
                    });
                }
            } catch (...) {}
            CoUninitialize();
            if (!PostMessageW(tray, WM_RECOVERY, asked ? 1 : 0, (LPARAM)report)) delete report;
            recoveryAlive = false;
        }).detach();
    }

    // ------------------------------------------------------------ backups

    void ExportAll(HWND owner) {
        if (items.empty()) { Dialogs::Info(Owner(owner), Tr(L"Nothing to back up", L"Сохранять нечего"), Tr(L"Add an account first", L"Сначала добавьте аккаунт")); return; }
        int how = Dialogs::Choose(Owner(owner), Tr(L"Back up all accounts", L"Резервная копия всех аккаунтов"),
                                  Util::Format(Tr(L"%s with their sign-ins, encrypted with a password you choose. Import it on another PC, or here after reinstalling Windows. Without the password it can't be opened",
                                                  L"%s со входами, зашифрованные паролем, который вы выберете. Импортируйте копию на другом компьютере или здесь после переустановки Windows. Без пароля её не открыть"),
                                               Util::Count((int)items.size(), L"account", L"accounts", L"аккаунт", L"аккаунта", L"аккаунтов").c_str()),
                                  { Tr(L"Save to a file…", L"Сохранить в файл…"), Tr(L"Copy as text", L"Скопировать текстом"), Tr(L"Cancel", L"Отмена") });
        if (how < 0 || how == 2) return;
        std::wstring password;
        if (!Dialogs::Prompt(Owner(owner), Tr(L"Choose a password", L"Придумайте пароль"),
                             Tr(L"At least 8 characters. Keep it: it can't be recovered, and the backup is useless without it",
                                L"Не короче 8 символов. Запомните его: восстановить пароль нельзя, а без него копия бесполезна"),
                             password, true, true, Tr(L"Encrypt", L"Зашифровать"))) return;
        auto pw = std::make_shared<std::wstring>(std::move(password));
        std::thread([pw, how] {
            std::string file; std::wstring error;
            try { file = Accounts::ExportBackup(*pw); } catch (const std::wstring& e) { error = e; } catch (...) { error = Tr(L"Something went wrong", L"Что-то пошло не так"); }
            SecureZeroMemory(&(*pw)[0], pw->size() * sizeof(wchar_t));
            OnUi([file, error, how] {
                if (!error.empty()) { Dialogs::Info(Owner(nullptr), Tr(L"Couldn't make the backup", L"Не удалось сделать копию"), error); return; }
                if (how == 1) {
                    auto text = Util::Wide(Accounts::BackupText(file));
                    bool copied = Util::CopyText(tray, text);
                    SecureZeroMemory(&text[0], text.size() * sizeof(wchar_t));
                    if (!copied) { Dialogs::Info(Owner(nullptr), Tr(L"Couldn't copy", L"Не удалось скопировать"), Tr(L"Another program is holding the clipboard. Try again, or save to a file", L"Буфер обмена занят другой программой. Попробуйте ещё раз или сохраните в файл")); return; }
                    Dialogs::Info(Owner(nullptr), Tr(L"Copied", L"Скопировано"), Tr(L"Paste it into Import accounts on the other PC and enter the password there. Don't leave it lying in chats or notes",
                                                                                     L"Вставьте его в «Импорт аккаунтов» на другом компьютере и введите там пароль. Не оставляйте его в чатах и заметках"));
                    return;
                }
                SYSTEMTIME t; GetLocalTime(&t);
                auto name = Util::Format(L"codex-accounts-%04d-%02d-%02d.casbackup", t.wYear, t.wMonth, t.wDay);
                wchar_t path[MAX_PATH * 2] = {}; wcscpy_s(path, name.c_str());
                OPENFILENAMEW of{ sizeof of };
                of.hwndOwner = Owner(nullptr);
                of.lpstrFilter = Tr(L"Codex accounts backup\0*.casbackup\0All files\0*.*\0", L"Резервная копия аккаунтов Codex\0*.casbackup\0Все файлы\0*.*\0");
                of.lpstrFile = path; of.nMaxFile = ARRAYSIZE(path); of.lpstrDefExt = L"casbackup";
                of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                if (!PickFile(of, true)) return;
                if (!Util::WriteBytes(path, file)) Dialogs::Info(Owner(nullptr), Tr(L"Couldn't save the file", L"Не удалось сохранить файл"), path);
                else Notify(Tr(L"Backup saved", L"Копия сохранена"), Util::Leaf(path));
            });
        }).detach();
    }

    static void ImportBackupData(HWND owner, std::string data);

    void ImportBackup(HWND owner) {
        int how = Dialogs::Choose(Owner(owner), Tr(L"Import accounts", L"Импорт аккаунтов"),
                                  Tr(L"From a backup made with Back up all accounts, as a file or text. Codex Switcher's own exports work too",
                                     L"Из резервной копии, сделанной кнопкой «Резервная копия всех аккаунтов», файлом или текстом. Экспорт из Codex Switcher тоже подходит"),
                                  { Tr(L"Choose a file…", L"Выбрать файл…"), Tr(L"Paste text", L"Вставить текст"), Tr(L"Cancel", L"Отмена") });
        if (how < 0 || how == 2) return;
        std::string data;
        if (how == 0) {
            wchar_t path[MAX_PATH * 2] = {};
            OPENFILENAMEW of{ sizeof of };
            of.hwndOwner = Owner(owner);
            of.lpstrFilter = Tr(L"Backups (*.casbackup, *.cswf)\0*.casbackup;*.cswf\0All files\0*.*\0", L"Резервные копии (*.casbackup, *.cswf)\0*.casbackup;*.cswf\0Все файлы\0*.*\0");
            of.lpstrFile = path; of.nMaxFile = ARRAYSIZE(path); of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (!PickFile(of, false)) return;
            data = Util::ReadBytes(path, 16u << 20);
        } else {
            std::wstring text;
            if (!Dialogs::Prompt(Owner(owner), Tr(L"Paste the backup", L"Вставьте копию"), Tr(L"It starts with cas1. (or css1. from Codex Switcher)", L"Она начинается с cas1. (или css1. из Codex Switcher)"), text, false, false, Tr(L"Next", L"Дальше"))) return;
            data = Util::Utf8(text);
        }
        ImportBackupData(owner, std::move(data));
    }

    // A backup already in hand: from the file dialog, the clipboard or a file dropped on a window.
    static void ImportBackupData(HWND owner, std::string data) {
        auto kind = Accounts::KindOf(data);
        if (kind == Accounts::BackupKind::None) { Dialogs::Info(Owner(owner), Tr(L"Couldn't import", L"Не удалось импортировать"), Tr(L"This isn't a backup of accounts", L"Это не резервная копия аккаунтов")); return; }
        std::wstring password;
        if (kind == Accounts::BackupKind::Ours &&
            !Dialogs::Prompt(Owner(owner), Tr(L"Backup password", L"Пароль копии"), Tr(L"The one chosen when the backup was made", L"Тот, что выбрали при создании копии"), password, true, false, Tr(L"Import", L"Импортировать"))) return;
        auto shared = std::make_shared<std::pair<std::string, std::wstring>>(std::move(data), std::move(password));
        std::thread([shared] {
            Accounts::Imported r;
            try { r = Accounts::ImportBackup(shared->first, shared->second); } catch (const std::wstring& e) { r.error = e; } catch (...) { r.error = Tr(L"Something went wrong", L"Что-то пошло не так"); }
            if (!shared->second.empty()) SecureZeroMemory(&shared->second[0], shared->second.size() * sizeof(wchar_t));
            OnUi([r] {
                Refresh();
                if (r.added + r.updated == 0) { Dialogs::Info(Owner(nullptr), Tr(L"Couldn't import", L"Не удалось импортировать"), r.error.empty() ? std::wstring(Tr(L"There were no accounts in it", L"В ней не было аккаунтов")) : r.error); return; }
                std::wstring text = Util::Format(Tr(L"New: %d, updated: %d", L"Новых: %d, обновлено: %d"), r.added, r.updated);
                if (r.skipped) text += Util::Format(Tr(L". Skipped: %d", L". Пропущено: %d"), r.skipped);
                if (!r.error.empty()) text += Tr(L". The import stopped early: ", L". Импорт прервался: ") + r.error;
                Dialogs::Info(Owner(nullptr), Tr(L"Accounts imported", L"Аккаунты импортированы"), text);
                RefreshUsage(true);
            });
        }).detach();
    }

    // ------------------------------------------------------------ adding, whatever it comes as

    // Text that holds an account in one of the three shapes it travels in: a sign-in
    // (auth.json), a backup, or a shop's order or a line email|password|2FA key.
    static bool TakeText(const std::wstring& text, HWND owner) {
        auto t = Util::Trim(text);
        if (t.empty()) return false;
        if (t[0] == L'{') {
            json::Value v;
            if (json::Parser::Parse(t, v) && v.type == json::Value::Object && !Accounts::IdentityOf(v).empty()) { Test::Trace(L"take|auth"); AddAuth(v, L"", false); return true; }
        }
        auto bytes = Util::Utf8(t);
        if (Accounts::KindOf(bytes) != Accounts::BackupKind::None) { Test::Trace(L"take|backup"); ImportBackupData(owner, bytes); return true; }
        if (!Creds::Parse(t, Core::NowMs()).creds.empty()) { Test::Trace(L"take|order"); Manager::ImportText(owner, t); return true; }
        return false;
    }

    static void Say(HWND owner, const std::wstring& title, const std::wstring& text) {
        auto where = Surface::From(owner);
        if (where && where->Visible()) where->Toast(title);
        else Dialogs::Info(Owner(owner), title, text);
    }

    void PasteAccounts(HWND owner) {
        if (TakeText(Util::PasteText(owner), owner)) return;
        Test::Trace(L"take|nothing");
        Say(owner, Tr(L"No account on the clipboard", L"В буфере обмена нет аккаунта"),
            Tr(L"Copy a shop's message, a line email|password|2FA key, an auth.json or a backup, and paste again",
               L"Скопируйте сообщение магазина, строку email|пароль|ключ 2FA, auth.json или резервную копию и вставьте снова"));
    }

    void AddFiles(const std::vector<std::wstring>& files, HWND owner) {
        for (auto& path : files) {
            Test::Trace(L"drop|" + Util::Leaf(path));
            auto bytes = Util::ReadBytes(path, 16u << 20);
            if (Accounts::KindOf(bytes) != Accounts::BackupKind::None) { Test::Trace(L"take|backup"); ImportBackupData(owner, bytes); continue; }
            if (TakeText(Util::Wide(bytes), owner)) continue;
            Dialogs::Info(Owner(owner), Util::Format(Tr(L"%s isn't an account", L"%s — не аккаунт"), Util::Q(Util::Leaf(path)).c_str()),
                          Tr(L"A file dropped here can be an auth.json, a backup of accounts, or text with lines email|password|2FA key",
                             L"Сюда можно перетащить auth.json, резервную копию аккаунтов или текст со строками email|пароль|ключ 2FA"));
        }
    }

    void AddMenu(POINT at, HWND owner) {
        WinShell::AllowDark(owner, Gfx::T.dark);
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING | (Login::Active() ? MF_GRAYED : 0), 1, Tr(L"Sign in in the browser\tCtrl+N", L"Войти в браузере\tCtrl+N"));
        AppendMenuW(m, MF_STRING | (Login::Active() ? MF_GRAYED : 0), 7, Tr(L"Copy the sign-in link, for another browser", L"Скопировать ссылку входа, для другого браузера"));
        AppendMenuW(m, MF_STRING, 2, Tr(L"Paste an order or email|password|2FA key\tCtrl+V", L"Вставить заказ или email|пароль|ключ 2FA\tCtrl+V"));
        AppendMenuW(m, MF_STRING | (state.unknownLogin.empty() ? MF_GRAYED : 0), 3,
                    state.unknownLogin.empty() ? Tr(L"Take the sign-in Codex uses now", L"Взять вход, который сейчас у Codex")
                                               : Util::Format(Tr(L"Take the sign-in Codex uses now (%s)", L"Взять вход, который сейчас у Codex (%s)"), state.unknownLogin.c_str()).c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 4, Tr(L"From an auth.json file…", L"Из файла auth.json…"));
        AppendMenuW(m, MF_STRING, 5, Tr(L"From a backup…", L"Из резервной копии…"));
        if (state.legacy > 0) AppendMenuW(m, MF_STRING, 6, Util::Format(Tr(L"From Codex Switcher (%d)…", L"Из Codex Switcher (%d)…"), state.legacy).c_str());
        SetMenuDefaultItem(m, 1, FALSE);
        int cmd = RunMenu(m, TPM_RIGHTBUTTON, at, owner);
        DestroyMenu(m);
        switch (cmd) {
        case 1: AddAccount(); break;
        case 7: AddAccountByLink(); break;
        case 2: PasteAccounts(owner); break;
        case 3: AddCurrentLogin(); break;
        case 4: ImportFile(owner); break;
        case 5: ImportBackup(owner); break;
        case 6: ImportLegacy(owner); break;
        }
    }

    // ------------------------------------------------------------ the browser dashboard

    // Runs on the UI thread and waits for it: five seconds at most, and not at all once the
    // server is stopping (the UI thread may be the one stopping it). A job that was given up
    // never runs later, so the page is never told "failed" about something that then happens.
    struct SyncJob {
        std::function<void()> f; HANDLE done;
        std::atomic<int> state{ 0 };       // 0 waiting, 1 taken by the UI thread, 2 given up
        ~SyncJob() { CloseHandle(done); }
    };
    static bool RunAndWait(std::function<void()> f) {
        auto job = std::make_shared<SyncJob>();
        job->f = std::move(f); job->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        auto boxed = new std::shared_ptr<SyncJob>(job);
        if (!PostMessageW(tray, WM_SYNC, 0, (LPARAM)boxed)) { delete boxed; return false; }
        HANDLE waits[] = { job->done, Web::StopSignal() };
        if (WaitForMultipleObjects(2, waits, FALSE, 5000) == WAIT_OBJECT_0) return true;
        int waiting = 0;
        if (job->state.compare_exchange_strong(waiting, 2)) return false;
        WaitForSingleObject(job->done, INFINITE);   // the UI thread is running it right now
        return true;
    }

    static std::string BuildState() {
        auto v = json::Value::MakeObject();
        v[L"lang"] = L10n::ru ? L"ru" : L"en";
        v[L"version"] = Update::Version();
        auto& c = v[L"codex"]; c = json::Value::MakeObject();
        c[L"installed"] = state.appInstalled; c[L"appRunning"] = state.appRunning; c[L"terminals"] = state.terminals;
        c[L"appName"] = Codex::AppName();
        auto& list = v[L"accounts"]; list = json::Value::MakeArray();
        auto best = MostLeft();
        long long now = Core::NowMs() / 1000;
        for (auto& it : items) {
            auto a = json::Value::MakeObject();
            a[L"id"] = it.id; a[L"name"] = it.shownName; a[L"email"] = it.shownEmail; a[L"plan"] = it.PlanLabel();
            a[L"active"] = it.active; a[L"best"] = &it == best; a[L"apiKey"] = it.apiKey;
            a[L"busy"] = it.switching || it.warming || it.deleting; a[L"status"] = it.Status();
            if (!it.usage.problem.empty()) a[L"problem"] = it.usage.problem;
            if (it.usage.known) {
                auto& lim = a[L"limits"]; lim = json::Value::MakeArray();
                auto& u = it.usage;
                if (u.five >= 0) {
                    auto l = json::Value::MakeObject(); l[L"week"] = false; l[L"left"] = u.FiveLeft(); l[L"reset"] = (double)u.fiveReset;
                    l[L"started"] = u.FiveLeft() <= 0 || (u.fiveReset > now && (u.FiveNow() > 0 || u.fiveReset - now < 5 * 3600 - 180));
                    lim.arr.push_back(l);
                }
                if (u.week >= 0) { auto l = json::Value::MakeObject(); l[L"week"] = true; l[L"left"] = u.WeekLeft(); l[L"reset"] = (double)u.weekReset; l[L"started"] = true; lim.arr.push_back(l); }
            }
            list.arr.push_back(a);
        }
        std::wstring text; json::Write(text, v);
        return Util::Utf8(text);
    }

    std::string StateJson() {
        auto out = std::make_shared<std::string>();
        if (!RunAndWait([out] { *out = BuildState(); })) return "";
        return *out;
    }

    int RunRemote(const std::wstring& what, const std::wstring& id) {
        auto ok = std::make_shared<bool>(false);
        bool ran = RunAndWait([ok, what, id] {
            if (what == L"refresh") { RefreshUsage(true); *ok = true; }
            else if (what == L"open") { OpenCodex(); *ok = true; }
            else if (what == L"warmup") { if (Find(id)) { Warmup({ id }); *ok = true; } }
            // The page asked already, so no second question on this PC's screen.
            else if (what == L"switch") { if (Find(id)) { DoSwitch(id, false, false, false); *ok = true; } }
        });
        return !ran ? -1 : *ok ? 1 : 0;
    }

    // ------------------------------------------------------------ messages

    static ULONGLONG trayDoubleAt = 0;

    static LRESULT CALLBACK TrayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
        case WM_TRAY:
            switch (LOWORD(l)) {
            // Version 4 also sends the raw button messages; reacting to those as well would
            // open and immediately close the flyout.
            case NIN_SELECT: case NIN_KEYSELECT:
                // The shell can follow a double click with one more select; it must not reopen the flyout.
                if (GetTickCount64() - trayDoubleAt < GetDoubleClickTime()) return 0;
                Flyout::Toggle(); return 0;
            // The first click of the two has opened the flyout already; the double click swaps it for the window.
            case WM_LBUTTONDBLCLK: trayDoubleAt = GetTickCount64(); Flyout::Hide(); ShowPicker(); return 0;
            case WM_CONTEXTMENU: TrayMenu(); return 0;
            case NIN_BALLOONUSERCLICK: if (auto f = std::move(balloonClick)) { balloonClick = nullptr; f(); } return 0;
            case NIN_BALLOONTIMEOUT: case NIN_BALLOONHIDE: balloonClick = nullptr; return 0;
            }
            return 0;
        case WM_HOTKEY:
            if (w == HotkeyPicker) { if (Picker::Visible() && Picker::Active()) ClosePicker(); else ShowPicker(); }
            else if (w > HotkeyAccount && w <= HotkeyAccount + 9) {
                int d = (int)w - HotkeyAccount;
                for (auto& i : items) if (i.index == d) { Switch(i.id, false); break; }
            }
            return 0;
        case WM_REFRESHED: {
            std::unique_ptr<Snapshot> s((Snapshot*)l);
            refreshing = false;
            Merge(*s);
            if (!pendingRename.empty() && Find(pendingRename)) PostMessageW(tray, WM_RENAME, 0, 0);
            if (refreshAgain) { refreshAgain = false; Refresh(); }
            return 0;
        }
        case WM_SWITCHED: {
            std::unique_ptr<SwitchDone> d((SwitchDone*)l);
            if (auto it = Find(d->id)) it->switching = false;
            auto& r = d->r;
            auto it = Find(d->id);
            auto name = it ? it->shownName : d->id;
            auto appName = Codex::AppName();
            Test::Trace(Util::Format(L"switched|%s|%s|%s%s%s", d->id.c_str(), r.error.empty() ? (r.already ? L"already" : L"ok") : r.error.c_str(),
                                     r.closedApp ? L"closed " : L"", r.startedApp ? L"started " : L"", r.appFailed ? L"app-failed" : L""));
            if (!r.error.empty()) {
                if (Picker::Visible()) Dialogs::Info(Picker::Handle(), Tr(L"Couldn't switch", L"Не удалось переключить"), r.error);
                else Notify(Tr(L"Couldn't switch", L"Не удалось переключить"), r.error, true);
            } else if (r.appFailed) {
                AppDidntOpen(r.already ? Util::Format(Tr(L"Couldn't open %s", L"Не удалось открыть %s"), appName.c_str())
                                       : Util::Format(Tr(L"Codex uses %s now, but %s didn't open", L"Codex теперь в %s, но %s не открылся"), name.c_str(), appName.c_str()));
            } else if (!r.already) {
                std::vector<std::wstring> notes;
                if (r.terminals) notes.push_back(Util::Format(Tr(L"%s in terminals keep the previous account until restarted", L"%s в терминалах останутся на прежнем аккаунте до перезапуска"),
                                                              Util::Count(r.terminals, L"Codex session", L"Codex sessions", L"сеанс Codex", L"сеанса Codex", L"сеансов Codex").c_str()));
                if (r.appKeepsOld) notes.push_back(Util::Format(Tr(L"%s keeps the previous account until it restarts", L"%s останется на прежнем аккаунте до перезапуска"), appName.c_str()));
                if (!r.keptUnknown.empty()) notes.push_back(Util::Format(Tr(L"Codex was signed in to %s, which wasn't on the list: it was added so it isn't lost", L"Codex был в аккаунте %s, которого не было в списке: он добавлен, чтобы не пропасть"), r.keptUnknown.c_str()));
                std::wstring text;
                for (auto& n : notes) text += (text.empty() ? L"" : L". ") + n;
                if (text.empty()) text = r.closedApp ? Util::Format(Tr(L"%s restarted with it", L"%s перезапущен с ним"), appName.c_str())
                                       : r.startedApp ? Util::Format(Tr(L"%s is opening with it", L"%s открывается с ним"), appName.c_str())
                                       : std::wstring(Tr(L"Codex, the CLI and editors use it from now on", L"Codex, CLI и редакторы теперь используют его"));
                // The app coming up says it all; a note still matters.
                if ((!d->quiet && !r.startedApp) || !notes.empty()) Notify(Util::Format(Tr(L"Now using %s", L"Теперь используется %s"), name.c_str()), text);
            }
            Refresh();
            RefreshUsage(true, d->id);
            SetTimer(tray, TimerSoon, 2500, nullptr);
            // The app is coming back up with the new account: give it a moment, then continue
            // what the limit stopped.
            if (r.error.empty() && !r.already && r.startedApp && ContinueStopped()) SetTimer(tray, TimerRecovery, 5000, nullptr);
            return 0;
        }
        case WM_USAGE: {
            std::unique_ptr<std::pair<std::wstring, Usage>> u((std::pair<std::wstring, Usage>*)l);
            TakeUsage(u->first, u->second);
            return 0;
        }
        case WM_USAGE_DONE:
            fetching = false;
            for (auto& i : items) i.fetching = false;
            Accounts::SaveUsage(usage);
            RedrawAll();
            if (fetchAgain) { fetchAgain = false; fetchLater.clear(); RefreshUsage(true); }
            else if (!fetchLater.empty()) {
                // One at a time keeps this simple; each call starts after the one before ends.
                auto next = *fetchLater.begin(); fetchLater.erase(fetchLater.begin());
                RefreshUsage(true, next);
            }
            return 0;
        case WM_WARMED: {
            std::unique_ptr<std::pair<std::wstring, std::wstring>> r((std::pair<std::wstring, std::wstring>*)l);
            auto it = Find(r->first);
            auto name = it ? it->shownName : r->first;
            if (it) { it->warming = false; it->warmedAt = Core::NowMs(); }
            if (!r->second.empty()) Notify(Util::Format(Tr(L"Couldn't start the window for %s", L"Не удалось запустить окно для %s"), name.c_str()), r->second, true);
            else if (!AutoWarmup() || Picker::Visible()) Notify(Util::Format(Tr(L"The 5-hour window of %s is running", L"5-часовое окно %s запущено"), name.c_str()), Tr(L"It resets five hours from now", L"Оно сбросится через пять часов"));
            RefreshUsage(true, r->first);
            Refresh();
            return 0;
        }
        case WM_LOGIN: {
            std::unique_ptr<Login::Result> r((Login::Result*)l);
            signInLink.clear();
            auto forId = signInFor; signInFor.clear();
            auto credId = signInCred; signInCred.clear();
            RedrawAll();
            if (r->cancelled || !r->error.empty()) {
                if (!credId.empty()) Manager::SignInStopped();
                if (!r->cancelled) Dialogs::Info(credId.empty() ? Owner(nullptr) : Manager::Handle(), Tr(L"Sign-in didn't finish", L"Вход не завершён"), r->error);
                return 0;
            }
            // A renewed sign-in must be that account's. Another one is offered as an account of
            // its own rather than added behind the person's back.
            if (!forId.empty()) {
                auto got = Accounts::IdentityOf(r->auth);
                std::wstring want;
                for (auto& a : Accounts::Load()) if (a.id == forId) want = a.Identity();
                if (!got.empty() && !want.empty() && got != want) {
                    auto it = Find(forId);
                    auto wanted = it ? it->shownName : forId, who = Accounts::Describe(r->auth).email;
                    if (who.empty()) who = Tr(L"another account", L"другой аккаунт");
                    Test::Trace(L"login|other|" + forId);
                    int c = Dialogs::Choose(Owner(nullptr), Util::Format(Tr(L"You signed in as %s, not %s", L"Вы вошли как %s, а не %s"), who.c_str(), Util::Q(wanted).c_str()),
                                            Util::Format(Tr(L"%s keeps its old sign-in. Try again with the right account, or add %s as an account of its own",
                                                            L"У %s остался прежний вход. Попробуйте ещё раз под нужным аккаунтом или добавьте %s отдельным аккаунтом"), Util::Q(wanted).c_str(), who.c_str()),
                                            { Tr(L"Try again", L"Попробовать ещё раз"), Tr(L"Add it separately", L"Добавить отдельно"), Tr(L"Cancel", L"Отмена") });
                    if (c == 0) { BeginLogin(forId); return 0; }
                    if (c != 1) return 0;
                    forId.clear();   // a new account
                }
            }
            AddAuth(r->auth, forId, true, credId);
            return 0;
        }
        case WM_ADDED: {
            std::unique_ptr<AddDone> d((AddDone*)l);
            if (d->forId == L"*legacy") {
                Refresh();
                if (!d->a.error.empty() && !d->imported) { Dialogs::Info(Owner(nullptr), Tr(L"Couldn't import", L"Не удалось импортировать"), d->a.error); return 0; }
                Dialogs::Info(Owner(nullptr), Util::Format(Tr(L"Imported: %s", L"Импортировано: %s"), Util::Count(d->imported, L"account", L"accounts", L"аккаунт", L"аккаунта", L"аккаунтов").c_str()),
                              d->skipped ? Util::Format(Tr(L"%d couldn't be read and were skipped", L"Не прочитано и пропущено: %d"), d->skipped)
                                         : std::wstring(Tr(L"Their limits appear on the cards in a moment", L"Лимиты появятся на карточках через мгновение")));
                RefreshUsage(true);
                return 0;
            }
            if (!d->a.error.empty()) { Dialogs::Info(Owner(nullptr), Tr(L"Couldn't add the account", L"Не удалось добавить аккаунт"), d->a.error); return 0; }
            usage[d->a.id].signedOut = false; usage[d->a.id].problem.clear();
            if (!d->forId.empty() && d->forId != d->a.id) {
                auto wanted = Find(d->forId);
                Dialogs::Info(Owner(nullptr), Tr(L"That was another account", L"Это был другой аккаунт"),
                              Util::Format(Tr(L"You signed in with a different account, so it was added separately. %s still needs its own sign-in",
                                              L"Вход выполнен в другой аккаунт, поэтому он добавлен отдельно. %s по-прежнему нужно войти"), Util::Q(wanted ? wanted->name : d->forId).c_str()));
            }
            Refresh();
            RefreshUsage(true, d->a.id);
            if (!d->credId.empty()) { Manager::Added(d->credId, d->a.id); return 0; }
            if (d->fromLogin || Picker::Visible()) { Picker::Show(); Picker::Added(d->a.id, d->a.existed); if (!d->a.existed) BeginRename(d->a.id); }
            else Notify(d->a.existed ? Tr(L"Sign-in updated", L"Вход обновлён") : Tr(L"Account added", L"Аккаунт добавлен"), Tr(L"It's on the list now", L"Он теперь в списке"));
            return 0;
        }
        case WM_RENAME:
            if (!pendingRename.empty()) {
                // An account just added may not be on the list yet: it is named once the refresh brings it.
                if (!Find(pendingRename)) return 0;
                auto id = pendingRename; pendingRename.clear(); Picker::Rename(id);
            }
            return 0;
        case WM_RUN: { std::unique_ptr<std::function<void()>> f((std::function<void()>*)l); (*f)(); return 0; }
        case WM_SYNC: {
            std::unique_ptr<std::shared_ptr<SyncJob>> job((std::shared_ptr<SyncJob>*)l);
            int waiting = 0;
            if ((*job)->state.compare_exchange_strong(waiting, 1)) (*job)->f();
            SetEvent((*job)->done);
            return 0;
        }
        case WM_STATS: {
            std::unique_ptr<std::pair<std::wstring, Stats>> r((std::pair<std::wstring, Stats>*)l);
            statsBusy.erase(r->first);
            auto& kept = stats[r->first];
            // A failed attempt keeps what was there, with the problem noted.
            if (!r->second.known && kept.known) { auto p = r->second.problem; auto resets = r->second.resets; r->second = kept; r->second.problem = p; if (resets) r->second.resets = resets; }
            kept = r->second;
            if (auto it = Find(r->first)) {
                if (kept.subscriptionUntil) it->subscriptionUntil = kept.subscriptionUntil;
                if (kept.resets) { it->usage.resetCredits = kept.resets; it->usage.resetExpires = kept.resetNext; usage[r->first] = it->usage; }
            }
            RedrawAll();
            return 0;
        }
        case WM_RECOVERY: {
            if (!l) { recovery.done = (int)w; RedrawAll(); return 0; }
            std::unique_ptr<Recovery::Report> rep((Recovery::Report*)l);
            OnRecovered(*rep, w != 0);
            return 0;
        }
        case WM_COPYDATA: {
            // "switch <id>" or "open <id>", from a shortcut or the jump list.
            auto cd = (COPYDATASTRUCT*)l;
            if (!cd || cd->dwData != 0xC0DE || !cd->lpData || cd->cbData > 1024) return FALSE;
            std::wstring req((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
            while (!req.empty() && req.back() == L'\0') req.pop_back();
            auto sp = req.find(L' ');
            if (sp == std::wstring::npos) return FALSE;
            auto verb = req.substr(0, sp), id = req.substr(sp + 1);
            if (!Find(id)) return FALSE;
            // "open" from a shortcut opens the app; "switch" (the jump list, --switch) does what a
            // click does.
            OnUi([verb, id] { Switch(id, false, verb == L"open"); });
            return TRUE;
        }
        case WM_SIGNAL:
            if (w == 0) ShowPicker();
            else if (w == 1) Exit();
            else if (w == 2) { ShowPicker(); AddAccount(); }
            else if (w == 3) Refresh();
            else if (w == 4 && Test::On()) Manager::Remind();   // the demo's checks: reminders now, not in 30 s
            return 0;
        case WM_TIMER:
            if (w == TimerUndo) { ForgetDeleted(); return 0; }   // the way back closed
            if (w == TimerRefresh) {
                bool busy = Picker::Visible() || Flyout::Visible() || SettingsWin::Visible() || Manager::Visible();
                for (auto& i : items) busy |= i.switching;
                // Every 2 s while a window shows; every 20 s from the tray, to keep renewed tokens.
                if (busy || ++quietTicks >= 10) { quietTicks = 0; Refresh(); }
            } else if (w == TimerSoon) {
                KillTimer(h, TimerSoon);
                Refresh();
            } else if (w == TimerRecovery) {
                KillTimer(h, TimerRecovery);
                ContinueConversations(false);
            } else if (w == TimerUsage) {
                if (UsageMinutes() > 0) RefreshUsage(false);
                CheckTimedWarmups();
                Manager::Remind();
            } else if (w == TimerUpdate) {
                SetTimer(h, TimerUpdate, 6 * 60 * 60 * 1000, nullptr);
                if (Update::Auto()) Update::Check(false);
            } else if (w == TimerHide) {
                KillTimer(h, TimerHide);
                if (Picker::Visible()) ClosePicker();
            }
            return 0;
        case WM_SETTINGCHANGE: {
            // The theme, high contrast and "Animation effects" all arrive here.
            Gfx::ReadSystem();
            bool colours = l && wcscmp((const wchar_t*)l, L"ImmersiveColorSet") == 0;
            if (colours || w == SPI_SETHIGHCONTRAST || Gfx::HighContrast() != Gfx::T.contrast) ThemeChanged();
            return 0;
        }
        }
        if (m == taskbarCreated) { AddIcon(); return 0; }   // Explorer restarted
        return DefWindowProcW(h, m, w, l);
    }

    bool Forward(const std::wstring& request) {
        HWND h = FindWindowW(L"CodexAccountSwitcherTray", nullptr);
        if (!h) return false;
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        // Only the installed switcher (or this very exe), never a demo started from elsewhere.
        auto image = Util::ImageOf(pid);
        if (!Paths::Same(image, Paths::InstalledExe) && !Paths::Same(image, Paths::Self)) return false;
        AllowSetForegroundWindow(pid);
        COPYDATASTRUCT cd{ 0xC0DE, (DWORD)((request.size() + 1) * sizeof(wchar_t)), (PVOID)request.c_str() };
        DWORD_PTR result = 0;
        return SendMessageTimeoutW(h, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &result) && result == TRUE;
    }

    // Single-instance requests from other copies arrive as events, turned into messages here.
    static void Watch(HANDLE e, WPARAM which) {
        HANDLE wait;
        RegisterWaitForSingleObject(&wait, e, [](PVOID p, BOOLEAN) { PostMessageW(tray, WM_SIGNAL, (WPARAM)p, 0); }, (PVOID)which, INFINITE, WT_EXECUTEDEFAULT);
    }

    void Start(StartMode mode) {
        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        WNDCLASSEXW wc{ sizeof wc };
        wc.lpfnWndProc = TrayProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"CodexAccountSwitcherTray";
        RegisterClassExW(&wc);
        tray = CreateWindowExW(0, wc.lpszClassName, L"CodexAccountSwitcherTray", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        smallIcon = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        AddIcon();
        RegisterPicker();
        RegisterAccounts();
        WinShell::DarkMenus(Gfx::T.dark);

        showEvt = CreateEventW(nullptr, FALSE, FALSE, Core::ShowEvent);
        exitEvt = CreateEventW(nullptr, FALSE, FALSE, Core::ExitEvent);
        addEvt = CreateEventW(nullptr, FALSE, FALSE, Core::AddEvent);
        reloadEvt = CreateEventW(nullptr, FALSE, FALSE, Core::ReloadEvent);
        // We own the single-instance mutex, so anything still pending was meant for a copy that is gone.
        ResetEvent(exitEvt); ResetEvent(addEvt);
        Watch(showEvt, 0); Watch(exitEvt, 1); Watch(addEvt, 2); Watch(reloadEvt, 3);

        // The first start: the account Codex is signed in to becomes the first card.
        if (!Accounts::Exists()) {
            auto live = Accounts::ReadLive();
            if (!Accounts::IdentityOf(live).empty()) { try { Accounts::Add(live); } catch (...) {} }
        }
        usage = Accounts::LoadUsage();
        {
            // Names come up at once; the rest follows a moment later from the worker.
            Snapshot s;
            s.installed = Paths::Demo ? !Settings::Flag(L"DemoNoApp", false) : Codex::App().found();
            s.accounts = Accounts::Load();
            s.activeId = Accounts::ActiveId(s.accounts, Accounts::ReadLive());
            for (auto& a : s.accounts) a.auth = json::Value(a.ApiKey() ? json::Value::MakeObject() : json::Value());
            Merge(s);
        }
        Refresh();
        Web::Apply();
        SetTimer(tray, TimerRefresh, 2000, nullptr);
        SetTimer(tray, TimerUsage, 30000, nullptr);
        // The first look for a new version waits until start-up has settled.
        SetTimer(tray, TimerUpdate, 10000, nullptr);
        Update::Cleanup();
        RefreshUsage(false);

        if (mode == StartMode::Window) ShowPicker();
    }
}

// ================================================================ hotkey text

namespace Hotkeys {
    static std::wstring KeyName(UINT vk) {
        if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::wstring(1, (wchar_t)vk);
        if (vk >= VK_F1 && vk <= VK_F24) return Util::Format(L"F%d", vk - VK_F1 + 1);
        switch (vk) {
        case VK_SPACE: return L"Space"; case VK_OEM_COMMA: return L","; case VK_OEM_PERIOD: return L".";
        case VK_OEM_3: return L"`"; case VK_OEM_MINUS: return L"-"; case VK_OEM_PLUS: return L"=";
        case VK_INSERT: return L"Insert"; case VK_HOME: return L"Home"; case VK_END: return L"End";
        case VK_PRIOR: return L"PageUp"; case VK_NEXT: return L"PageDown";
        }
        return Util::Format(L"0x%02X", vk);
    }

    std::wstring FromKeys(UINT mods, UINT vk) {
        std::wstring s;
        if (mods & MOD_CONTROL) s += L"Ctrl+";
        if (mods & MOD_ALT) s += L"Alt+";
        if (mods & MOD_SHIFT) s += L"Shift+";
        if (mods & MOD_WIN) s += L"Win+";
        return s + KeyName(vk);
    }

    bool Parse(const std::wstring& combo, UINT& mods, UINT& vk) {
        mods = 0; vk = 0;
        if (combo.empty()) return false;
        size_t s = 0;
        while (s <= combo.size()) {
            auto e = combo.find(L'+', s);
            auto part = combo.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s);
            auto lp = Util::Lower(part);
            if (lp == L"ctrl") mods |= MOD_CONTROL;
            else if (lp == L"alt") mods |= MOD_ALT;
            else if (lp == L"shift") mods |= MOD_SHIFT;
            else if (lp == L"win") mods |= MOD_WIN;
            else if (part.size() == 1) vk = part[0] == L',' ? VK_OEM_COMMA : part[0] == L'.' ? VK_OEM_PERIOD : (UINT)towupper(part[0]);
            else if (lp.size() > 1 && lp[0] == L'f' && iswdigit(lp[1])) vk = VK_F1 + _wtoi(lp.c_str() + 1) - 1;
            else if (lp == L"space") vk = VK_SPACE;
            else if (lp.rfind(L"0x", 0) == 0) vk = wcstoul(lp.c_str(), nullptr, 16);
            if (e == std::wstring::npos) break;
            s = e + 1;
        }
        return vk != 0;
    }

    std::wstring Pretty(const std::wstring& combo) {
        std::wstring out;
        for (wchar_t c : combo) { if (c == L'+') out += L" + "; else out += c; }
        return out;
    }
}
