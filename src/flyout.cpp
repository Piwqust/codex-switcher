#include "app.h"
#include <shellapi.h>
#include <cmath>

using namespace Gfx;

namespace {
    const float W = 400, RowH = 56;

    struct FlyoutWin : Surface {
        ULONGLONG hiddenAt = 0;
        LARGE_INTEGER shownAt{};
        bool fromBelow = true;

        void Start() {
            sizeToContent = true;
            backdrop = 3;   // Acrylic, like the system's own tray flyouts
            Create(Tr(L"Account list", L"Список аккаунтов"), WS_POPUP, WS_EX_TOOLWINDOW | WS_EX_TOPMOST, W, 300, nullptr, true);
        }

        float Appear() {
            // Slides in 10 px and fades, over ~180 ms, as Windows 11 flyouts do.
            LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
            float t = (float)((now.QuadPart - shownAt.QuadPart) * 1000.0 / f.QuadPart) / 180.f;
            if (t >= 1 || !Motion()) return 1;
            Animate();
            return 1 - (1 - t) * (1 - t) * (1 - t);
        }

        // Two small meters of what is left, "5h ▓▓▓░ 82%" over "Wk ▓░░░ 18%", like the window's rows.
        void Limits(const Item& it, float x, float y) {
            auto& u = it.usage;
            if (it.apiKey || !u.known) return;
            struct L { const wchar_t* label; double left; };
            std::vector<L> lines;
            if (u.five >= 0) lines.push_back({ Tr(L"5h", L"5ч"), u.FiveLeft() });
            if (u.week >= 0) lines.push_back({ Tr(L"Wk", L"Нед"), u.WeekLeft() });
            bool stale = u.Stale() || !u.problem.empty();
            float ly = lines.size() > 1 ? y + 12 : y + RowH / 2 - 8;
            for (auto& l : lines) {
                Text(l.label, { x, ly, 22, 16 }, Font(11), T.text3);
                Meter({ x + 24, ly + 5, 48, 5 }, l.left, stale);
                Text(Util::Format(L"%d%%", (int)std::lround(l.left)), { x + 74, ly - 1, 38, 18 }, Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), l.left <= 0 ? T.danger : stale ? T.text3 : T.text, Align::Right);
                ly += 17;
            }
        }

        void Row(UINT id, float y, const std::wstring& name, std::function<void()> click, Role role = Role::Button, int state = -1) {
            RectF r{ 4, y, W - 8, RowH };
            float h = HotT(id), p = PressT(id);
            Fill(r, Veil(h, p), 4);
            Hit(id, r, click);
            Describe(name, role, state);
            FocusRing(id, r, 4);
        }

        void Build() override {
            float t = Appear();
            PushOpacity(t);
            Translate(0, (fromBelow ? 10 : -10) * (1 - t));
            Stroke(Client(), T.menuStroke, 8);

            // A new version shows as a button next to the title, so the update is one click from the tray.
            float titleW = 180;
            auto up = Update::State();
            if (up == Update::Phase::Ready || up == Update::Phase::Downloading || up == Update::Phase::Installing) {
                bool busy = up != Update::Phase::Ready;
                auto label = busy ? std::wstring(Tr(L"Updating…", L"Обновляю…")) : std::wstring(Tr(L"Update", L"Обновить"));
                float bw = Measure(label, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD)).width + 28, bx = W - 124 - bw;
                Button(Id("fo-update"), { bx, 12, bw, 28 }, label, true, [] { Update::Install(); }, false, !busy);
                titleW = bx - 24;
            }
            Text(AppTitle(), { 16, 10, titleW, 32 }, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text);
            if (App::FetchingUsage()) Spinner(W - 100, 26, 7);
            else IconButton(Id("fo-refresh"), { W - 116, 10, 32, 32 }, Glyphs::Refresh, Tr(L"Refresh limits (F5)", L"Обновить лимиты (F5)"), [] { App::RefreshUsage(true); });
            IconButton(Id("fo-open"), { W - 80, 10, 32, 32 }, Glyphs::Open, Tr(L"Open the window with the list", L"Открыть окно со списком"), [this] { Hide(); App::ShowPicker(); });
            IconButton(Id("fo-settings"), { W - 44, 10, 32, 32 }, Glyphs::Settings, Tr(L"Settings", L"Настройки"), [this] { Hide(); App::ShowSettings(); });

            float y = 48;
            for (auto& it : App::items) {
                UINT id = Id("fo-row", it.index);
                auto pid = it.id;
                Row(id, y, it.Spoken(App::MostLeft() == &it && !it.switching), [this, pid] { Hide(); App::Switch(pid, false); }, Role::Row, it.active ? 1 : 0);
                float limitsX = W - 44 - 116, tw = limitsX - 56 - 14;
                Avatar(30, y + RowH / 2, 32, it.letters, it.color, it.active, T.dark ? Rgba(0x2C2C2C) : Rgba(0xF3F3F3));
                if (it.switching || it.warming || it.deleting) Spinner(30, y + RowH / 2, 19);
                Text(it.shownName, { 56, y + 9, tw, 20 }, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text);
                auto status = App::MostLeft() == &it && !it.switching ? std::wstring(Tr(L"Most limit left", L"Больше всего лимита")) : it.Status();
                bool good = App::MostLeft() == &it && !it.switching;
                Text(status, { 56, y + 30, tw, 18 }, Font(12.5f), it.switching || it.warming || it.deleting ? T.text : it.SignedOut() ? T.danger : it.active || good ? T.success : T.text2);
                Limits(it, limitsX, y);
                if (it.index <= 9) {
                    RectF b{ W - 38, y + RowH / 2 - 10, 20, 20 };   // centred under the settings button above
                    Fill(b, T.badge, 4);
                    Text(std::to_wstring(it.index), b, Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text2, Align::Center);
                }
                y += RowH + 2;
            }
            if (!App::items.empty()) { Line(0, y + 4, W, y + 4, T.menuStroke); y += 9; }
            Row(Id("fo-add"), y, Tr(L"Add account", L"Добавить аккаунт"), [this] { Hide(); App::AddAccount(); });
            Glyph(Glyphs::Add, { 14, y, 32, RowH }, 14, T.text2);
            Text(Tr(L"Add account", L"Добавить аккаунт"), { 56, y, 220, RowH }, Font(14), T.text);
            y += RowH;
            Row(Id("fo-exit"), y, Tr(L"Quit switcher", L"Закрыть переключатель"), [this] { Hide(); App::Exit(); });
            Glyph(Glyphs::Power, { 14, y, 32, RowH }, 14, T.text2);
            Text(Tr(L"Quit switcher", L"Закрыть переключатель"), { 56, y, 240, RowH }, Font(14), T.text);
            y += RowH + 6;
            contentHeight = y;
            PopOpacity();
            Translate(0, 0);
        }

        bool OnKey(UINT vk, bool) override {
            if (vk == VK_ESCAPE) { Hide(); return true; }
            if (vk == VK_F5) { App::RefreshUsage(true); return true; }
            int d = vk >= '1' && vk <= '9' ? vk - '0' : vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9 ? vk - VK_NUMPAD0 : 0;
            for (auto& i : App::items) if (d && i.index == d) { auto id = i.id; Hide(); App::Switch(id, false); return true; }
            return false;
        }

        void OnDeactivate() override {
            if (GetEnvironmentVariableW(L"CAS_FLYOUT_STAY", nullptr, 0)) return;   // for screenshots only
            if (Visible()) { Hide(); hiddenAt = GetTickCount64(); }
        }
        void OnClose() override { Hide(); }
        // Grown or shrunk while open (an account added, the scale changed): stay against the taskbar.
        void OnResized() override { if (Visible()) Place(); }

        void Place() {
            if (Offscreen()) return;
            NOTIFYICONIDENTIFIER nid{ sizeof nid }; nid.hWnd = App::tray; nid.uID = 1;
            RECT icon{}; POINT pt;
            if (SUCCEEDED(Shell_NotifyIconGetRect(&nid, &icon))) pt = { (icon.left + icon.right) / 2, (icon.top + icon.bottom) / 2 };
            else GetCursorPos(&pt);
            MONITORINFO mi{ sizeof mi }; GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
            RECT wr; GetWindowRect(hwnd, &wr);
            int w = wr.right - wr.left, h = wr.bottom - wr.top, gap = (int)(12 * scale);
            int x = std::max((int)mi.rcWork.left + gap, std::min((int)pt.x - w / 2, (int)mi.rcWork.right - w - gap));
            fromBelow = pt.y > (mi.rcWork.top + mi.rcWork.bottom) / 2;
            int y = fromBelow ? mi.rcWork.bottom - h - gap : mi.rcWork.top + gap;
            SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }

        void Open() {
            // Measure first so the placement knows the height, then show. Placing can move the
            // window to a monitor with another scale: measure again there, and place by that size.
            LayoutNow();
            Place();
            LayoutNow();
            Place();
            QueryPerformanceCounter(&shownAt);
            Show(true);
            Place();
            App::Refresh();
            App::RefreshUsage(false);
        }
    };

    FlyoutWin* win = nullptr;
    FlyoutWin& Get() { if (!win) { win = new FlyoutWin(); win->Start(); } return *win; }
}

namespace Flyout {
    void Toggle() {
        auto& w = Get();
        if (w.Visible()) { w.Hide(); return; }
        // Clicking the tray icon while the flyout is open deactivates it first; without
        // this, that click would reopen it straight away.
        if (GetTickCount64() - w.hiddenAt < 300) return;
        w.Open();
    }
    void Hide() { if (win) win->Hide(); }
    bool Visible() { return win && win->Visible(); }
    void Restyle() { if (win) win->Restyle(); }
    void Invalidate() { if (win) win->Invalidate(); }
}
