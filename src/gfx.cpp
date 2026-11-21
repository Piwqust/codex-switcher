#include "gfx.h"
#include <dwmapi.h>
#include <commctrl.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <cmath>

namespace Gfx {
    ID2D1Factory* D2D; IDWriteFactory3* DW; IWICImagingFactory* WIC;
    Theme T;
    std::unordered_map<UINT, std::string>* idNames = nullptr;
    static std::wstring textFamily = L"Segoe UI Variable Text", displayFamily = L"Segoe UI Variable Display", iconFamily = L"Segoe Fluent Icons", monoFamily = L"Cascadia Mono";

    static bool HasFamily(const wchar_t* name) {
        IDWriteFontCollection* c = nullptr; DW->GetSystemFontCollection(&c);
        UINT32 i; BOOL exists = FALSE; c->FindFamilyName(name, &i, &exists); c->Release();
        return exists != FALSE;
    }

    void Init() {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &D2D);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), (IUnknown**)&DW);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&WIC));
        // Windows 10 has neither Segoe UI Variable nor Segoe Fluent Icons.
        if (!HasFamily(textFamily.c_str())) textFamily = displayFamily = L"Segoe UI";
        if (!HasFamily(iconFamily.c_str())) iconFamily = L"Segoe MDL2 Assets";
        if (!HasFamily(monoFamily.c_str())) monoFamily = L"Consolas";
        if (Test::On()) idNames = new std::unordered_map<UINT, std::string>();
        ReadSystem();
        ApplyTheme();
    }

    D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
        // A fully transparent end has no colour of its own: it takes the other's, so a fade from
        // nothing doesn't pass through grey.
        if (a.a <= 0) { a.r = b.r; a.g = b.g; a.b = b.b; }
        if (b.a <= 0) { b.r = a.r; b.g = a.g; b.b = a.b; }
        return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
    }

    D2D1_COLOR_F Veil(float hover, float press) {
        float over = T.contrast ? .16f : T.dark ? .09f : .045f, down = T.contrast ? .26f : T.dark ? .03f : .028f;
        float a = over * hover;
        a += (down - a) * press;
        auto c = T.contrast ? T.text : T.dark ? Rgba(0xFFFFFF) : Rgba(0x000000);
        c.a = a;
        return c;
    }

    bool SystemDark() {
        DWORD v = 1, n = sizeof v;
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n);
        return v == 0;
    }

    bool BackdropSupported() {
        static int build = [] {
            typedef LONG(WINAPI * Fn)(OSVERSIONINFOW*);
            OSVERSIONINFOW o{ sizeof o };
            ((Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))(&o);
            return (int)o.dwBuildNumber;
        }();
        return build >= 22621;
    }

    bool Offscreen() {
        static bool on = Paths::Isolated && GetEnvironmentVariableW(L"CAS_OFFSCREEN", nullptr, 0) > 0;
        return on;
    }

    bool HighContrast() {
        HIGHCONTRASTW hc{ sizeof hc };
        return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
    }

    static bool motion = true;
    bool Motion() { return motion; }
    void ReadSystem() {
        BOOL on = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
        wchar_t forced[8] = {};
        // CAS_MOTION=off in the demo, to look at the windows without animation.
        if (Paths::Isolated && GetEnvironmentVariableW(L"CAS_MOTION", forced, 8) && !wcscmp(forced, L"off")) on = FALSE;
        motion = on != FALSE;
    }

    void ApplyTheme() {
        wchar_t forced[16] = {};
        GetEnvironmentVariableW(L"CAS_THEME", forced, 16);   // light|dark, for checking both looks
        // CAS_THEME first (screenshots), then the Theme setting, then Windows.
        auto choice = forced[0] ? std::wstring(forced) : Settings::Str(L"Theme", L"auto");
        bool dark = choice == L"dark" || (choice != L"light" && SystemDark());
        auto c = [](UINT rgb, float a = 1) { return Rgba(rgb, a); };
        // High contrast: only the system's colours, every edge a solid line, state shown by
        // text and shape rather than by colour. CAS_THEME=contrast shows it for screenshots.
        if (HighContrast() || choice == L"contrast") {
            auto sys = [](int i, float a = 1) { return FromColorref(GetSysColor(i), a); };
            auto bg = sys(COLOR_WINDOW), ink = sys(COLOR_WINDOWTEXT), hi = sys(COLOR_HIGHLIGHT), hiText = sys(COLOR_HIGHLIGHTTEXT);
            auto none = D2D1::ColorF(0, 0, 0, 0);
            T = { bg.r * .299f + bg.g * .587f + bg.b * .114f < .5f, bg, ink, ink, ink,
                none, sys(COLOR_WINDOWTEXT, .16f), sys(COLOR_WINDOWTEXT, .26f), ink, none, ink,
                none, sys(COLOR_WINDOWTEXT, .16f), sys(COLOR_WINDOWTEXT, .26f), ink, ink, bg, ink,
                hi, hi, hi, sys(COLOR_HOTLIGHT), hiText, sys(COLOR_WINDOWTEXT, .14f), ink, ink, ink, hi, sys(COLOR_WINDOWTEXT, .16f) };
            T.contrast = true;
            return;
        }
        // text3 (emails, reset times, hints) keeps 4.5:1 on every surface it sits on.
        if (dark) T = { true, c(0x202020), c(0xFFFFFF), c(0xC8C8C8), c(0x9E9E9E),
            c(0xFFFFFF, .05f), c(0xFFFFFF, .09f), c(0xFFFFFF, .03f), c(0xFFFFFF, .08f), c(0xFFFFFF, .05f), c(0x000000, .10f),
            c(0xFFFFFF, .06f), c(0xFFFFFF, .09f), c(0xFFFFFF, .03f), c(0xFFFFFF, .08f), c(0xFFFFFF, .55f), c(0x2C2C2C), c(0x3D3D3D),
            c(0xECECEC), c(0xFFFFFF), c(0xC9C9C9), c(0xE3E3E3), c(0x0D0D0D), c(0xFFFFFF, .08f), c(0x6CCB5F), c(0xFCE100), c(0xFF99A4), c(0xFFFFFF), c(0xFFFFFF, .08f) };
        else T = { false, c(0xF3F3F3), c(0x1B1B1B), c(0x5D5D5D), c(0x6B6B6B),
            c(0xFFFFFF, .70f), c(0xFFFFFF, .95f), c(0xFFFFFF, .50f), c(0x000000, .07f), c(0xFFFFFF, .70f), c(0x000000, .06f),
            c(0xFFFFFF, .70f), c(0xF9F9F9, .50f), c(0xF9F9F9, .30f), c(0x000000, .08f), c(0x000000, .45f), c(0xF9F9F9), c(0xE2E2E2),
            c(0x0D0D0D), c(0x2F2F2F), c(0x000000), c(0x0D0D0D), c(0xFFFFFF), c(0x000000, .05f), c(0x0F7B0F), c(0x9D5D00), c(0xC42B1C), c(0x1B1B1B), c(0x000000, .05f) };
    }

    struct FontKey { float size; int weight; int kind; bool operator==(const FontKey& o) const { return size == o.size && weight == o.weight && kind == o.kind; } };
    struct FontHash { size_t operator()(const FontKey& k) const { return std::hash<float>()(k.size) ^ (k.weight << 8) ^ (k.kind << 20); } };

    static IDWriteTextFormat* FontOf(const FontKey& k, const std::wstring& fam) {
        static std::unordered_map<FontKey, IDWriteTextFormat*, FontHash> cache;
        auto it = cache.find(k);
        if (it != cache.end()) return it->second;
        IDWriteTextFormat* f = nullptr;
        DW->CreateTextFormat(fam.c_str(), nullptr, (DWRITE_FONT_WEIGHT)k.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, k.size, L10n::ru ? L"ru-ru" : L"en-us", &f);
        cache[k] = f;
        return f;
    }

    IDWriteTextFormat* Font(float size, DWRITE_FONT_WEIGHT weight, bool display, bool icons) {
        return FontOf({ size, (int)weight, icons ? 2 : display ? 1 : 0 }, icons ? iconFamily : display ? displayFamily : textFamily);
    }

    IDWriteTextFormat* Mono(float size, DWRITE_FONT_WEIGHT weight) { return FontOf({ size, (int)weight, 3 }, monoFamily); }

    ID2D1Bitmap* BitmapFromIcon(ID2D1RenderTarget* rt, HICON icon) {
        if (!icon) return nullptr;
        IWICBitmap* wb = nullptr; IWICFormatConverter* conv = nullptr; ID2D1Bitmap* out = nullptr;
        if (SUCCEEDED(WIC->CreateBitmapFromHICON(icon, &wb)) && SUCCEEDED(WIC->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(wb, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            rt->CreateBitmapFromWicBitmap(conv, nullptr, &out);
        if (conv) conv->Release();
        if (wb) wb->Release();
        return out;
    }

}

using namespace Gfx;

// ============================================================ window plumbing

static void ForgetTarget(ID2D1RenderTarget* rt);

Surface::~Surface() {
    if (link) *link = nullptr;
    if (tipWnd) DestroyWindow(tipWnd);
    if (hwndRt) { ForgetTarget(hwndRt); hwndRt->Release(); }
    if (editFont) DeleteObject(editFont);
    if (editBrush) DeleteObject(editBrush);
    if (hwnd) { SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); DestroyWindow(hwnd); }
}

// CAS_SCALE=2 in the demo renders every window at that scale whatever the monitor's, so
// screenshots come out sharp on high-DPI screens. Never applies to the real switcher.
static float ForcedScale() {
    static float s = [] { wchar_t b[8] = {}; GetEnvironmentVariableW(L"CAS_SCALE", b, 8); float v = (float)_wtof(b); return Paths::Demo && v >= 1 && v <= 4 ? v : 0.f; }();
    return s;
}
static float WindowDpi(HWND h) { return ForcedScale() ? 96.f * ForcedScale() : (float)GetDpiForWindow(h); }

bool Surface::Create(const wchar_t* title, DWORD style, DWORD exStyle, float w, float h, HWND owner, bool noFrame) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof wc };
        wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"CodexSwitcherSurface";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        wc.hIconSm = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        RegisterClassExW(&wc);
        registered = true;
    }
    borderless = noFrame;
    width = w;
    if (!link) link = std::make_shared<Surface*>(this);
    // WS_CLIPCHILDREN: the drawing must not paint over child windows such as the text box.
    style |= WS_CLIPCHILDREN;
    hwnd = CreateWindowExW(exStyle, L"CodexSwitcherSurface", title, style, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd) return false;
    dpi = WindowDpi(hwnd); scale = dpi / 96.f;
    RECT rc{ 0, 0, (LONG)std::lround(w * scale), (LONG)std::lround(h * scale) };
    if (!borderless) AdjustWindowRectExForDpi(&rc, style, FALSE, exStyle, GetDpiForWindow(hwnd));   // the frame is drawn at the real DPI
    SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ApplyBackdrop();
    return true;
}

void Surface::ApplyBackdrop() {
    BOOL dark = T.dark;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    int round = 2;
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    // Mica takes its tint from the system theme, so a forced theme gets a plain background.
    // Screenshots (CAS_SCALE) get the plain fill: PrintWindow cannot capture the backdrop.
    backdropOn = BackdropSupported() && T.dark == SystemDark() && !T.contrast && !ForcedScale();
    MARGINS m = backdropOn ? MARGINS{ -1, -1, -1, -1 } : MARGINS{ 0, 0, 0, 0 };
    DwmExtendFrameIntoClientArea(hwnd, &m);
    if (BackdropSupported()) {
        int type = backdropOn ? backdrop : 1;
        DwmSetWindowAttribute(hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &type, sizeof type);
    }
}

void Surface::Restyle() { if (!hwnd) return; ApplyBackdrop(); if (editBrush) { DeleteObject(editBrush); editBrush = nullptr; } Invalidate(); }

void Surface::Show(bool activate) {
    if (Offscreen()) {
        SetWindowLongW(hwnd, GWL_EXSTYLE, GetWindowLongW(hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW);
        RECT r; GetWindowRect(hwnd, &r);
        SetWindowPos(hwnd, HWND_BOTTOM, -32000 + 600 * (int)(((UINT_PTR)hwnd >> 4) % 8), 200, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        OnShown();
        Invalidate();
        return;
    }
    ShowWindow(hwnd, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate) { SetForegroundWindow(hwnd); SetFocus(hwnd); }
    OnShown();
    Invalidate();
}

void Surface::Hide() { if (edit) CloseEdit(false); HideTip(); SavePlace(); ShowWindow(hwnd, SW_HIDE); hot = pressed = 0; dragging = false; }

void Surface::EnsureTarget() {
    RECT rc; GetClientRect(hwnd, &rc);
    if (!hwndRt) {
        // Software rendering: no GPU device to bring up, so the first frame lands in tens of
        // milliseconds, and a window this size draws in well under one.
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
        D2D->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), &hwndRt);
        if (hwndRt) hwndRt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    } else {
        auto s = hwndRt->GetPixelSize();
        if (s.width != (UINT32)rc.right || s.height != (UINT32)rc.bottom) hwndRt->Resize(D2D1::SizeU(rc.right, rc.bottom));
        hwndRt->SetDpi(dpi, dpi);
    }
    rt = hwndRt;
}

void Surface::Render() {
    // A hidden window moved to a monitor with another scale may never get WM_DPICHANGED,
    // so the scale is read again before every frame.
    if (!ForcedScale()) {
        float d = (float)GetDpiForWindow(hwnd);
        if (d && d != dpi) { dpi = d; scale = d / 96.f; if (editFont) { DeleteObject(editFont); editFont = nullptr; } }
    }
    EnsureTarget();
    if (!rt) return;
    LARGE_INTEGER now, freq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
    // The first frame after a quiet spell counts as one ordinary frame, so a hover that
    // starts from rest eases in instead of jumping.
    frameDt = lastFrame.QuadPart ? (float)((now.QuadPart - lastFrame.QuadPart) * 1000.0 / freq.QuadPart) : 16.f;
    if (frameDt > 100) frameDt = 16;
    lastFrame = now;
    animating = false;
    building.clear(); buildingTexts.clear(); inScroll = false;
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(backdropOn ? D2D1::ColorF(0, 0, 0, 0) : (borderless ? T.menu : T.bg));
    Build();
    DrawToast();
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) { ForgetTarget(hwndRt); hwndRt->Release(); hwndRt = nullptr; rt = nullptr; Invalidate(); }
    regions.swap(building); texts.swap(buildingTexts);
    if (!rt) return;

    if (animating && !timerOn) { SetTimer(hwnd, 1, 8, nullptr); timerOn = true; }
    if (!animating && timerOn) { KillTimer(hwnd, 1); timerOn = false; lastFrame.QuadPart = 0; }

    if (sizeToContent && contentHeight > 0) {
        RECT rc; GetClientRect(hwnd, &rc);
        // The width follows the scale too: otherwise a flyout drawn at 125% stays as wide as at 100% and loses its right side.
        int want = (int)std::lround(contentHeight * scale), wantW = (int)std::lround(width * scale);
        if (want != rc.bottom || wantW != rc.right) {
            RECT wr{ 0, 0, wantW, want };
            DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE), ex = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
            if (!borderless) AdjustWindowRectExForDpi(&wr, style, FALSE, ex, GetDpiForWindow(hwnd));
            SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            Invalidate();
            OnResized();
        }
    }
}

// The demo's screenshots: the window draws one frame into a bitmap and saves it, so the
// picture doesn't depend on where the window is or on what DWM composes. The backdrop can't
// be drawn this way, so the plain background stands in for it.
void Surface::Snapshot(const std::wstring& png) {
    RECT rc; GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    IWICBitmap* bmp = nullptr; ID2D1RenderTarget* target = nullptr;
    WIC->CreateBitmap(rc.right, rc.bottom, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    if (!bmp || FAILED(D2D->CreateWicBitmapRenderTarget(bmp, props, &target))) { if (bmp) bmp->Release(); return; }
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    auto saved = rt; bool backdrop = backdropOn;
    rt = target; backdropOn = false;
    building.clear(); buildingTexts.clear(); inScroll = false;
    target->BeginDraw();
    target->Clear(borderless ? T.menu : T.bg);
    Build();
    DrawToast();
    target->EndDraw();
    regions.swap(building); texts.swap(buildingTexts);
    rt = saved; backdropOn = backdrop;
    ForgetTarget(target);
    target->Release();
    IWICStream* stream = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* frame = nullptr;
    if (SUCCEEDED(WIC->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename((png + L".part").c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(WIC->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))) {
        frame->SetSize(rc.right, rc.bottom);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA; frame->SetPixelFormat(&fmt);
        frame->WriteSource(bmp, nullptr); frame->Commit(); enc->Commit();
    }
    if (frame) frame->Release(); if (enc) enc->Release(); if (stream) stream->Release();
    bmp->Release();
    MoveFileExW((png + L".part").c_str(), png.c_str(), MOVEFILE_REPLACE_EXISTING);   // whole, or not at all
}

const Region* Surface::At(float x, float y) const {
    for (auto it = regions.rbegin(); it != regions.rend(); ++it) if (it->r.Has(x, y)) return &*it;
    return nullptr;
}

void Surface::MoveFocus(int dir) {
    std::vector<UINT> ids;
    for (auto& r : regions) if (r.focusable) ids.push_back(r.id);
    if (ids.empty()) return;
    int i = -1;
    for (int k = 0; k < (int)ids.size(); k++) if (ids[k] == focus) i = k;
    i = i < 0 ? (dir > 0 ? 0 : (int)ids.size() - 1) : (i + dir + (int)ids.size()) % (int)ids.size();
    focus = ids[i]; keyboardCues = true;
    EnsureVisible(focus);
    ArmTip(focus);
    Acc::FocusChanged(this);
    Invalidate();
}

// Keyboard focus on a control scrolled out of view brings it into view.
void Surface::EnsureVisible(UINT id) {
    if (scrollMax <= 0 || scrollView.h <= 0) return;
    for (auto& r : regions) if (r.id == id) {
        if (!r.scrolls) return;
        const float pad = 8;
        float to = scrollY;
        if (r.bounds.h > scrollView.h - 2 * pad || r.bounds.y < scrollView.y + pad) to -= scrollView.y + pad - r.bounds.y;
        else if (r.bounds.B() > scrollView.B() - pad) to += r.bounds.B() - (scrollView.B() - pad);
        scrollY = std::max(0.f, std::min(scrollMax, to));
        return;
    }
}

void Surface::Activate(UINT id) { PostMessageW(hwnd, WM_APP + 63, id, 0); }

void Surface::FocusOn(UINT id) {
    for (auto& r : regions) if (r.id == id && r.focusable) { focus = id; keyboardCues = true; EnsureVisible(id); Invalidate(); return; }
}

// ---- the text shown when the pointer rests on a control, or keyboard focus reaches it

void Surface::ArmTip(UINT id) {
    HideTip();
    tipFor = 0;
    for (auto& r : regions) if (r.id == id && !r.tip.empty()) { tipFor = id; SetTimer(hwnd, 2, 500, nullptr); return; }
}

void Surface::HideTip() {
    if (!hwnd) return;
    KillTimer(hwnd, 2);
    if (tipWnd && IsWindowVisible(tipWnd)) { TOOLINFOW ti{ sizeof ti }; ti.hwnd = hwnd; ti.uId = 1; SendMessageW(tipWnd, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti); }
}

void Surface::ShowTip() {
    KillTimer(hwnd, 2);
    const Region* reg = nullptr;
    for (auto& r : regions) if (r.id == tipFor && !r.tip.empty() && (hot == r.id || (keyboardCues && focus == r.id))) reg = &r;
    if (!reg) return;
    Test::Trace(L"tip|" + reg->tip);
    if (Offscreen()) return;   // nothing may appear on the screen of whoever works at this PC
    if (!tipWnd) {
        tipWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TRANSPARENT, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                 0, 0, 0, 0, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!tipWnd) return;
        TOOLINFOW ti{ sizeof ti }; ti.uFlags = TTF_TRACK | TTF_ABSOLUTE; ti.hwnd = hwnd; ti.uId = 1; ti.lpszText = (LPWSTR)L"";
        SendMessageW(tipWnd, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        tipDark = !T.dark;
    }
    SendMessageW(tipWnd, TTM_SETMAXTIPWIDTH, 0, (LPARAM)(320 * scale));
    if (tipDark != T.dark) { tipDark = T.dark; SetWindowTheme(tipWnd, T.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr); }
    TOOLINFOW ti{ sizeof ti }; ti.hwnd = hwnd; ti.uId = 1; ti.lpszText = (LPWSTR)reg->tip.c_str();
    SendMessageW(tipWnd, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    // Under the control, centred; above it when there is no room below.
    DWORD size = (DWORD)SendMessageW(tipWnd, TTM_GETBUBBLESIZE, 0, (LPARAM)&ti);
    int tw = LOWORD(size), th = HIWORD(size), gap = (int)(6 * scale);
    POINT a{ (LONG)((reg->r.x + reg->r.w / 2) * scale), (LONG)(reg->r.B() * scale) }, b{ 0, (LONG)(reg->r.y * scale) };
    ClientToScreen(hwnd, &a); ClientToScreen(hwnd, &b);
    MONITORINFO mi{ sizeof mi }; GetMonitorInfoW(MonitorFromPoint(a, MONITOR_DEFAULTTONEAREST), &mi);
    int x = std::max((int)mi.rcWork.left + 4, std::min((int)a.x - tw / 2, (int)mi.rcWork.right - tw - 4));
    int y = a.y + gap + th <= mi.rcWork.bottom ? a.y + gap : b.y - gap - th;
    SendMessageW(tipWnd, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
    SendMessageW(tipWnd, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
}

// ---- where the window was left

void Surface::SavePlace() {
    if (placeKey.empty() || Offscreen() || !hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return;
    RECT r; GetWindowRect(hwnd, &r);
    auto now = Util::Format(L"%d,%d,%d,%d", (int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top));
    if (Settings::Str(L"Place." + placeKey) != now) Settings::Set(L"Place." + placeKey, json::Value(now));
}

bool Surface::RestorePlace(const wchar_t* key) {
    placeKey = key;
    if (Offscreen()) return false;
    int x, y, w, h;
    if (swscanf_s(Settings::Str(L"Place." + placeKey).c_str(), L"%d,%d,%d,%d", &x, &y, &w, &h) != 4 || w < 200 || h < 150) return false;
    // Only where a screen still is: the monitor it was on may be gone.
    RECT want{ x, y, x + w, y + h }, seen;
    HMONITOR mon = MonitorFromRect(&want, MONITOR_DEFAULTTONULL);
    if (!mon) return false;
    MONITORINFO mi{ sizeof mi }; GetMonitorInfoW(mon, &mi);
    if (!IntersectRect(&seen, &want, &mi.rcWork) || seen.right - seen.left < 160 || seen.bottom - seen.top < 80 || want.top < mi.rcWork.top - 8) return false;
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

LRESULT CALLBACK Surface::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
    auto self = (Surface*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!self) return DefWindowProcW(h, m, w, l);
    if (m == WM_NCCREATE) self->hwnd = h;
    // A finger has no pointer to rest on a control. Windows leaves its unseen pointer where
    // the tap was, so the control would stay lit as if hovered: once the finger lifts and
    // the tap has done its work, nothing is under the pointer any more. A pen does hover.
    bool lifted = m == WM_LBUTTONUP && (GetMessageExtraInfo() & 0xFFFFFF80) == 0xFF515780;   // MI_WP_SIGNATURE, touch
    auto alive = self->link;
    LRESULT r = self->Handle(m, w, l);
    if (lifted && alive && *alive) {
        self->mx = self->my = -1; self->hot = 0;
        self->HideTip(); self->Invalidate();
    }
    return r;
}

LRESULT Surface::Handle(UINT m, WPARAM w, LPARAM l) {
    bool handled = false;
    LRESULT r = Extra(m, w, l, handled);
    if (handled) return r;
    switch (m) {
    case WM_NCCALCSIZE:
        if (borderless && w) return 0;   // keep the DWM shadow and rounded corners, lose the frame
        break;
    case WM_NCHITTEST:
        if (borderless) return HTCLIENT;
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); Render(); EndPaint(hwnd, &ps); return 0; }
    case WM_APP + 61: {
        // tools/capture.ps1 in the demo: the picture goes to <CAS_SNAPSHOT>\snapshot-<w>.png.
        wchar_t dir[MAX_PATH] = {};
        if (Paths::Isolated && GetEnvironmentVariableW(L"CAS_SNAPSHOT", dir, MAX_PATH)) Snapshot(Util::Format(L"%s\\snapshot-%u.png", dir, (unsigned)w));
        return 0;
    }
    case WM_APP + 62: {
        // tools\uitest.ps1: where each control is right now, by name, in DIPs. A fresh frame
        // first, so hover that was just posted is in it.
        if (!Test::On()) return 0;
        Render();
        // The first line: the scale, the client size, how far it is scrolled, and the control with keyboard focus.
        auto cl = Client();
        std::wstring out = Util::Format(L"scale %.4f client %.1f %.1f scroll %.1f %.1f focus %s\r\n", scale, cl.w, cl.h, scrollY, scrollMax,
                                        idNames && idNames->count(focus) ? Util::Wide((*idNames)[focus]).c_str() : L"-");
        for (auto& rg : regions) {
            auto name = idNames && idNames->count(rg.id) ? Util::Wide((*idNames)[rg.id]) : Util::Format(L"#%u", rg.id);
            // After the place: whether the keyboard reaches it, then what a screen reader says and the hover text.
            out += Util::Format(L"%s %.1f %.1f %.1f %.1f %d\t%s\t%s\r\n", name.c_str(), rg.r.x, rg.r.y, rg.r.w, rg.r.h, rg.focusable ? 1 : 0, rg.name.c_str(), rg.tip.c_str());
        }
        Util::WriteText(Util::Format(L"%s\\regions-%u.txt", Paths::Root.c_str(), (unsigned)w), out, false);
        return 0;
    }
    case WM_GETOBJECT: {
        LRESULT res = 0;
        if (Acc::Answer(this, w, l, res)) return res;
        break;
    }
    case WM_DESTROY: Acc::Gone(hwnd); break;
    case WM_APP + 63:
        // A screen reader pressed a control: as a click on it.
        for (auto& rg : regions) if (rg.id == (UINT)w && rg.click) { if (rg.focusable) focus = rg.id; auto fn = rg.click; fn(); Invalidate(); break; }
        return 0;
    case WM_EXITSIZEMOVE: SavePlace(); break;
    case WM_DROPFILES: {
        auto drop = (HDROP)w;
        std::vector<std::wstring> files;
        for (UINT i = 0, n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0); i < n; i++) {
            wchar_t path[MAX_PATH * 2] = {};
            if (DragQueryFileW(drop, i, path, ARRAYSIZE(path))) files.push_back(path);
        }
        DragFinish(drop);
        if (!files.empty()) OnDrop(files);
        return 0;
    }
    case WM_APP + 64: {
        // tools\uitest.ps1: a file dropped on the window, its path in <demo>\test-drop.txt.
        if (!Test::On()) return 0;
        auto path = Util::Trim(Test::Take(L"test-drop.txt"));
        if (!path.empty()) OnDrop({ path });
        return 0;
    }
    case WM_SIZE: Invalidate(); return 0;
    case WM_TIMER:
        if (w == 1) Invalidate();
        if (w == 2) ShowTip();
        return 0;
    case WM_DPICHANGED: {
        if (ForcedScale()) return 0;
        dpi = (float)HIWORD(w); scale = dpi / 96.f;
        auto rc = (RECT*)l;
        SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (editFont) { DeleteObject(editFont); editFont = nullptr; }
        Invalidate(); return 0;
    }
    case WM_MOUSEMOVE: {
        mx = GET_X_LPARAM(l) / scale; my = GET_Y_LPARAM(l) / scale;
        // The checks move a pointer that isn't there: Windows would report it gone at once.
        if (!Test::On()) { TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&t); }
        if (dragging) {
            // The scroll bar's thumb follows the pointer, wherever it wanders sideways.
            float track = scrollView.h - thumbH;
            if (track > 0) scrollY = std::max(0.f, std::min(scrollMax, (my - dragOffset - scrollView.y) / track * scrollMax));
            Invalidate(); return 0;
        }
        auto hit = At(mx, my); UINT id = hit ? hit->id : 0;
        if (id != hot) { hot = id; ArmTip(id); Invalidate(); }
        return 0;
    }
    case WM_MOUSELEAVE: mx = my = -1; HideTip(); if (hot) { hot = 0; Invalidate(); } return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            auto hit = At(mx, my);
            SetCursor(LoadCursorW(nullptr, hit && hit->hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEWHEEL:
        if (scrollMax > 0) {
            // The text box for a name stays where it was put: the list doesn't scroll away
            // from under it. Scrolling finishes the edit, as a click elsewhere does.
            if (edit) CloseEdit(true);
            // As many lines per notch as Windows is set to, or a page.
            UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            float step = lines == WHEEL_PAGESCROLL ? std::max(48.f, scrollView.h - 48) : std::max(1u, lines) * 16.f;
            scrollY = std::max(0.f, std::min(scrollMax, scrollY - GET_WHEEL_DELTA_WPARAM(w) / 120.f * step));
            HideTip();
            Invalidate(); return 0;
        }
        break;
    case WM_LBUTTONDOWN: {
        mx = GET_X_LPARAM(l) / scale; my = GET_Y_LPARAM(l) / scale;   // a click may arrive without a move first (touch, pen)
        if (edit) CloseEdit(true);
        OnMouseDown();
        keyboardCues = false;
        HideTip();
        auto hit = At(mx, my);
        pressed = hit ? hit->id : 0;
        if (hit && hit->focusable) focus = hit->id;
        if (hit && hit->id == Id("scrollbar") && scrollMax > 0) {
            // On the thumb: drag it. Beside it: a page that way.
            if (my >= thumbY && my < thumbY + thumbH) { dragging = true; dragOffset = my - thumbY; }
            else scrollY = std::max(0.f, std::min(scrollMax, scrollY + (my < thumbY ? -1 : 1) * std::max(48.f, scrollView.h - 48)));
        }
        SetCapture(hwnd); Invalidate(); return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        dragging = false;
        auto hit = At(GET_X_LPARAM(l) / scale, GET_Y_LPARAM(l) / scale);
        UINT was = pressed; pressed = 0; Invalidate();
        if (hit && hit->id == was && hit->click) { auto fn = hit->click; fn(); }
        return 0;
    }
    case WM_RBUTTONUP: {
        OnMouseDown();
        auto hit = At(GET_X_LPARAM(l) / scale, GET_Y_LPARAM(l) / scale);
        if (hit && hit->rclick) { auto fn = hit->rclick; fn(); }
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        HideTip();
        if (OnKey((UINT)w, alt)) return 0;
        if (scrollMax > 0 && (w == VK_PRIOR || w == VK_NEXT || w == VK_HOME || w == VK_END)) {
            float page = std::max(48.f, scrollView.h - 48);
            float to = w == VK_HOME ? 0 : w == VK_END ? scrollMax : scrollY + (w == VK_PRIOR ? -page : page);
            scrollY = std::max(0.f, std::min(scrollMax, to));
            Invalidate(); return 0;
        }
        if (w == VK_TAB) { MoveFocus(GetKeyState(VK_SHIFT) & 0x8000 ? -1 : 1); return 0; }
        if (w == VK_RIGHT || w == VK_DOWN) { MoveFocus(1); return 0; }
        if (w == VK_LEFT || w == VK_UP) { MoveFocus(-1); return 0; }
        if (w == VK_RETURN || w == VK_SPACE) {
            for (auto& rg : regions) if (rg.id == focus && rg.click) { auto fn = rg.click; fn(); return 0; }
        }
        if (w == VK_ESCAPE) { OnClose(); return 0; }
        break;
    }
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) { HideTip(); dragging = false; OnDeactivate(); } Invalidate(); return 0;
    case WM_CLOSE: OnClose(); return 0;
    case WM_CTLCOLOREDIT: {
        auto dc = (HDC)w;
        auto c = T.menu; auto t = T.text;
        SetTextColor(dc, RGB((int)(t.r * 255), (int)(t.g * 255), (int)(t.b * 255)));
        SetBkColor(dc, RGB((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255)));
        if (!editBrush) editBrush = CreateSolidBrush(RGB((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255)));
        return (LRESULT)editBrush;
    }
    case WM_APP + 50: CloseEdit(w != 0); return 0;
    case WM_APP + 52: if (edit) { SetFocus(edit); SendMessageW(edit, EM_SETSEL, 0, -1); } return 0;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// ============================================================ drawing helpers

float Surface::A(UINT id, float target, float ms) {
    auto it = anims.find(id);
    if (it == anims.end()) { anims[id] = { target, target }; return target; }
    auto& v = it->second;
    v.second = target;
    if (!Motion()) { v.first = target; return target; }   // "Animation effects" is off in Windows
    if (v.first != target) {
        // Exponential approach: smooth, and a change of mind mid-way simply heads elsewhere.
        float k = 1 - std::exp(-frameDt / (ms / 3.f));
        v.first += (target - v.first) * k;
        if (std::fabs(target - v.first) < 0.003f) v.first = target; else animating = true;
    }
    return v.first;
}

// One brush, recoloured per call, belonging to the target that last drew. Dropped when its
// target goes: a new target can be given the same address, and would then draw with a
// brush of the released one.
static ID2D1SolidColorBrush* brush = nullptr; static ID2D1RenderTarget* brushOwner = nullptr;

static void ForgetTarget(ID2D1RenderTarget* rt) {
    if (brushOwner == rt) { if (brush) brush->Release(); brush = nullptr; brushOwner = nullptr; }
}

static ID2D1SolidColorBrush* Brush(ID2D1RenderTarget* rt, D2D1_COLOR_F c) {
    if (brushOwner != rt) { if (brush) brush->Release(); brush = nullptr; brushOwner = rt; }
    if (!brush) rt->CreateSolidColorBrush(c, &brush); else brush->SetColor(c);
    return brush;
}

void Surface::Fill(RectF r, D2D1_COLOR_F c, float radius) {
    if (c.a <= 0) return;
    if (radius > 0) rt->FillRoundedRectangle(D2D1::RoundedRect(r.D(), radius, radius), Brush(rt, c));
    else rt->FillRectangle(r.D(), Brush(rt, c));
}

void Surface::Stroke(RectF r, D2D1_COLOR_F c, float radius, float width) {
    if (c.a <= 0) return;
    auto d = r.Inset(width / 2).D();
    if (radius > 0) rt->DrawRoundedRectangle(D2D1::RoundedRect(d, radius - width / 2, radius - width / 2), Brush(rt, c), width);
    else rt->DrawRectangle(d, Brush(rt, c), width);
}

void Surface::Circle(float cx, float cy, float rad, D2D1_COLOR_F c) { rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), Brush(rt, c)); }
void Surface::Ring(float cx, float cy, float rad, D2D1_COLOR_F c, float w) { rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad - w / 2, rad - w / 2), Brush(rt, c), w); }
void Surface::Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float w) { rt->DrawLine({ x1, y1 }, { x2, y2 }, Brush(rt, c), w); }

static IDWriteTextLayout* Layout(const std::wstring& s, RectF r, IDWriteTextFormat* f, Align a, bool vcenter, bool wrap) {
    IDWriteTextLayout* l = nullptr;
    DW->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, std::max(1.f, r.w), std::max(1.f, r.h), &l);
    if (!l) return nullptr;
    l->SetTextAlignment(a == Align::Left ? DWRITE_TEXT_ALIGNMENT_LEADING : a == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
    l->SetParagraphAlignment(vcenter ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    l->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap) {
        IDWriteInlineObject* sign = nullptr; DW->CreateEllipsisTrimmingSign(f, &sign);
        DWRITE_TRIMMING t{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        l->SetTrimming(&t, sign);
        if (sign) sign->Release();
    }
    return l;
}

void Surface::Text(const std::wstring& s, RectF r, IDWriteTextFormat* f, D2D1_COLOR_F c, Align a, bool vcenter, bool wrap) {
    if (s.empty() || c.a <= 0) return;
    auto l = Layout(s, r, f, a, vcenter, wrap);
    if (!l) return;
    rt->DrawTextLayout({ r.x, r.y }, l, Brush(rt, c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    l->Release();
    if (!noRuns) buildingTexts.push_back({ s, r });
}

void Surface::Glyph(const wchar_t* g, RectF r, float size, D2D1_COLOR_F c) {
    noRuns = true;
    Text(g, r, Font(size, DWRITE_FONT_WEIGHT_NORMAL, false, true), c, Align::Center);
    noRuns = false;
}

D2D1_SIZE_F Surface::Measure(const std::wstring& s, IDWriteTextFormat* f, float maxW, bool wrap) {
    auto l = Layout(s, { 0, 0, maxW, 10000 }, f, Align::Left, false, wrap);
    DWRITE_TEXT_METRICS m{}; if (l) { l->GetMetrics(&m); l->Release(); }
    return { m.widthIncludingTrailingWhitespace, m.height };
}

void Surface::Image(ID2D1Bitmap* b, RectF r, float opacity) { if (b) rt->DrawBitmap(b, r.D(), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR); }

void Surface::Hit(UINT id, RectF r, std::function<void()> click, std::function<void()> rclick, bool focusable, bool hand) {
    Region g{ id, r, std::move(click), std::move(rclick), focusable, hand };
    g.bounds = r; g.scrolls = inScroll;
    building.push_back(std::move(g));
}

void Surface::Describe(const std::wstring& name, Role role, int state) {
    if (building.empty()) return;
    auto& g = building.back(); g.name = name; g.role = role; g.state = state;
}
void Surface::Tip(const std::wstring& tip) { if (!building.empty()) building.back().tip = tip; }
void Surface::Bounds(RectF whole) { if (!building.empty()) building.back().bounds = whole; }

void Surface::BeginScroll(RectF view) { scrollView = view; inScroll = true; }

void Surface::EndScroll(float total) {
    inScroll = false;
    scrollMax = std::max(0.f, total - scrollView.h);
    if (scrollY > scrollMax) { scrollY = scrollMax; Invalidate(); }
    if (scrollMax <= 0) { dragging = false; return; }
    // A thin line that thickens under the pointer, as Windows 11 draws it; it can be dragged,
    // and a click beside the thumb moves a page.
    UINT id = Id("scrollbar");
    thumbH = std::max(40.f, scrollView.h * scrollView.h / total);
    thumbY = scrollView.y + (scrollView.h - thumbH) * scrollY / scrollMax;
    float h = dragging ? 1.f : HotT(id), w = 3 + 3 * h;
    auto c = Mix(T.text3, T.text2, h); if (!T.contrast) c.a *= .7f + .3f * h;
    Fill({ scrollView.R() - 3 - w, thumbY + 2, w, thumbH - 4 }, c, w / 2);
    bool was = inScroll;
    Hit(id, { scrollView.R() - 12, scrollView.y, 12, scrollView.h }, nullptr, nullptr, false, false);
    inScroll = was;
}

void Surface::Toast(const std::wstring& text, const std::wstring& action, std::function<void()> act) {
    toast = text; toastAction = action; toastAct = std::move(act); toastAt = GetTickCount64();
    Test::Trace(L"toast|" + text + (action.empty() ? L"" : L"|" + action));
    Invalidate();
}

Surface* Surface::From(HWND h) {
    wchar_t cls[64] = {};
    if (!h || !GetClassNameW(h, cls, 64) || wcscmp(cls, L"CodexSwitcherSurface")) return nullptr;
    return (Surface*)GetWindowLongPtrW(h, GWLP_USERDATA);
}

void Surface::AcceptFiles() { if (hwnd) DragAcceptFiles(hwnd, TRUE); }

void Surface::Snap(UINT id, float value) { anims[id] = { value, value }; }

void Surface::DrawToast() {
    if (toast.empty()) return;
    // With something to press it stays long enough to be read and reached.
    bool acts = !toastAction.empty() && toastAct;
    const float life = acts ? 9000.f : 1900.f, in = 120, out = 220;
    float age = (float)(GetTickCount64() - toastAt);
    if (age >= life) { toast.clear(); toastAction.clear(); toastAct = nullptr; Invalidate(); return; }
    float o = !Motion() ? 1.f : std::min(1.f, std::min(age / in, (life - age) / out));
    auto f = Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto c = Client();
    float aw = acts ? Measure(toastAction, f).width + 24 : 0;
    float w = std::min(c.w - 32, Measure(toast, f).width + 44 + (acts ? aw + 10 : 0)), x = (c.w - w) / 2, y = c.h - 20 - 34;
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    PushOpacity(o);
    RectF r{ x, y, w, 34 };
    Fill(r, T.dark ? Rgba(0x3A3A3A) : Rgba(0x1F1F1F), 17);
    if (T.contrast) { Fill(r, T.bg, 17); Stroke(r, T.text, 17, 2); }
    auto ink = T.contrast ? T.text : Rgba(0xFFFFFF);
    noRuns = true;
    Text(Glyphs::Check, { r.x + 10, r.y, 20, r.h }, Font(12.5f, DWRITE_FONT_WEIGHT_NORMAL, false, true), ink, Align::Center);
    noRuns = false;
    Text(toast, { r.x + 32, r.y, r.w - 44 - (acts ? aw + 10 : 0), r.h }, f, ink);
    if (acts) {
        UINT id = Id("toast-action");
        RectF b{ r.R() - aw - 5, r.y + 5, aw, r.h - 10 };
        auto bg = ink; bg.a = .14f + .1f * HotT(id);
        Fill(b, bg, b.h / 2);
        if (T.contrast) Stroke(b, T.text, b.h / 2);
        noRuns = true; Text(toastAction, b, f, ink, Align::Center); noRuns = false;
        Hit(id, b, [this] { auto fn = toastAct; toast.clear(); toastAction.clear(); toastAct = nullptr; if (fn) fn(); Invalidate(); });
        Describe(toastAction);
        FocusRing(id, b, b.h / 2);
    }
    PopOpacity();
    Animate();
}

void Surface::FocusRing(UINT id, RectF r, float radius) {
    if (keyboardCues && focus == id && GetForegroundWindow() == hwnd) Stroke({ r.x - 3, r.y - 3, r.w + 6, r.h + 6 }, T.focus, radius + 3, T.contrast ? 3.f : 2.f);
}

void Surface::DashedStroke(RectF r, D2D1_COLOR_F c, float radius) {
    static ID2D1StrokeStyle* dash = nullptr;
    if (!dash) {
        float d[] = { 4, 3 };
        D2D->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM, 0), d, 2, &dash);
    }
    // The line's middle runs .6 inside the edge, so its radius is that much less: the outer
    // edge then has the radius asked for, the same as a fill of the same rectangle.
    auto i = r.Inset(.6f).D();
    rt->DrawRoundedRectangle(D2D1::RoundedRect(i, radius - .6f, radius - .6f), Brush(rt, c), 1.2f, dash);
}

void Surface::FieldLine(RectF box, D2D1_COLOR_F c, float radius) {
    rt->PushAxisAlignedClip(D2D1::RectF(box.x, box.B() - 2, box.R(), box.B()), D2D1_ANTIALIAS_MODE_ALIASED);
    Fill(box, c, radius);
    rt->PopAxisAlignedClip();
}

void Surface::PushOpacity(float o) {
    rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), o), nullptr);
}
void Surface::PopOpacity() { rt->PopLayer(); }
void Surface::Translate(float dx, float dy) { rt->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy)); }

void Surface::RefreshHover() {
    POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p);
    RECT rc; GetClientRect(hwnd, &rc);
    if (PtInRect(&rc, p)) { mx = p.x / scale; my = p.y / scale; } else mx = my = -1;
    auto hit = At(mx, my);
    hot = hit ? hit->id : 0; pressed = 0;
    Invalidate();
}

RectF Surface::Client() const { RECT rc; GetClientRect(hwnd, &rc); return { 0, 0, rc.right / scale, rc.bottom / scale }; }

// ============================================================ controls

bool Surface::Button(UINT id, RectF r, const std::wstring& label, bool primary, std::function<void()> click, bool danger, bool enabled) {
    float h = enabled ? HotT(id) : 0, p = enabled ? PressT(id) : 0;
    auto f = Font(14, primary ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
    D2D1_COLOR_F bg, fg;
    if (danger && T.contrast) { bg = T.accent; fg = T.onAccent; }
    else if (danger) { bg = Mix(Mix(Rgba(0xC42B1C), Rgba(0xD13A2B), h), Rgba(0xA82518), p); fg = Rgba(0xFFFFFF); }
    else if (primary) { bg = Mix(Mix(T.accent, T.accentHover, h), T.accentPress, p); fg = T.onAccent; }
    else { bg = Mix(Mix(T.control, T.controlHover, h), T.controlPress, p); fg = Mix(T.text, T.text2, p); }
    if (!enabled) { bg.a *= .45f; fg.a *= .45f; }
    Fill(r, bg, 4);
    if (!primary && !danger) Stroke(r, T.controlStroke, 4);
    Text(label, r.Inset(10, 0), f, fg, Align::Center);
    if (enabled) { Hit(id, r, click); Describe(label); FocusRing(id, r, 4); }
    return true;
}

void Surface::IconButton(UINT id, RectF r, const wchar_t* glyph, const std::wstring& tip, std::function<void()> click, float radius) {
    float h = HotT(id), p = PressT(id);
    Fill(r, Veil(h, p), radius);
    Glyph(glyph, r, 16, Mix(T.text2, T.text, h));
    Hit(id, r, click);
    Describe(tip); Tip(tip);
    FocusRing(id, r, radius);
}

void Surface::Toggle(UINT id, float x, float y, bool on, std::function<void()> click, bool enabled, const std::wstring& name) {
    RectF track{ x, y, 40, 20 };
    float t = A(id ^ 0x99, on ? 1.f : 0.f, 160), h = enabled ? HotT(id) : 0;
    auto off = Mix(T.control, T.controlHover, h), onc = Mix(T.accent, T.accentHover, h);
    auto bg = Mix(off, onc, t);
    if (!enabled) bg.a *= .45f;
    Fill(track, bg, 10);
    if (t < 1) { auto s = T.controlStrong; s.a *= (1 - t) * (enabled ? 1 : .45f); Stroke(track, s, 10); }
    float knob = 12 + 2 * h, cx = x + 10 + 20 * t;
    auto kc = Mix(T.text2, T.onAccent, t); if (!enabled) kc.a *= .45f;
    Circle(cx, y + 10, knob / 2, kc);
    if (enabled) { Hit(id, { x - 4, y - 6, 48, 32 }, click); Describe(name, Role::Toggle, on ? 1 : 0); FocusRing(id, track, 10); }
}

void Surface::Check(UINT id, float x, float y, bool on, D2D1_COLOR_F fill) {
    float t = A(id ^ 0x3C, on ? 1.f : 0.f, 100);
    RectF b{ x, y, 20, 20 };
    Fill(b, Mix(fill, T.accent, t), 4);
    if (t < 1) { auto s = T.controlStrong; s.a *= 1 - t; Stroke(b, s, 4); }
    if (t > 0) { auto c = T.onAccent; c.a = t; Glyph(Glyphs::Check, b, 12, c); }
}

void Surface::Avatar(float cx, float cy, float d, const std::wstring& letters, COLORREF color, bool active, D2D1_COLOR_F ringBg) {
    // Top-lit, like the badges on the shortcut icons.
    auto base = FromColorref(color);
    D2D1_GRADIENT_STOP stops[] = { { 0, Mix(base, Rgba(0xFFFFFF), .20f) }, { 1, Mix(base, Rgba(0x000000), .08f) } };
    ID2D1GradientStopCollection* coll = nullptr; ID2D1LinearGradientBrush* grad = nullptr;
    rt->CreateGradientStopCollection(stops, 2, &coll);
    if (coll) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, cy - d / 2 }, { 0, cy + d / 2 }), coll, &grad);
    if (grad) { rt->FillEllipse(D2D1::Ellipse({ cx, cy }, d / 2, d / 2), grad); grad->Release(); }
    else Circle(cx, cy, d / 2, base);
    if (coll) coll->Release();
    auto f = Font(d * (letters.size() > 1 ? .36f : .42f), DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
    Text(letters, { cx - d / 2, cy - d / 2, d, d }, f, Rgba(0xFFFFFF), Align::Center);
    if (active) {
        float r = d * .15f, ox = cx + d * .35f, oy = cy + d * .35f;
        Circle(ox, oy, r + 2.5f, ringBg);
        Circle(ox, oy, r, T.success);
    }
}

void Surface::Pill(RectF r, const std::wstring& text, D2D1_COLOR_F bg, D2D1_COLOR_F fg) {
    Fill(r, bg, r.h / 2);
    Text(text, r, Font(11, DWRITE_FONT_WEIGHT_SEMI_BOLD), fg, Align::Center);
}

void Surface::Spinner(float cx, float cy, float r) {
    // An arc that grows, shrinks and turns, like ProgressRing.
    LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    double t = (double)now.QuadPart / f.QuadPart;
    float spin = (float)fmod(t * 360.0, 360.0), sweep = 60 + 200 * (float)(0.5 + 0.5 * sin(t * 2.6));
    ID2D1PathGeometry* path = nullptr; D2D->CreatePathGeometry(&path);
    if (path) {
        ID2D1GeometrySink* sink = nullptr; path->Open(&sink);
        if (sink) {
            auto at = [&](float deg) { float a = (deg - 90) * 3.14159265f / 180; return D2D1::Point2F(cx + r * cosf(a), cy + r * sinf(a)); };
            sink->BeginFigure(at(spin), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(at(spin + sweep), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, sweep > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close(); sink->Release();
            static ID2D1StrokeStyle* round = nullptr;
            if (!round) D2D->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &round);
            rt->DrawGeometry(path, Brush(rt, T.accent), std::max(2.f, r * .22f), round);
        }
        path->Release();
    }
    Animate();
}

void Surface::Meter(RectF r, double left, bool stale) {
    Fill(r, T.badge, r.h / 2);
    if (T.contrast) Stroke(r.Inset(-1), T.text, r.h / 2 + 1);
    if (left < 0) return;
    // Amber softer than the theme's warning yellow, which glares as a bar on dark.
    auto amber = T.contrast ? T.text : T.dark ? Rgba(0xE8A33D) : Rgba(0xB86E00);
    auto c = left >= 40 ? T.success : left >= 15 ? amber : T.danger;
    if (stale) c.a *= .45f;   // an old reading: still useful, but not to be read as now
    float w = (float)(r.w * std::min(100.0, std::max(0.0, left)) / 100);
    if (w > 0) Fill({ r.x, r.y, std::max(w, r.h), r.h }, c, r.h / 2);
}

void Surface::Bar(RectF r, double pct, bool stale) {
    Fill(r, T.badge, r.h / 2);
    if (pct < 0) return;
    auto c = pct >= 90 ? T.danger : pct >= 70 ? T.warning : T.success;
    if (stale) c.a *= .45f;   // an old reading: still useful, but not to be read as now
    float w = (float)(r.w * std::min(100.0, std::max(0.0, pct)) / 100);
    if (w > 0) Fill({ r.x, r.y, std::max(w, r.h), r.h }, c, r.h / 2);
}

// ============================================================ text box

void Surface::ShowEdit(RectF r, const std::wstring& text, std::function<void(bool, const std::wstring&)> done) {
    if (edit) CloseEdit(true);
    editDone = std::move(done);
    if (!editFont) editFont = CreateFontW(-(int)std::lround(13 * scale), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");
    // A layered child, fully opaque: plain GDI text drawn on Mica would otherwise come out
    // see-through, and the box would look empty.
    edit = CreateWindowExW(WS_EX_LAYERED, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | ES_CENTER | ES_AUTOHSCROLL,
                           (int)(r.x * scale), (int)(r.y * scale), (int)(r.w * scale), (int)(r.h * scale), hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetLayeredWindowAttributes(edit, 0, 255, LWA_ALPHA);
    editSince = GetTickCount64();
    SendMessageW(edit, WM_SETFONT, (WPARAM)editFont, TRUE);
    SendMessageW(edit, EM_SETLIMITTEXT, 40, 0);
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(4, 4));
    SendMessageW(edit, EM_SETSEL, 0, -1);
    SetWindowSubclass(edit, EditProc, 1, (DWORD_PTR)this);
    SetFocus(edit);
}

void Surface::CloseEdit(bool commit) {
    if (!edit) return;
    int n = GetWindowTextLengthW(edit);
    std::wstring s(n, L'\0'); GetWindowTextW(edit, &s[0], n + 1);
    HWND e = edit; edit = nullptr;
    RemoveWindowSubclass(e, EditProc, 1);
    DestroyWindow(e);
    SetFocus(hwnd);
    auto fn = std::move(editDone); editDone = nullptr;
    if (fn) fn(commit, s);
    Invalidate();
}

LRESULT CALLBACK Surface::EditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    auto self = (Surface*)ref;
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) { PostMessageW(self->hwnd, WM_APP + 50, w == VK_RETURN, 0); return 0; }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;
    if (m == WM_KILLFOCUS) {
        // Clicking elsewhere finishes the edit. A focus change in the first moments is the
        // system settling focus after a menu, so the box takes focus back instead.
        if (GetTickCount64() - self->editSince < 400) { PostMessageW(self->hwnd, WM_APP + 52, 0, 0); return DefSubclassProc(h, m, w, l); }
        PostMessageW(self->hwnd, WM_APP + 50, 1, 0);
    }
    return DefSubclassProc(h, m, w, l);
}
