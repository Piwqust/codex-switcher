// Codex itself: where it keeps its sign-in, where the app is installed, and which Codex
// processes run right now.
#pragma once
#include "core.h"

namespace Codex {
    struct Install {
        std::wstring kind;           // L"Msix" (Microsoft Store), L"Classic" (an exe you pointed at) or empty
        std::wstring exe, version, aumid;
        bool found() const { return !exe.empty(); }
    };
    Install App();                   // a copy: Detect runs on several threads
    void Detect();                   // one registry key; cheap enough to run before every launch
    // What the app is called on this PC: the Store's Codex app shows as "ChatGPT" (app\ChatGPT.exe)
    // since it became part of ChatGPT; an older one, or one you pointed at, is "Codex".
    std::wstring AppName();

    std::wstring Home();             // CODEX_HOME, or %USERPROFILE%\.codex
    std::wstring AuthFile();         // <home>\auth.json, the sign-in every Codex reads
    bool KeyringStore();             // config.toml keeps the sign-in in Windows Credential Manager ("keyring" or "auto")

    struct Processes {
        std::vector<DWORD> app;      // the desktop app's main processes
        bool appVisible = false;     // one of them shows a window (not just the tray)
        std::vector<DWORD> cli;      // Codex running in a terminal
        std::vector<DWORD> helpers;  // app servers that editors such as VS Code start; they come back on their own
        std::map<DWORD, ULONGLONG> started;   // start times, so a pid reused meanwhile is never ended
        DWORD daemon = 0;            // Codex's shared background app server, when one runs
        bool appRunning() const { return !app.empty(); }
        bool anyRunning() const { return !app.empty() || !cli.empty() || !helpers.empty() || daemon; }
    };
    Processes Scan();
    // Ends the app's whole process tree, the app servers it started included. Returns once
    // they are gone, so a new start isn't handed to the dying copy.
    bool CloseApp(const Processes& p);
    void EndHelpers(const Processes& p);
    bool StartApp();                 // through the package, as the Start menu does
    bool ShowApp();                  // brings an open window forward; false when there is none
}
