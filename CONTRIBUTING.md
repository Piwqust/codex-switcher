# Contributing

Thanks for wanting to help. Issues and pull requests are welcome, in English or Russian.

## Building

You need Visual Studio 2022 or the Build Tools with the "Desktop development with C++" workload (and "MSVC ARM64 build tools" for ARM64).

```cmd
build.cmd                 :: x64   -> build\x64\CodexAccountSwitcher.exe
build.cmd arm64           :: ARM64 -> build\arm64\CodexAccountSwitcher.exe
build.cmd x64 build\x64\Tests.exe test
build\x64\Tests.exe       :: the unit tests: tokens, switching, limits, statistics, backups, importing, the sign-in server
build.cmd x64 build\x64\CompatTests.exe compat
build\x64\CompatTests.exe :: Codex Switcher's export formats: zlib, XChaCha20-Poly1305, PBKDF2, against known vectors
```

The exe uses the static runtime and needs nothing installed. Build without warnings.

## Layout

| File | What it is |
| --- | --- |
| `src/accounts.*` | The vault, who an account is, taking renewed tokens back, switching, limits, statistics, warm-ups, importing, backups |
| `src/login.cpp` | The browser sign-in: PKCE and the one-off server on localhost:1455 |
| `src/codex.*` | Where Codex keeps its sign-in, finding the app, its processes, restarting it |
| `src/recovery.*` | Finding conversations the usage limit stopped and continuing them through the Codex CLI |
| `src/web.*` | The account list in a browser: a small HTTP server on localhost (or the home network) |
| `src/compat.*` | Reading Codex Switcher's exports: zlib, XChaCha20-Poly1305 |
| `src/net.*` | HTTPS, base64url, SHA-256, PBKDF2, AES-GCM, DPAPI, JWT claims |
| `src/app.*` | The running switcher: the account list, the tray, hotkeys, timers, every action |
| `src/picker.cpp`, `flyout.cpp`, `settings.cpp`, `details.cpp`, `dialogs.cpp` | The windows |
| `src/gfx.*` | Drawing with Direct2D and DirectWrite, and the small immediate-mode UI layer |
| `src/shell.cpp`, `icons.inc` | Icons, shortcuts, start with Windows, installing, the jump list |
| `src/update.cpp` | Updating from GitHub releases |
| `src/core.*` | Paths, settings.json, the interface language, helpers, the error log |

Interface text sits in the code as `Tr(L"English", L"Русский")`: add both.

## Trying it without your accounts

`tools\demo.ps1` starts a separate switcher over four made-up accounts in `%TEMP%\cas-demo`. It never contacts OpenAI or touches Codex, your accounts, shortcuts, autostart or hotkeys. By default its windows open off screen, without focus or a taskbar button, and `tools\capture.ps1` saves them as pictures:

```powershell
.\tools\demo.ps1 -Theme dark -CodexOpen -Web
.\tools\capture.ps1 -Window picker -Out picker.png
.\tools\capture.ps1 -Window details -OpenDetails -Out details.png
.\tools\capture.ps1 -Window flyout -OpenFlyout -Out tray.png
.\tools\capture.ps1 -Window settings -ClickSettings -Scale 1.25 -Scroll 10 -Out settings.png   # -Scale: your display scale
.\tools\demo.ps1 -Visible -Language ru -Theme light   # on screen, to click around
```

With `-Web` the demo's browser list is at <http://localhost:3291/?key=demo-key-not-secret>. `tools\make-images.ps1 -Scale 1.25` redraws every picture in `docs/images` in both languages.

## Checking every button

`tools\uitest.ps1` clicks through the whole interface in the demo, off screen: every row, button, toggle, menu item and key of the account list, the tray list and menu, Settings, account details and the dialogs, including adding an account, renaming, deleting, backups both ways and the browser list. Each step is checked against what the demo did (its trace in `%TEMP%\cas-uitest\trace.log`, its settings and files), and the script ends with a count of passed and failed checks. Menus and file dialogs never show: the demo takes the item or file the script names, and signing in is simulated.

```powershell
.\tools\uitest.ps1                     # about two minutes
.\tools\uitest.ps1 -Language ru -Shots out   # in Russian, saving a picture of each window and dialog
```

Give a new control an `Id("name")` and add a step for it here. In the demo a control can be found by that name (`WM_APP+62` writes where each one is), and `Test::Trace` notes what an action did.

`CAS_ROOT=<folder>` runs the real thing, OpenAI included, over a folder of its own; with `CODEX_HOME` pointing at a copy of your Codex folder, nothing of yours is touched. It doesn't install itself, take hotkeys, start with Windows, or see, close or start the Codex app. `CAS_OFFSCREEN=1` keeps its windows off screen as in the demo. Delete that copy afterwards: it holds sign-ins.

## Before a pull request

- `build.cmd` and `build.cmd arm64` build without warnings, both test programs pass, and `tools\uitest.ps1` passes in both languages.
- Tried in the demo, in both themes and both languages.
- A line under "Unreleased" in CHANGELOG.md.
- Nothing that sends a token anywhere but OpenAI, or writes one to a log.

## Releasing

1. Set the version in `src/version.h` (both lines) and `src/app.manifest`, and move "Unreleased" in CHANGELOG.md under `## [x.y.z] - date`.
2. Commit, then tag and push: `git tag vx.y.z && git push origin main vx.y.z`.
3. The `release` workflow builds both exes, zips them with the docs, writes `SHA256SUMS.txt` and publishes the release with that version's changelog section as its notes. Running copies find it within six hours, or at once with **Check now** in Settings.
