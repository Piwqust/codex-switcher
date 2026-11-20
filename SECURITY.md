# Security

## Supported versions

Fixes ship in the latest version. Please update to it before reporting a problem.

## Reporting a vulnerability

Please do not open a public issue. Use GitHub's [private vulnerability reporting](https://github.com/Piwqust/codex-switcher/security/advisories/new) instead.

Describe what the vulnerability allows, how to reproduce it and which version you used. You will get a reply within a week. Once it is fixed, a new version is released and its notes credit you, unless you would rather they did not.

## What the program does and does not do

- It keeps a copy of each account's Codex sign-in (`auth.json`: ChatGPT id, access and refresh tokens, or an API key) in `%LOCALAPPDATA%\CodexAccounts\accounts.dat`, encrypted with Windows DPAPI for the current user and an entropy of its own.
- Switching writes the chosen sign-in to `auth.json` in `%USERPROFILE%\.codex` (or `CODEX_HOME`), through a temporary file in the same folder, and nothing else in that folder.
- Tokens are sent only over HTTPS, with redirects refused, to `auth.openai.com/oauth/token` (signing in and renewing, the same requests Codex makes), `chatgpt.com/backend-api/wham/usage` and `/wham/rate-limit-reset-credits` (limits), `/wham/profiles/me` and `/accounts/check/v4-2023-04-27` (statistics and the subscription) and `chatgpt.com/backend-api/codex/responses` (warm-ups you start or turn on).
- Signing in runs a local server on `127.0.0.1:1455` or `1457` for the length of one sign-in. It accepts only an answer with that sign-in's random `state`, and uses PKCE.
- **The browser page**, when you turn it on, runs an HTTP server on `127.0.0.1:3210`, or on every interface with **Allow other devices**. Every request needs a 144-bit random key from the link, compared in constant time; actions need POST. It shows names, emails, plans and limits and can switch accounts, start 5-hour windows and open Codex. It never sends a token. It is plain HTTP, so on an untrusted network the link can be seen by whoever sees the traffic; **New link** replaces the key.
- **Continuing conversations** reads Codex's session logs from the last 24 hours, and runs the Codex CLI (`codex queue`, without a window: the npm package's under `%APPDATA%\npm`, or the copy inside the Codex app) to queue one message per conversation that stopped at the usage limit. It writes receipts to `%LOCALAPPDATA%\CodexAccounts\recovery` so no conversation gets it twice.
- **Backups** are encrypted with AES-256-GCM under a key derived from your password with PBKDF2-HMAC-SHA256 (600 000 iterations, random salt). Codex Switcher's `.cswf` exports use a passphrase built into that program, so anyone with such a file can open it; this switcher only reads them.
- The update check asks `api.github.com` for this repository's latest release at start and every 6 hours (Settings → About can turn it off). Nothing about you or your accounts is sent. An update is downloaded from that release over HTTPS, installed only when its SHA-256 matches the release's `SHA256SUMS.txt`, and only when you click **Update**.
- The error log (`%LOCALAPPDATA%\CodexAccounts\switcher-error.log`) records what failed, never a token.
- **Save auth.json** writes one account's sign-in to a file you choose, unencrypted, after a warning. Treat that file as a password, and a backup too: with its password it signs in to every account in it.
- To restart the Codex app it ends the processes of the Store package `OpenAI.Codex` and the Codex app servers editors start from their `openai.chatgpt` extension, identified by their executable paths. Codex sessions in terminals are left running.
- It runs with your user's rights and writes to `%LOCALAPPDATA%\CodexAccounts`, to Codex's `auth.json`, to shortcuts on the desktop and in the Start menu, and to the `Codex Account Switcher` value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.

## Verifying a download

Releases are built by GitHub Actions from this repository, and every release has a `SHA256SUMS.txt` next to its files. Compare the hash of what you downloaded:

```powershell
Get-FileHash .\CodexAccountSwitcher-x64.exe -Algorithm SHA256
```
