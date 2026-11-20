#include "recovery.h"
#include "codex.h"
#include "net.h"
#include <shellapi.h>
#include <algorithm>
#include <atomic>

namespace Recovery {
    namespace {
        const long long Window = 24ll * 3600 * 1000;    // ms
        const size_t MaxSessions = 32;
        const DWORD Budget = 120000, PerSession = 60000;
        const size_t Tail = 2u << 20;                   // how a log ends is in its last records
        const char* Prompt = "Continue the previous task.";
        std::atomic<bool> cancelled{ false };

        std::vector<std::string> Lines(const std::string& text, size_t from = 0) {
            std::vector<std::string> out;
            size_t s = from;
            while (s < text.size()) {
                auto e = text.find('\n', s);
                if (e == std::string::npos) { out.push_back(text.substr(s)); break; }
                out.push_back(text.substr(s, e - s));
                s = e + 1;
            }
            return out;
        }

        bool Parse(const std::string& line, json::Value& v) { return json::Parser::Parse(Util::Wide(line), v) && v.type == json::Value::Object; }

        // [from, from + limit) of a log Codex may be writing to. False when it can't be opened or
        // is shorter than `from`: moved, archived or rewritten.
        bool ReadRange(const std::wstring& path, unsigned long long from, size_t limit, std::string& out, unsigned long long* size = nullptr) {
            out.clear();
            HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) return false;
            LARGE_INTEGER len{}, at{}; at.QuadPart = (LONGLONG)from;
            bool ok = GetFileSizeEx(h, &len) && (unsigned long long)len.QuadPart >= from && SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
            if (ok) {
                if (size) *size = (unsigned long long)len.QuadPart;
                out.resize((size_t)std::min<unsigned long long>(limit, (unsigned long long)len.QuadPart - from));
                size_t done = 0;
                while (done < out.size()) {
                    DWORD got = 0;
                    if (!ReadFile(h, &out[done], (DWORD)std::min<size_t>(out.size() - done, 1u << 20), &got, nullptr) || !got) break;
                    done += got;
                }
                out.resize(done);
            }
            CloseHandle(h);
            return ok;
        }

        // The session header, the log's first line.
        bool FirstLine(const std::wstring& path, std::string& line) {
            std::string text;
            if (!ReadRange(path, 0, 1u << 20, text)) return false;
            auto e = text.find('\n');
            if (e == std::string::npos) return false;
            line = text.substr(0, e);
            return true;
        }

        // The whole lines in the log's last couple of megabytes.
        bool LastLines(const std::wstring& path, std::vector<std::string>& lines) {
            unsigned long long size = 0;
            std::string text;
            if (!ReadRange(path, 0, 0, text, &size)) return false;
            unsigned long long from = size > Tail ? size - Tail : 0;
            if (!ReadRange(path, from, Tail, text)) return false;
            size_t start = 0;
            if (from > 0) { auto e = text.find('\n'); if (e == std::string::npos) return false; start = e + 1; }
            lines = Lines(text, start);
            return true;
        }

        bool IsLimitError(const json::Value* e) {
            if (!e || e->type != json::Value::Object) return false;
            if (e->Str(L"codex_error_info") == L"usage_limit_exceeded") return true;
            auto m = Util::Trim(e->Str(L"message"));
            return m.rfind(L"You've hit your usage limit.", 0) == 0 || m.rfind(L"You’ve hit your usage limit.", 0) == 0;
        }

        std::wstring Uuid() {
            auto r = Net::Random(16);
            r[6] = (char)((r[6] & 0x0F) | 0x40); r[8] = (char)((r[8] & 0x3F) | 0x80);
            auto h = Net::Hex(r);
            return h.substr(0, 8) + L"-" + h.substr(8, 4) + L"-" + h.substr(12, 4) + L"-" + h.substr(16, 4) + L"-" + h.substr(20);
        }

        // What a receipt remembers: where the log ended when the message was sent, the
        // message itself (unique, so only its own turn confirms it) and whether queue took it.
        struct Receipt { unsigned long long offset = 0; std::string prompt; bool queued = false; std::wstring error; };

        std::wstring ReceiptFile(const std::wstring& id) { return Paths::Root + L"\\recovery\\" + id + L".json"; }

        bool LoadReceipt(const std::wstring& id, Receipt& r) {
            json::Value v;
            if (!json::Parser::Parse(Util::ReadText(ReceiptFile(id)), v)) return false;
            r.offset = (unsigned long long)v.Num(L"offset", 0); r.prompt = Util::Utf8(v.Str(L"prompt"));
            r.queued = v.Flag(L"queued", false); r.error = v.Str(L"error");
            return !r.prompt.empty();
        }

        bool SaveReceipt(const std::wstring& id, const Receipt& r) {
            CreateDirectoryW((Paths::Root + L"\\recovery").c_str(), nullptr);
            auto v = json::Value::MakeObject();
            v[L"offset"] = (double)r.offset; v[L"prompt"] = Util::Wide(r.prompt); v[L"queued"] = r.queued;
            if (!r.error.empty()) v[L"error"] = r.error;
            std::wstring text; json::Write(text, v);
            return Util::WriteText(ReceiptFile(id), text, false);
        }

        // What the log shows since the receipt.
        enum class Seen {
            Pending,      // nothing of it yet
            Done,         // its turn started or finished well
            Failed,       // its turn ran and stopped with an error: it was delivered all the same
            Unknown,      // the log can't be read or is shorter than before: keep the receipt
            Superseded,   // someone wrote in the conversation since, and the message never showed
        };

        Seen Result(const std::wstring& path, const Receipt& r, std::wstring& why) {
            std::string text;
            if (!ReadRange(path, r.offset, 64u << 20, text)) {
                why = Tr(L"The conversation's log was moved or changed. Open it to check", L"Журнал разговора перемещён или изменился. Откройте его и проверьте");
                return Seen::Unknown;
            }
            bool started = false, matched = false, other = false;
            std::wstring turn;
            auto prompt = Util::Wide(r.prompt);
            for (auto& line : Lines(text)) {
                if (line.find("\"event_msg\"") == std::string::npos && line.find("\"response_item\"") == std::string::npos) continue;
                json::Value v;
                if (!Parse(line, v)) continue;
                auto payload = v.Find(L"payload");
                if (!payload) continue;
                auto type = v.Str(L"type"), pt = payload->Str(L"type");
                if (started && type == L"response_item" && payload->Str(L"role") == L"user") {
                    std::wstring msgTurn;
                    if (auto meta = payload->Find(L"internal_chat_message_metadata_passthrough")) msgTurn = meta->Str(L"turn_id");
                    if (msgTurn.empty() || msgTurn == turn)
                        if (auto content = payload->Find(L"content"))
                            for (auto& item : content->arr) matched |= item.Str(L"text") == prompt;
                }
                if (type != L"event_msg") continue;
                if (pt == L"task_started" || pt == L"turn_started") {
                    if (matched) return Seen::Done;
                    started = true; turn = payload->Str(L"turn_id");
                } else if (pt == L"user_message") {
                    if (payload->Str(L"message") == prompt) matched |= started;
                    else other = true;
                } else if (pt == L"task_complete" || pt == L"turn_failed" || pt == L"turn_aborted") {
                    if (matched) {
                        auto err = payload->Find(L"error");
                        if (pt == L"task_complete" && (!err || err->IsNull())) return Seen::Done;
                        why = Tr(L"The continued turn stopped or failed. Open the conversation to check", L"Продолженный ход остановился или завершился ошибкой. Откройте разговор и проверьте");
                        return Seen::Failed;
                    }
                    started = false;
                } else if (pt == L"error" && matched) {
                    why = Tr(L"The continued turn reported an error. Open the conversation to check", L"Продолженный ход сообщил об ошибке. Откройте разговор и проверьте");
                    return Seen::Failed;
                }
            }
            return matched ? Seen::Done : other ? Seen::Superseded : Seen::Pending;
        }

        // `codex queue` keeps the message until the conversation is opened. Run without a
        // window, ten seconds at most. Yes, No (it surely wasn't queued) or Maybe.
        enum class Queued { Yes, No, Maybe };
        Queued Queue(const std::wstring& cli, const std::wstring& id, const std::string& prompt, std::wstring& error) {
            auto arg = [](const std::wstring& s) {
                std::wstring out = L"\"";
                for (wchar_t c : s) { if (c == L'"') out += L"\\\""; else out += c; }
                return out + L"\"";
            };
            std::wstring cmd = arg(cli) + L" queue --thread " + arg(id) + L" --message " + arg(Util::Wide(prompt));
            STARTUPINFOW si{ sizeof si }; PROCESS_INFORMATION pi{};
            si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
            if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                error = Util::Format(Tr(L"Couldn't run the Codex CLI (code %lu)", L"Не удалось запустить Codex CLI (код %lu)"), GetLastError());
                return Queued::No;
            }
            Queued result = Queued::Maybe;
            DWORD wait = WAIT_TIMEOUT;
            for (int i = 0; i < 100 && wait == WAIT_TIMEOUT && !cancelled; i++) wait = WaitForSingleObject(pi.hProcess, 100);
            if (wait == WAIT_OBJECT_0) {
                DWORD code = 1;
                GetExitCodeProcess(pi.hProcess, &code);
                if (code == 0) result = Queued::Yes;
                else { result = Queued::No; error = Util::Format(Tr(L"The Codex CLI ended with code %lu", L"Codex CLI завершился с кодом %lu"), code); }
            } else if (!cancelled) {
                TerminateProcess(pi.hProcess, 1);
                error = Tr(L"The Codex CLI took longer than 10 seconds", L"Codex CLI не ответил за 10 секунд");
            }
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            return result;
        }

        std::wstring Interrupted() {
            return Tr(L"Stopped because accounts were switched meanwhile. If its message was queued, it waits in the conversation",
                      L"Остановлено: тем временем переключили аккаунт. Если сообщение встало в очередь, оно ждёт в разговоре");
        }

        // One conversation: queue the continuation (once, whatever happens), open the chat so
        // Codex sends it, and wait until its turn shows in the log.
        bool Resume(const Session& s, const std::wstring& cli, ULONGLONG deadline, std::wstring& why) {
            unsigned long long length = 0;
            std::string none;
            if (!ReadRange(s.path, 0, 0, none, &length)) { why = Tr(L"The conversation's log can't be read", L"Журнал разговора не читается"); return false; }
            Receipt r;
            bool pending = LoadReceipt(s.id, r);
            if (pending) {
                std::wstring w;
                auto seen = Result(s.path, r, w);
                // It may still wait in the queue: never queue a second one on a guess.
                if (seen == Seen::Unknown) { why = w; return false; }
                if (seen != Seen::Pending) pending = false;   // that one arrived, or the conversation went on without it
            }
            if (!pending) {
                r = Receipt();
                r.offset = length;
                r.prompt = std::string(Prompt) + "\n\n[Codex Account Switcher recovery: " + Util::Utf8(Uuid()) + "]";
                // Written down before anything is sent, so a crash or a second run can't send twice.
                if (!SaveReceipt(s.id, r)) { why = Tr(L"Couldn't note down the message before sending it, so it wasn't sent", L"Не удалось записать сообщение перед отправкой, поэтому оно не отправлено"); return false; }
                std::wstring err;
                auto q = Queue(cli, s.id, r.prompt, err);
                if (q == Queued::No) { DeleteFileW(ReceiptFile(s.id).c_str()); why = err; return false; }
                r.queued = q == Queued::Yes; r.error = err;
                SaveReceipt(s.id, r);
            }
            if (cancelled) { why = Interrupted(); return false; }
            Open(s.id);
            auto until = std::min<ULONGLONG>(deadline, GetTickCount64() + PerSession);
            for (;;) {
                auto seen = Result(s.path, r, why);
                if (seen == Seen::Done || seen == Seen::Failed) { DeleteFileW(ReceiptFile(s.id).c_str()); return seen == Seen::Done; }
                if (seen == Seen::Unknown) return false;
                if (cancelled) { why = Interrupted(); return false; }
                if (GetTickCount64() >= until) break;
                Sleep(500);
            }
            why = r.queued ? std::wstring(Tr(L"The continuation is queued but its turn didn't start yet. Open the conversation; retrying won't send it twice", L"Продолжение в очереди, но ход ещё не начался. Откройте разговор; повтор не отправит его дважды"))
                           : (r.error.empty() ? std::wstring(Tr(L"It isn't known whether the message was queued. Open the conversation and check its queue", L"Неизвестно, встало ли сообщение в очередь. Откройте разговор и проверьте")) : r.error);
            return false;
        }

        void Collect(const std::wstring& dir, long long now, std::vector<std::pair<ULONGLONG, Session>>& out) {
            WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return;
            do {
                if (fd.cFileName[0] == L'.') continue;
                auto path = dir + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) Collect(path, now, out); continue; }
                std::wstring name = fd.cFileName;
                if (name.size() < 6 || _wcsicmp(name.c_str() + name.size() - 6, L".jsonl") != 0) continue;
                ULONGLONG ft = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
                long long ms = (long long)((ft - 116444736000000000ull) / 10000);
                if (now - ms > Window) continue;
                std::string first;
                std::vector<std::string> last;
                if (!FirstLine(path, first)) continue;
                auto id = ThreadId(first);
                if (id.empty() || !LastLines(path, last) || !StoppedByLimit(last)) continue;
                out.push_back({ ft, { id, path } });
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    void Cancel(bool on) { cancelled = on; }

    bool StoppedByLimit(const std::vector<std::string>& lines) {
        static const char* turnEvents[] = { "task_started", "turn_started", "user_message", "turn_aborted", "task_complete", "turn_failed", "\"error\"" };
        bool stopped = false;
        for (auto& raw : lines) {
            size_t end = raw.size();
            while (end && (raw[end - 1] == '\r' || raw[end - 1] == ' ')) end--;
            if (!end) continue;
            // A record cut short (Codex writing it, or a crash) leaves the answer unknown.
            if (raw[end - 1] != '}') { stopped = false; continue; }
            // Only records that can change the answer are parsed; a log runs to megabytes.
            bool ev = raw.find("\"event_msg\"") != std::string::npos, item = raw.find("\"response_item\"") != std::string::npos;
            if (!ev && !item) continue;
            if (!ev && raw.find("\"user\"") == std::string::npos) continue;
            if (ev && !item && std::none_of(std::begin(turnEvents), std::end(turnEvents), [&](const char* k) { return raw.find(k) != std::string::npos; })) continue;
            json::Value v;
            if (!Parse(raw, v)) { stopped = false; continue; }
            auto payload = v.Find(L"payload");
            if (!payload) continue;
            auto type = v.Str(L"type"), pt = payload->Str(L"type");
            if (type == L"response_item" && pt == L"message" && payload->Str(L"role") == L"user") stopped = false;
            // Never error text quoted in messages or tool output: only Codex's own events count.
            if (type != L"event_msg") continue;
            if (pt == L"task_started" || pt == L"turn_started" || pt == L"user_message" || pt == L"turn_aborted") stopped = false;
            else if (pt == L"task_complete" || pt == L"turn_failed") stopped = IsLimitError(payload->Find(L"error"));
            else if (pt == L"error") stopped = IsLimitError(payload);
        }
        return stopped;
    }

    std::wstring ThreadId(const std::string& firstLine) {
        json::Value v;
        if (!Parse(firstLine, v) || v.Str(L"type") != L"session_meta") return L"";
        auto p = v.Find(L"payload");
        if (!p) return L"";
        // Subagents have logs of their own, but their parent conversation drives them.
        if (auto src = p->Find(L"source")) {
            if (src->type == json::Value::Object && src->Find(L"subagent")) return L"";
            if (src->type == json::Value::String && src->s == L"subagent") return L"";
        }
        auto agent = p->Str(L"agent_path");
        if (!agent.empty() && agent != L"/root") return L"";
        auto id = p->Str(L"id");
        if (id.size() != 36 || id[8] != L'-' || id[13] != L'-' || id[18] != L'-' || id[23] != L'-') return L"";
        for (wchar_t c : id) if (c != L'-' && !iswxdigit(c)) return L"";
        return id;
    }

    std::vector<Session> FindStopped() {
        std::vector<std::pair<ULONGLONG, Session>> found;
        Collect(Codex::Home() + L"\\sessions", Core::NowMs(), found);
        std::sort(found.begin(), found.end(), [](const std::pair<ULONGLONG, Session>& a, const std::pair<ULONGLONG, Session>& b) { return a.first > b.first; });
        std::vector<Session> out;
        for (auto& f : found) { if (out.size() >= MaxSessions) break; out.push_back(f.second); }
        return out;
    }

    static std::wstring FindExe(const std::wstring& dir, int depth) {
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return L"";
        std::wstring hit;
        do {
            if (fd.cFileName[0] == L'.') continue;
            auto path = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth > 0 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) hit = FindExe(path, depth - 1); }
            else if (_wcsicmp(fd.cFileName, L"codex.exe") == 0 && Util::Lower(dir).find(L"\\bin") != std::wstring::npos) hit = path;
        } while (hit.empty() && FindNextFileW(h, &fd));
        FindClose(h);
        return hit;
    }

    // CODEX_BIN, the npm package's own binary, or the one inside the Codex app.
    std::wstring Cli() {
        wchar_t b[MAX_PATH * 2];
        if (GetEnvironmentVariableW(L"CODEX_BIN", b, ARRAYSIZE(b)) && Util::Exists(b)) return b;
        if (GetEnvironmentVariableW(L"APPDATA", b, ARRAYSIZE(b))) {
            auto hit = FindExe(std::wstring(b) + L"\\npm\\node_modules\\@openai\\codex", 6);
            if (!hit.empty()) return hit;
        }
        auto app = Codex::App();
        if (app.kind == L"Msix") {
            auto inApp = Util::Parent(app.exe) + L"\\resources\\codex.exe";
            if (Util::Exists(inApp)) return inApp;
        }
        return L"";
    }

    void Open(const std::wstring& id) {
        auto url = L"codex://threads/" + id;
        AllowSetForegroundWindow(ASFW_ANY);
        Util::Open(url);
    }

    Report Continue(const std::vector<Session>& sessions, const std::function<void(int, int)>& progress) {
        Report r;
        r.total = (int)sessions.size();
        auto cli = Cli();
        auto deadline = GetTickCount64() + Budget;
        for (size_t i = 0; i < sessions.size(); i++) {
            auto& s = sessions[i];
            if (progress) progress((int)i, r.total);
            if (cli.empty()) { r.failed.push_back({ s.id, Tr(L"The Codex CLI wasn't found: install it with npm i -g @openai/codex", L"Codex CLI не найден: установите его через npm i -g @openai/codex") }); continue; }
            if (cancelled) { r.failed.push_back({ s.id, Interrupted() }); continue; }
            if (GetTickCount64() >= deadline) {
                r.timedOut = true;
                r.failed.push_back({ s.id, Tr(L"Not tried: the two-minute limit ran out. Open the conversation to continue it", L"Не обработан: истекли две минуты. Откройте разговор, чтобы продолжить его") });
                continue;
            }
            // Someone may have continued it meanwhile.
            std::vector<std::string> last;
            if (!LastLines(s.path, last) || !StoppedByLimit(last)) { r.skipped++; continue; }
            std::wstring why;
            if (Resume(s, cli, deadline, why)) r.resumed++;
            else r.failed.push_back({ s.id, why });
        }
        if (progress) progress(r.total, r.total);
        return r;
    }
}
