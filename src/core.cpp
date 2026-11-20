#include "core.h"
#include <shlobj.h>
#include <shellapi.h>
#include <winternl.h>
#include <algorithm>
#include <cstdarg>
#include <set>

// ================================================================ paths

namespace Paths {
    std::wstring Root, Home, IconDir, InstalledExe, SettingsFile, VaultFile, UsageFile, Log, Self;
    bool Demo = false, Isolated = false;

    static std::wstring Known(REFKNOWNFOLDERID id) {
        PWSTR p = nullptr; std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
        CoTaskMemFree(p);
        return out;
    }

    void Init() {
        Root = Known(FOLDERID_LocalAppData) + L"\\CodexAccounts";
        // CAS_DEMO=<folder> runs a separate switcher over stand-in accounts in that folder,
        // for screenshots and trying things out. It never contacts OpenAI or touches Codex,
        // and leaves shortcuts, autostart, hotkeys, the jump list and the real switcher alone.
        wchar_t demo[MAX_PATH * 2];
        bool isDemo = GetEnvironmentVariableW(L"CAS_DEMO", demo, ARRAYSIZE(demo)) > 0;
        if (isDemo || GetEnvironmentVariableW(L"CAS_ROOT", demo, ARRAYSIZE(demo))) {
            Demo = isDemo; Isolated = true; Root = demo;
            Core::MutexName = L"Local\\CodexAccountSwitcherDemo";
            Core::ShowEvent = L"Local\\CodexAccountSwitcherDemo.Show";
            Core::ExitEvent = L"Local\\CodexAccountSwitcherDemo.Exit";
            Core::AddEvent = L"Local\\CodexAccountSwitcherDemo.Add";
            Core::ReloadEvent = L"Local\\CodexAccountSwitcherDemo.Reload";
        }
        Home = Root + L"\\.switcher";
        IconDir = Home + L"\\icons";
        InstalledExe = Home + L"\\CodexAccountSwitcher.exe";
        SettingsFile = Root + L"\\settings.json";
        VaultFile = Root + L"\\accounts.dat";
        UsageFile = Root + L"\\usage.json";
        Log = Root + L"\\switcher-error.log";
        wchar_t buf[MAX_PATH * 2]; GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf)); Self = buf;
    }

    // Shortcuts point at the installed copy, so moving the download breaks nothing.
    std::wstring Launcher() { return Util::Exists(InstalledExe) ? InstalledExe : Self; }
    // A copy over its own data (the demo, CAS_ROOT) keeps its shortcuts in that folder, so it
    // never makes, renames or deletes the real ones.
    static std::wstring Own(const wchar_t* name) { auto d = Root + L"\\" + name; CreateDirectoryW(Root.c_str(), nullptr); CreateDirectoryW(d.c_str(), nullptr); return d; }
    std::wstring Desktop() { return Isolated ? Own(L"Desktop") : Known(FOLDERID_Desktop); }
    std::wstring StartMenu() { return Isolated ? Own(L"Start menu") : Known(FOLDERID_Programs); }
    std::wstring Startup() { return Known(FOLDERID_Startup); }
    std::wstring UserProfile() { return Known(FOLDERID_Profile); }

    bool Same(const std::wstring& a, const std::wstring& b) {
        wchar_t fa[MAX_PATH * 2], fb[MAX_PATH * 2];
        GetFullPathNameW(a.c_str(), ARRAYSIZE(fa), fa, nullptr);
        GetFullPathNameW(b.c_str(), ARRAYSIZE(fb), fb, nullptr);
        std::wstring x = fa, y = fb;
        while (!x.empty() && x.back() == L'\\') x.pop_back();
        while (!y.empty() && y.back() == L'\\') y.pop_back();
        return _wcsicmp(x.c_str(), y.c_str()) == 0;
    }
}

// ================================================================ language

namespace L10n {
    bool ru = false;

    static bool SystemRussian() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN; }

    static void Apply(const std::wstring& choice) {
        wchar_t forced[8] = {};
        GetEnvironmentVariableW(L"CAS_LANG", forced, ARRAYSIZE(forced));
        if (!wcscmp(forced, L"en") || !wcscmp(forced, L"ru")) { ru = !wcscmp(forced, L"ru"); return; }
        ru = choice == L"ru" || (choice != L"en" && SystemRussian());
    }

    std::wstring Choice() { auto c = Settings::Str(L"Language", L"auto"); return c == L"en" || c == L"ru" ? c : L"auto"; }
    void Init() { Apply(Choice()); }
    void Choose(const std::wstring& choice) { Settings::Set(L"Language", choice); Apply(choice); }
}

// ================================================================ util

namespace Util {
    std::string ReadBytes(const std::wstring& path, size_t limit) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return "";
        LARGE_INTEGER size{}; GetFileSizeEx(h, &size);
        std::string bytes((size_t)std::min<LONGLONG>(size.QuadPart, (LONGLONG)limit), '\0');
        DWORD got = 0;
        if (!bytes.empty()) ReadFile(h, &bytes[0], (DWORD)bytes.size(), &got, nullptr);
        CloseHandle(h);
        bytes.resize(got);
        return bytes;
    }

    std::wstring ReadText(const std::wstring& path, bool) {
        auto bytes = ReadBytes(path);
        if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) bytes.erase(0, 3);
        return Wide(bytes);
    }

    bool WriteBytes(const std::wstring& path, const std::string& bytes) {
        auto tmp = path + Util::Format(L".%lu.tmp", GetCurrentProcessId());
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD wrote = 0;
        bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) && wrote == bytes.size() && FlushFileBuffers(h);
        CloseHandle(h);
        // A reader can hold the file for a moment (Codex reading auth.json); try a few times.
        for (int i = 0; ok && i < 20; i++) {
            if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
            Sleep(50);
        }
        DeleteFileW(tmp.c_str());
        return false;
    }

    bool WriteText(const std::wstring& path, const std::wstring& text, bool bom) {
        return WriteBytes(path, (bom ? std::string("\xEF\xBB\xBF") : std::string()) + Utf8(text));
    }

    std::string Utf8(const std::wstring& s) {
        if (s.empty()) return "";
        int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string out(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
        return out;
    }

    std::wstring Wide(const std::string& s) {
        if (s.empty()) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring out(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
        return out;
    }

    bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
    bool IsDir(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
    std::wstring Parent(const std::wstring& p) { auto i = p.find_last_of(L"\\/"); return i == std::wstring::npos ? L"" : p.substr(0, i); }
    std::wstring Leaf(const std::wstring& p) { auto i = p.find_last_of(L"\\/"); return i == std::wstring::npos ? p : p.substr(i + 1); }
    std::wstring Join(const std::wstring& a, const std::wstring& b) { return a.empty() || a.back() == L'\\' ? a + b : a + L"\\" + b; }
    std::wstring Lower(std::wstring s) { if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size()); return s; }

    std::wstring Tilde(const std::wstring& path) {
        static const std::wstring home = Paths::UserProfile();
        if (home.empty() || path.size() <= home.size() || path[home.size()] != L'\\' || _wcsnicmp(path.c_str(), home.c_str(), home.size()) != 0) return path;
        return L"~" + path.substr(home.size());
    }
    std::wstring Trim(std::wstring s) {
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        size_t i = 0; while (i < s.size() && iswspace(s[i])) i++;
        return s.substr(i);
    }

    std::wstring Format(const wchar_t* fmt, ...) {
        wchar_t buf[2048]; va_list a; va_start(a, fmt); _vsnwprintf_s(buf, _TRUNCATE, fmt, a); va_end(a); return buf;
    }

    ULONGLONG FileTime(const std::wstring& path) {
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
        return ((ULONGLONG)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    }

    // The command line of another process of this user, without WMI: a few microseconds
    // instead of a few hundred milliseconds.
    std::wstring CommandLineOf(DWORD pid) {
        typedef NTSTATUS(NTAPI* Fn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        static Fn query = (Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return L"";
        std::wstring out;
        ULONG len = 0;
        query(h, 60 /* ProcessCommandLineInformation */, nullptr, 0, &len);
        if (len) {
            std::vector<BYTE> buf(len);
            if (query(h, 60, buf.data(), len, &len) >= 0) {
                auto us = (UNICODE_STRING*)buf.data();
                out.assign(us->Buffer, us->Length / sizeof(wchar_t));
            }
        }
        CloseHandle(h);
        return out;
    }

    std::wstring ImageOf(DWORD pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return L"";
        wchar_t buf[MAX_PATH * 2]; DWORD n = ARRAYSIZE(buf);
        std::wstring out = QueryFullProcessImageNameW(h, 0, buf, &n) ? buf : L"";
        CloseHandle(h);
        return out;
    }

    ULONGLONG CreatedAt(DWORD pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return 0;
        FILETIME c, e, k, u; ULONGLONG t = 0;
        if (GetProcessTimes(h, &c, &e, &k, &u)) t = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
        CloseHandle(h);
        return t;
    }

    std::vector<HWND> AppWindowsOf(DWORD pid) {
        struct Ctx { DWORD pid; std::vector<HWND> out; } ctx{ pid };
        EnumWindows([](HWND h, LPARAM l) -> BOOL {
            auto c = (Ctx*)l; DWORD p = 0; GetWindowThreadProcessId(h, &p);
            if (p == c->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER) && !(GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
                c->out.push_back(h);
            return TRUE;
        }, (LPARAM)&ctx);
        return ctx.out;
    }

    std::wstring Plural(int n, const wchar_t* one, const wchar_t* few, const wchar_t* many) {
        int m10 = n % 10, m100 = n % 100;
        const wchar_t* w = m10 == 1 && m100 != 11 ? one : (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) ? few : many;
        return Format(L"%d %s", n, w);
    }

    std::wstring Count(int n, const wchar_t* enOne, const wchar_t* enMany, const wchar_t* ruOne, const wchar_t* ruFew, const wchar_t* ruMany) {
        return L10n::ru ? Plural(n, ruOne, ruFew, ruMany) : Format(L"%d %s", n, n == 1 ? enOne : enMany);
    }

    std::wstring Q(const std::wstring& s) { return L10n::ru ? L"«" + s + L"»" : L"“" + s + L"”"; }

    static const wchar_t* Months[] = { L"января", L"февраля", L"марта", L"апреля", L"мая", L"июня", L"июля",
                                       L"августа", L"сентября", L"октября", L"ноября", L"декабря" };
    static const wchar_t* MonthsEn[] = { L"January", L"February", L"March", L"April", L"May", L"June", L"July",
                                         L"August", L"September", L"October", L"November", L"December" };

    // 10:09 in Russian; in English the user's own clock format (10:09 AM, or 10:09).
    std::wstring Clock(const SYSTEMTIME& t) {
        if (L10n::ru) return Format(L"%d:%02d", t.wHour, t.wMinute);
        wchar_t b[64];
        if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &t, nullptr, b, ARRAYSIZE(b))) return b;
        return Format(L"%d:%02d", t.wHour, t.wMinute);
    }

    static SYSTEMTIME LocalFromMs(long long ms) {
        ULONGLONG ft = (ULONGLONG)ms * 10000 + 116444736000000000ull;
        FILETIME f{ (DWORD)ft, (DWORD)(ft >> 32) }; SYSTEMTIME u, t;
        FileTimeToSystemTime(&f, &u); SystemTimeToTzSpecificLocalTime(nullptr, &u, &t);
        return t;
    }

    static long long DayNumber(const SYSTEMTIME& s) {
        FILETIME x; SYSTEMTIME d = s; d.wHour = d.wMinute = d.wSecond = d.wMilliseconds = 0; SystemTimeToFileTime(&d, &x);
        return (long long)(((ULONGLONG)x.dwHighDateTime << 32 | x.dwLowDateTime) / 864000000000ull);
    }

    std::wstring When(long long ms) {
        if (ms <= 0) return Tr(L"Not used yet", L"Не использовался");
        SYSTEMTIME t = LocalFromMs(ms), now; GetLocalTime(&now);
        auto diff = DayNumber(now) - DayNumber(t);
        if (diff == 0) return Tr(L"Today, ", L"Сегодня, ") + Clock(t);
        if (diff == 1) return Tr(L"Yesterday, ", L"Вчера, ") + Clock(t);
        if (!L10n::ru) return t.wYear == now.wYear ? Format(L"%s %d", MonthsEn[t.wMonth - 1], t.wDay) : Format(L"%s %d, %d", MonthsEn[t.wMonth - 1], t.wDay, t.wYear);
        if (t.wYear == now.wYear) return Format(L"%d %s", t.wDay, Months[t.wMonth - 1]);
        return Format(L"%d %s %d", t.wDay, Months[t.wMonth - 1], t.wYear);
    }

    // “Work”, “Work” and “Personal”, or "3 accounts".
    std::wstring Names(const std::vector<std::wstring>& n) {
        if (n.size() == 1) return Q(n[0]);
        if (n.size() == 2) return Q(n[0]) + Tr(L" and ", L" и ") + Q(n[1]);
        return Count((int)n.size(), L"account", L"accounts", L"аккаунт", L"аккаунта", L"аккаунтов");
    }

    bool CopyText(HWND owner, const std::wstring& text) {
        if (Test::On()) { Test::Trace(L"clipboard|" + text); return true; }
        if (!OpenClipboard(owner)) return false;
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
        bool ok = false;
        if (g) {
            memcpy(GlobalLock(g), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(g);
            ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
            if (!ok) GlobalFree(g);
        }
        CloseClipboard();
        return ok;
    }

    bool Open(const std::wstring& what, const std::wstring& args) {
        if (Test::On()) { Test::Trace(L"open|" + what + (args.empty() ? L"" : L"|" + args)); return true; }
        return (INT_PTR)ShellExecuteW(nullptr, L"open", what.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL) > 32;
    }
}

// ================================================================ the demo's checks

namespace Test {
    bool On() {
        static bool on = Paths::Demo && GetEnvironmentVariableW(L"CAS_OFFSCREEN", nullptr, 0) > 0;
        return on;
    }

    void Trace(const std::wstring& line) {
        if (!On()) return;
        static CRITICAL_SECTION cs; static bool init = (InitializeCriticalSection(&cs), true); (void)init;
        auto bytes = Util::Utf8(Util::Format(L"%llu ", GetTickCount64()) + line + L"\r\n");
        EnterCriticalSection(&cs);
        HANDLE h = CreateFileW((Paths::Root + L"\\trace.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, bytes.data(), (DWORD)bytes.size(), &w, nullptr); CloseHandle(h); }
        LeaveCriticalSection(&cs);
    }

    std::wstring Take(const wchar_t* name) {
        if (!On()) return L"";
        auto path = Paths::Root + L"\\" + name;
        if (!Util::Exists(path)) return L"";
        auto text = Util::Trim(Util::ReadText(path));
        DeleteFileW(path.c_str());
        return text;
    }
}

// ================================================================ settings

namespace Settings {
    static CRITICAL_SECTION* Lock() {
        static CRITICAL_SECTION cs; static bool init = (InitializeCriticalSection(&cs), true); (void)init; return &cs;
    }

    // Every Flag/Str call reads the settings, many times per frame, so the parsed file is kept
    // and read again only when its timestamp moves (another copy, or a hand edit).
    static json::Value cache;
    static ULONGLONG cacheTime = ~0ull;
    static std::wstring cacheFile;

    json::Value Load() {
        EnterCriticalSection(Lock());
        auto stamp = Util::FileTime(Paths::SettingsFile);
        if (stamp == 0 || stamp != cacheTime || cacheFile != Paths::SettingsFile) {
            json::Value v;
            auto text = Util::ReadText(Paths::SettingsFile);
            if (text.empty() || !json::Parser::Parse(text, v) || v.type != json::Value::Object) v = json::Value::MakeObject();
            cache = std::move(v); cacheTime = stamp; cacheFile = Paths::SettingsFile;
        }
        json::Value copy = cache;
        LeaveCriticalSection(Lock());
        return copy;
    }

    void Save(const json::Value& v) {
        EnterCriticalSection(Lock());
        CreateDirectoryW(Paths::Root.c_str(), nullptr);
        std::wstring out; json::Write(out, v);
        if (Util::WriteText(Paths::SettingsFile, out, false)) { cache = v; cacheTime = Util::FileTime(Paths::SettingsFile); cacheFile = Paths::SettingsFile; }
        else cacheTime = ~0ull;
        LeaveCriticalSection(Lock());
    }

    void Set(const std::wstring& key, json::Value value) {
        if (Test::On()) { std::wstring text; json::Write(text, value); Test::Trace(L"setting|" + key + L"|" + text); }
        EnterCriticalSection(Lock());
        auto v = Load(); v[key] = std::move(value); Save(v);
        LeaveCriticalSection(Lock());
    }

    void Update(const std::function<void(json::Value&)>& change) {
        EnterCriticalSection(Lock());
        auto v = Load(); change(v); Save(v);
        LeaveCriticalSection(Lock());
    }

    bool Flag(const std::wstring& key, bool def) { return Load().Flag(key, def); }
    int Int(const std::wstring& key, int def) { return (int)Load().Num(key, def); }
    std::wstring Str(const std::wstring& key, const std::wstring& def) { return Load().Str(key, def); }
    bool Has(const std::wstring& key) { return Load().Find(key) != nullptr; }
}

// ================================================================ core

namespace Core {
    const wchar_t* MutexName = L"Local\\CodexAccountSwitcherExe";
    const wchar_t* ShowEvent = L"Local\\CodexAccountSwitcherExe.Show";
    const wchar_t* ExitEvent = L"Local\\CodexAccountSwitcherExe.Exit";
    const wchar_t* AddEvent = L"Local\\CodexAccountSwitcherExe.Add";
    const wchar_t* ReloadEvent = L"Local\\CodexAccountSwitcherExe.Reload";

    long long NowMs() {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        return (long long)((((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull) / 10000);
    }

    long long ParseIso(const std::wstring& s) {
        int y, mo, d, h, mi, se; wchar_t sign = 0; int oh = 0, om = 0;
        if (swscanf_s(s.c_str(), L"%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
        auto tz = s.size() > 19 ? s.find_first_of(L"+-Z", 19) : std::wstring::npos;
        if (tz != std::wstring::npos && s[tz] != L'Z') { sign = s[tz]; swscanf_s(s.c_str() + tz + 1, L"%d:%d", &oh, &om); }
        SYSTEMTIME st{ (WORD)y, (WORD)mo, 0, (WORD)d, (WORD)h, (WORD)mi, (WORD)se, 0 };
        FILETIME ft; if (!SystemTimeToFileTime(&st, &ft)) return 0;
        long long ms = (long long)((((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull) / 10000);
        long long off = (oh * 60ll + om) * 60000;
        return sign == L'+' ? ms - off : sign == L'-' ? ms + off : ms;
    }

    std::wstring IsoNow() {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        ULONGLONG ticks = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        SYSTEMTIME t; FileTimeToSystemTime(&ft, &t);
        // Seven digits of 100 ns ticks, padded to nanoseconds like Rust's chrono writes them.
        return Util::Format(L"%04d-%02d-%02dT%02d:%02d:%02d.%07llu00Z", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, ticks % 10000000ull);
    }

    std::wstring Age(long long ms) {
        if (ms <= 0) return L"";
        long long mins = (NowMs() - ms) / 60000;
        if (mins < 2) return Tr(L"just now", L"только что");
        if (mins < 60) return Util::Format(Tr(L"%lld min ago", L"%lld мин назад"), mins);
        ULONGLONG ft = (ULONGLONG)ms * 10000 + 116444736000000000ull;
        FILETIME f{ (DWORD)ft, (DWORD)(ft >> 32) }; SYSTEMTIME u, t, now;
        FileTimeToSystemTime(&f, &u); SystemTimeToTzSpecificLocalTime(nullptr, &u, &t); GetLocalTime(&now);
        if (mins < 20 * 60 && t.wDay == now.wDay) return Tr(L"at ", L"в ") + Util::Clock(t);
        static const wchar_t* m[] = { L"янв", L"фев", L"мар", L"апр", L"мая", L"июн", L"июл", L"авг", L"сен", L"окт", L"ноя", L"дек" };
        static const wchar_t* en[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
        if (!L10n::ru) return Util::Format(L"%s %d, ", en[t.wMonth - 1], t.wDay) + Util::Clock(t);
        return Util::Format(L"%d %s, %02d:%02d", t.wDay, m[t.wMonth - 1], t.wHour, t.wMinute);
    }

    // Within a day: the clock. Within a week: the weekday and the clock. Later: the date.
    std::wstring ResetTime(long long sec) {
        if (sec <= 0) return L"";
        long long ms = sec * 1000, left = ms - NowMs();
        ULONGLONG ft = (ULONGLONG)ms * 10000 + 116444736000000000ull;
        FILETIME f{ (DWORD)ft, (DWORD)(ft >> 32) }; SYSTEMTIME u, t, now;
        FileTimeToSystemTime(&f, &u); SystemTimeToTzSpecificLocalTime(nullptr, &u, &t); GetLocalTime(&now);
        static const wchar_t* daysEn[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
        static const wchar_t* daysRu[] = { L"вс", L"пн", L"вт", L"ср", L"чт", L"пт", L"сб" };
        static const wchar_t* m[] = { L"янв", L"фев", L"мар", L"апр", L"мая", L"июн", L"июл", L"авг", L"сен", L"окт", L"ноя", L"дек" };
        static const wchar_t* en[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
        if (left < 20ll * 3600 * 1000 && (t.wDay == now.wDay || left < 12ll * 3600 * 1000)) return Util::Clock(t);
        if (left < 6ll * 24 * 3600 * 1000) return std::wstring(L10n::ru ? daysRu[t.wDayOfWeek] : daysEn[t.wDayOfWeek]) + L" " + Util::Clock(t);
        return L10n::ru ? Util::Format(L"%d %s", t.wDay, m[t.wMonth - 1]) : Util::Format(L"%s %d", en[t.wMonth - 1], t.wDay);
    }

    std::wstring ShortNumber(long long n) {
        if (n < 0) return L"—";
        struct S { double at; const wchar_t* en; const wchar_t* ru; };
        static const S s[] = { { 1e9, L"B", L" млрд" }, { 1e6, L"M", L" млн" }, { 1e3, L"K", L" тыс" } };
        for (auto& x : s) if (n >= x.at) {
            double v = n / x.at;
            auto t = v < 10 ? Util::Format(L"%.1f", v) : Util::Format(L"%.0f", v);
            if (t.size() > 2 && t.compare(t.size() - 2, 2, L".0") == 0) t.resize(t.size() - 2);
            if (L10n::ru) for (auto& c : t) if (c == L'.') c = L',';
            return t + (L10n::ru ? x.ru : x.en);
        }
        return std::to_wstring(n);
    }

    // Appended rather than read and written back whole on every line; it starts over once it
    // passes 256 KB. Never given a token: callers pass descriptions, not responses.
    void Log(const std::wstring& text) {
        CreateDirectoryW(Paths::Root.c_str(), nullptr);
        SYSTEMTIME t; GetLocalTime(&t);
        auto line = Util::Format(L"[%04d-%02d-%02dT%02d:%02d:%02d] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond) + text + L"\r\n";
        auto bytes = Util::Utf8(line);
        static CRITICAL_SECTION cs; static bool init = (InitializeCriticalSection(&cs), true); (void)init;
        EnterCriticalSection(&cs);
        HANDLE h = CreateFileW(Paths::Log.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
        LARGE_INTEGER size{};
        if (h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &size) && size.QuadPart > (256 << 10)) {
            CloseHandle(h);
            h = CreateFileW(Paths::Log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
        }
        if (h != INVALID_HANDLE_VALUE) { DWORD wrote = 0; WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr); CloseHandle(h); }
        LeaveCriticalSection(&cs);
    }

    // Eight colours that stay apart on both themes and under the white initials.
    static const COLORREF Palette[8] = { RGB(79, 91, 213), RGB(46, 125, 74), RGB(186, 82, 60), RGB(150, 96, 180),
                                        RGB(40, 140, 150), RGB(66, 133, 180), RGB(196, 140, 40), RGB(90, 96, 112) };

    // Assigned once and remembered, so two accounts rarely share a colour and a colour never
    // changes later.
    COLORREF AccountColor(const std::wstring& id) {
        // Asked for every card on every refresh: read first, write only for a new account.
        {
            auto all = Settings::Load();
            if (auto colors = all.Find(L"Colors"))
                for (auto& kv : colors->obj)
                    if (_wcsicmp(kv.first.c_str(), id.c_str()) == 0) return Palette[kv.second.type == json::Value::Number ? ((int)kv.second.n & 7) : 0];
        }
        COLORREF result = Palette[0];
        Settings::Update([&](json::Value& all) {
            auto& colors = all[L"Colors"];
            if (colors.type != json::Value::Object) colors = json::Value::MakeObject();
            // Hand-edited settings may hold anything; & 7 keeps the index inside the palette.
            auto slot = [](const json::Value& v) { return v.type == json::Value::Number ? ((int)v.n & 7) : 0; };
            for (auto& kv : colors.obj) if (_wcsicmp(kv.first.c_str(), id.c_str()) == 0) { result = Palette[slot(kv.second)]; return; }
            int use[8] = {};
            for (auto& kv : colors.obj) use[slot(kv.second)]++;
            int pick = 0; for (int i = 1; i < 8; i++) if (use[i] < use[pick]) pick = i;
            colors[id] = pick;
            result = Palette[pick];
        });
        return result;
    }

    void ForgetColor(const std::wstring& id) {
        Settings::Update([&](json::Value& all) { if (auto c = all.Find(L"Colors")) { (void)c; all[L"Colors"].Remove(id); } });
    }

    // Initials: "Work" gives W, "john.doe" gives JD, "Рабочий профиль" gives РП.
    std::wstring Initials(const std::wstring& label) {
        std::vector<wchar_t> firsts; bool start = true;
        for (wchar_t c : label) {
            if (c == L' ' || c == L'-' || c == L'_' || c == L'.' || c == L'@') { start = true; continue; }
            if (start && IsCharAlphaNumericW(c)) { firsts.push_back(c); start = false; }
        }
        std::wstring s;
        if (firsts.empty()) s = label.substr(0, 1);
        else if (firsts.size() == 1) s = std::wstring(1, firsts[0]);
        else { s += firsts[0]; s += firsts[1]; }
        if (!s.empty()) CharUpperBuffW(&s[0], (DWORD)s.size());
        return s;
    }
}
