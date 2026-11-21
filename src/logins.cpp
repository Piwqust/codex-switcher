#include "logins.h"
#include "net.h"
#include <shellapi.h>
#include <shlwapi.h>

// ================================================================ 2FA codes

namespace Totp {
    static const wchar_t* Alphabet = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

    std::wstring Clean(const std::wstring& text) {
        auto t = Util::Trim(text);
        auto lower = Util::Lower(t);
        if (auto at = lower.find(L"secret="); at != std::wstring::npos) {
            // otpauth://totp/OpenAI:me@x.com?secret=ABCD…&issuer=OpenAI
            auto v = t.substr(at + 7);
            v = v.substr(0, v.find_first_of(L"&#"));
            t = Util::Wide(Net::UrlDecode(Util::Utf8(v)));
        } else if (t.find(L'/') != std::wstring::npos) {
            // https://2fa.live/tok/ABCD… and the like: the key is the last part of the path.
            t = t.substr(0, t.find_first_of(L"?#"));
            while (!t.empty() && t.back() == L'/') t.pop_back();
            t = t.substr(t.rfind(L'/') + 1);
        }
        std::wstring key;
        for (wchar_t c : t) {
            if (c == L' ' || c == L'-' || c == L'=' || c == L'\t' || c == 0xA0) continue;
            wchar_t u = towupper(c);
            if (!wcschr(Alphabet, u) || !u) return L"";
            key += u;
        }
        // Shorter than 16 characters (80 bits) is a password or a code, not a key.
        return key.size() >= 16 && key.size() <= 128 ? key : L"";
    }

    std::string Decode(const std::wstring& base32) {
        std::string out;
        unsigned buffer = 0; int bits = 0;
        for (wchar_t c : base32) {
            if (c == L'=' || c == L' ') continue;
            auto p = wcschr(Alphabet, towupper(c));
            if (!p || !c) return "";
            buffer = (buffer << 5) | (unsigned)(p - Alphabet);
            bits += 5;
            if (bits >= 8) { out += (char)((buffer >> (bits - 8)) & 0xFF); bits -= 8; }
        }
        return out;
    }

    std::wstring Hotp(const std::string& secret, long long counter, int digits) {
        std::string msg(8, '\0');
        for (int i = 7; i >= 0; i--) { msg[i] = (char)(counter & 0xFF); counter >>= 8; }
        auto h = Net::HmacSha1(secret, msg);
        if (h.size() != 20) return L"";
        int o = h[19] & 0x0F;
        unsigned bin = ((unsigned)(h[o] & 0x7F) << 24) | ((unsigned)(unsigned char)h[o + 1] << 16) | ((unsigned)(unsigned char)h[o + 2] << 8) | (unsigned char)h[o + 3];
        unsigned mod = 1; for (int i = 0; i < digits; i++) mod *= 10;
        auto s = std::to_wstring(bin % mod);
        return std::wstring(digits - s.size(), L'0') + s;
    }

    std::wstring Code(const std::wstring& key, long long unixSec) {
        auto secret = Decode(key);
        if (secret.empty()) return L"";
        auto code = Hotp(secret, unixSec / 30);
        SecureZeroMemory(&secret[0], secret.size());
        return code;
    }

    int SecondsLeft(long long unixSec) { return 30 - (int)(unixSec % 30); }
    std::wstring Spaced(const std::wstring& code) { return code.size() == 6 ? code.substr(0, 3) + L" " + code.substr(3) : code; }
}

// ================================================================ the saved details

namespace Creds {
    static bool blocked = false;   // the file is there but couldn't be read: never written over

    std::wstring File() { return Util::Join(Paths::Root, L"logins.dat"); }

    json::Value ToJson(const Cred& c) {
        auto o = json::Value::MakeObject();
        o[L"id"] = c.id; o[L"email"] = c.email; o[L"password"] = c.password; o[L"key"] = c.key;
        if (!c.newPassword.empty()) o[L"newPassword"] = c.newPassword;
        if (!c.newKey.empty()) o[L"newKey"] = c.newKey;
        if (!c.oldPassword.empty()) o[L"oldPassword"] = c.oldPassword;
        if (!c.oldKey.empty()) o[L"oldKey"] = c.oldKey;
        if (!c.order.empty()) o[L"order"] = c.order;
        if (!c.product.empty()) o[L"product"] = c.product;
        if (!c.note.empty()) o[L"note"] = c.note;
        o[L"received"] = (double)c.received;
        if (c.warrantyUntil) o[L"warrantyUntil"] = (double)c.warrantyUntil;
        if (c.paidUntil) o[L"paidUntil"] = (double)c.paidUntil;
        o[L"passwordDone"] = c.passwordDone; o[L"keyDone"] = c.keyDone; o[L"phoneDone"] = c.phoneDone;
        if (!c.accountId.empty()) o[L"accountId"] = c.accountId;
        if (c.warnedWarranty) o[L"warnedWarranty"] = true;
        if (c.warnedPaid) o[L"warnedPaid"] = true;
        return o;
    }

    Cred FromJson(const json::Value& v) {
        Cred c;
        c.id = v.Str(L"id"); c.email = v.Str(L"email"); c.password = v.Str(L"password"); c.key = v.Str(L"key");
        c.newPassword = v.Str(L"newPassword"); c.newKey = v.Str(L"newKey");
        c.oldPassword = v.Str(L"oldPassword"); c.oldKey = v.Str(L"oldKey");
        c.order = v.Str(L"order"); c.product = v.Str(L"product"); c.note = v.Str(L"note");
        c.received = (long long)v.Num(L"received", 0);
        c.warrantyUntil = (long long)v.Num(L"warrantyUntil", 0); c.paidUntil = (long long)v.Num(L"paidUntil", 0);
        c.passwordDone = v.Flag(L"passwordDone", false); c.keyDone = v.Flag(L"keyDone", false); c.phoneDone = v.Flag(L"phoneDone", false);
        c.accountId = v.Str(L"accountId");
        c.warnedWarranty = v.Flag(L"warnedWarranty", false); c.warnedPaid = v.Flag(L"warnedPaid", false);
        return c;
    }

    std::vector<Cred> Load() {
        std::vector<Cred> list;
        auto file = File();
        if (!Util::Exists(file)) { blocked = false; return list; }
        std::string plain;
        json::Value v;
        if (!Net::Unprotect(Util::ReadBytes(file), plain) || !json::Parser::Parse(Util::Wide(plain), v)) {
            if (!blocked) Core::Log(L"The saved sign-in details in " + file + L" can't be read on this Windows account; nothing is written over them");
            blocked = true;
            return list;
        }
        blocked = false;
        if (auto arr = v.Find(L"logins"))
            for (auto& o : arr->arr) { auto c = FromJson(o); if (!c.id.empty() && !c.email.empty()) list.push_back(c); }
        if (!plain.empty()) SecureZeroMemory(&plain[0], plain.size());
        return list;
    }

    void Save(const std::vector<Cred>& list) {
        if (blocked && Util::Exists(File())) throw std::wstring(Tr(L"The saved sign-in details can't be read, so nothing was written over them", L"Сохранённые данные входа не читаются, поэтому поверх них ничего не записано"));
        auto v = json::Value::MakeObject();
        v[L"version"] = 1;
        auto& arr = v[L"logins"]; arr = json::Value::MakeArray();
        for (auto& c : list) arr.arr.push_back(ToJson(c));
        std::wstring text; json::Write(text, v);
        auto plain = Util::Utf8(text);
        SecureZeroMemory(&text[0], text.size() * sizeof(wchar_t));
        auto sealed = Net::Protect(plain);
        if (!plain.empty()) SecureZeroMemory(&plain[0], plain.size());
        CreateDirectoryW(Paths::Root.c_str(), nullptr);
        if (sealed.empty() || !Util::WriteBytes(File(), sealed)) throw std::wstring(Tr(L"Couldn't save the sign-in details", L"Не удалось сохранить данные входа"));
    }

    // ---- reading orders

    static std::wstring NewId() { auto h = Net::Hex(Net::Random(6)); return h; }

    static bool IsEmail(const std::wstring& s) {
        auto at = s.find(L'@');
        return at != std::wstring::npos && at > 0 && s.find(L'.', at) != std::wstring::npos && s.find_first_of(L" \t") == std::wstring::npos && s.back() != L'.';
    }

    static std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep, size_t most = 99) {
        std::vector<std::wstring> out;
        size_t a = 0;
        while (out.size() + 1 < most) {
            auto b = s.find(sep, a);
            if (b == std::wstring::npos) break;
            out.push_back(Util::Trim(s.substr(a, b - a)));
            a = b + 1;
        }
        out.push_back(Util::Trim(s.substr(a)));
        return out;
    }

    // "Заказ: X" / "Order: X": the value after the colon, for a line that starts with one of the words.
    static bool Labelled(const std::wstring& line, std::initializer_list<const wchar_t*> words, std::wstring& value) {
        auto lower = Util::Lower(line);
        for (auto w : words) {
            if (lower.rfind(w, 0) != 0) continue;
            auto colon = line.find(L':');
            if (colon == std::wstring::npos || colon > wcslen(w) + 12) return false;
            value = Util::Trim(line.substr(colon + 1));
            return !value.empty();
        }
        return false;
    }

    // "ChatGPT plus | Apple Pay | 1M | W24H": 1 month paid, a 24-hour warranty.
    static void ReadProduct(const std::wstring& product, int& months, long long& warrantyMs) {
        std::wstring token;
        auto take = [&] {
            auto t = Util::Lower(token); token.clear();
            if (t.size() >= 2 && t.back() == L'm' && t.find_first_not_of(L"0123456789") == t.size() - 1) months = _wtoi(t.c_str());
            else if (t.size() >= 3 && t[0] == L'w' && (t.back() == L'h' || t.back() == L'd') && t.find_first_not_of(L"0123456789", 1) == t.size() - 1) {
                long long n = _wtoi(t.c_str() + 1);
                warrantyMs = n * (t.back() == L'h' ? 3600000ll : 86400000ll);
            }
        };
        for (wchar_t c : product) { if (c == L'|' || c == L' ' || c == L',' || c == L'/') take(); else token += c; }
        take();
    }

    static Cred ReadLine(std::wstring line, bool& looked) {
        Cred c;
        looked = false;
        // "1) email|…" or "1. email|…"
        if (!line.empty() && iswdigit(line[0])) {
            size_t i = 0; while (i < line.size() && iswdigit(line[i])) i++;
            if (i < line.size() && (line[i] == L')' || line[i] == L'.')) line = Util::Trim(line.substr(i + 1));
        }
        if (line.find(L'@') == std::wstring::npos) return c;
        std::vector<std::wstring> f;
        if (line.find(L'|') != std::wstring::npos) f = Split(line, L'|');
        else if (line.find(L'\t') != std::wstring::npos) f = Split(line, L'\t');
        else if (line.find(L';') != std::wstring::npos) f = Split(line, L';');
        else if (line.find(L':') != std::wstring::npos) f = Split(line, L':', 3);   // the key may be a link with a colon
        else return c;
        looked = true;
        if (f.size() < 2 || !IsEmail(f[0]) || f[1].empty()) return c;
        c.email = f[0]; c.password = f[1];
        if (f.size() > 2 && !f[2].empty()) {
            c.key = Totp::Clean(f[2]);
            if (c.key.empty()) c.note = L"2FA: " + f[2];
        }
        for (size_t i = 3; i < f.size(); i++) if (!f[i].empty()) c.note += (c.note.empty() ? L"" : L" | ") + f[i];
        return c;
    }

    Parsed Parse(const std::wstring& text, long long nowMs) {
        Parsed p;
        size_t a = 0;
        while (a <= text.size()) {
            auto b = text.find(L'\n', a);
            auto line = Util::Trim(text.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a));
            a = b == std::wstring::npos ? text.size() + 1 : b + 1;
            if (line.empty()) continue;
            std::wstring value;
            if (Labelled(line, { L"заказ", L"order", L"номер заказа" }, value)) { p.order = value; continue; }
            if (Labelled(line, { L"товар", L"product", L"item" }, value)) { p.product = value; ReadProduct(value, p.months, p.warrantyMs); continue; }
            bool looked = false;
            auto c = ReadLine(line, looked);
            if (c.email.empty()) { if (looked) p.bad++; continue; }
            // The same account twice in one message: once is enough.
            bool dup = false;
            for (auto& x : p.creds) dup |= Util::Lower(x.email) == Util::Lower(c.email);
            if (!dup) p.creds.push_back(c);
        }
        for (auto& c : p.creds) {
            c.id = NewId();
            c.order = p.order; c.product = p.product;
            c.received = nowMs;
            if (p.warrantyMs) c.warrantyUntil = nowMs + p.warrantyMs;
            if (p.months) c.paidUntil = nowMs + p.months * 30ll * 86400000ll;
        }
        return p;
    }

    Merged Merge(std::vector<Cred>& list, const Parsed& p) {
        Merged m;
        for (auto& c : p.creds) {
            Cred* e = nullptr;
            for (auto& x : list) if (Util::Lower(x.email) == Util::Lower(c.email)) { e = &x; break; }
            if (!e) { list.push_back(c); m.added++; m.ids.push_back(c.id); continue; }
            bool changed = false;
            auto fill = [&](std::wstring& to, const std::wstring& from) { if (to.empty() && !from.empty()) { to = from; changed = true; } };
            // Another order for the same account (a new month): its dates and, unless changed here since, its details.
            if (!c.order.empty() && c.order != e->order) {
                e->order = c.order; e->product = c.product; e->received = c.received;
                e->warrantyUntil = c.warrantyUntil; e->paidUntil = c.paidUntil;
                e->warnedWarranty = e->warnedPaid = false;
                if (!e->passwordDone && !c.password.empty()) e->password = c.password;
                if (!e->keyDone && !c.key.empty()) e->key = c.key;
                changed = true;
            }
            fill(e->password, c.password); fill(e->key, c.key); fill(e->note, c.note);
            if (!e->paidUntil && c.paidUntil) { e->paidUntil = c.paidUntil; changed = true; }
            if (!e->warrantyUntil && c.warrantyUntil) { e->warrantyUntil = c.warrantyUntil; changed = true; }
            if (changed) m.updated++;
            m.ids.push_back(e->id);
        }
        return m;
    }

    int MergeJson(const json::Value& arr) {
        auto list = Load();
        int added = 0;
        for (auto& o : arr.arr) {
            auto c = FromJson(o);
            if (c.email.empty()) continue;
            bool have = false;
            for (auto& x : list) have |= Util::Lower(x.email) == Util::Lower(c.email);
            if (have) continue;
            for (auto& x : list) if (x.id == c.id) c.id.clear();
            if (c.id.empty()) c.id = NewId();
            c.accountId.clear();   // the accounts get their own ids on this PC; the email links them again
            list.push_back(c); added++;
        }
        if (added) Save(list);
        return added;
    }

    std::wstring NewPassword() {
        // No look-alikes (l, I, 1, O, 0), so it can be read off the screen if it must be.
        static const wchar_t* sets[] = { L"abcdefghijkmnopqrstuvwxyz", L"ABCDEFGHJKLMNPQRSTUVWXYZ", L"23456789", L"!@#$%*-_+=?" };
        auto r = Net::Random(40);
        std::wstring pw;
        for (int i = 0; i < 20; i++) {
            auto set = sets[i < 4 ? i : ((unsigned char)r[i] % 10 < 7 ? (unsigned char)r[i] % 2 : 2 + (unsigned char)r[i] % 2)];
            pw += set[(unsigned char)r[20 + i] % wcslen(set)];
        }
        // The four required kinds sit at the front; shuffle them in.
        for (int i = 19; i > 0; i--) std::swap(pw[i], pw[(unsigned char)r[i] % (i + 1)]);
        return pw;
    }

    Stage StageOf(const Cred& c, bool signedIn, long long paidUntil, long long nowMs) {
        if (paidUntil && paidUntil <= nowMs) return Stage::Expired;
        if (signedIn) return Stage::Ready;
        if (c.passwordDone && c.keyDone && c.phoneDone) return Stage::SignIn;
        if (c.passwordDone || c.keyDone || c.phoneDone) return Stage::Securing;
        return Stage::New;
    }

    int StepsDone(const Cred& c, bool signedIn) { return (int)c.passwordDone + (int)c.keyDone + (int)c.phoneDone + (int)signedIn; }
}

// ================================================================ a browser per account

namespace Browser {
    struct Found { std::wstring exe, name; int kind = 0; };   // kind 1: Chromium flags, 2: Firefox flags

    static Found Detect() {
        Found f;
        wchar_t buf[MAX_PATH * 2]; DWORD n = ARRAYSIZE(buf);
        if (SUCCEEDED(AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_EXECUTABLE, L"https", L"open", buf, &n))) {
            std::wstring exe = buf, lower = Util::Lower(exe), leaf = Util::Leaf(lower);
            struct { const wchar_t* leaf; const wchar_t* name; int kind; } known[] = {
                { L"msedge.exe", L"Microsoft Edge", 1 }, { L"chrome.exe", L"Google Chrome", 1 }, { L"brave.exe", L"Brave", 1 },
                { L"vivaldi.exe", L"Vivaldi", 1 }, { L"chromium.exe", L"Chromium", 1 }, { L"firefox.exe", L"Firefox", 2 } };
            for (auto& k : known) if (leaf == k.leaf) { f = { exe, k.name, k.kind }; break; }
            if (!f.kind && leaf == L"browser.exe" && lower.find(L"yandex") != std::wstring::npos) f = { exe, L"Yandex Browser", 1 };
        }
        if (f.kind && Util::Exists(f.exe)) return f;
        // Anything else: Edge, which every Windows 10 and 11 has.
        n = sizeof buf;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe", nullptr, RRF_RT_REG_SZ, nullptr, buf, &n) == ERROR_SUCCESS && Util::Exists(buf))
            return { buf, L"Microsoft Edge", 1 };
        for (auto var : { L"ProgramFiles(x86)", L"ProgramFiles" }) {
            wchar_t pf[MAX_PATH];
            if (GetEnvironmentVariableW(var, pf, MAX_PATH)) {
                auto p = std::wstring(pf) + L"\\Microsoft\\Edge\\Application\\msedge.exe";
                if (Util::Exists(p)) return { p, L"Microsoft Edge", 1 };
            }
        }
        return {};
    }

    static const Found& Get() { static Found f = Detect(); return f; }

    std::wstring Profile(const std::wstring& credId) { return Util::Join(Paths::Root, L"Browsers\\" + credId); }
    std::wstring Name() { auto& f = Get(); return f.kind ? f.name : std::wstring(Tr(L"your browser", L"ваш браузер")); }

    bool Open(const std::wstring& credId, const std::wstring& url) {
        if (Test::On()) { Test::Trace(L"browser|" + credId + L"|" + url); return true; }
        auto& f = Get();
        if (!f.kind) return Util::Open(url);
        auto dir = Profile(credId);
        CreateDirectoryW(Paths::Root.c_str(), nullptr);
        CreateDirectoryW(Util::Join(Paths::Root, L"Browsers").c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring cmd = L"\"" + f.exe + L"\" " + (f.kind == 1
            ? L"--user-data-dir=\"" + dir + L"\" --no-first-run --no-default-browser-check --new-window \"" + url + L"\""
            : L"-profile \"" + dir + L"\" -no-remote -new-window \"" + url + L"\"");
        STARTUPINFOW si{ sizeof si }; PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            Core::Log(L"Couldn't start " + f.exe + Util::Format(L" (error %lu)", GetLastError()));
            return Util::Open(url);
        }
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return true;
    }

    void Forget(const std::wstring& credId) {
        if (credId.empty()) return;
        if (Test::On()) { Test::Trace(L"browser-forget|" + credId); return; }
        auto dir = Profile(credId);
        if (!Util::IsDir(dir)) return;
        std::wstring from = dir; from.push_back(L'\0');
        SHFILEOPSTRUCTW op{}; op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_NO_UI;
        SHFileOperationW(&op);
    }
}
