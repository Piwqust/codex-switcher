// The account manager: every account with its sign-in details, where each one stands (new,
// being secured, signed in here, its paid month over), and the steps that take an account
// bought in a shop from the shop's message to ready to use:
//
//   paste the order → open the site in the account's own browser profile → change the
//   password → change the 2FA key → confirm the phone (by hand) → sign in to the switcher.
//
// The switcher types the email, the password and the 2FA code into the field under the cursor
// when Ctrl+Alt+V is pressed, the way KeePass does, so nothing has to be copied by hand. The
// sites themselves are used by the person: nothing here clicks through them.
#include "app.h"
#include "codex.h"
#include "logins.h"
#include "net.h"
#include <commctrl.h>
#include <algorithm>
#include <climits>
#include <set>

using namespace Gfx;

namespace {
    const float Pad = 24, RowH = 64, StepH = 52, ChipsH = 46;
    enum : UINT_PTR { TimerTick = 0x51, TimerType = 0x52, TimerClear = 0x53 };
    const int HotkeyId = 0x4A01;
    const long long Hour = 3600000ll, Day = 24 * Hour;

    enum class Field { Email, Password, NewPassword, Code, NewCode };
    struct Typing { Field field; bool enter; };

    // The saved details, read again whenever the file changes.
    std::vector<Cred> creds;
    ULONGLONG credsStamp = ~0ull;
    void LoadCreds() {
        auto stamp = Util::FileTime(Creds::File());
        if (stamp == credsStamp) return;
        creds = Creds::Load(); credsStamp = stamp;
    }
    bool SaveCreds(HWND owner) {
        try { Creds::Save(creds); credsStamp = Util::FileTime(Creds::File()); return true; }
        catch (const std::wstring& e) { Dialogs::Info(owner, Tr(L"Couldn't save", L"Не удалось сохранить"), e); credsStamp = ~0ull; return false; }
    }
    Cred* CredById(const std::wstring& id) { for (auto& c : creds) if (c.id == id) return &c; return nullptr; }
    // A card with nothing saved on it: the window shows it like an account that has no card at all.
    bool Bare(const Cred& c) { return c.id.empty() || (c.password.empty() && c.key.empty() && c.order.empty() && c.newPassword.empty() && c.newKey.empty()); }

    Item* AccountOf(const Cred& c) {
        if (!c.accountId.empty()) if (auto it = App::Find(c.accountId)) return it;
        for (auto& it : App::items) if (!c.email.empty() && _wcsicmp(it.email.c_str(), c.email.c_str()) == 0) return &it;
        return nullptr;
    }

    std::wstring Dur(long long ms) {
        if (ms < 0) ms = 0;
        long long m = ms / 60000, h = m / 60, d = h / 24;
        if (d >= 2) return Util::Format(Tr(L"%lld days", L"%lld дн"), d);
        if (h >= 10) return Util::Format(Tr(L"%lld h", L"%lld ч"), h);
        if (h >= 1) return m % 60 ? Util::Format(Tr(L"%lld h %lld min", L"%lld ч %lld мин"), h, m % 60) : Util::Format(Tr(L"%lld h", L"%lld ч"), h);
        return Util::Format(Tr(L"%lld min", L"%lld мин"), std::max(1ll, m));
    }

    std::wstring DateOf(long long ms) {
        static const wchar_t* en[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
        static const wchar_t* ru[] = { L"янв", L"фев", L"мар", L"апр", L"мая", L"июн", L"июл", L"авг", L"сен", L"окт", L"ноя", L"дек" };
        ULONGLONG ft = (ULONGLONG)ms * 10000 + 116444736000000000ull;
        FILETIME f{ (DWORD)ft, (DWORD)(ft >> 32) }; SYSTEMTIME u, t; FileTimeToSystemTime(&f, &u); SystemTimeToTzSpecificLocalTime(nullptr, &u, &t);
        return L10n::ru ? Util::Format(L"%d %s", t.wDay, ru[t.wMonth - 1]) : Util::Format(L"%s %d", en[t.wMonth - 1], t.wDay);
    }

    std::wstring Shown(const std::wstring& email, const Item* it) {
        if (it) return it->shownEmail.empty() ? it->email : it->shownEmail;
        return App::MaskAll() ? App::Masked(email) : email;
    }

    // One line of the table: saved details, an account of the switcher, or both.
    struct Row {
        Cred cred;                 // no id: an account whose details aren't saved here
        Item* item = nullptr;      // valid while one frame is drawn
        Creds::Stage stage = Creds::Stage::New;
        long long paidUntil = 0;
        bool action = false, expiring = false;
        std::wstring Key() const { return cred.id.empty() ? L"acct:" + (item ? item->id : std::wstring()) : cred.id; }
    };

    enum Filter { All, Action, Ready, Expiring, Expired, FilterCount };

    struct ManagerWin : Surface {
        int filter = All;
        std::wstring expanded, scrollTo;
        std::vector<std::wstring> scratch;     // keys pasted for a code, this session only
        // What Ctrl+Alt+V types: for which card, and the fields in order.
        std::wstring armed; std::vector<Typing> queue; size_t next = 0;
        bool hotkeyOn = false, hotkeyFailed = false;
        ULONGLONG typeStart = 0;
        int pending = 0;                      // presses not typed yet: each one types the next field
        DWORD clipSeq = 0;                     // the clipboard as left by copying a secret
        bool placed = false;
        // Signing in to several cards one after another: the ones still to come, and how many there were.
        std::vector<std::wstring> lineup; int lineupTotal = 0;
        // The search box: a real text box over the drawing, so pasting and the caret work as everywhere.
        HWND search = nullptr; HFONT searchFont = nullptr; float searchFontScale = 0;
        std::wstring query;
        int cueRu = -1;
        bool searchOpen = false;

        void Start() {
            backdrop = 2;
            Create(Title(), WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, 0, 1000, 700);
            AcceptFiles();
        }
        void OnDrop(const std::vector<std::wstring>& files) override { App::AddFiles(files, hwnd); }
        static const wchar_t* Title() { return Tr(L"Account manager — Codex Accounts", L"Управление аккаунтами — Аккаунты Codex"); }

        // ------------------------------------------------------------ rows

        std::vector<Row> Rows() {
            LoadCreds();
            long long now = Core::NowMs();
            std::vector<Row> rows;
            std::set<std::wstring> linked;
            for (auto& c : creds) {
                Row r; r.cred = c; r.item = AccountOf(c);
                if (!r.item && Bare(c)) continue;      // nothing saved and no account: nothing to show
                if (r.item) linked.insert(r.item->id);
                rows.push_back(r);
            }
            for (auto& it : App::items) if (!linked.count(it.id) && !it.apiKey) { Row r; r.cred.email = it.email; r.cred.accountId = it.id; r.item = &it; rows.push_back(r); }
            for (auto& r : rows) {
                r.paidUntil = r.item && r.item->subscriptionUntil ? r.item->subscriptionUntil : r.cred.paidUntil;
                r.stage = Creds::StageOf(r.cred, r.item != nullptr, r.paidUntil, now);
                r.expiring = r.stage != Creds::Stage::Expired && r.paidUntil && r.paidUntil - now < 3 * Day;
                r.action = (!Bare(r.cred) && (r.stage == Creds::Stage::New || r.stage == Creds::Stage::Securing || r.stage == Creds::Stage::SignIn)) ||
                           (r.item && r.item->SignedOut() && r.stage != Creds::Stage::Expired);
            }
            // What needs doing first, then the soonest to run out; the ones over at the end.
            auto rank = [](const Row& r) { return r.stage == Creds::Stage::Expired ? 2 : r.action ? 0 : 1; };
            std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
                if (rank(a) != rank(b)) return rank(a) < rank(b);
                if (rank(a) == 0) return a.cred.received > b.cred.received;
                long long pa = a.paidUntil ? a.paidUntil : LLONG_MAX, pb = b.paidUntil ? b.paidUntil : LLONG_MAX;
                return pa < pb;
            });
            return rows;
        }

        // The search looks in what tells one bought account from another: the address, the
        // order it came in, the note, and the name it has on the list.
        bool Matches(const Row& r) const {
            auto q = Util::Lower(Util::Trim(query));
            if (q.empty()) return true;
            auto has = [&](const std::wstring& s) { return Util::Lower(s).find(q) != std::wstring::npos; };
            return has(r.cred.email) || has(r.cred.order) || has(r.cred.note) || (r.item && (has(r.item->name) || has(r.item->email)));
        }

        static LRESULT CALLBACK SearchKeys(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
            auto self = (ManagerWin*)ref;
            if (m == WM_KEYDOWN && w == VK_ESCAPE) {
                // Esc clears what is typed; with nothing typed it leaves the box.
                if (GetWindowTextLengthW(h)) SetWindowTextW(h, L""); else SetFocus(self->hwnd);
                return 0;
            }
            if (m == WM_KEYDOWN && (w == VK_TAB || w == VK_RETURN || w == VK_DOWN)) { SetFocus(self->hwnd); PostMessageW(self->hwnd, WM_KEYDOWN, VK_TAB, 0); return 0; }
            if (m == WM_CHAR && (w == VK_ESCAPE || w == VK_TAB || w == VK_RETURN)) return 0;
            return DefSubclassProc(h, m, w, l);
        }

        // The text box exists only while something is typed or about to be: at rest the field
        // is drawn like every other control, so Tab reaches it and it reads "Search".
        void OpenSearch() {
            if (!search) {
                // A layered child, fully opaque: plain GDI text on Mica would come out see-through.
                search = CreateWindowExW(WS_EX_LAYERED, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                SetLayeredWindowAttributes(search, 0, 255, LWA_ALPHA);
                SendMessageW(search, EM_SETLIMITTEXT, 120, 0);
                SetWindowSubclass(search, SearchKeys, 3, (DWORD_PTR)this);
                searchFontScale = 0;
            }
            searchOpen = true;
            LayoutNow();                           // the box takes its place before it takes the keyboard
            SetFocus(search);
            SendMessageW(search, EM_SETSEL, 0, -1);
            Invalidate();
        }

        void SearchField(RectF r) {
            auto hint = Tr(L"Search: email, order, note", L"Поиск: почта, заказ, заметка");
            bool open = search && (searchOpen || !query.empty());
            if (r.w < 150) { if (search) ShowWindow(search, SW_HIDE); return; }
            UINT id = Id("search");
            bool typing = open && GetFocus() == search;
            Fill(r, Mix(T.menu, T.controlHover, open ? 0 : HotT(id) * .5f), 4);
            Stroke(r, T.controlStroke, 4);
            FieldLine(r, typing ? T.text : T.controlStrong, 4);
            Glyph(Glyphs::Search, { r.x + 4, r.y, 28, r.h }, 14, T.text3);
            if (!open) {
                if (search) ShowWindow(search, SW_HIDE);
                Text(hint, { r.x + 34, r.y, r.w - 42, r.h }, Font(14), T.text3);
                Hit(id, r, [this] { OpenSearch(); });
                Describe(hint);
                FocusRing(id, r, 4);
                return;
            }
            float clearW = query.empty() ? 0.f : 30.f;
            RECT want{ (LONG)((r.x + 32) * scale), (LONG)((r.y + 6) * scale), (LONG)((r.R() - 8 - clearW) * scale), (LONG)((r.B() - 6) * scale) };
            if (!searchFont || searchFontScale != scale) {
                if (searchFont) DeleteObject(searchFont);
                searchFont = CreateFontW(-(int)(14 * scale), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");
                searchFontScale = scale;
                SendMessageW(search, WM_SETFONT, (WPARAM)searchFont, TRUE);
            }
            RECT now; GetWindowRect(search, &now); MapWindowPoints(nullptr, hwnd, (POINT*)&now, 2);
            if (now.left != want.left || now.top != want.top || now.right != want.right || now.bottom != want.bottom)
                MoveWindow(search, want.left, want.top, want.right - want.left, want.bottom - want.top, TRUE);
            if (!IsWindowVisible(search)) ShowWindow(search, SW_SHOWNA);
            // The same words inside the empty box, in the window's language.
            if (cueRu != (L10n::ru ? 1 : 0)) { cueRu = L10n::ru ? 1 : 0; SendMessageW(search, EM_SETCUEBANNER, TRUE, (LPARAM)hint); }
            if (clearW > 0) IconButton(Id("search-clear"), { r.R() - 32, r.y + 2, 28, 28 }, Glyphs::Clear, Tr(L"Clear the search", L"Очистить поиск"), [this] { SetWindowTextW(search, L""); searchOpen = false; Invalidate(); }, 2);   // 2 inside the field's 4
        }

        void OnMouseDown() override { if (search && GetFocus() == search) SetFocus(hwnd); }

        static bool In(const Row& r, int f) {
            switch (f) {
            case Action: return r.action;
            case Ready: return r.stage == Creds::Stage::Ready && !r.action && !r.expiring;
            case Expiring: return r.expiring;
            case Expired: return r.stage == Creds::Stage::Expired;
            default: return true;
            }
        }

        // The card's saved details, saved first if the card is an account with none yet.
        Cred* Ensure(const std::wstring& key) {
            if (key.rfind(L"acct:", 0) != 0) return CredById(key);
            auto it = App::Find(key.substr(5));
            if (!it) return nullptr;
            Cred c; c.id = Net::Hex(Net::Random(6)); c.email = it->email; c.accountId = it->id; c.received = Core::NowMs();
            creds.push_back(c);
            if (expanded == key) expanded = c.id;
            if (armed == key) armed = c.id;
            return &creds.back();
        }

        // ------------------------------------------------------------ actions

        void Copy(const std::wstring& text, bool secret, const std::wstring& said) {
            if (text.empty()) return;
            if (!Util::CopyText(hwnd, text)) { Toast(Tr(L"The clipboard is busy, try again", L"Буфер обмена занят, попробуйте ещё раз")); return; }
            // Said in the window: a silent copy leaves one wondering whether the click landed.
            Toast(secret ? said + Tr(L" · leaves the clipboard in 45 s", L" · уйдёт из буфера через 45 с") : said);
            // A password or a code doesn't stay on the clipboard: cleared in 45 s unless something else was copied since.
            if (secret && !Test::On()) { clipSeq = GetClipboardSequenceNumber(); SetTimer(hwnd, TimerClear, 45000, nullptr); }
        }

        void Arm(const std::wstring& key, std::vector<Typing> q) {
            armed = key; queue = std::move(q); next = 0;
            std::wstring names;
            for (auto& t : queue) names += L"," + FieldName(t.field);
            Test::Trace(L"arm|" + key + L"|" + names.substr(1));
            if (!hotkeyOn && !Paths::Isolated) {
                hotkeyOn = RegisterHotKey(hwnd, HotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'V') != FALSE;
                hotkeyFailed = !hotkeyOn;
            }
            Invalidate();
        }
        void Disarm() {
            armed.clear(); queue.clear(); next = 0;
            if (hotkeyOn) { UnregisterHotKey(hwnd, HotkeyId); hotkeyOn = false; }
        }
        static std::vector<Typing> LoginQueue() { return { { Field::Email, true }, { Field::Password, true }, { Field::Code, true } }; }

        static std::wstring FieldName(Field f) {
            switch (f) {
            case Field::Email: return L"email";
            case Field::Password: return L"password";
            case Field::NewPassword: return L"new-password";
            case Field::Code: return L"code";
            default: return L"new-code";
            }
        }
        static std::wstring FieldLabel(Field f) {
            switch (f) {
            case Field::Email: return L"Email";
            case Field::Password: return Tr(L"Password", L"Пароль");
            case Field::NewPassword: return Tr(L"New password", L"Новый пароль");
            case Field::Code: return Tr(L"2FA code", L"Код 2FA");
            default: return Tr(L"New 2FA code", L"Новый код 2FA");
            }
        }
        static std::wstring Value(const Cred& c, Field f) {
            long long now = Core::NowMs() / 1000;
            switch (f) {
            case Field::Email: return c.email;
            case Field::Password: return c.password;
            case Field::NewPassword: return c.newPassword;
            case Field::Code: return Totp::Code(c.key, now);
            default: return Totp::Code(c.newKey, now);
            }
        }

        static void SendText(const std::wstring& s, bool enter) {
            std::vector<INPUT> in;
            for (wchar_t ch : s) {
                INPUT d{}; d.type = INPUT_KEYBOARD; d.ki.wScan = ch; d.ki.dwFlags = KEYEVENTF_UNICODE; in.push_back(d);
                d.ki.dwFlags |= KEYEVENTF_KEYUP; in.push_back(d);
            }
            if (enter) { INPUT d{}; d.type = INPUT_KEYBOARD; d.ki.wVk = VK_RETURN; in.push_back(d); d.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(d); }
            SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
            SecureZeroMemory(in.data(), in.size() * sizeof(INPUT));
        }

        void TypeNext() {
            auto c = CredById(armed);
            if (!c || queue.empty()) return;
            if (next >= queue.size()) next = 0;
            auto t = queue[next];
            auto v = Value(*c, t.field);
            if (v.empty()) { App::Notify(Tr(L"Nothing to type", L"Нечего вводить"), FieldLabel(t.field) + Tr(L" isn't saved for this account", L": для этого аккаунта не сохранено"), true); return; }
            if (Test::On()) Test::Trace(L"type|" + FieldName(t.field) + (t.enter ? L"|enter" : L""));
            else SendText(v, t.enter);
            SecureZeroMemory(&v[0], v.size() * sizeof(wchar_t));
            next++;
            Invalidate();
        }

        void OpenSite(const std::wstring& key) {
            auto c = Ensure(key);
            if (!c) return;
            auto id = c->id;
            if (!SaveCreds(hwnd)) return;
            expanded = id;
            Arm(id, LoginQueue());
            Browser::Open(id, L"https://chatgpt.com/");
        }

        void SignIn(const std::wstring& key) {
            auto c = Ensure(key);
            if (!c) return;
            auto id = c->id;
            if (!SaveCreds(hwnd)) return;
            expanded = id;
            Arm(id, LoginQueue());
            App::SignInWith(id);
        }

        // The cards not signed in to the switcher yet, in the order shown.
        std::vector<std::wstring> Unsigned() {
            std::vector<std::wstring> out;
            for (auto& r : Rows()) if (!Bare(r.cred) && !r.item && r.stage != Creds::Stage::Expired) out.push_back(r.cred.id);
            return out;
        }
        // One sign-in window at a time: the next opens when the one before has come back.
        // The typing and the clicking on the site stay with the person.
        void NextInLine() {
            if (lineup.empty()) { lineupTotal = 0; Invalidate(); return; }
            auto id = lineup.front(); lineup.erase(lineup.begin());
            Test::Trace(L"lineup|" + id);
            scrollTo = id;
            SignIn(id);
            if (!App::SigningIn()) { lineup.clear(); lineupTotal = 0; }
            Invalidate();
        }
        void StartLineup() { lineup = Unsigned(); lineupTotal = (int)lineup.size(); filter = All; NextInLine(); }
        void StopLineup() { lineup.clear(); lineupTotal = 0; if (App::SigningIn()) App::CancelSignIn(); Invalidate(); }

        void MakePassword(const std::wstring& id) {
            auto c = CredById(id); if (!c) return;
            c->newPassword = Creds::NewPassword();
            if (!SaveCreds(hwnd)) return;
            Copy(c->newPassword, true, Tr(L"New password copied", L"Новый пароль скопирован"));
            Arm(id, { { Field::Password, false }, { Field::NewPassword, false }, { Field::NewPassword, false } });
        }

        void PasswordDone(const std::wstring& id, bool done) {
            auto c = CredById(id); if (!c) return;
            if (done && !c->newPassword.empty()) { c->oldPassword = c->password; c->password = c->newPassword; c->newPassword.clear(); }
            c->passwordDone = done;
            SaveCreds(hwnd);
            if (armed == id) Arm(id, LoginQueue());
        }

        void PasteKey(const std::wstring& id) {
            auto c = CredById(id); if (!c) return;
            std::wstring value;
            for (;;) {
                if (!Dialogs::Prompt(hwnd, Tr(L"The new 2FA key", L"Новый ключ 2FA"),
                                     Tr(L"On the site: Settings → Security → turn 2FA off and on again with an authenticator app. Under the QR code, choose to enter the key by hand, copy it and paste it here",
                                        L"На сайте: «Настройки → Безопасность» — выключите 2FA и включите снова через приложение-аутентификатор. Под QR-кодом выберите ввод ключа вручную, скопируйте его и вставьте сюда"),
                                     value, false, false, Tr(L"Show the code", L"Показать код"))) return;
                auto key = Totp::Clean(value);
                if (!key.empty()) { c = CredById(id); if (!c) return; c->newKey = key; break; }
                Dialogs::Info(hwnd, Tr(L"That isn't a 2FA key", L"Это не ключ 2FA"), Tr(L"A key is 16 or more letters A–Z and digits 2–7, like JBSW Y3DP EHPK 3PXP, or an otpauth:// link", L"Ключ — это 16 и больше латинских букв и цифр 2–7, например JBSW Y3DP EHPK 3PXP, или ссылка otpauth://"));
            }
            if (!SaveCreds(hwnd)) return;
            Copy(Totp::Code(c->newKey, Core::NowMs() / 1000), true, Tr(L"Code copied", L"Код скопирован"));
            Arm(id, { { Field::NewCode, false } });
        }

        void KeyDone(const std::wstring& id, bool done) {
            auto c = CredById(id); if (!c) return;
            if (done && !c->newKey.empty()) { c->oldKey = c->key; c->key = c->newKey; c->newKey.clear(); }
            c->keyDone = done;
            SaveCreds(hwnd);
            if (armed == id) Arm(id, LoginQueue());
        }

        void SetText(const std::wstring& key, int what) {
            auto c = Ensure(key); if (!c) return;
            auto id = c->id;
            std::wstring value = what == 2 ? c->note : L"";
            const wchar_t* title = what == 0 ? Tr(L"Password", L"Пароль") : what == 1 ? Tr(L"2FA key", L"Ключ 2FA") : Tr(L"Note", L"Заметка");
            const wchar_t* text = what == 0 ? Tr(L"The account's current password, to keep it here", L"Текущий пароль аккаунта, чтобы он хранился здесь")
                                : what == 1 ? Tr(L"The key from the site's 2FA setup, or an otpauth:// link", L"Ключ из настройки 2FA на сайте или ссылка otpauth://")
                                : Tr(L"Anything to remember about this account", L"Всё, что стоит помнить об этом аккаунте");
            for (;;) {
                if (!Dialogs::Prompt(hwnd, title + std::wstring(L" — ") + c->email, text, value, false, false, Tr(L"Save", L"Сохранить"))) { SaveCreds(hwnd); return; }
                c = CredById(id); if (!c) return;
                if (what == 0) { if (Util::Trim(value).empty()) continue; if (!c->password.empty() && c->password != value) { c->oldPassword = c->password; c->passwordDone = true; } c->password = value; break; }
                if (what == 2) { c->note = Util::Trim(value); break; }
                auto k = Totp::Clean(value);
                if (!k.empty()) { if (!c->key.empty() && c->key != k) { c->oldKey = c->key; c->keyDone = true; } c->key = k; break; }
                Dialogs::Info(hwnd, Tr(L"That isn't a 2FA key", L"Это не ключ 2FA"), Tr(L"A key is 16 or more letters A–Z and digits 2–7, or an otpauth:// link", L"Ключ — это 16 и больше латинских букв и цифр 2–7 или ссылка otpauth://"));
            }
            SaveCreds(hwnd);
            Invalidate();
        }

        void Remove(const std::wstring& key, bool ask) {
            auto rows = Rows();
            const Row* row = nullptr;
            for (auto& r : rows) if (r.Key() == key) { row = &r; break; }
            if (!row) return;
            auto email = row->cred.email;
            auto itemId = row->item ? row->item->id : std::wstring();
            bool saved = !row->cred.id.empty(), details = !Bare(row->cred), all = true;
            if (ask) {
                if (details && !itemId.empty()) {
                    int c = Dialogs::Choose(hwnd, Util::Format(Tr(L"Delete %s?", L"Удалить %s?"), email.c_str()),
                                            Tr(L"Its password, 2FA key and browser profile are removed from this PC. The account can stay in the switcher's list, or go too",
                                               L"Пароль, ключ 2FA и профиль браузера удалятся с этого компьютера. Сам аккаунт можно оставить в списке переключателя или убрать тоже"),
                                            { Tr(L"Delete everything", L"Удалить всё"), Tr(L"Only the details", L"Только данные входа") });
                    if (c < 0) return;
                    all = c == 0;
                } else if (!Dialogs::Ask(hwnd, Util::Format(Tr(L"Delete %s?", L"Удалить %s?"), email.c_str()),
                                         details ? Tr(L"Its password, 2FA key and browser profile are removed from this PC", L"Пароль, ключ 2FA и профиль браузера удалятся с этого компьютера")
                                               : Tr(L"It is removed from the switcher's list. The account itself stays with OpenAI", L"Он уберётся из списка переключателя. Сам аккаунт у OpenAI останется"),
                                         Tr(L"Delete", L"Да, удалить"), true)) return;
            }
            if (saved) {
                creds.erase(std::remove_if(creds.begin(), creds.end(), [&](const Cred& c) { return c.id == key; }), creds.end());
                if (!SaveCreds(hwnd)) return;
                Browser::Forget(key);
                if (armed == key) Disarm();
            }
            if (all && !itemId.empty()) App::Delete(itemId, hwnd, false);
            Test::Trace(L"manager-delete|" + key + (all && !itemId.empty() ? L"|account" : L""));
            if (expanded == key) expanded.clear();
            Invalidate();
        }

        void Menu(const Row& row, POINT at) {
            auto key = row.Key();
            auto itemId = row.item ? row.item->id : std::wstring();
            bool active = row.item && row.item->active;
            auto& c = row.cred;
            WinShell::DarkMenus(T.dark); WinShell::AllowDark(hwnd, T.dark);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, Util::Format(Tr(L"Open the site (%s)", L"Открыть сайт (%s)"), Browser::Name().c_str()).c_str());
            AppendMenuW(m, MF_STRING, 2, Tr(L"Copy email", L"Копировать email"));
            AppendMenuW(m, MF_STRING | (c.password.empty() ? MF_GRAYED : 0), 3, Tr(L"Copy password", L"Копировать пароль"));
            AppendMenuW(m, MF_STRING | (c.key.empty() ? MF_GRAYED : 0), 4, Tr(L"Copy 2FA code", L"Копировать код 2FA"));
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 5, Tr(L"Set password…", L"Задать пароль…"));
            AppendMenuW(m, MF_STRING, 6, Tr(L"Set 2FA key…", L"Задать ключ 2FA…"));
            AppendMenuW(m, MF_STRING, 7, Tr(L"Note…", L"Заметка…"));
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING | (itemId.empty() || active ? MF_GRAYED : 0), 8, Tr(L"Use it in Codex", L"Использовать в Codex"));
            AppendMenuW(m, MF_STRING | (itemId.empty() ? MF_GRAYED : 0), 9, Tr(L"Details and statistics…", L"Подробности и статистика…"));
            AppendMenuW(m, MF_STRING, 10, itemId.empty() ? Tr(L"Sign in to the switcher", L"Войти в переключатель") : Tr(L"Sign in to the switcher again", L"Войти в переключатель заново"));
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 11, Tr(L"Delete…", L"Удалить…"));
            int cmd = App::PopupMenu(m, at, hwnd);
            DestroyMenu(m);
            RefreshHover();
            auto email = c.email, password = c.password, k = c.key;
            switch (cmd) {
            case 1: OpenSite(key); break;
            case 2: Copy(email, false, Tr(L"Email copied", L"Email скопирован")); break;
            case 3: Copy(password, true, Tr(L"Password copied", L"Пароль скопирован")); break;
            case 4: Copy(Totp::Code(k, Core::NowMs() / 1000), true, Tr(L"Code copied", L"Код скопирован")); break;
            case 5: SetText(key, 0); break;
            case 6: SetText(key, 1); break;
            case 7: SetText(key, 2); break;
            case 8: if (App::Find(itemId)) App::Switch(itemId, false); break;
            case 9: if (App::Find(itemId)) App::ShowDetails(itemId); break;
            case 10: SignIn(key); break;
            case 11: Remove(key, true); break;
            }
            Invalidate();
        }

        void CodeByKey() {
            std::wstring value;
            for (;;) {
                if (!Dialogs::Prompt(hwnd, Tr(L"A 2FA code from a key", L"Код 2FA по ключу"),
                                     Tr(L"Paste a 2FA key or an otpauth:// or 2fa.live link. Its code shows here and is copied",
                                        L"Вставьте ключ 2FA или ссылку otpauth:// или 2fa.live. Его код появится здесь и скопируется"),
                                     value, false, false, Tr(L"Show the code", L"Показать код"))) return;
                auto key = Totp::Clean(value);
                if (!key.empty()) {
                    scratch.erase(std::remove(scratch.begin(), scratch.end(), key), scratch.end());
                    scratch.insert(scratch.begin(), key);
                    if (scratch.size() > 6) scratch.resize(6);
                    Copy(Totp::Code(key, Core::NowMs() / 1000), true, Tr(L"Code copied", L"Код скопирован"));
                    Invalidate();
                    return;
                }
                Dialogs::Info(hwnd, Tr(L"That isn't a 2FA key", L"Это не ключ 2FA"), Tr(L"A key is 16 or more letters A–Z and digits 2–7, like JBSW Y3DP EHPK 3PXP", L"Ключ — это 16 и больше латинских букв и цифр 2–7, например JBSW Y3DP EHPK 3PXP"));
            }
        }

        // ------------------------------------------------------------ drawing

        // "287 082" with a bar under it that runs out with the code.
        void CodeView(UINT id, RectF r, const std::wstring& key, float size) {
            long long now = Core::NowMs() / 1000;
            auto code = Totp::Code(key, now);
            int left = Totp::SecondsLeft(now);
            float h = HotT(id);
            if (h > 0.01f) Fill(r.Inset(-6, -2), Veil(h), 4);
            Text(Totp::Spaced(code), { r.x, r.y, r.w, size + 8 }, Font(size, DWRITE_FONT_WEIGHT_SEMI_BOLD), left <= 5 ? T.warning : T.text);
            float bw = std::min(r.w, size * 4.2f);
            Fill({ r.x, r.y + size + 11, bw, 3 }, T.layerStroke, 1.5f);
            Fill({ r.x, r.y + size + 11, bw * left / 30.f, 3 }, left <= 5 ? T.warning : T.text3, 1.5f);
            Hit(id, r.Inset(-6, -2), [this, key] { Copy(Totp::Code(key, Core::NowMs() / 1000), true, Tr(L"Code copied", L"Код скопирован")); });
            Describe(Tr(L"2FA code ", L"Код 2FA ") + Totp::Spaced(code) + Util::Format(Tr(L", %d s left. Copies it", L", ещё %d с. Копирует его"), left));
            Tip(Tr(L"Click to copy the code", L"Щелчок копирует код"));
            FocusRing(id, r.Inset(-6, -2), 4);
        }

        float ButtonW(const std::wstring& label, bool primary) { return Measure(label, Font(14, primary ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL)).width + 30; }

        // The circle on the left: what is done of the four steps, or ready, or over.
        void Badge(float cx, float cy, const Row& row) {
            using S = Creds::Stage;
            if (row.stage == S::Ready && !(row.item && row.item->SignedOut())) {
                Circle(cx, cy, 16, Mix(T.layer, T.success, .22f));
                Glyph(Glyphs::Check, { cx - 12, cy - 12, 24, 24 }, 14, T.success);
            } else if (row.stage == S::Expired || (row.item && row.item->SignedOut())) {
                Circle(cx, cy, 16, Mix(T.layer, T.danger, .2f));
                Glyph(Glyphs::Warning, { cx - 12, cy - 12, 24, 24 }, 13, T.danger);
            } else {
                Ring(cx, cy, 16, T.controlStrong, 2);
                Text(Util::Format(L"%d/4", Creds::StepsDone(row.cred, row.item != nullptr)), { cx - 16, cy - 10, 32, 20 }, Font(11, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text2, Align::Center);
            }
        }

        std::wstring StageText(const Row& row, D2D1_COLOR_F& color) {
            using S = Creds::Stage;
            color = T.text2;
            switch (row.stage) {
            case S::New: return Tr(L"New: secure it", L"Новый: защитите его");
            case S::Securing: return Util::Format(Tr(L"Securing: %d of 4 steps", L"Защита: %d из 4 шагов"), Creds::StepsDone(row.cred, false));
            case S::SignIn: return Tr(L"Sign in to the switcher next", L"Осталось войти в переключатель");
            case S::Expired: color = T.danger; return Tr(L"Paid month over", L"Подписка кончилась");
            default:
                if (row.item && row.item->SignedOut()) { color = T.danger; return Tr(L"Signed out: sign in again", L"Вылетел: войдите заново"); }
                color = T.success;
                return row.item && row.item->active ? Tr(L"Ready · in use", L"Готов · используется") : Tr(L"Ready", L"Готов");
            }
        }

        std::wstring Details(const Row& row) {
            long long now = Core::NowMs();
            std::vector<std::wstring> parts;
            auto& c = row.cred;
            if (Bare(c)) parts.push_back(Tr(L"no password saved", L"пароль не сохранён"));
            else if (!c.order.empty()) {
                auto tail = c.order;
                if (tail.size() > 18) { auto dash = tail.rfind(L'-'); tail = dash != std::wstring::npos && tail.size() - dash > 4 ? tail.substr(dash + 1) : L"…" + tail.substr(tail.size() - 10); }
                parts.push_back(Tr(L"order ", L"заказ ") + tail);
            }
            if (!Bare(c) && row.stage != Creds::Stage::Ready && row.stage != Creds::Stage::Expired && c.warrantyUntil) {
                if (c.warrantyUntil > now) parts.push_back(Tr(L"warranty ", L"гарантия ещё ") + Dur(c.warrantyUntil - now) + (L10n::ru ? L"" : L" left"));
                else parts.push_back(Tr(L"warranty over", L"гарантия кончилась"));
            }
            if (!c.note.empty()) parts.push_back(c.note.size() > 40 ? c.note.substr(0, 38) + L"…" : c.note);
            std::wstring s;
            for (auto& p : parts) s += L"  ·  " + p;
            return s;
        }

        struct Btn { std::wstring label; std::function<void()> click; bool primary = false; UINT id = 0; bool enabled = true; };
        // Buttons from the right edge leftwards; returns where they begin.
        float ButtonsRight(float right, float cy, const std::vector<Btn>& bs) {
            float x = right;
            for (int i = (int)bs.size() - 1; i >= 0; i--) {
                float bw = std::max(84.f, ButtonW(bs[i].label, bs[i].primary));
                x -= bw;
                Button(bs[i].id, { x, cy - 16, bw, 32 }, bs[i].label, bs[i].primary, bs[i].click, false, bs[i].enabled);
                x -= 8;
            }
            return x;
        }

        int Steps(const Row& row) { return Bare(row.cred) ? 1 : 5; }
        float PanelH(const Row& row) { return 6 + Steps(row) * StepH + (Bare(row.cred) ? 0 : ChipsH) + 6; }

        // A step: its circle (done or not), title and line, and its buttons.
        void Step(RectF r, int i, int n, bool done, bool canFlip, std::function<void()> flip, const std::wstring& title, const std::wstring& line, const std::vector<Btn>& bs, D2D1_COLOR_F lineColor = {}) {
            UINT cid = Id("step-check", i * 10 + n);
            float cx = r.x + 32, cy = r.y + r.h / 2;
            float t = A(cid ^ 0x3C, done ? 1.f : 0.f, 120), h = canFlip ? HotT(cid) : 0;
            if (t > 0.01f) { Circle(cx, cy, 10, Mix(T.layer, T.success, t)); Glyph(Glyphs::Check, { cx - 10, cy - 10, 20, 20 }, 11, T.dark ? Rgba(0x0D0D0D, t) : Rgba(0xFFFFFF, t)); }
            if (t < 0.99f) Ring(cx, cy, 10, Mix(T.controlStrong, T.text, h * .5f), 1.5f);
            if (canFlip) { Hit(cid, { cx - 16, cy - 16, 32, 32 }, [this, flip] { flip(); Invalidate(); }); Describe(title, Role::Check, done ? 1 : 0); FocusRing(cid, { cx - 13, cy - 13, 26, 26 }, 13); }
            float bx = ButtonsRight(r.R() - 16, cy, bs);
            float tx = r.x + 60, tw = bx - tx - 12;
            Text(title, { tx, r.y + 8, tw, 20 }, Font(14), done ? T.text2 : T.text);
            Text(line, { tx, r.y + 29, tw, 18 }, Font(12.5f), lineColor.a > 0 ? lineColor : T.text3);
        }

        void Panel(const Row& row, RectF r, int i) {
            auto& c = row.cred;
            auto key = row.Key();
            bool signedIn = row.item != nullptr;
            Fill({ r.x + 16, r.y, r.w - 32, 1 }, T.layerStroke);
            float y = r.y + 6;
            auto stepRect = [&] { RectF s{ r.x, y, r.w, StepH }; y += StepH; return s; };
            if (Bare(c)) {
                Step(stepRect(), i, 0, false, false, nullptr, Tr(L"No password or 2FA key saved for it", L"Для него не сохранены пароль и ключ 2FA"),
                     Tr(L"Save them to type them in with Ctrl+Alt+V and see its codes here", L"Сохраните их, чтобы вводить по Ctrl+Alt+V и видеть здесь его коды"),
                     { { Tr(L"Set password…", L"Задать пароль…"), [this, key] { SetText(key, 0); }, false, Id("set-pass", i) },
                       { Tr(L"Set 2FA key…", L"Задать ключ 2FA…"), [this, key] { SetText(key, 1); }, false, Id("set-key", i) } });
                return;
            }
            auto id = c.id;
            bool nextIsSignIn = c.passwordDone && c.keyDone && c.phoneDone && !signedIn;
            Step(stepRect(), i, 0, c.passwordDone || c.keyDone || signedIn, false, nullptr, Tr(L"Sign in on the site", L"Войдите на сайт"),
                 Util::Format(Tr(L"In a %s profile of its own, apart from your other accounts", L"В отдельном профиле %s, не смешиваясь с другими аккаунтами"), Browser::Name().c_str()),
                 { { Tr(L"Open the site", L"Открыть сайт"), [this, id] { OpenSite(id); }, !c.passwordDone && !c.keyDone, Id("site", i) } });

            if (c.newPassword.empty())
                Step(stepRect(), i, 1, c.passwordDone, true, [this, id, d = c.passwordDone] { PasswordDone(id, !d); },
                     Tr(L"Change the password", L"Смените пароль"),
                     c.passwordDone ? std::wstring(Tr(L"Done. The new one is saved here", L"Готово, новый сохранён здесь")) : std::wstring(Tr(L"Settings → Security → Password. A new strong one is made here", L"«Настройки → Безопасность → Пароль». Новый надёжный придумается здесь")),
                     { { Tr(L"New password", L"Придумать новый"), [this, id] { MakePassword(id); }, !c.passwordDone, Id("make-pass", i) } });
            else
                Step(stepRect(), i, 1, false, true, [this, id] { PasswordDone(id, true); },
                     Tr(L"Change the password", L"Смените пароль"), Tr(L"New: ", L"Новый: ") + (App::MaskAll() ? std::wstring(L"••••••••••••") : c.newPassword),   // hidden while the screen is shared
                     { { Tr(L"Copy", L"Копировать"), [this, id] { if (auto x = CredById(id)) Copy(x->newPassword, true, Tr(L"Password copied", L"Пароль скопирован")); }, false, Id("copy-pass", i) },
                       { Tr(L"Changed it", L"Сменил"), [this, id] { PasswordDone(id, true); }, true, Id("pass-done", i) } }, T.text);

            if (c.newKey.empty())
                Step(stepRect(), i, 2, c.keyDone, true, [this, id, d = c.keyDone] { KeyDone(id, !d); },
                     Tr(L"Change the 2FA key", L"Смените ключ 2FA"),
                     c.keyDone ? std::wstring(Tr(L"Done. Codes come from the new key", L"Готово, коды идут от нового ключа")) : std::wstring(Tr(L"Turn 2FA off and on again on the site, then paste the new key here", L"Выключите и снова включите 2FA на сайте и вставьте сюда новый ключ")),
                     { { Tr(L"Paste the key…", L"Вставить ключ…"), [this, id] { PasteKey(id); }, c.passwordDone && !c.keyDone, Id("paste-key", i) } });
            else {
                auto code = Totp::Spaced(Totp::Code(c.newKey, Core::NowMs() / 1000));
                Step(stepRect(), i, 2, false, true, [this, id] { KeyDone(id, true); },
                     Tr(L"Change the 2FA key", L"Смените ключ 2FA"), Tr(L"Enter this code on the site: ", L"Введите на сайте этот код: ") + code,
                     { { Tr(L"Copy code", L"Копировать код"), [this, id] { if (auto x = CredById(id)) Copy(Totp::Code(x->newKey, Core::NowMs() / 1000), true, Tr(L"Code copied", L"Код скопирован")); }, false, Id("copy-newcode", i) },
                       { Tr(L"Changed it", L"Сменил"), [this, id] { KeyDone(id, true); }, true, Id("key-done", i) } }, T.text);
            }

            Step(stepRect(), i, 3, c.phoneDone, true, [this, id, d = c.phoneDone] { if (auto x = CredById(id)) { x->phoneDone = !d; SaveCreds(hwnd); } },
                 Tr(L"Confirm the phone number", L"Подтвердите номер телефона"),
                 c.phoneDone ? std::wstring(Tr(L"Done", L"Готово")) : std::wstring(Tr(L"Codex asks for it at the first sign-in. Do it yourself, then tick it here", L"Codex попросит его при первом входе. Подтвердите сами и отметьте здесь")),
                 { { Tr(L"Done", L"Готово"), [this, id] { if (auto x = CredById(id)) { x->phoneDone = true; SaveCreds(hwnd); } }, false, Id("phone", i), !c.phoneDone } });

            bool waiting = App::SigningIn() && App::SigningInFor() == id;
            Step(stepRect(), i, 4, signedIn, false, nullptr, Tr(L"Sign in to the switcher", L"Войдите в переключатель"),
                 signedIn ? std::wstring(Tr(L"Done: it's on the list", L"Готово: аккаунт в списке"))
                          : waiting ? std::wstring(Tr(L"Waiting for the sign-in in the browser…", L"Жду входа в браузере…"))
                                    : std::wstring(Tr(L"In your browser. The link can be copied into the account's own window", L"В вашем браузере. Ссылку можно скопировать в окно этого аккаунта")),
                 waiting ? std::vector<Btn>{ { Tr(L"Copy link", L"Скопировать ссылку"), [this] { Copy(App::SignInLink(), false, Tr(L"Link copied: paste it into any browser", L"Ссылка скопирована: вставьте её в любой браузер")); }, false, Id("signin-link", i) },
                                             { Tr(L"Cancel", L"Отмена"), [] { App::CancelSignIn(); }, false, Id("signin-cancel", i) } }
                         : std::vector<Btn>{ { signedIn ? Tr(L"Again", L"Заново") : Tr(L"Sign in", L"Войти"), [this, id] { SignIn(id); }, nextIsSignIn, Id("signin", i) } });

            // What Ctrl+Alt+V types next.
            RectF chips{ r.x + 60, y + 6, r.w - 76, ChipsH - 12 };
            if (armed == id && !queue.empty()) {
                auto lead = hotkeyFailed ? std::wstring(Tr(L"Ctrl+Alt+V is taken by another app; use the copy buttons", L"Ctrl+Alt+V занято другой программой, копируйте кнопками"))
                                         : std::wstring(Tr(L"Ctrl+Alt+V types into the field under the cursor:", L"Ctrl+Alt+V вводит в поле под курсором:"));
                float lw = Measure(lead, Font(12.5f)).width;
                Text(lead, { chips.x, chips.y, lw + 4, chips.h }, Font(12.5f), hotkeyFailed ? T.warning : T.text2);
                float x = chips.x + lw + 12;
                for (size_t k = 0; k < queue.size(); k++) {
                    auto label = FieldLabel(queue[k].field) + (queue[k].enter ? L" ⏎" : L"");
                    float w = Measure(label, Font(12.5f)).width + 22;
                    RectF b{ x, chips.y + chips.h / 2 - 13, w, 26 };
                    UINT cid = Id("chip", (int)k);
                    bool isNext = k == (next >= queue.size() ? 0 : next), done = k < next && next <= queue.size();
                    float h = HotT(cid);
                    Fill(b, isNext ? T.accent : Mix(T.control, T.controlHover, h), 13);
                    if (!isNext) Stroke(b, T.controlStroke, 13);
                    Text(label, b, Font(12.5f, isNext ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL), isNext ? T.onAccent : done ? T.text3 : T.text, Align::Center);
                    Hit(cid, b, [this, k] { next = k; Invalidate(); });
                    Describe(label + (isNext ? Tr(L", typed next", L", вводится следующим") : L""));
                    x += w + 6;
                    if (k + 1 < queue.size()) { Text(L"→", { x, chips.y, 14, chips.h }, Font(12.5f), T.text3, Align::Center); x += 20; }
                }
            } else {
                Text(Tr(L"Open the site or sign in, and Ctrl+Alt+V types the email, the password and the 2FA code into the field under the cursor",
                        L"Откройте сайт или войдите, и Ctrl+Alt+V введёт в поле под курсором email, пароль и код 2FA"), chips, Font(12.5f), T.text3);
            }
        }

        void RowUi(const Row& row, float y, float w, int i) {
            auto key = row.Key();
            bool open = expanded == key;
            float panel = open ? PanelH(row) : 0;
            RectF card{ Pad, y, w - 2 * Pad, RowH + panel };
            RectF head{ card.x, card.y, card.w, RowH };
            UINT id = Id("mrow", i);
            float h = A(id ^ 0x51, Over(head) ? 1.f : 0.f), p = PressT(id);
            Fill(card, T.layer, 8);
            if (h > 0.01f || p > 0.01f) Fill(head.Inset(1), Veil(h * .8f, p), 7);   // 1 inside the card's 8
            Stroke(card, T.layerStroke, 8);
            Hit(id, head, [this, key] { expanded = expanded == key ? L"" : key; Invalidate(); }, [this, i] { POINT pt; GetCursorPos(&pt); auto rows = FilteredRows(); if (i < (int)rows.size()) Menu(rows[i], pt); }, true, false);
            {
                D2D1_COLOR_F unused;
                auto paid = row.paidUntil ? Tr(L", paid until ", L", оплачен до ") + DateOf(row.paidUntil) : std::wstring();
                Describe(Shown(row.cred.email, row.item) + L", " + StageText(row, unused) + paid + (open ? Tr(L", steps shown", L", шаги показаны") : Tr(L", opens its steps", L", открывает шаги")), Role::Row);
            }
            FocusRing(id, card, 8);

            Badge(head.x + 34, head.y + RowH / 2, row);

            // The chevron says the row opens; it belongs to the row and isn't a button of its own.
            float right = head.R() - 8;
            Glyph(open ? Glyphs::ChevronUp : Glyphs::ChevronDown, { right - 24, head.y, 24, RowH }, 11, Mix(T.text3, T.text, h));
            right -= 26;
            UINT mid = Id("mmore", i);
            RectF more{ right - 32, head.y + RowH / 2 - 16, 32, 32 };
            float mh = HotT(mid);
            Fill(more, Veil(mh, PressT(mid)), 4);
            Glyph(Glyphs::More, more, 14, Mix(T.text2, T.text, mh));
            Hit(mid, more, [this, i, more] { POINT pt{ (LONG)(more.x * scale), (LONG)(more.B() * scale) }; ClientToScreen(hwnd, &pt); auto rows = FilteredRows(); if (i < (int)rows.size()) Menu(rows[i], pt); });
            Describe(Util::Format(Tr(L"More for %s", L"Ещё для %s"), Shown(row.cred.email, row.item).c_str()));
            Tip(Tr(L"Copy, set the password or key, delete and more", L"Копировать, задать пароль или ключ, удалить и другое"));
            FocusRing(mid, more, 4);
            right = more.x - 8;

            // The next step, as a button.
            using S = Creds::Stage;
            Btn nb;
            bool signedOut = row.item && row.item->SignedOut();
            auto itemId = row.item ? row.item->id : std::wstring();
            if (row.stage == S::Expired) nb = { Tr(L"Delete…", L"Удалить…"), [this, key] { Remove(key, true); } };
            else if (signedOut) nb = { Tr(L"Sign in again", L"Войти заново"), [this, key] { SignIn(key); }, true };
            else if (row.stage == S::Ready) nb = row.item->active ? Btn{ Tr(L"Open the site", L"Открыть сайт"), [this, key] { OpenSite(key); } }
                                                                  : Btn{ Tr(L"Use", L"Использовать"), [itemId] { if (App::Find(itemId)) App::Switch(itemId, false); } };
            else if (row.stage == S::SignIn) nb = { Tr(L"Sign in here", L"Войти в переключатель"), [this, key] { SignIn(key); }, true };
            else nb = { Tr(L"Open the site", L"Открыть сайт"), [this, key] { OpenSite(key); }, row.stage == S::New };
            nb.id = Id("mnext", i);
            float nw = std::max(120.f, ButtonW(nb.label, nb.primary));
            bool inSteps = open && !Bare(row.cred) && (row.stage == S::New || row.stage == S::Securing || row.stage == S::SignIn);
            if (!inSteps) Button(nb.id, { right - nw, head.y + RowH / 2 - 16, nw, 32 }, nb.label, nb.primary, nb.click);
            right -= nw + 20;

            // Paid until.
            long long now = Core::NowMs();
            RectF paid{ right - 112, head.y + 13, 112, 40 };
            Text(Tr(L"paid until", L"оплачен до"), { paid.x, paid.y, paid.w, 16 }, Font(11), T.text3);
            std::wstring pv = row.paidUntil ? (row.paidUntil <= now ? Tr(L"over ", L"кончился ") + DateOf(row.paidUntil) : DateOf(row.paidUntil)) : L"—";
            Text(pv, { paid.x, paid.y + 17, paid.w, 20 }, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), row.stage == S::Expired ? T.danger : row.expiring ? T.warning : T.text);
            right = paid.x - 16;

            // The 2FA code, a click copies it.
            RectF code{ right - 104, head.y + 12, 104, 40 };
            if (!row.cred.key.empty()) CodeView(Id("mcode", i), code, row.cred.key, 20);
            else Text(Tr(L"no 2FA key", L"нет ключа 2FA"), { code.x, code.y + 10, code.w, 20 }, Font(12.5f), T.text3);
            right = code.x - 16;

            // Email and where it stands.
            float tx = head.x + 64, tw = right - tx;
            auto email = Shown(row.cred.email, row.item);
            auto plan = row.item ? row.item->PlanLabel() : std::wstring();
            float pw = plan.empty() ? 0 : Measure(plan, Font(11, DWRITE_FONT_WEIGHT_SEMI_BOLD)).width + 14;
            float ew = std::min(Measure(email, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD)).width + 2, tw - (pw ? pw + 8 : 0));
            Text(email, { tx, head.y + 12, ew, 22 }, Font(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text);
            if (pw) Pill({ tx + ew + 8, head.y + 14, pw, 18 }, plan, T.accentTint, T.text2);
            D2D1_COLOR_F sc;
            auto stage = StageText(row, sc);
            float sw = Measure(stage, Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)).width;
            Text(stage, { tx, head.y + 36, std::min(sw + 2, tw), 18 }, Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), sc);
            auto details = Details(row);
            if (!details.empty() && sw + 20 < tw) {
                bool soon = row.cred.warrantyUntil > now && row.cred.warrantyUntil - now < 3 * Hour && row.stage != S::Ready;
                Text(details, { tx + sw + 2, head.y + 36, tw - sw - 2, 18 }, Font(12.5f), soon ? T.warning : T.text3);
            }

            if (open) Panel(row, { card.x, card.y + RowH, card.w, panel }, i);
        }

        std::vector<Row> FilteredRows() {
            std::vector<Row> out;
            for (auto& r : Rows()) if (In(r, filter) && Matches(r)) out.push_back(r);
            return out;
        }

        void Build() override {
            auto c = Client();
            BeginScroll({ 0, 0, c.w, c.h });
            float w = c.w, y = 10 - scrollY;
            auto all = Rows();
            // The tabs count what the search leaves; the line under the title counts everything.
            int counts[FilterCount] = {}, totals[FilterCount] = {};
            for (auto& r : all) for (int f = 0; f < FilterCount; f++) if (In(r, f)) { totals[f]++; if (Matches(r)) counts[f]++; }

            // The ways in on the right; the title and what stands where take the rest.
            float titleW = w - 2 * Pad - 380;
            {
                std::vector<Btn> bs;
                int waiting = (int)Unsigned().size();
                if (lineupTotal) bs.push_back({ Util::Format(Tr(L"Stop (%d of %d)", L"Остановить (%d из %d)"), lineupTotal - (int)lineup.size(), lineupTotal), [this] { StopLineup(); }, false, Id("lineup") });
                else if (waiting >= 2 && !App::SigningIn()) bs.push_back({ Util::Format(Tr(L"Sign in to all: %d", L"Войти во все: %d"), waiting), [this] { StartLineup(); }, false, Id("lineup") });
                bs.push_back({ Tr(L"Code from a key…", L"Код по ключу…"), [this] { CodeByKey(); }, false, Id("bykey") });
                bs.push_back({ Tr(L"Paste an order", L"Вставить заказ"), [this] { Manager::ImportOrder(hwnd); }, true, Id("paste-order") });
                titleW = ButtonsRight(w - Pad, y + 26, bs) - Pad - 8;
            }
            Text(Tr(L"Account manager", L"Управление аккаунтами"), { Pad, y, titleW, 44 }, Font(28, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), T.text);
            std::wstring sum = Util::Count(totals[All], L"account", L"accounts", L"аккаунт", L"аккаунта", L"аккаунтов");
            if (totals[Action]) sum += Util::Format(Tr(L" · %d need you", L" · ждут действий: %d"), totals[Action]);
            if (totals[Expiring]) sum += Util::Format(Tr(L" · %d run out soon", L" · скоро кончатся: %d"), totals[Expiring]);
            if (lineupTotal) sum = Util::Format(Tr(L"Signing in one by one: the next window opens when this sign-in is done", L"Вход по очереди: следующее окно откроется, когда закончится этот вход"));
            Text(sum, { Pad, y + 44, titleW, 18 }, Font(12.5f), lineupTotal ? T.text : T.text2);
            y += 78;

            // Filters.
            {
                const wchar_t* names[FilterCount] = { Tr(L"All", L"Все"), Tr(L"Need you", L"Нужно действие"), Tr(L"Ready", L"Готовы"), Tr(L"Run out soon", L"Скоро кончатся"), Tr(L"Over", L"Истекли") };
                float x = Pad;
                RectF bar{ Pad, y, 0, 34 };
                std::vector<std::pair<std::wstring, float>> segs;
                float total = 8;
                for (int f = 0; f < FilterCount; f++) { auto l = Util::Format(L"%s  %d", names[f], counts[f]); float sw = Measure(l, Font(12.5f)).width + 26; segs.push_back({ l, sw }); total += sw; }
                bar.w = total;
                Fill(bar, T.control, 8); Stroke(bar, T.controlStroke, 8);   // the tabs' 4 + the 4 around them
                x = bar.x + 4;
                for (int f = 0; f < FilterCount; f++) {
                    RectF b{ x, bar.y + 4, segs[f].second, 26 };
                    x += segs[f].second;
                    UINT id = Id("filter", f);
                    float t = A(id ^ 0x5E, filter == f ? 1.f : 0.f, 140), hh = HotT(id);
                    Fill(b, Mix(Veil(hh), T.accent, t), 4);
                    Text(segs[f].first, b, Font(12.5f), Mix(f == Action && counts[f] ? T.warning : T.text, T.onAccent, t), Align::Center);
                    Hit(id, b, [this, f] { filter = f; scrollY = 0; Invalidate(); });
                    Describe(segs[f].first, Role::Tab, filter == f ? 1 : 0);
                    FocusRing(id, b, 4);
                }
                // To the right of the tabs: the search, and before it the way to clear out what is over.
                float right = w - Pad;
                if (filter == Expired && counts[Expired])
                    right = ButtonsRight(right, y + 17, { { Util::Format(Tr(L"Delete all %d", L"Удалить все: %d"), counts[Expired]), [this] { RemoveExpired(); }, false, Id("delete-expired") } });
                float sw = std::min(280.f, right - (bar.R() + 12));
                SearchField({ right - sw, y + 1, sw, 32 });
            }
            y += 50;

            // Codes for keys pasted just now, like 2fa.fb.tools.
            if (!scratch.empty()) {
                float ch = 18 + 22 + scratch.size() * 48.f;
                RectF card{ Pad, y, w - 2 * Pad, ch };
                Fill(card, T.layer, 8); Stroke(card, T.layerStroke, 8);
                Text(Tr(L"Codes from keys (until the switcher closes)", L"Коды по ключам (до закрытия переключателя)"), { card.x + 16, card.y + 12, card.w - 32, 18 }, Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text2);
                float sy = card.y + 36;
                for (size_t k = 0; k < scratch.size(); k++) {
                    auto& key = scratch[k];
                    Text(key.substr(0, 4) + L" •••• " + key.substr(key.size() - 4), { card.x + 16, sy + 8, 200, 20 }, Font(12.5f), T.text2);
                    CodeView(Id("scode", (int)k), { card.x + 230, sy + 2, 110, 38 }, key, 16);
                    IconButton(Id("sdel", (int)k), { card.R() - 48, sy + 6, 32, 32 }, Glyphs::Clear, Tr(L"Remove this key", L"Убрать этот ключ"), [this, key] { scratch.erase(std::remove(scratch.begin(), scratch.end(), key), scratch.end()); Invalidate(); });
                    sy += 48;
                }
                y += ch + 14;
            }

            // The accounts.
            auto rows = FilteredRows();
            if (rows.empty()) {
                RectF e{ Pad, y + 10, w - 2 * Pad, 150 };
                DashedStroke(e, T.controlStrong, 8);
                Glyph(Glyphs::Contact, { e.x, e.y + 22, e.w, 30 }, 22, T.text3);
                bool searching = !Util::Trim(query).empty();
                std::wstring t1 = searching ? Tr(L"Nothing found", L"Ничего не найдено") : totals[All] ? Tr(L"Nothing here", L"Здесь пусто") : Tr(L"No accounts yet", L"Аккаунтов пока нет");
                std::wstring t2 = searching ? Util::Format(Tr(L"No account has %s in its email, order or note. Esc clears the search", L"Ни у одного аккаунта нет %s в почте, заказе или заметке. Esc очищает поиск"), Util::Q(Util::Trim(query)).c_str())
                                : totals[All] ? std::wstring(Tr(L"Other filters have accounts", L"Аккаунты есть в других фильтрах"))
                                              : std::wstring(Tr(L"Copy the shop's message with the account details and press Paste an order, or copy a line email|password|2FA key",
                                                                L"Скопируйте сообщение магазина с данными аккаунта и нажмите «Вставить заказ», или скопируйте строку email|пароль|ключ 2FA"));
                Text(t1, { e.x, e.y + 62, e.w, 22 }, Font(16, DWRITE_FONT_WEIGHT_SEMI_BOLD), T.text, Align::Center);
                Text(t2, { e.x + 40, e.y + 90, e.w - 80, 40 }, Font(12.5f), T.text2, Align::Center, false, true);
                y += 170;
            }
            for (int i = 0; i < (int)rows.size(); i++) {
                auto key = rows[i].Key();
                if (!scrollTo.empty() && scrollTo == key) { scrollTo.clear(); float top = y + scrollY - 150; if (top > 0) { scrollY = top; Invalidate(); } }
                float h = RowH + (expanded == key ? PanelH(rows[i]) : 0);
                RowUi(rows[i], y, w, i);
                y += h + 8;
            }
            y += 16;

            EndScroll(y + scrollY);
        }

        void RemoveExpired() {
            std::vector<std::wstring> keys;
            for (auto& r : Rows()) if (r.stage == Creds::Stage::Expired && Matches(r)) keys.push_back(r.Key());   // the ones on screen
            if (keys.empty()) return;
            if (!Dialogs::Ask(hwnd, Util::Format(Tr(L"Delete %s whose paid month is over?", L"Удалить %s с кончившейся подпиской?"), Util::Count((int)keys.size(), L"account", L"accounts", L"аккаунт", L"аккаунта", L"аккаунтов").c_str()),
                              Tr(L"Their details, browser profiles and places in the switcher's list are removed from this PC", L"Их данные входа, профили браузера и места в списке переключателя удалятся с этого компьютера"),
                              Tr(L"Delete", L"Да, удалить"), true)) return;
            for (auto& k : keys) Remove(k, false);
        }

        // ------------------------------------------------------------ input and messages

        bool OnKey(UINT vk, bool) override {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrl && vk == 'V') { App::PasteAccounts(hwnd); return true; }      // an order, or an account in any other shape
            if (ctrl && vk == 'F') { OpenSearch(); return true; }
            if (vk == VK_F5) { App::Refresh(); App::RefreshUsage(true); return true; }
            if (vk == VK_ESCAPE) { OnClose(); return true; }
            return false;
        }

        void OnShown() override { SetTimer(hwnd, TimerTick, 1000, nullptr); }
        void OnClose() override { KillTimer(hwnd, TimerTick); Disarm(); Hide(); }

        LRESULT Extra(UINT m, WPARAM w, LPARAM l, bool& handled) override {
            if (m == WM_GETMINMAXINFO && hwnd) {
                auto mm = (MINMAXINFO*)l;
                mm->ptMinTrackSize.x = (LONG)(900 * scale); mm->ptMinTrackSize.y = (LONG)(480 * scale);
                handled = true; return 0;
            }
            if (m == WM_COMMAND && search && (HWND)l == search) {
                handled = true;
                if (HIWORD(w) == EN_CHANGE) {
                    int n = GetWindowTextLengthW(search);
                    std::wstring text(n, L'\0'); if (n) GetWindowTextW(search, &text[0], n + 1);
                    if (text != query) { query = text; scrollY = 0; Test::Trace(L"search|" + query); }
                }
                if (HIWORD(w) == EN_KILLFOCUS) searchOpen = false;   // left empty, it goes back to being drawn
                Invalidate();   // typed text, or the underline that shows the box has focus
                return 0;
            }
            if (m == WM_HOTKEY && w == HotkeyId) {
                handled = true;
                if (armed.empty()) return 0;
                // Wait for Ctrl and Alt to be let go, or the typing would come out as shortcuts.
                if (!pending++) typeStart = GetTickCount64();
                SetTimer(hwnd, TimerType, 25, nullptr);
                return 0;
            }
            if (m == WM_TIMER && w == TimerTick) { handled = true; if (Visible()) Invalidate(); return 0; }
            if (m == WM_TIMER && w == TimerType) {
                handled = true;
                bool held = false;
                if (!Test::On()) for (int vk : { VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN }) held |= (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (held && GetTickCount64() - typeStart < 3000) return 0;
                KillTimer(hwnd, TimerType);
                int presses = pending; pending = 0;
                if (held) { App::Notify(Tr(L"Nothing typed", L"Ничего не введено"), Tr(L"Let go of Ctrl and Alt after pressing Ctrl+Alt+V", L"Отпустите Ctrl и Alt после нажатия Ctrl+Alt+V"), true); return 0; }
                while (presses-- > 0) TypeNext();
                return 0;
            }
            if (m == WM_TIMER && w == TimerClear) {
                handled = true;
                KillTimer(hwnd, TimerClear);
                if (GetClipboardSequenceNumber() == clipSeq && OpenClipboard(hwnd)) { EmptyClipboard(); CloseClipboard(); }
                return 0;
            }
            return 0;
        }
    };

    ManagerWin* win = nullptr;
    ManagerWin& Get() { if (!win) { win = new ManagerWin(); win->Start(); } return *win; }

    // "Добавить 2 аккаунта?": what the order holds, before anything is saved.
    std::wstring Summary(const Creds::Parsed& p, long long now) {
        std::wstring s;
        if (!p.order.empty()) s += Tr(L"Order ", L"Заказ ") + p.order + L"\n";
        if (!p.product.empty()) s += p.product + L"\n";
        std::vector<std::wstring> dates;
        if (p.warrantyMs) dates.push_back(Tr(L"warranty for ", L"гарантия на ") + Dur(p.warrantyMs));
        if (p.months) dates.push_back(Tr(L"paid until ", L"оплачен до ") + DateOf(now + p.months * 30ll * Day));
        for (size_t i = 0; i < dates.size(); i++) s += (i ? L" · " : L"") + dates[i];
        if (!dates.empty()) s += L"\n";
        for (size_t i = 0; i < p.creds.size() && i < 6; i++) s += L"\n" + p.creds[i].email + (p.creds[i].key.empty() ? Tr(L" (no 2FA key)", L" (без ключа 2FA)") : L"");
        if (p.creds.size() > 6) s += Util::Format(Tr(L"\n…and %d more", L"\n…и ещё %d"), (int)p.creds.size() - 6);
        if (p.bad) s += Util::Format(Tr(L"\n\n%d lines didn't read as email|password|key and were skipped", L"\n\nНе распознано и пропущено строк: %d"), p.bad);
        return s;
    }
}

namespace Manager {
    void Show(const std::wstring& focus) {
        auto& w = Get();
        if (!focus.empty()) { w.filter = All; w.expanded = focus; w.scrollTo = focus; }
        // Where it was left; the first time, in the middle of the screen the pointer is on.
        if (!w.Visible() && !w.RestorePlace(L"manager") && !w.placed) {
            w.placed = true;
            POINT pt; GetCursorPos(&pt);
            MONITORINFO mi{ sizeof mi }; GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
            RECT wr; GetWindowRect(w.hwnd, &wr);
            int ww = std::min<int>(wr.right - wr.left, mi.rcWork.right - mi.rcWork.left - 40), wh = std::min<int>(wr.bottom - wr.top, mi.rcWork.bottom - mi.rcWork.top - 40);
            SetWindowPos(w.hwnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2,
                         mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        SetWindowTextW(w.hwnd, ManagerWin::Title());
        if (IsIconic(w.hwnd)) ShowWindow(w.hwnd, SW_RESTORE);
        Test::Trace(L"window|manager");
        w.Show(true);
        w.Invalidate();
    }
    bool Visible() { return win && win->Visible(); }
    void Restyle() { if (win) win->Restyle(); }
    void Invalidate() { if (win) win->Invalidate(); }
    HWND Handle() { return win ? win->hwnd : nullptr; }

    void ShowAccount(const std::wstring& accountId) {
        LoadCreds();
        auto it = App::Find(accountId);
        std::wstring key = L"acct:" + accountId;
        for (auto& c : creds) if (c.accountId == accountId || (it && !c.email.empty() && _wcsicmp(c.email.c_str(), it->email.c_str()) == 0)) key = c.id;
        Show(key);
    }

    void ImportOrder(HWND owner) { ImportText(owner, Util::PasteText(owner)); }

    void ImportText(HWND owner, const std::wstring& text) {
        long long now = Core::NowMs();
        auto p = Creds::Parse(text, now);
        if (p.creds.empty()) {
            std::wstring line;
            for (;;) {
                if (!Dialogs::Prompt(owner, Tr(L"Add an account by its details", L"Добавить аккаунт по данным"),
                                     Tr(L"There's no order on the clipboard. Copy the shop's whole message and press Paste an order again, or type a line here: email|password|2FA key",
                                        L"В буфере обмена нет заказа. Скопируйте сообщение магазина целиком и снова нажмите «Вставить заказ» или впишите сюда строку: email|пароль|ключ 2FA"),
                                     line, false, false, Tr(L"Add", L"Добавить"))) return;
                p = Creds::Parse(line, now);
                if (!p.creds.empty()) break;
                Dialogs::Info(owner, Tr(L"No account in that line", L"В строке нет аккаунта"), Tr(L"Write it like me@example.com|password|JBSWY3DPEHPK3PXP", L"Пишите так: me@example.com|пароль|JBSWY3DPEHPK3PXP"));
            }
        }
        auto title = p.creds.size() == 1 ? Util::Format(Tr(L"Add %s?", L"Добавить %s?"), p.creds[0].email.c_str())
                                         : Util::Format(Tr(L"Add %s?", L"Добавить %s?"), Util::Count((int)p.creds.size(), L"account", L"accounts", L"аккаунт", L"аккаунта", L"аккаунтов").c_str());
        if (!Dialogs::Ask(owner, title, Summary(p, now), Tr(L"Add", L"Добавить"))) return;
        LoadCreds();
        auto m = Creds::Merge(creds, p);
        if (!SaveCreds(owner)) return;
        Test::Trace(Util::Format(L"import|%d|%d", m.added, m.updated));
        auto& w = Get();
        w.filter = All;
        if (!m.ids.empty()) { w.expanded = m.ids[0]; w.scrollTo = m.ids[0]; }
        Show();
    }

    // A sign-in started from a card ended: the account is linked to the card, or, if it was
    // another account, the card stays as it was.
    void Added(const std::wstring& credId, const std::wstring& accountId) {
        LoadCreds();
        auto c = CredById(credId);
        if (!c) return;
        std::wstring email;
        for (auto& a : Accounts::Load()) if (a.id == accountId) email = a.email;
        if (Bare(*c)) {
            // A card with nothing saved on it. If the account has a card already, that one
            // stays and this one goes.
            for (auto& o : creds) if (o.id != credId && (o.accountId == accountId || (!email.empty() && _wcsicmp(o.email.c_str(), email.c_str()) == 0))) {
                if (o.accountId.empty()) o.accountId = accountId;
                creds.erase(std::remove_if(creds.begin(), creds.end(), [&](const Cred& x) { return x.id == credId; }), creds.end());
                SaveCreds(win ? win->hwnd : nullptr);
                Browser::Forget(credId);
                Invalidate();
                return;
            }
            c->accountId = accountId;
            if (!email.empty()) c->email = email;
            SaveCreds(win ? win->hwnd : nullptr);
            Test::Trace(L"profile|" + credId + L"|" + accountId);
            Invalidate();
            return;
        }
        if (!email.empty() && _wcsicmp(email.c_str(), c->email.c_str()) != 0) {
            if (win) { win->lineup.clear(); win->lineupTotal = 0; }
            Dialogs::Info(win ? win->hwnd : nullptr, Tr(L"That was another account", L"Это был другой аккаунт"),
                          Util::Format(Tr(L"You signed in as %s, not %s, so it was added to the list separately. Sign in again with %s",
                                          L"Вход выполнен в %s, а не в %s, поэтому он добавлен в список отдельно. Войдите снова под %s"), email.c_str(), c->email.c_str(), c->email.c_str()));
            return;
        }
        c->accountId = accountId;
        SaveCreds(win ? win->hwnd : nullptr);
        Test::Trace(L"linked|" + credId + L"|" + accountId);
        bool more = win && win->lineupTotal && !win->lineup.empty();
        // In a queue the next window says enough; a notification each time would only pile up.
        if (!more) App::Notify(Tr(L"Account ready", L"Аккаунт готов"), Util::Format(Tr(L"%s is on the list and can be used", L"%s в списке, им можно пользоваться"), c->email.c_str()));
        if (win && win->lineupTotal) win->NextInLine();
        Invalidate();
    }

    void SignInStopped() { if (win && win->lineupTotal) { win->lineup.clear(); win->lineupTotal = 0; win->Invalidate(); } }

    void AccountGone(const std::wstring& accountId) {
        if (!Util::Exists(Creds::File())) return;
        LoadCreds();
        std::vector<std::wstring> gone;
        for (auto& c : creds) if (c.accountId == accountId && Bare(c)) gone.push_back(c.id);
        if (gone.empty()) return;
        creds.erase(std::remove_if(creds.begin(), creds.end(), [&](const Cred& x) { return x.accountId == accountId && Bare(x); }), creds.end());
        try { Creds::Save(creds); credsStamp = Util::FileTime(Creds::File()); } catch (...) { credsStamp = ~0ull; }
        for (auto& id : gone) Browser::Forget(id);
        Invalidate();
    }

    // Once a minute or so: a warranty about to end on an account that isn't ready, or a paid
    // month about to end, gets one notification each.
    void Remind() {
        if (!Util::Exists(Creds::File())) return;
        LoadCreds();
        long long now = Core::NowMs();
        // A card with nothing saved and no account (an earlier build made them for sign-ins
        // that never came back): it and its browser profile belong to nobody.
        for (auto& c : creds) if (!c.id.empty() && Bare(c) && c.accountId.empty() && now - c.received > 2 * Hour && !App::SigningIn()) {
            auto id = c.id;
            creds.erase(std::remove_if(creds.begin(), creds.end(), [&](const Cred& x) { return x.id == id; }), creds.end());
            try { Creds::Save(creds); credsStamp = Util::FileTime(Creds::File()); } catch (...) { credsStamp = ~0ull; }
            Browser::Forget(id);
            return;
        }
        for (auto& c : creds) {
            if (Bare(c)) continue;
            auto it = AccountOf(c);
            long long paid = it && it->subscriptionUntil ? it->subscriptionUntil : c.paidUntil;
            auto stage = Creds::StageOf(c, it != nullptr, paid, now);
            auto id = c.id;
            if (!c.warnedWarranty && c.warrantyUntil > now && c.warrantyUntil - now < 2 * Hour && stage != Creds::Stage::Ready && stage != Creds::Stage::Expired) {
                c.warnedWarranty = true;
                try { Creds::Save(creds); credsStamp = Util::FileTime(Creds::File()); } catch (...) {}
                App::Notify(Util::Format(Tr(L"The warranty on %s ends in %s", L"Гарантия на %s кончится через %s"), c.email.c_str(), Dur(c.warrantyUntil - now).c_str()),
                            Tr(L"Check that it signs in and secure it while the seller can still replace it", L"Проверьте вход и защитите аккаунт, пока продавец ещё может его заменить"), false, [id] { Show(id); });
                return;
            }
            if (!c.warnedPaid && paid > now && paid - now < Day) {
                c.warnedPaid = true;
                try { Creds::Save(creds); credsStamp = Util::FileTime(Creds::File()); } catch (...) {}
                App::Notify(Util::Format(Tr(L"The paid month of %s ends in %s", L"Подписка %s кончится через %s"), c.email.c_str(), Dur(paid - now).c_str()),
                            Tr(L"Renew it, or delete the account from the manager once it's over", L"Продлите её или удалите аккаунт в управлении, когда она кончится"), false, [id] { Show(id); });
                return;
            }
        }
    }
}
