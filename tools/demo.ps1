<#
.SYNOPSIS
    Opens the switcher over stand-in accounts, for screenshots and trying out the UI.

.DESCRIPTION
    Creates a throwaway folder with four made-up accounts (names, plans, limits, one of them
    in use) and starts a separate switcher on it through CAS_DEMO. The demo never contacts
    OpenAI, never starts or closes Codex and does not touch your real accounts, auth.json,
    shortcuts, autostart, hotkeys, jump list or the switcher you normally run.

    By default its windows open far off screen, without focus, a taskbar button or a tray
    icon, so it can run while you work; tools\capture.ps1 takes their pictures from there.
    -Visible shows them normally.

.EXAMPLE
    .\tools\demo.ps1
    .\tools\demo.ps1 -Theme light -Language ru
    .\tools\demo.ps1 -Visible -Scale 2      # on screen, every window at 200%
#>
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\build\x64\CodexAccountSwitcher.exe'),
    [string]$Folder = (Join-Path $env:TEMP 'cas-demo'),
    [ValidateSet('system', 'dark', 'light')][string]$Theme = 'system',
    [ValidateSet('en', 'ru', 'settings')][string]$Language = 'en',   # settings: whatever the Language setting says
    [ValidateRange(1, 4)][double]$Scale = 0,               # 0: follow the monitor
    [switch]$Visible,       # on screen instead of off it
    [switch]$Tray,          # start in the tray only
    [switch]$Settings,      # open Settings too
    [switch]$CodexOpen,     # pretend the Codex app is open
    [switch]$KeepFlyout,    # the tray flyout stays open when it loses focus, for screenshots
    [switch]$Web,           # the account list in a browser too, at http://localhost:3291/?key=demo-key-not-secret
    [switch]$Stress         # accounts named after long emails, a weekly-only plan, one signed out: for checking layouts
)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path

# A demo switcher may still be running from last time. The installed switcher runs from
# %LOCALAPPDATA%\CodexAccounts\.switcher, so it never matches this path.
Get-Process CodexAccountSwitcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe } | Stop-Process -Force
Start-Sleep -Milliseconds 300
if (Test-Path $Folder) { Remove-Item $Folder -Recurse -Force }
New-Item -ItemType Directory -Force $Folder, (Join-Path $Folder '.codex') | Out-Null

$utf8 = New-Object System.Text.UTF8Encoding $false
function Write-Json($path, $value) { [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 8), $utf8) }
function Base64Url([string]$s) { [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($s)).TrimEnd('=').Replace('+', '-').Replace('/', '_') }
function Jwt($claims) { 'eyJhbGciOiJub25lIn0.' + (Base64Url ($claims | ConvertTo-Json -Compress -Depth 5)) + '.demo' }

$now = [DateTimeOffset]::UtcNow
$nowMs = $now.ToUnixTimeMilliseconds(); $nowS = $now.ToUnixTimeSeconds()
$names = if ($Language -eq 'ru') { @('Работа', 'Личный', 'Проект', 'Запасной') } else { @('Work', 'Personal', 'Side project', 'Spare') }
$accounts = @(
    @{ Id = 'a1b2c3d4'; Name = $names[0]; Email = 'alex@acme.dev';      Plan = 'team'; Used = 0;    Five = 34;  Week = 61;  FiveIn = 9000;  WeekIn = 300000 },
    @{ Id = 'e5f6a7b8'; Name = $names[1]; Email = 'alex.k@example.com'; Plan = 'plus'; Used = 3;    Five = 92;  Week = 48;  FiveIn = 2400;  WeekIn = 400000; Resets = 2 },
    @{ Id = 'c9d0e1f2'; Name = $names[2]; Email = 'side@example.com';   Plan = 'pro';  Used = 26;   Five = 100; Week = 77;  FiveIn = 5400;  WeekIn = 180000 },
    @{ Id = '0a1b2c3d'; Name = $names[3]; Email = 'spare@example.com';  Plan = 'free'; Used = 240;  Five = 0;   Week = 12;  FiveIn = 18000; WeekIn = 500000 }
)
if ($Stress) {
    # Names left as the switcher makes them from emails, which is what most people keep.
    $accounts = @(
        @{ Id = 'a1b2c3d4'; Name = 'jessicamontgomery482+1'; Email = 'jessicamontgomery482+1@example.com'; Plan = 'plus';    Used = 0;   Five = 0;  Week = 82; FiveIn = 18000; WeekIn = 300000 },
        @{ Id = 'e5f6a7b8'; Name = 'operations.team.member'; Email = 'operations.team.member@northwind-labs.example'; Plan = 'prolite'; Used = 230; Five = -1; Week = 11; FiveIn = 0; WeekIn = 400000; Resets = 2 },
        @{ Id = 'c9d0e1f2'; Name = 'trinewalters';            Email = 'trinewalters@example.com';            Plan = 'plus';    Used = 30;  Five = 0;  Week = 0;  FiveIn = 18000; WeekIn = 590000; Resets = 1 },
        @{ Id = '0a1b2c3d'; Name = 'Work';                    Email = 'alex@acme.dev';                     Plan = 'team';    Used = 52;  Five = 100; Week = 77; FiveIn = 5400; WeekIn = 180000 },
        @{ Id = '5e6f7a8b'; Name = 'old.account';             Email = 'old.account@example.net';           Plan = 'free';    Used = 900; Five = 0;  Week = 0;  FiveIn = 18000; WeekIn = 500000; Out = $true }
    )
}
$vault = @{ version = 1; accounts = @() }
$usage = @{}
foreach ($a in $accounts) {
    $workspace = "ws-$($a.Id)"; $user = "user-$($a.Id)"
    $id = Jwt @{ email = $a.Email; sub = "auth0|$user"; exp = $nowS + 864000; 'https://api.openai.com/auth' = @{ chatgpt_account_id = $workspace; chatgpt_user_id = $user; chatgpt_plan_type = $a.Plan } }
    $auth = @{ tokens = @{ id_token = $id; access_token = (Jwt @{ exp = $nowS + 864000 }); refresh_token = "demo-$($a.Id)"; account_id = $workspace }; last_refresh = $now.ToString('o') }
    $vault.accounts += @{ id = $a.Id; name = $a.Name; email = $a.Email; plan = $a.Plan; workspace = $workspace; user = $user
                          added = $nowMs - 86400000 * 30; lastUsed = $(if ($a.Used) { $nowMs - $a.Used * 3600000 } else { $nowMs }); warmedAt = 0; auth = $auth }
    $usage[$a.Id] = @{ known = -not $a.Out; five = $a.Five; week = $a.Week; fiveReset = $nowS + $a.FiveIn; weekReset = $nowS + $a.WeekIn
                       limited = ($a.Five -ge 100); plan = $a.Plan; at = $nowMs - 60000; resetCredits = [int]$a.Resets; resetExpires = $(if ($a.Resets) { $nowS + 6 * 86400 } else { 0 }); credits = $false; unlimited = $false; balance = '0'; problem = $(if ($a.Out) { 'Sign in again' } else { '' }); signedOut = [bool]$a.Out }
    # The first one is the account Codex uses.
    if ($a -eq $accounts[0]) { Write-Json (Join-Path $Folder '.codex\auth.json') $auth }
}
Write-Json (Join-Path $Folder 'accounts.dat') $vault
Write-Json (Join-Path $Folder 'usage.json') $usage
Write-Json (Join-Path $Folder 'settings.json') @{ Autostart = $true; DemoCodexOpen = [bool]$CodexOpen; UsageMinutes = 0; WebOn = [bool]$Web; WebPort = 3291; WebKey = 'demo-key-not-secret'; Colors = @{ a1b2c3d4 = 0; e5f6a7b8 = 3; c9d0e1f2 = 4; '0a1b2c3d' = 6 } }

function Set-Env($name, $value) { if ($value) { Set-Item "Env:$name" $value } else { Remove-Item "Env:$name" -ErrorAction SilentlyContinue } }
Set-Env CAS_DEMO $Folder
Set-Env CAS_LANG $(if ($Language -ne 'settings') { $Language })
Set-Env CAS_THEME $(if ($Theme -ne 'system') { $Theme })
Set-Env CAS_SCALE $(if ($Scale) { "$Scale" })
Set-Env CAS_FLYOUT_STAY $(if ($KeepFlyout) { '1' })
Set-Env CAS_OFFSCREEN $(if (-not $Visible) { '1' })
Set-Env CAS_SNAPSHOT (Join-Path $Folder 'snapshots')
New-Item -ItemType Directory -Force (Join-Path $Folder 'snapshots') | Out-Null

$arguments = @()
if ($Tray) { $arguments += '--tray' }
if ($Settings) { $arguments += '--settings' }
$start = @{ FilePath = $Exe; WorkingDirectory = (Split-Path $Exe) }
if ($arguments) { $start.ArgumentList = $arguments }
Start-Process @start
"Demo switcher started over $Folder" + $(if (-not $Visible) { ' (off screen)' })
