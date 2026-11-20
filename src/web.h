// The account list in a browser: on this PC, or from a phone or another computer on the same
// network (or over Tailscale) when that is allowed. Off by default. Every page and request
// needs the secret key in its link; without it the server answers nothing useful.
#pragma once
#include "core.h"

namespace Web {
    void Apply();                    // starts, stops or moves the server to match the settings
    void Stop();
    bool Running();
    std::wstring Problem();          // why it couldn't start (the port is taken, …)
    int Port();
    std::wstring Url(bool local);    // http://localhost:<port>/?key=…, or with this PC's network address
    std::wstring NewKey();
    HANDLE StopSignal();             // set while the server stops: work waiting for the UI thread gives up
}
