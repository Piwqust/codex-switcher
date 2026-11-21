#include "app.h"
#include <set>
#include <commctrl.h>

using namespace Gfx;

namespace {
    std::set<Surface*> open;

    // Runs a window modally: the owner is disabled until the dialog is done.
    struct Modal : Surface {
        std::function<void()> onFirstFrame;
        bool done = false;
        int result = 0;
        HWND owner = nullptr;
        bool ownerWasEnabled = false;

        void Run(const wchar_t* title, float w, HWND own) {
            owner = own && IsWindowVisible(own) ? own : nullptr;
            sizeToContent = true; backdrop = 2;
            Create(title, WS_POPUP | WS_CAPTION | WS_SYSMENU, owner ? 0 : WS_EX_TOPMOST, w, 200, owner);
            open.insert(this);
            RECT ref;
            if (owner) GetWindowRect(owner, &ref);
            else { POINT pt; GetCursorPos(&pt); MONITORINFO mi{ sizeof mi }; GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi); ref = mi.rcWork; }
            // A hidden window gets no WM_PAINT, so it is measured directly: before centring, and
            // again after the move in case that monitor has another scale.
            for (int pass = 0; pass < 2; pass++) {
                LayoutNow();
                RECT wr; GetWindowRect(hwnd, &wr);
                int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
                SetWindowPos(hwnd, nullptr, (ref.left + ref.right - ww) / 2, (ref.top + ref.bottom - wh) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
            // A dialog may open over another one with the same owner: only the first disables it,
            // and only that one gives it back.
            ownerWasEnabled = owner && IsWindowEnabled(owner);
            if (ownerWasEnabled) EnableWindow(owner, FALSE);
            Show(true);
            Test::Trace(std::wstring(L"dialog|") + title);
            if (onFirstFrame) { UpdateWindow(hwnd); onFirstFrame(); }
            // GetMessage takes WM_QUIT off the queue and returns 0, so it is posted again for
            // the main loop; otherwise quitting while a dialog is open would be lost.
            MSG m;
            while (!done) {
                BOOL got = GetMessageW(&m, nullptr, 0, 0);
                if (got == 0) { PostQuitMessage((int)m.wParam); break; }
                if (got < 0) break;
                TranslateMessage(&m); DispatchMessageW(&m);
            }
            if (ownerWasEnabled) { EnableWindow(owner, TRUE); if (!Offscreen()) SetForegroundWindow(owner); }
            open.erase(this);
            ShowWindow(hwnd, SW_HIDE);
            Test::Trace(Util::Format(L"dialog-end|%s|%d", title, result));
        }
        void End(int r) { result = r; done = true; }
        void OnClose() override { End(0); }
        // A footer button: v is its result. A dialog with fields checks them first.
        virtual void OnButton(int v) { End(v); }

        // ContentDialog layout: body on the window, buttons on a darker band at the bottom.
        float Footer(float y, const std::vector<std::pair<std::wstring, int>>& buttons, bool danger) {
            float w = Client().w;
            Fill({ 0, y, w, 80 }, T.layer);
            Line(0, y, w, y, T.layerStroke);
            float bx = w - 24;
            for (int i = (int)buttons.size() - 1; i >= 0; i--) {
                float bw = std::max(120.f, Measure(buttons[i].first, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD)).width + 32);
                bx -= bw;
                int v = buttons[i].second;
                Button(Id("dlg", i), { bx, y + 24, bw, 32 }, buttons[i].first, i == 0 && !danger, [this, v] { OnButton(v); }, i == 0 && danger);
                bx -= 8;
            }
            return 80;
        }
    };

    struct AskDialog : Modal {
        std::wstring title, text, yes, no; bool danger = false;
        void Build() override {
            float w = Client().w, y = 32;
            auto th = Measure(title, Font(20, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), w - 48, true).height;
            Text(title, { 24, y, w - 48, th }, Font(20, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), T.text, Align::Left, false, true);
            y += th + 10;
            if (!text.empty()) {
                auto h = Measure(text, Font(14), w - 48, true).height;
                Text(text, { 24, y, w - 48, h }, Font(14), T.text2, Align::Left, false, true);
                y += h;
            }
            y += 24;
            std::vector<std::pair<std::wstring, int>> b{ { yes, 1 } };
            if (!no.empty()) b.push_back({ no, 0 });
            y += Footer(y, b, danger);
            contentHeight = y;
            // Keyboard focus on the safe choice for destructive actions.
            if (!focus) focus = Id("dlg", danger && !no.empty() ? 1 : 0);
        }
        bool OnKey(UINT vk, bool) override {
            if (vk == VK_RETURN && !keyboardCues) { End(danger ? 0 : 1); return true; }
            return false;
        }
    };

    // Several answers: the first is the main one; result is its number + 1, 0 when closed.
    struct ChooseDialog : Modal {
        std::wstring title, text; std::vector<std::wstring> choices;
        void Build() override {
            float w = Client().w, y = 32;
            auto th = Measure(title, Font(20, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), w - 48, true).height;
            Text(title, { 24, y, w - 48, th }, Font(20, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), T.text, Align::Left, false, true);
            y += th + 10;
            if (!text.empty()) {
                auto h = Measure(text, Font(14), w - 48, true).height;
                Text(text, { 24, y, w - 48, h }, Font(14), T.text2, Align::Left, false, true);
                y += h;
            }
            y += 24;
            std::vector<std::pair<std::wstring, int>> b;
            for (size_t i = 0; i < choices.size(); i++) b.push_back({ choices[i], (int)i + 1 });
            y += Footer(y, b, false);
            contentHeight = y;
            if (!focus) focus = Id("dlg", 0);
        }
    };

    // A line of text to type: a password (hidden, optionally asked twice) or a pasted one.
    struct PromptDialog : Modal {
        std::wstring title, text, value, error, yes;
        bool password = false, confirm = false;
        HWND fields[2] = {};
        HFONT font = nullptr; float fontScale = 0;
        static std::wstring Read(HWND e) { int n = GetWindowTextLengthW(e); std::wstring s(n, L'\0'); if (n) GetWindowTextW(e, &s[0], n + 1); return s; }

        void Field(int i, RectF r) {
            Fill(r.Inset(-1), T.menu, 4);
            Stroke(r.Inset(-1), T.controlStroke, 4);
            FieldLine(r.Inset(-1), fields[i] && GetFocus() == fields[i] ? T.text : T.controlStrong, 4);
            RECT want{ (LONG)((r.x + 8) * scale), (LONG)((r.y + 7) * scale), (LONG)((r.R() - 8) * scale), (LONG)((r.B() - 6) * scale) };
            if (!font || fontScale != scale) {
                if (font) DeleteObject(font);
                font = CreateFontW(-(int)(14 * scale), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");
                fontScale = scale;
                for (auto f : fields) if (f) SendMessageW(f, WM_SETFONT, (WPARAM)font, TRUE);
            }
            if (!fields[i]) {
                // A layered child, fully opaque: plain GDI text on Mica would come out see-through.
                fields[i] = CreateWindowExW(WS_EX_LAYERED, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (password ? ES_PASSWORD : 0),
                                            want.left, want.top, want.right - want.left, want.bottom - want.top, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                SetLayeredWindowAttributes(fields[i], 0, 255, LWA_ALPHA);
                SendMessageW(fields[i], WM_SETFONT, (WPARAM)font, TRUE);
                SendMessageW(fields[i], EM_SETLIMITTEXT, 1u << 20, 0);
                if (i == 0) { SetWindowTextW(fields[0], value.c_str()); SetFocus(fields[0]); }
            } else {
                RECT now; GetWindowRect(fields[i], &now); MapWindowPoints(nullptr, hwnd, (POINT*)&now, 2);
                if (now.left != want.left || now.top != want.top || now.right != want.right || now.bottom != want.bottom)
                    MoveWindow(fields[i], want.left, want.top, want.right - want.left, want.bottom - want.top, TRUE);
            }
        }

        void Build() override {
            float w = Client().w, y = 32;
            auto th = Measure(title, Font(20, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), w - 48, true).height;
            Text(title, { 24, y, w - 48, th }, Font(20, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), T.text, Align::Left, false, true);
            y += th + 10;
            if (!text.empty()) {
                auto h = Measure(text, Font(14), w - 48, true).height;
                Text(text, { 24, y, w - 48, h }, Font(14), T.text2, Align::Left, false, true);
                y += h + 14;
            }
            Field(0, { 24, y, w - 48, 34 }); y += 44;
            if (confirm) {
                Text(Tr(L"Once more", L"Ещё раз"), { 24, y, w - 48, 18 }, Font(12.5f), T.text2); y += 22;
                Field(1, { 24, y, w - 48, 34 }); y += 44;
            }
            if (!error.empty()) {
                auto h = Measure(error, Font(12.5f), w - 48, true).height;
                Text(error, { 24, y, w - 48, h }, Font(12.5f), T.danger, Align::Left, false, true);
                y += h + 6;
            }
            y += 10;
            y += Footer(y, { { yes, 1 }, { Tr(L"Cancel", L"Отмена"), 0 } }, false);
            contentHeight = y;
        }

        bool Accept() {
            if (!value.empty()) SecureZeroMemory(&value[0], value.size() * sizeof(wchar_t));
            value = Read(fields[0]);
            if (value.empty()) { error = password ? Tr(L"Type the password", L"Введите пароль") : Tr(L"Paste the text first", L"Сначала вставьте текст"); Invalidate(); return false; }
            if (confirm && fields[1]) {
                auto again = Read(fields[1]);
                bool same = again == value;
                if (!again.empty()) SecureZeroMemory(&again[0], again.size() * sizeof(wchar_t));
                if (!same) { error = Tr(L"The two passwords don't match", L"Пароли не совпадают"); Invalidate(); return false; }
            }
            End(1);
            return true;
        }
        void OnButton(int v) override { if (v == 1) Accept(); else End(v); }

        LRESULT Extra(UINT m, WPARAM w, LPARAM, bool&) override {
            if (m == WM_COMMAND && (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS)) Invalidate();
            return 0;
        }

        bool OnKey(UINT vk, bool) override {
            if (vk == VK_RETURN) { Accept(); return true; }
            return false;
        }

        ~PromptDialog() override {
            for (auto f : fields) if (f) {
                int n = GetWindowTextLengthW(f); std::wstring wipe(n, L'*'); SetWindowTextW(f, wipe.c_str());
                DestroyWindow(f);
            }
            if (font) DeleteObject(font);
        }
    };

    // Enter, Esc and Tab inside an edit box go to its dialog instead of beeping.
    LRESULT CALLBACK EditKeys(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        auto d = (PromptDialog*)ref;
        if (m == WM_KEYDOWN && w == VK_RETURN) { d->Accept(); return 0; }
        if (m == WM_KEYDOWN && w == VK_ESCAPE) { d->End(0); return 0; }
        if (m == WM_KEYDOWN && w == VK_TAB) { SetFocus(h == d->fields[0] && d->fields[1] ? d->fields[1] : d->fields[0]); return 0; }
        if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB)) return 0;
        // A one-line box keeps only the first line of what is pasted; a backup text wrapped by
        // a chat or a mail comes in whole, without its line breaks.
        if (m == WM_PASTE && !d->password && IsClipboardFormatAvailable(CF_UNICODETEXT) && OpenClipboard(h)) {
            std::wstring text;
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) if (auto p = (const wchar_t*)GlobalLock(data)) { text = p; GlobalUnlock(data); }
            CloseClipboard();
            std::wstring joined;
            for (wchar_t c : text) if (c != L'\r' && c != L'\n' && c != L'\t') joined += c;
            SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)joined.c_str());
            return 0;
        }
        return DefSubclassProc(h, m, w, l);
    }
}

namespace Dialogs {
    bool Ask(HWND owner, const std::wstring& title, const std::wstring& text, const std::wstring& yes, bool danger, const std::wstring& no) {
        AskDialog d; d.title = title; d.text = text; d.yes = yes; d.no = no; d.danger = danger;
        d.Run(title.c_str(), 460, owner);
        return d.result == 1;
    }

    void Info(HWND owner, const std::wstring& title, const std::wstring& text) { Ask(owner, title, text, Tr(L"OK", L"Понятно"), false, L""); }

    int Choose(HWND owner, const std::wstring& title, const std::wstring& text, const std::vector<std::wstring>& choices) {
        ChooseDialog d; d.title = title; d.text = text; d.choices = choices;
        d.Run(title.c_str(), 520, owner);
        return d.result - 1;
    }

    bool Prompt(HWND owner, const std::wstring& title, const std::wstring& text, std::wstring& value, bool password, bool confirm, const std::wstring& yes) {
        PromptDialog d; d.title = title; d.text = text; d.value = value; d.password = password; d.confirm = confirm;
        d.yes = yes.empty() ? std::wstring(Tr(L"OK", L"Готово")) : yes;
        // The edit boxes appear with the first frame; their keys are hooked once they exist.
        d.onFirstFrame = [&d] { for (auto f : d.fields) if (f) SetWindowSubclass(f, EditKeys, 7, (DWORD_PTR)&d); if (d.fields[0]) SetFocus(d.fields[0]); };
        d.Run(title.c_str(), 480, owner);
        bool ok = d.result == 1;
        if (ok) value = d.value;
        if (!d.value.empty()) SecureZeroMemory(&d.value[0], d.value.size() * sizeof(wchar_t));
        return ok;
    }

    void RestyleAll() { for (auto s : open) s->Restyle(); }
}
