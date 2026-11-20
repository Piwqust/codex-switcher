#include "codex.h"
#include <shobjidl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <set>

namespace Codex {
    static Install current;
    static CRITICAL_SECTION* Guard() { static CRITICAL_SECTION cs; static bool init = (InitializeCriticalSection(&cs), true); (void)init; return &cs; }
    Install App() { EnterCriticalSection(Guard()); auto copy = current; LeaveCriticalSection(Guard()); return copy; }

    static std::wstring Env(const wchar_t* name) { wchar_t b[MAX_PATH * 2]; DWORD n = GetEnvironmentVariableW(name, b, ARRAYSIZE(b)); return n && n < ARRAYSIZE(b) ? b : L""; }

    // The Store package is OpenAI.Codex_<version>_<arch>__<publisher>; its runtime packages
    // (OpenAI.CodexPrimaryRuntime...) share the prefix but not the underscore.
    static bool FindMsix(Install& out) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages",
                          0, KEY_READ, &k) != ERROR_SUCCESS) return false;
        bool found = false; ULONGLONG bestVer = 0;
        wchar_t name[512];
        for (DWORD i = 0;; i++) {
            DWORD n = ARRAYSIZE(name);
            if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::wstring full = name;
            if (_wcsnicmp(full.c_str(), L"OpenAI.Codex_", 13) != 0) continue;
            std::vector<std::wstring> parts; size_t s = 0, p;
            while ((p = full.find(L'_', s)) != std::wstring::npos) { parts.push_back(full.substr(s, p - s)); s = p + 1; }
            parts.push_back(full.substr(s));
            if (parts.size() < 5) continue;
            int a = 0, b = 0, c = 0, d = 0; swscanf_s(parts[1].c_str(), L"%d.%d.%d.%d", &a, &b, &c, &d);
            ULONGLONG ver = ((ULONGLONG)a << 48) | ((ULONGLONG)b << 32) | ((ULONGLONG)c << 16) | (ULONGLONG)d;
            wchar_t root[MAX_PATH * 2] = {}; DWORD rl = sizeof(root);
            if (RegGetValueW(k, name, L"PackageRootFolder", RRF_RT_REG_SZ, nullptr, root, &rl) != ERROR_SUCCESS)
                wcscpy_s(root, (Env(L"ProgramFiles") + L"\\WindowsApps\\" + full).c_str());
            auto exe = Util::Join(root, L"app\\ChatGPT.exe");
            if (!Util::Exists(exe)) exe = Util::Join(root, L"app\\Codex.exe");
            if (!Util::Exists(exe) || (found && ver <= bestVer)) continue;
            out = { L"Msix", exe, parts[1], parts[0] + L"_" + parts.back() + L"!App" };
            found = true; bestVer = ver;
        }
        RegCloseKey(k);
        return found;
    }

    void Detect() {
        Install found;
        auto saved = Settings::Str(L"CodexPath");
        if (!saved.empty() && Util::Exists(saved)) found = { L"Classic", saved, L"", L"" };
        else if (!FindMsix(found)) {
            auto local = Env(L"LOCALAPPDATA");
            for (auto dir : { L"\\Programs\\Codex\\Codex.exe", L"\\Codex\\Codex.exe" })
                if (Util::Exists(local + dir)) { found = { L"Classic", local + dir, L"", L"" }; break; }
        }
        EnterCriticalSection(Guard()); current = found; LeaveCriticalSection(Guard());
    }

    std::wstring AppName() {
        if (Paths::Demo) return Settings::Str(L"DemoAppName", L"ChatGPT");
        auto app = App();
        return app.found() && _wcsicmp(Util::Leaf(app.exe).c_str(), L"ChatGPT.exe") != 0 ? L"Codex" : L"ChatGPT";
    }

    std::wstring Home() {
        if (Paths::Demo) return Paths::Root + L"\\.codex";
        auto env = Env(L"CODEX_HOME");
        if (!env.empty()) return env;
        return Paths::UserProfile() + L"\\.codex";
    }

    std::wstring AuthFile() { return Home() + L"\\auth.json"; }

    // cli_auth_credentials_store = "keyring" moves the sign-in out of auth.json, where the
    // switcher cannot follow it. Only a top-level line counts, not one inside a [profile].
    static bool ReadKeyringStore(const std::wstring& path) {
        auto text = Util::ReadText(path);
        size_t s = 0;
        while (s < text.size()) {
            auto e = text.find(L'\n', s);
            auto line = Util::Trim(text.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s));
            if (!line.empty() && line[0] == L'[') return false;
            // "auto" uses Credential Manager wherever Windows has it, which is everywhere.
            if (line.rfind(L"cli_auth_credentials_store", 0) == 0 && (line.find(L"keyring") != std::wstring::npos || line.find(L"auto") != std::wstring::npos)) return true;
            if (e == std::wstring::npos) break;
            s = e + 1;
        }
        return false;
    }

    // Read again only when config.toml changes.
    bool KeyringStore() {
        static std::wstring file; static ULONGLONG stamp = ~0ull; static bool cached = false;
        auto path = Home() + L"\\config.toml";
        auto t = Util::FileTime(path);
        EnterCriticalSection(Guard());
        bool hit = path == file && t == stamp, result = cached;
        LeaveCriticalSection(Guard());
        if (hit) return result;
        result = ReadKeyringStore(path);
        EnterCriticalSection(Guard()); file = path; stamp = t; cached = result; LeaveCriticalSection(Guard());
        return result;
    }

    // ------------------------------------------------------------ processes

    struct Proc { DWORD pid, parent; std::wstring exe; };

    static std::vector<Proc> Snapshot() {
        std::vector<Proc> all;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return all;
        PROCESSENTRY32W pe{ sizeof pe };
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) all.push_back({ pe.th32ProcessID, pe.th32ParentProcessID, Util::Lower(pe.szExeFile) });
        CloseHandle(snap);
        return all;
    }

    static bool InCodexPackage(const std::wstring& image) {
        auto i = image.find(L"\\windowsapps\\openai.codex_");
        return i != std::wstring::npos;
    }

    static bool IsAppImage(const std::wstring& image, const Install& app) {
        if (app.kind == L"Classic") return image == Util::Lower(app.exe);
        return InCodexPackage(image) && (image.size() > 16 && image.compare(image.size() - 16, 16, L"\\app\\chatgpt.exe") == 0);
    }

    static DWORD DaemonPid() {
        json::Value v;
        if (!json::Parser::Parse(Util::ReadText(Home() + L"\\app-server-daemon\\daemon.pid"), v)) return 0;
        DWORD pid = (DWORD)v.Num(L"pid", 0);
        auto started = v.Str(L"processStartTime");
        if (!pid || started.empty()) return 0;
        // A pid outlives its process and gets reused: it counts only with the same start time.
        return Util::CreatedAt(pid) == _wcstoui64(started.c_str(), nullptr, 10) ? pid : 0;
    }

    Processes Scan() {
        Processes out;
        if (Paths::Demo) {
            if (Settings::Flag(L"DemoCodexOpen", false)) { out.app.push_back(1); out.appVisible = true; }
            return out;
        }
        // A copy over CAS_ROOT is for trying things on a copy of the data: it sees no Codex,
        // so it never closes, ends or starts the real one.
        if (Paths::Isolated) return out;
        auto all = Snapshot();
        auto app = App();
        DWORD daemon = DaemonPid();
        std::set<DWORD> cliPids;
        std::map<DWORD, DWORD> parentOf;
        for (auto& p : all) parentOf[p.pid] = p.parent;
        for (auto& p : all) {
            if (p.exe != L"chatgpt.exe" && p.exe != L"codex.exe") continue;
            auto image = Util::Lower(Util::ImageOf(p.pid));
            if (image.empty()) continue;
            if (IsAppImage(image, app)) {
                auto cmd = Util::CommandLineOf(p.pid);
                if (cmd.find(L"--type=") != std::wstring::npos) continue;
                out.app.push_back(p.pid);
                if (!Util::AppWindowsOf(p.pid).empty()) out.appVisible = true;
                continue;
            }
            if (InCodexPackage(image) || p.exe != L"codex.exe") continue;   // the app's own helpers go with it
            if (p.pid == daemon) { out.daemon = p.pid; continue; }
            auto cmd = Util::Lower(Util::CommandLineOf(p.pid));
            // VS Code, Cursor, Windsurf and the like run the extension's own codex app-server.
            if (image.find(L"\\extensions\\openai.chatgpt") != std::wstring::npos) {
                if (cmd.find(L"app-server") != std::wstring::npos) { out.helpers.push_back(p.pid); out.started[p.pid] = Util::CreatedAt(p.pid); }
                continue;
            }
            cliPids.insert(p.pid);
        }
        // A terminal session may start codex.exe children; count each session once.
        for (DWORD pid : cliPids) {
            bool nested = false;
            for (DWORD up = parentOf.count(pid) ? parentOf[pid] : 0, hops = 0; up && hops < 8; up = parentOf.count(up) ? parentOf[up] : 0, hops++)
                if (cliPids.count(up)) { nested = true; break; }
            if (!nested) out.cli.push_back(pid);
        }
        // Several app processes without --type: the launcher and the browser process. Keep the topmost.
        std::vector<DWORD> roots;
        for (DWORD pid : out.app) {
            DWORD up = parentOf.count(pid) ? parentOf[pid] : 0;
            if (std::find(out.app.begin(), out.app.end(), up) == out.app.end()) roots.push_back(pid);
        }
        out.app = roots;
        for (DWORD pid : out.app) out.started[pid] = Util::CreatedAt(pid);
        return out;
    }

    // The tree under a process. A parent id can outlive its process and be reused, so a
    // child counts only if it was created after the process it claims as its parent.
    static std::vector<DWORD> Tree(DWORD root, const std::vector<Proc>& all) {
        std::vector<DWORD> tree{ root };
        for (size_t k = 0; k < tree.size(); k++) {
            auto parentAt = Util::CreatedAt(tree[k]);
            for (auto& p : all) {
                if (p.parent != tree[k] || std::find(tree.begin(), tree.end(), p.pid) != tree.end()) continue;
                auto at = Util::CreatedAt(p.pid);
                if (parentAt && at && at >= parentAt) tree.push_back(p.pid);
            }
        }
        return tree;
    }

    // Frozen first, so the dying copy can't start children again, then ended at idle priority
    // a batch at a time: ending hundreds of processes at once stalls Explorer for a second.
    static size_t EndAll(const std::vector<DWORD>& pids) {
        typedef LONG(NTAPI * Fn)(HANDLE);
        static Fn suspend = (Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSuspendProcess");
        static Fn resume = (Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtResumeProcess");
        std::vector<HANDLE> hs;
        for (DWORD pid : pids) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_SUSPEND_RESUME | PROCESS_SET_INFORMATION | SYNCHRONIZE, FALSE, pid);
            if (!h) h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
            if (!h) continue;
            if (suspend) suspend(h);
            SetPriorityClass(h, IDLE_PRIORITY_CLASS);
            hs.push_back(h);
        }
        size_t ended = 0;
        auto deadline = GetTickCount64() + 10000;
        for (size_t k = 0; k < hs.size(); k += 16) {
            std::vector<HANDLE> ending;
            for (size_t j = k; j < std::min<size_t>(hs.size(), k + 16); j++) {
                if (TerminateProcess(hs[j], 0)) ending.push_back(hs[j]);
                else if (resume) resume(hs[j]);
            }
            auto now = GetTickCount64();
            if (!ending.empty()) WaitForMultipleObjects((DWORD)ending.size(), ending.data(), TRUE, now < deadline ? (DWORD)(deadline - now) : 0);
            ended += ending.size();
        }
        for (HANDLE h : hs) CloseHandle(h);
        return ended;
    }

    bool CloseApp(const Processes& p) {
        if (Paths::Demo) { Test::Trace(L"app|close"); Settings::Set(L"DemoCodexOpen", false); return true; }
        if (p.app.empty() || Paths::Isolated) return true;
        auto all = Snapshot();
        std::vector<DWORD> pids;
        for (DWORD root : p.app) {
            // Still the same process: a pid reused since the scan is someone else's.
            auto at = p.started.find(root);
            if (at == p.started.end() || !at->second || Util::CreatedAt(root) != at->second) continue;
            for (DWORD pid : Tree(root, all)) if (std::find(pids.begin(), pids.end(), pid) == pids.end()) pids.push_back(pid);
        }
        EndAll(pids);
        for (DWORD root : p.app) {
            HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, root);
            if (h) { bool gone = WaitForSingleObject(h, 3000) == WAIT_OBJECT_0; CloseHandle(h); if (!gone) return false; }
        }
        return true;
    }

    void EndHelpers(const Processes& p) {
        if (Paths::Isolated) return;
        std::vector<DWORD> pids;
        for (DWORD pid : p.helpers) {
            auto at = p.started.find(pid);
            if (at != p.started.end() && at->second && Util::CreatedAt(pid) == at->second) pids.push_back(pid);
        }
        // Checked again right before: the daemon may have gone and its pid been reused. Left
        // alone while terminal sessions run, which may be working through it.
        if (p.daemon && p.cli.empty() && DaemonPid() == p.daemon) pids.push_back(p.daemon);
        if (!pids.empty()) EndAll(pids);
    }

    bool StartApp() {
        if (Paths::Demo) {
            bool missing = Settings::Flag(L"DemoNoApp", false);
            Test::Trace(missing ? L"app|start|missing" : L"app|start");
            if (!missing) Settings::Set(L"DemoCodexOpen", true);
            return !missing;
        }
        if (Paths::Isolated) return false;
        Detect();
        auto App = Codex::App();
        if (!App.found()) { Core::Log(L"Opening the app: it isn't installed"); return false; }
        AllowSetForegroundWindow(ASFW_ANY);
        if (App.kind == L"Msix") {
            // Through the package, so Codex keeps its identity: updates, links and its own services.
            // A copy that is still on its way out can refuse the start, so it is tried again.
            HRESULT hr = E_FAIL;
            for (int attempt = 0; attempt < 3; attempt++) {
                if (attempt) Sleep(700);
                IApplicationActivationManager* m = nullptr; DWORD pid = 0;
                hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&m));
                if (SUCCEEDED(hr)) { hr = m->ActivateApplication(App.aumid.c_str(), nullptr, AO_NONE, &pid); m->Release(); }
                if (SUCCEEDED(hr)) return true;
            }
            Core::Log(Util::Format(L"Opening the app: activating %s failed (0x%08X), trying the Start menu way", App.aumid.c_str(), (unsigned)hr));
            auto args = L"shell:AppsFolder\\" + App.aumid;
            if ((INT_PTR)ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL) > 32) return true;
            Core::Log(L"Opening the app: the Start menu way failed too");
            return false;
        }
        bool ok = (INT_PTR)ShellExecuteW(nullptr, L"open", App.exe.c_str(), nullptr, Util::Parent(App.exe).c_str(), SW_SHOWNORMAL) > 32;
        if (!ok) Core::Log(Util::Format(L"Opening the app: %s didn't start (%lu)", App.exe.c_str(), GetLastError()));
        return ok;
    }

    // Windows lets only the foreground process hand focus on. A synthetic Alt press counts as
    // input again; without it the window only flashes on the taskbar.
    bool ShowApp() {
        if (Paths::Demo) { bool open = Settings::Flag(L"DemoCodexOpen", false); Test::Trace(open ? L"app|show" : L"app|show|closed"); return open; }
        if (Paths::Isolated) return false;
        auto p = Scan();
        for (DWORD pid : p.app) {
            auto wins = Util::AppWindowsOf(pid);
            if (wins.empty()) continue;
            HWND h = wins[0];
            AllowSetForegroundWindow(pid);
            if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
            if (!SetForegroundWindow(h) || GetForegroundWindow() != h) {
                INPUT alt[2] = {};
                alt[0].type = alt[1].type = INPUT_KEYBOARD;
                alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
                alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, alt, sizeof(INPUT));
                SetForegroundWindow(h);
            }
            BringWindowToTop(h);
            return true;
        }
        return false;
    }
}
