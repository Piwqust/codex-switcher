// What every part of the switcher shares: where its files live, the interface language,
// settings.json, small helpers and the error log.
#pragma once
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include "json.h"

// Interface language: English, or Russian when Windows itself is in Russian. Settings can
// pin either one (and CAS_LANG=en|ru does, for screenshots). Strings sit in the code as
// Tr(english, russian), so both stay side by side and neither can be forgotten.
namespace L10n {
    extern bool ru;
    void Init();                                   // after Paths::Init
    std::wstring Choice();                         // L"auto", L"en" or L"ru"
    void Choose(const std::wstring& choice);
}
inline const wchar_t* Tr(const wchar_t* en, const wchar_t* ru) { return L10n::ru ? ru : en; }

namespace Paths {
    // Root      %LOCALAPPDATA%\CodexAccounts         accounts, settings, usage, the error log
    // Home      Root\.switcher                         the installed exe and its icons
    extern std::wstring Root, Home, IconDir, InstalledExe, SettingsFile, VaultFile, UsageFile, Log, Self;
    extern bool Demo;                  // CAS_DEMO is set: stand-in accounts, nothing outside that folder is touched
    // CAS_ROOT=<folder>, or the demo: a separate copy with its own data that doesn't install
    // itself, start with Windows, take hotkeys, fill the jump list or look for updates.
    extern bool Isolated;
    void Init();
    std::wstring Launcher();
    std::wstring Desktop();
    std::wstring StartMenu();
    std::wstring Startup();
    std::wstring UserProfile();
    bool Same(const std::wstring& a, const std::wstring& b);
}

namespace Settings {
    json::Value Load();
    void Save(const json::Value& v);
    void Set(const std::wstring& key, json::Value value);
    void Update(const std::function<void(json::Value&)>& change);   // read, change and write as one step
    bool Flag(const std::wstring& key, bool def);
    int Int(const std::wstring& key, int def);
    std::wstring Str(const std::wstring& key, const std::wstring& def = L"");
    bool Has(const std::wstring& key);
}

namespace Util {
    std::wstring ReadText(const std::wstring& path, bool shared = false);
    bool WriteText(const std::wstring& path, const std::wstring& text, bool bom);
    std::string ReadBytes(const std::wstring& path, size_t limit = 64u << 20);
    // Written beside and swapped in, so a crash never leaves half a file.
    bool WriteBytes(const std::wstring& path, const std::string& bytes);
    bool Exists(const std::wstring& path);
    bool IsDir(const std::wstring& path);
    std::wstring Parent(const std::wstring& path);
    std::wstring Leaf(const std::wstring& path);
    std::wstring Join(const std::wstring& a, const std::wstring& b);
    std::wstring Tilde(const std::wstring& path); // the profile folder as ~, for showing paths
    std::wstring Lower(std::wstring s);
    std::wstring Trim(std::wstring s);
    std::wstring Format(const wchar_t* fmt, ...);
    std::string Utf8(const std::wstring& s);
    std::wstring Wide(const std::string& s);
    ULONGLONG FileTime(const std::wstring& path);
    std::wstring CommandLineOf(DWORD pid);
    std::wstring ImageOf(DWORD pid);
    ULONGLONG CreatedAt(DWORD pid);                // FILETIME ticks, 0 when it can't be read
    std::vector<HWND> AppWindowsOf(DWORD pid);
    std::wstring Plural(int n, const wchar_t* one, const wchar_t* few, const wchar_t* many);   // Russian forms
    // "3 accounts" / "3 аккаунта": the number with the word in the current language.
    std::wstring Count(int n, const wchar_t* enOne, const wchar_t* enMany, const wchar_t* ruOne, const wchar_t* ruFew, const wchar_t* ruMany);
    std::wstring Q(const std::wstring& s);         // “quoted” or «в кавычках»
    std::wstring When(long long unixMs);           // "Today, 10:09", "Yesterday, 18:00", "14 October"
    std::wstring Clock(const SYSTEMTIME& t);
    std::wstring Names(const std::vector<std::wstring>& names);
    bool CopyText(HWND owner, const std::wstring& text);
    // A link, a file or a folder in its usual program (the browser, Explorer).
    bool Open(const std::wstring& what, const std::wstring& args = L"");
}

// The demo's automatic checks (tools\uitest.ps1): CAS_DEMO with CAS_OFFSCREEN. What would reach
// the screen, the clipboard, a browser or Codex is written to <demo>\trace.log instead, and the
// checks answer menus and file dialogs through files they put in the demo folder.
namespace Test {
    bool On();
    void Trace(const std::wstring& line);
    std::wstring Take(const wchar_t* name);        // <demo>\<name>: read once, then removed; "" when absent
}

namespace Core {
    extern const wchar_t* MutexName;
    extern const wchar_t* ShowEvent;
    extern const wchar_t* ExitEvent;
    extern const wchar_t* AddEvent;
    extern const wchar_t* ReloadEvent;            // another copy changed the accounts (a shortcut switched)

    long long NowMs();
    long long ParseIso(const std::wstring& s);     // "2026-10-13T10:00:00.320078+00:00" -> unix ms, 0 if it does not parse
    std::wstring IsoNow();                         // "2026-11-20T05:41:15.123456700Z", the way Codex writes it
    std::wstring Age(long long unixMs);            // "только что", "12 мин назад", "в 07:42", "14 окт"
    std::wstring ResetTime(long long unixSec);     // "14:20", "Mon 09:00" or "24 Nov": when a limit resets
    std::wstring ShortNumber(long long n);         // 1.2M, 845K; in Russian 1,2 млн, 845 тыс
    void Log(const std::wstring& text);

    COLORREF AccountColor(const std::wstring& id);
    void ForgetColor(const std::wstring& id);
    std::wstring Initials(const std::wstring& label);
}
