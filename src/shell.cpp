// Windows integration: icon files, shortcuts, start with Windows, installing, the jump list
// and native menus that follow the theme.
#include "app.h"
#include "codex.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <tlhelp32.h>
#include <cmath>

using namespace Gfx;

#include "icons.inc"

namespace WinShell {
    std::wstring AccountIcon(const Item& it) { return IconFile(it.id, it.letters, it.color, false); }
    std::wstring AddIcon() { return IconFile(L"add-account", L"plus", RGB(46, 125, 74), true); }
    void RemoveIcons(const std::wstring& id) { RemoveIconsFor(id); }
    void WriteAppIcon(const std::wstring& path) { WriteIco(path, DrawApp); }

    // Into a 32-bit DIB with straight alpha, which is what icons take.
    static HICON ToIcon(IWICBitmap* bmp, int s) {
        BITMAPV5HEADER bi{}; bi.bV5Size = sizeof bi; bi.bV5Width = s; bi.bV5Height = -s; bi.bV5Planes = 1; bi.bV5BitCount = 32;
        bi.bV5Compression = BI_BITFIELDS; bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
        void* bits = nullptr;
        HDC dc = GetDC(nullptr);
        HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, dc);
        if (!color) return nullptr;
        WICRect all{ 0, 0, s, s };
        bmp->CopyPixels(&all, s * 4, s * s * 4, (BYTE*)bits);
        auto px = (BYTE*)bits;
        for (int i = 0; i < s * s; i++) {
            BYTE a = px[i * 4 + 3];
            if (a && a < 255) for (int c = 0; c < 3; c++) px[i * 4 + c] = (BYTE)std::min(255, px[i * 4 + c] * 255 / a);
        }
        HBITMAP mask = CreateBitmap(s, s, 1, 1, nullptr);
        ICONINFO ii{ TRUE, 0, 0, mask, color };
        HICON icon = CreateIconIndirect(&ii);
        DeleteObject(mask); DeleteObject(color);
        return icon;
    }

    // Any drawing as an icon of s×s pixels.
    static HICON MakeIcon(int s, const std::function<void(ID2D1RenderTarget*, float)>& draw) {
        if (s < 8 || s > 256) return nullptr;
        Com<IWICBitmap> bmp; Com<ID2D1RenderTarget> rt;
        WIC->CreateBitmap(s, s, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (!bmp || FAILED(D2D->CreateWicBitmapRenderTarget(bmp, props, &rt))) return nullptr;
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        draw(rt, (float)s);
        rt->EndDraw();
        return ToIcon(bmp, s);
    }

    // The tray's icon: the switcher's own mark, drawn for the taskbar's colour.
    HICON TrayIcon(int s, bool darkTaskbar) { return MakeIcon(s, [darkTaskbar](ID2D1RenderTarget* rt, float size) { DrawAppStyled(rt, size, darkTaskbar); }); }

    // A small badge with what is left, white figures on green, amber or red, the colour of
    // the meters. Three digits get a smaller size so "100" still fits 16 px.
    HICON PercentIcon(int s, int left) {
        return MakeIcon(s, [left](ID2D1RenderTarget* rt, float size) {
            auto base = left >= 40 ? Rgba(0x2E9E5B) : left >= 15 ? Rgba(0xD98B1C) : Rgba(0xD13438);
            auto fill = Gradient(rt, Lighten(base, .12f), Darken(base, .10f), 0, size);
            float r = size * .24f;
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, size, size), r, r), fill);
            auto text = std::to_wstring(std::max(0, std::min(100, left)));
            Com<IDWriteTextFormat> f;
            DW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, text.size() > 2 ? DWRITE_FONT_STRETCH_CONDENSED : DWRITE_FONT_STRETCH_NORMAL,
                                 size * (text.size() > 2 ? .50f : text.size() > 1 ? .66f : .74f), L"en-us", &f);
            if (!f) return;
            Com<IDWriteTextLayout> l;
            DW->CreateTextLayout(text.c_str(), (UINT32)text.size(), f, size, size, &l);
            l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); l->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            Com<ID2D1SolidColorBrush> white; rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt->DrawTextLayout({ 0, -size * .02f }, l, white);
        });
    }

    // For reviewing the look: the app icon and a few account icons, written to a folder.
    void WriteIconSamples(const std::wstring& dir) {
        WriteIco(dir + L"\\app.ico", DrawApp);
        WriteIco(dir + L"\\app-on-dark.ico", [](ID2D1RenderTarget* rt, float s) { DrawAppStyled(rt, s, true); });
        WriteIco(dir + L"\\w.ico", [](ID2D1RenderTarget* rt, float s) { DrawAccount(rt, s, L"W", RGB(79, 91, 213), false); });
        WriteIco(dir + L"\\jd.ico", [](ID2D1RenderTarget* rt, float s) { DrawAccount(rt, s, L"JD", RGB(150, 96, 180), false); });
        WriteIco(dir + L"\\add.ico", [](ID2D1RenderTarget* rt, float s) { DrawAccount(rt, s, L"", RGB(46, 125, 74), true); });
        int n = 0;
        for (auto& it : App::items) {
            auto letters = it.letters; auto color = it.color;
            WriteIco(Util::Format(L"%s\\account-%d.ico", dir.c_str(), ++n), [&](ID2D1RenderTarget* rt, float s) { DrawAccount(rt, s, letters, color, false); });
        }
    }

    // ============================================================ shortcuts

    static void SaveLink(const std::wstring& lnk, const std::wstring& target, const std::wstring& args, const std::wstring& icon, const std::wstring& desc) {
        CreateDirectoryW(Util::Parent(lnk).c_str(), nullptr);
        CreateDirectoryW(Paths::Home.c_str(), nullptr);
        IShellLinkW* link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return;
        link->SetPath(target.c_str()); link->SetArguments(args.c_str());
        link->SetIconLocation(icon.c_str(), 0); link->SetDescription(desc.c_str());
        link->SetWorkingDirectory(Paths::Home.c_str());
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf)))) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
        link->Release();
    }

    // Goes through the switcher, which switches if needed and then opens Codex.
    std::wstring CreateShortcut(const Item& it, const std::wstring& dir) {
        auto lnk = dir + L"\\Codex - " + it.name + L".lnk";
        SaveLink(lnk, Paths::Launcher(), L"--switch \"" + it.id + L"\" --open", AccountIcon(it),
                 Util::Format(Tr(L"Open %s with the %s account", L"Открыть %s с аккаунтом %s"), Codex::AppName().c_str(), Util::Q(it.name).c_str()));
        return lnk;
    }

    std::vector<std::wstring> RemoveShortcuts(const std::wstring& name) {
        std::vector<std::wstring> removed;
        for (auto& dir : { Paths::Desktop(), Paths::StartMenu() }) {
            auto lnk = dir + L"\\Codex - " + name + L".lnk";
            if (Util::Exists(lnk) && DeleteFileW(lnk.c_str())) removed.push_back(lnk);
        }
        return removed;
    }

    // ============================================================ autostart

    static const wchar_t* RunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    static const wchar_t* RunValue = L"Codex Account Switcher";

    bool AutostartEnabled() {
        if (Paths::Isolated) return Settings::Flag(L"Autostart", false);
        wchar_t v[MAX_PATH * 2]; DWORD n = sizeof v;
        return RegGetValueW(HKEY_CURRENT_USER, RunKey, RunValue, RRF_RT_REG_SZ, nullptr, v, &n) == ERROR_SUCCESS && v[0];
    }

    void SetAutostart(bool on) {
        if (Paths::Isolated) { Settings::Set(L"Autostart", on); return; }
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, RunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
            if (on) { auto cmd = L"\"" + Paths::Launcher() + L"\" --startup"; RegSetValueExW(k, RunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * 2)); }
            else RegDeleteValueW(k, RunValue);
            RegCloseKey(k);
        }
        Settings::Set(L"Autostart", on);
    }

    // ============================================================ install

    // Closes a running switcher through its exit event and waits for it to let go of the exe.
    static void StopOthers() {
        if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, Core::ExitEvent)) {
            SetEvent(e); CloseHandle(e);
            for (int i = 0; i < 50; i++) {
                HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, Core::MutexName);
                if (!m) break;
                CloseHandle(m); Sleep(100);
            }
        }
    }

    void Install() {
        CreateDirectoryW(Paths::Root.c_str(), nullptr);
        CreateDirectoryW(Paths::Home.c_str(), nullptr);
        StopOthers();
        if (!Paths::Same(Paths::Self, Paths::InstalledExe)) {
            BOOL copied = FALSE;
            for (int i = 0; i < 20 && !copied; i++) { copied = CopyFileW(Paths::Self.c_str(), Paths::InstalledExe.c_str(), FALSE); if (!copied) Sleep(250); }
            if (!copied) throw Util::Format(Tr(L"Couldn't copy the program to %s (code %lu)", L"Не удалось скопировать программу в %s (код %lu)"), Paths::Home.c_str(), GetLastError());
            // Drop the "downloaded from the internet" mark so Windows does not warn on start.
            DeleteFileW((Paths::InstalledExe + L":Zone.Identifier").c_str());
        }
        auto exe = Paths::InstalledExe;
        auto about = Tr(L"Switch between Codex accounts", L"Переключение между аккаунтами Codex");
        SaveLink(Paths::Desktop() + L"\\Codex Account Switcher.lnk", exe, L"", exe, about);
        SaveLink(Paths::StartMenu() + L"\\Codex Account Switcher.lnk", exe, L"", exe, about);
        SaveLink(Paths::StartMenu() + L"\\Codex - Add account.lnk", exe, L"--add-account", AddIcon(),
                 Tr(L"Sign in to another Codex account", L"Войти в ещё один аккаунт Codex"));
        SetAutostart(Settings::Flag(L"Autostart", true));
        // Explorer caches shortcut icons; tell it they changed.
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }

    // ============================================================ jump list

    static IShellLinkW* Task(const std::wstring& title, const std::wstring& args, const std::wstring& icon, const std::wstring& desc) {
        IShellLinkW* l = nullptr;
        CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&l));
        if (!l) return nullptr;
        l->SetPath(Paths::Launcher().c_str()); l->SetArguments(args.c_str());
        l->SetIconLocation(icon.c_str(), 0); l->SetDescription(desc.c_str());
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(l->QueryInterface(IID_PPV_ARGS(&ps)))) {
            PROPVARIANT v; InitPropVariantFromString(title.c_str(), &v);
            ps->SetValue(PKEY_Title, v); ps->Commit(); PropVariantClear(&v); ps->Release();
        }
        return l;
    }

    // Right-click on the pinned taskbar button lists the accounts.
    void UpdateJumpList(const std::vector<Item>& items) {
        if (Paths::Isolated) return;
        static std::wstring signature;
        std::wstring sig = Paths::Launcher() + (L10n::ru ? L"|ru" : L"|en");
        for (auto& it : items) sig += L"|" + it.id + L"=" + it.shownName + Util::Format(L"/%06x", (unsigned)it.color);
        if (sig == signature) return;
        signature = sig;
        ICustomDestinationList* list = nullptr;
        if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list)))) return;
        UINT max; IObjectArray* removed = nullptr;
        if (SUCCEEDED(list->BeginList(&max, IID_PPV_ARGS(&removed)))) {
            IObjectCollection* accounts = nullptr, * tasks = nullptr;
            CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&accounts));
            CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tasks));
            for (auto& it : items) {
                if (auto t = Task(it.shownName, L"--switch \"" + it.id + L"\"", AccountIcon(it), Util::Format(Tr(L"Switch Codex to %s", L"Переключить Codex на %s"), Util::Q(it.shownName).c_str()))) { accounts->AddObject(t); t->Release(); }
            }
            if (auto t = Task(Tr(L"Add account", L"Добавить аккаунт"), L"--add-account", AddIcon(), Tr(L"Sign in to another Codex account", L"Войти в ещё один аккаунт Codex"))) { tasks->AddObject(t); t->Release(); }
            IObjectArray* arr = nullptr;
            if (!items.empty() && SUCCEEDED(accounts->QueryInterface(IID_PPV_ARGS(&arr)))) { list->AppendCategory(Tr(L"Accounts", L"Аккаунты"), arr); arr->Release(); }
            if (SUCCEEDED(tasks->QueryInterface(IID_PPV_ARGS(&arr)))) { list->AddUserTasks(arr); arr->Release(); }
            list->CommitList();
            accounts->Release(); tasks->Release(); if (removed) removed->Release();
        }
        list->Release();
    }

    // ============================================================ dark menus

    // uxtheme's ordinal exports are how Explorer and every Windows 11 app get dark native
    // menus; they are undocumented but stable since 1903.
    void DarkMenus(bool dark) {
        static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!ux) return;
        typedef int(WINAPI * SetMode)(int);
        typedef void(WINAPI * Flush)();
        auto set = (SetMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        auto flush = (Flush)GetProcAddress(ux, MAKEINTRESOURCEA(136));
        if (set) set(dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
        if (flush) flush();
    }

    void AllowDark(HWND h, bool dark) {
        static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        typedef bool(WINAPI * Allow)(HWND, bool);
        if (auto a = ux ? (Allow)GetProcAddress(ux, MAKEINTRESOURCEA(133)) : nullptr) a(h, dark);
    }
}
