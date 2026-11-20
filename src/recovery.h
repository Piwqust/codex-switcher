// Conversations the usage limit stopped, continued with the new account after a switch.
//
// Codex writes every conversation to <codex home>\sessions\…\rollout-*.jsonl. One whose last
// turn ended with the usage-limit error gets "Continue the previous task." through
// `codex queue`, and opening it in the Codex app (codex://threads/<id>) delivers that message.
// A receipt per conversation keeps a retry from sending a second one.
#pragma once
#include "core.h"
#include <functional>

namespace Recovery {
    struct Session { std::wstring id, path; };
    std::vector<Session> FindStopped();                 // the last 24 hours, newest first, at most 32
    bool StoppedByLimit(const std::vector<std::string>& lines);   // its log, line by line
    std::wstring ThreadId(const std::string& firstLine); // from the session_meta line; "" for subagents
    std::wstring Cli();                                  // a codex.exe that has `codex queue`, or ""

    struct Failure { std::wstring id, reason; };
    struct Report { int total = 0, resumed = 0, skipped = 0; std::vector<Failure> failed; bool timedOut = false; };
    // Two minutes at most in all. progress(done, total) runs on the calling thread.
    Report Continue(const std::vector<Session>& sessions, const std::function<void(int, int)>& progress);
    void Open(const std::wstring& id);                   // the conversation in the Codex app
    // Stops a run at its next step (a switch, quitting); Cancel(false) before starting one.
    void Cancel(bool on = true);
}
