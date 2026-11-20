<#
.SYNOPSIS
    Presses every control of the switcher's windows in the demo and checks what each one does.

.DESCRIPTION
    Starts tools\demo.ps1 -Stress off screen (no window shows, nothing takes focus), then clicks
    the rows, buttons, toggles, menu items and keys of the account list, the tray list, the tray
    menu, Settings, account details and the dialogs, through window messages. Each step is
    checked against the demo's trace (<demo>\trace.log, written only in this mode), its
    settings, its files and its windows.

    Menus and file dialogs never appear: the demo takes the menu item or file this script
    writes into the demo folder. Signing in is simulated. Nothing touches your real accounts,
    Codex, the clipboard, the browser or your desktop.

.EXAMPLE
    .\tools\uitest.ps1
    .\tools\uitest.ps1 -Language ru -Keep     # in Russian, and leave the demo running afterwards
#>
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\build\x64\CodexAccountSwitcher.exe'),
    [ValidateSet('en', 'ru')][string]$Language = 'en',
    [string]$Shots,          # a folder for pictures of the windows along the way
    [switch]$Keep            # leave the demo running at the end
)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
$Folder = Join-Path $env:TEMP 'cas-uitest'
$ru = $Language -eq 'ru'
function L($en, $r) { if ($ru) { $r } else { $en } }

Add-Type -Name U -Namespace CasUiTest -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, string l);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
[DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
public static System.Collections.Generic.List<IntPtr> Children(IntPtr parent, string cls) {
    var list = new System.Collections.Generic.List<IntPtr>();
    EnumChildWindows(parent, (h, l) => { var sb = new System.Text.StringBuilder(64); GetClassName(h, sb, 64); if (string.Equals(sb.ToString(), cls, StringComparison.OrdinalIgnoreCase)) list.Add(h); return true; }, IntPtr.Zero);
    return list;
}
public static System.Collections.Generic.List<IntPtr> Windows(uint pid) {
    var list = new System.Collections.Generic.List<IntPtr>();
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) list.Add(h); return true; }, IntPtr.Zero);
    return list;
}
public static string Title(IntPtr h) { var sb = new System.Text.StringBuilder(512); GetWindowText(h, sb, 512); return sb.ToString(); }
public static IntPtr Tray(uint pid) {
    for (IntPtr h = IntPtr.Zero; (h = FindWindowEx(IntPtr.Zero, h, "CodexAccountSwitcherTray", null)) != IntPtr.Zero;) { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) return h; }
    return IntPtr.Zero;
}
'@
[void][CasUiTest.U]::SetThreadDpiAwarenessContext([IntPtr]-4)

# ------------------------------------------------------------------ results

$results = New-Object System.Collections.Generic.List[object]
$section = ''
function Section($name) { $script:section = $name; Write-Host "`n== $name" -ForegroundColor Cyan }
function Pass($what) { $results.Add([pscustomobject]@{ Section = $section; Check = $what; Ok = $true; Detail = '' }); Write-Host "  ok    $what" -ForegroundColor Green }
function Fail($what, $detail) { $results.Add([pscustomobject]@{ Section = $section; Check = $what; Ok = $false; Detail = $detail }); Write-Host "  FAIL  $what  ($detail)" -ForegroundColor Red }
function Check($what, [bool]$ok, $detail = '') { if ($ok) { Pass $what } else { Fail $what $detail } }

# ------------------------------------------------------------------ the demo

function Start-Demo {
    Get-Process CodexAccountSwitcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe } | Stop-Process -Force
    Start-Sleep -Milliseconds 300
    & (Join-Path $PSScriptRoot 'demo.ps1') -Exe $Exe -Folder $Folder -Stress -Theme dark -Language $Language | Out-Null
    for ($i = 0; $i -lt 50; $i++) {
        $script:proc = Get-Process CodexAccountSwitcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe } | Select-Object -First 1
        if ($script:proc -and (Win (L 'Codex Accounts' 'Аккаунты Codex') 0)) { return $script:proc }
        Start-Sleep -Milliseconds 200
    }
    throw 'The demo switcher did not come up'
}

function Pid { [uint32]$script:proc.Id }
function Win([string]$title, [int]$ms = 3000, [switch]$Prefix) {
    $deadline = (Get-Date).AddMilliseconds($ms)
    do {
        foreach ($h in [CasUiTest.U]::Windows((Pid))) {
            $t = [CasUiTest.U]::Title($h)
            if ($t -eq $title -or ($Prefix -and $t.StartsWith($title)) -or ($title.StartsWith('*') -and $t.EndsWith($title.Substring(1)))) { return $h }
        }
        if ($ms -le 0) { break }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline)
    return $null
}
function Gone([string]$title, [int]$ms = 3000) {
    $deadline = (Get-Date).AddMilliseconds($ms)
    do { if (-not (Win $title 0)) { return $true }; Start-Sleep -Milliseconds 100 } while ((Get-Date) -lt $deadline)
    return $false
}
function Post($h, [uint32]$msg, $w = 0, $l = 0) { [void][CasUiTest.U]::PostMessage($h, $msg, [IntPtr]$w, [IntPtr]$l) }
function LParam([int]$x, [int]$y) { ($y -shl 16) -bor ($x -band 0xFFFF) }
function Tray { [CasUiTest.U]::Tray((Pid)) }
function Signal([int]$what) { Post (Tray) 0x8006 $what 0 }     # 0 show the list, 3 reload

# Where each control of a window is, by name ("row:2"), from the window itself.
function Regions($h) {
    $n = Get-Random -Minimum 1000 -Maximum 100000000
    $file = Join-Path $Folder "regions-$n.txt"
    Post $h 0x803E $n 0
    for ($i = 0; $i -lt 50 -and -not (Test-Path $file); $i++) { Start-Sleep -Milliseconds 60 }
    if (-not (Test-Path $file)) { return $null }
    Start-Sleep -Milliseconds 40
    $lines = [IO.File]::ReadAllLines($file); Remove-Item $file -Force
    $out = @{ _scale = [double]($lines[0].Split(' ')[1]) }
    foreach ($line in $lines | Select-Object -Skip 1) {
        $p = $line.Split(' ')
        if ($p.Count -ge 5 -and -not $out.ContainsKey($p[0])) { $out[$p[0]] = [pscustomobject]@{ X = [double]$p[1]; Y = [double]$p[2]; W = [double]$p[3]; H = [double]$p[4] } }
    }
    $out
}
function Has($h, $name) { $r = Regions $h; $r -and $r.ContainsKey($name) }
function Point($h, $name) {
    $r = Regions $h
    if (-not $r -or -not $r.ContainsKey($name)) { return $null }
    $c = $r[$name]; $s = $r._scale
    LParam ([int](($c.X + $c.W / 2) * $s)) ([int](($c.Y + $c.H / 2) * $s))
}
function Hover($h, $name) { $lp = Point $h $name; if ($null -eq $lp) { return $false }; Post $h 0x200 0 $lp; Start-Sleep -Milliseconds 150; $true }
function Click($h, $name, [switch]$Right) {
    if (-not $h) { Fail "click $name" 'no window'; return $false }
    $lp = Point $h $name
    if ($null -eq $lp) { Fail "click $name" 'no such control'; return $false }
    Post $h 0x200 0 $lp
    if ($Right) { Post $h 0x205 0 $lp } else { Post $h 0x201 1 $lp; Post $h 0x202 0 $lp }
    Start-Sleep -Milliseconds 120
    $true
}
function Key($h, [int]$vk) { Post $h 0x100 $vk 0; Start-Sleep -Milliseconds 120 }
function Type-Into($edit, [string]$text) { [void][CasUiTest.U]::SendMessage($edit, 0x000C, [IntPtr]::Zero, $text) }
function Edits($h) { @([CasUiTest.U]::Children($h, 'Edit')) }
function Shot($h, $name) {
    if (-not $Shots -or -not $h) { return }
    New-Item -ItemType Directory -Force $Shots | Out-Null
    $n = Get-Random -Minimum 1000 -Maximum 100000000
    $png = Join-Path $Folder "snapshots\snapshot-$n.png"
    Post $h 0x803D $n 0
    for ($i = 0; $i -lt 40 -and -not (Test-Path $png); $i++) { Start-Sleep -Milliseconds 80 }
    if (Test-Path $png) { Start-Sleep -Milliseconds 100; Move-Item $png (Join-Path $Shots "$name.png") -Force }
}

# ------------------------------------------------------------------ the trace

$traceFile = Join-Path $Folder 'trace.log'
function TraceLines {
    if (-not (Test-Path $traceFile)) { return @() }
    $fs = [IO.File]::Open($traceFile, 'Open', 'Read', 'ReadWrite, Delete')
    try { $r = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8); $text = $r.ReadToEnd() } finally { $fs.Dispose() }
    @($text -split "`r?`n" | Where-Object { $_ } | ForEach-Object { $_.Substring($_.IndexOf(' ') + 1) })
}
$script:mark = 0
function Mark { $script:mark = (TraceLines).Count }
function Since { @(TraceLines | Select-Object -Skip $script:mark) }
function Expect($what, [string]$pattern, [int]$ms = 4000) {
    $deadline = (Get-Date).AddMilliseconds($ms)
    do {
        $hit = Since | Where-Object { $_ -match $pattern } | Select-Object -First 1
        if ($hit) { Pass $what; return $hit }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline)
    Fail $what ("nothing like /$pattern/; last: " + ((Since | Select-Object -Last 4) -join ' ¦ '))
    $null
}
function ExpectNot($what, [string]$pattern, [int]$ms = 1200) {
    Start-Sleep -Milliseconds $ms
    $hit = Since | Where-Object { $_ -match $pattern } | Select-Object -First 1
    Check $what (-not $hit) "unexpected: $hit"
}
function Menu-Pick([string]$label) { [IO.File]::WriteAllText((Join-Path $Folder 'test-menu.txt'), $label, (New-Object Text.UTF8Encoding $false)) }
function File-Pick([string]$path) { [IO.File]::WriteAllText((Join-Path $Folder 'test-file.txt'), $path, (New-Object Text.UTF8Encoding $false)) }
function Setting($key) { $s = Get-Content (Join-Path $Folder 'settings.json') -Raw | ConvertFrom-Json; $s.$key }
function Set-Setting($key, $value) {
    $path = Join-Path $Folder 'settings.json'
    $s = Get-Content $path -Raw | ConvertFrom-Json
    $s | Add-Member -NotePropertyName $key -NotePropertyValue $value -Force
    [IO.File]::WriteAllText($path, ($s | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding $false))
    Start-Sleep -Milliseconds 300
}
function Live { (Get-Content (Join-Path $Folder '.codex\auth.json') -Raw | ConvertFrom-Json).tokens.refresh_token }
function Answer($title, [int]$button, [int]$ms = 4000) {
    $d = Win $title $ms
    if (-not $d) { Fail ('dialog ' + $title) 'did not appear'; return $null }
    Pass ('dialog ' + $title + ' appears')
    Shot $d ('dialog-' + ($title -replace '[^\w]+', '-').Trim('-'))
    [void](Click $d "dlg:$button")
    $d
}

$picker = L 'Codex Accounts' 'Аккаунты Codex'
$settingsTitle = L 'Settings — Codex Accounts' 'Настройки — Аккаунты Codex'
$flyoutTitle = L 'Account list' 'Список аккаунтов'
# The stress demo: 1 in use, 2 weekly-only Pro Lite, 3 the most left, 4 out of 5-hour limit, 5 signed out.
$ids = @{ 1 = 'a1b2c3d4'; 2 = 'e5f6a7b8'; 3 = 'c9d0e1f2'; 4 = '0a1b2c3d'; 5 = '5e6f7a8b' }

$script:proc = Start-Demo
Start-Sleep -Milliseconds 800

# ================================================================== the list
Section 'Account list'
$p = Win $picker
Shot $p 'picker'
$r = Regions $p
Check 'five rows, the add row, refresh and settings buttons, start with Windows' ($r -and $r.ContainsKey('row:5') -and $r.ContainsKey('addrow:0') -and $r.ContainsKey('refresh:0') -and $r.ContainsKey('settings:0') -and $r.ContainsKey('autostart:0')) ($r.Keys -join ', ')
Check 'the signed-out account has its Sign in again button' ($r.ContainsKey('signin:5'))
Check 'no ⋯ button until the pointer is over a row' (-not $r.ContainsKey('more:1'))

Mark; [void](Click $p 'refresh:0'); [void](Expect 'refresh button refreshes every account' '^refresh\|all$')

Mark; [void](Click $p 'row:2')
[void](Expect 'a click on another account switches to it and opens the app' "^switch\|$($ids[2])\|open$")
[void](Expect 'the app starts, as it was closed' '^app\|start$')
[void](Expect 'the switch succeeds' "^switched\|$($ids[2])\|ok\|.*started")
Check 'Codex now has that account''s sign-in' ((Live) -eq "demo-$($ids[2])") (Live)
[void](Expect 'the window hides after switching (the default)' '^window\|picker\|closed$')
Check 'the window is gone' (Gone $picker)

Mark; Signal 0
$p = Win $picker
Check 'the window comes back' ([bool]$p)
Start-Sleep -Milliseconds 400
Mark; [void](Click $p 'row:2')
[void](Expect 'a click on the account in use opens the app with it' "^switch\|$($ids[2])\|open$")
[void](Expect 'the app is already open: brought forward' '^app\|show$')
[void](Expect 'nothing is switched' "^switched\|$($ids[2])\|already")
Mark; Signal 0
ExpectNot 'opened again right away, the window stays (the hiding of the last choice is dropped)' '^window\|picker\|closed$' 900

Set-Setting 'HideAfterSwitch' $false
Signal 0; $p = Win $picker; Start-Sleep -Milliseconds 300

# The app is open now: switching asks first.
Mark; [void](Click $p 'row:3')
$ask = L 'Switch Codex to “trinewalters”?' 'Переключить Codex на «trinewalters»?'
[void](Answer $ask 1)
[void](Expect 'Cancel in the question leaves everything as it was' "^dialog-end\|.*\|0$")
ExpectNot 'no switch after Cancel' "^switched\|$($ids[3])"
Mark; [void](Click $p 'row:3')
[void](Answer $ask 0)
[void](Expect 'Switch closes the app' '^app\|close$')
[void](Expect 'and opens it again' '^app\|start$')
[void](Expect 'with the new account' "^switched\|$($ids[3])\|ok\|closed started")
Check 'Codex has trinewalters now' ((Live) -eq "demo-$($ids[3])") (Live)
Check 'the window stays when Hide after switching is off' ([bool](Win $picker 500))

# Keys
$p = Win $picker
Mark; Key $p 0x74; [void](Expect 'F5 refreshes' '^refresh\|')
Mark; Key $p 0x31
[void](Answer $ask.Replace('trinewalters', 'jessicamontgomery482+1') 0 2500)
[void](Expect 'digit 1 switches to the first account' "^switched\|$($ids[1])\|ok")
Mark; Key $p 0x49; [void](Expect 'I opens the details of the focused account (the one clicked last)' "^window\|details\|$($ids[3])$")
$d = Win "*$(L ' — Codex Accounts' ' — Аккаунты Codex')" 3000
Check 'the details window shows' ([bool]$d)
Shot $d 'details'
if ($d) {
    $dr = Regions $d
    Check 'details have refresh and Use this account' ($dr.ContainsKey('refresh:0') -and $dr.ContainsKey('use:0'))
    Mark; [void](Click $d 'refresh:0'); [void](Expect 'details refresh asks for that account' "^refresh\|$($ids[3])$")
    Post $d 0x0010 0 0   # WM_CLOSE
    Check 'details close' (Gone (L "trinewalters — Codex Accounts" "trinewalters — Аккаунты Codex"))
}

# The ⋯ menu
$p = Win $picker
Check 'the ⋯ button shows over a row' ((Hover $p 'row:4') -and (Has $p 'more:4'))
Menu-Pick (L 'Details and statistics' 'Подробности и статистика')
Mark; [void](Click $p 'more:4')
$m = Expect 'the ⋯ menu opens' '^menu\|'
Check 'it offers to open the app with the account, first' ($m -match "^menu\|[^|]+\|-\|$(L 'Open ChatGPT with it' 'Открыть ChatGPT с ним')\|") $m
[void](Expect 'Details and statistics opens details' "^window\|details\|$($ids[4])$")
$d = Win "*$(L ' — Codex Accounts' ' — Аккаунты Codex')" 3000
if ($d) {
    Mark; [void](Click $d 'use:0')
    [void](Expect 'Use this account in details switches' "^switch\|$($ids[4])\|")
    [void](Answer $ask.Replace('trinewalters', 'Work') 1 2500)
    Post $d 0x0010 0 0
}

function MenuItem($row, $label, $pattern, $what) {
    $p = Win $picker
    [void](Hover $p "row:$row")
    Menu-Pick $label
    Mark; [void](Click $p "more:$row")
    if ($pattern) { Expect $what $pattern } else { $null }
}
[void](MenuItem 4 (L 'Refresh limits' 'Обновить лимиты') "^refresh\|$($ids[4])$" '⋯ Refresh limits')
[void](MenuItem 4 (L 'Start the 5-hour window now' 'Запустить 5-часовое окно сейчас') "^warmup\|$($ids[4])$" '⋯ Start the 5-hour window now')
[void](Expect 'and says it runs' '^notify\|')
[void](MenuItem 4 (L 'Start its 5-hour windows automatically' 'Автоматически запускать его') '^setting\|AutoWarmupIds\|' '⋯ Start its 5-hour windows automatically')
[void](MenuItem 4 (L 'Hide its email' 'Скрывать его email') '^setting\|Masked\|' '⋯ Hide its email')
[void](MenuItem 4 (L 'Desktop shortcut' 'Ярлык на рабочий стол') '^notify\|' '⋯ Desktop shortcut')
Check 'the shortcut is in the demo''s own Desktop folder, not yours' (Test-Path (Join-Path $Folder 'Desktop\Codex - Work.lnk'))
[void](MenuItem 4 (L 'Move up' 'Поднять') "^move\|$($ids[4])\|-1$" '⋯ Move up')
[void](MenuItem 3 (L 'Move down' 'Опустить') "^move\|$($ids[4])\|1$" '⋯ Move down (the same account, now third)')
[void](MenuItem 2 (L 'Switch without opening the app' 'Переключить, не открывая') "^switch\|$($ids[2])\|stay$" '⋯ Switch without opening the app')
[void](Answer $ask.Replace('trinewalters', 'operations.team.member') 0 2500)
[void](Expect 'switches; the app, open before, restarts with it' "^switched\|$($ids[2])\|ok\|closed started")
[void](MenuItem 3 (L 'Open ChatGPT with it' 'Открыть ChatGPT с ним') "^switch\|$($ids[3])\|open$" '⋯ Open ChatGPT with it')
[void](Answer $ask 0 2500)
[void](Expect 'the app starts with it' "^switched\|$($ids[3])\|ok\|.*started")

# Rename in place
[void](MenuItem 4 (L 'Rename' 'Переименовать') "^rename-begin\|$($ids[4])$" '⋯ Rename')
Start-Sleep -Milliseconds 400
$p = Win $picker; $e = @(Edits $p)
Check 'a text box opens over the name' ($e.Count -ge 1)
if ($e.Count) { Mark; Type-Into $e[0] 'Office'; Post $e[0] 0x100 0x0D 0; [void](Expect 'Enter saves the new name' "^rename\|$($ids[4])\|Office$") }

# Save auth.json
$out = Join-Path $Folder 'out'; New-Item -ItemType Directory -Force $out | Out-Null
File-Pick (Join-Path $out 'auth.json')
[void](MenuItem 1 (L 'Save auth.json' 'Сохранить auth.json') $null $null)
[void](Answer (L 'Save the sign-in of “jessicamontgomery482+1”?' 'Сохранить вход «jessicamontgomery482+1» в файл?') 0)
[void](Expect '⋯ Save auth.json asks where' '^file-save\|')
Start-Sleep -Milliseconds 600
Check 'the sign-in is saved to that file' ((Test-Path (Join-Path $out 'auth.json')) -and ((Get-Content (Join-Path $out 'auth.json') -Raw) -match "demo-$($ids[1])"))

# Sign in again, then cancel it from the banner
[void](MenuItem 5 (L 'Sign in again' 'Войти заново') "^login\|begin\|$($ids[5])$" '⋯ Sign in again starts a sign-in')
[void](Expect 'the sign-in page opens in the browser' '^open\|https://auth\.openai\.com/')
$p = Win $picker
Shot $p 'signing-in'
Mark; [void](Click $p 'banner:1'); [void](Expect 'Cancel in the banner stops it' '^login\|cancel$')
Start-Sleep -Milliseconds 400
Mark; [void](Click (Win $picker) 'signin:5'); [void](Expect 'the row''s Sign in again button starts a sign-in' "^login\|begin\|$($ids[5])$")
Mark; Key (Win $picker) 0x1B; [void](Expect 'Esc cancels the sign-in' '^login\|cancel$')

# Add an account through the (simulated) browser sign-in
Start-Sleep -Milliseconds 400
Mark; [void](Click (Win $picker) 'addrow:0')
[void](Expect 'Add account starts a sign-in' '^login\|begin\|$')
[IO.File]::WriteAllText((Join-Path $Folder 'test-login.txt'), 'new.person@example.com', (New-Object Text.UTF8Encoding $false))
[void](Expect 'the signed-in account is added and its name offered for editing' '^rename-begin\|')
Start-Sleep -Milliseconds 500
$p = Win $picker
Shot $p 'added'
$r = Regions $p
Check 'it is the sixth row' ($r.ContainsKey('row:6'))
Check 'the Added banner shows' ($r.ContainsKey('banner:0'))
$e = @(); for ($i = 0; $i -lt 30 -and -not $e.Count; $i++) { $e = @(Edits $p); if (-not $e.Count) { Start-Sleep -Milliseconds 100 } }
Check 'the box for its name opens' ($e.Count -ge 1)
if ($e.Count) { Mark; Type-Into $e[0] 'Spare'; Post $e[0] 0x100 0x0D 0; [void](Expect 'Enter names it' '^rename\|[^|]+\|Spare$') }
Mark; [void](Click (Win $picker) 'banner:0'); Start-Sleep -Milliseconds 300
Check 'Done closes the banner' (-not (Has (Win $picker) 'banner:0'))

# Delete: cancel, then confirm
$del = L 'Delete “Spare”?' 'Удалить «Spare»?'
[void](MenuItem 6 (L 'Delete' 'Удалить') $null $null)
[void](Answer $del 1)
ExpectNot 'Cancel keeps the account' '^delete\|'
[void](MenuItem 6 (L 'Delete' 'Удалить') $null $null)
[void](Answer $del 0)
[void](Expect 'Delete removes it' '^delete\|')
Start-Sleep -Milliseconds 800
Check 'five rows again' (-not (Has (Win $picker) 'row:6'))

# Start with Windows, at the bottom
$before = [bool](Setting 'Autostart')
Mark; [void](Click (Win $picker) 'autostart:0'); Start-Sleep -Milliseconds 300
Check 'the Start with Windows switch flips it' ([bool](Setting 'Autostart') -ne $before)
[void](Click (Win $picker) 'autostart:0')

# Right click on a row opens the same menu
Menu-Pick 'nothing at all'
Mark; [void](Click (Win $picker) 'row:2' -Right); [void](Expect 'a right click on a row opens its menu' '^menu\|')

# ================================================================== the tray
Section 'Tray'
Mark; Post (Tray) 0x8001 0 0x400   # NIN_SELECT: a click on the icon
$f = Win $flyoutTitle
Check 'a click on the tray icon opens the list' ([bool]$f)
Shot $f 'flyout'
if ($f) {
    $fr = Regions $f
    Check 'it has the five accounts, refresh, the window, settings, add and quit' ($fr.ContainsKey('fo-row:5') -and $fr.ContainsKey('fo-refresh:0') -and $fr.ContainsKey('fo-open:0') -and $fr.ContainsKey('fo-settings:0') -and $fr.ContainsKey('fo-add:0') -and $fr.ContainsKey('fo-exit:0'))
    Mark; [void](Click $f 'fo-refresh:0'); [void](Expect 'its refresh button refreshes' '^refresh\|all$')
    Mark; [void](Click $f 'fo-row:4')
    [void](Expect 'a click on an account switches and opens the app' "^switch\|$($ids[4])\|open$")
    [void](Answer $ask.Replace('trinewalters', 'Office') 0 2500)
    [void](Expect 'and it switches' "^switched\|$($ids[4])\|ok")
    Check 'the list closes after a choice' (Gone $flyoutTitle)
}
foreach ($t in @(@('fo-open:0', '^window\|picker$', 'the window button opens the account list'), @('fo-settings:0', '^window\|settings$', 'the settings button opens Settings'), @('fo-add:0', '^login\|begin', 'Add account starts a sign-in'))) {
    Post (Tray) 0x8001 0 0x400; $f = Win $flyoutTitle
    Mark; [void](Click $f $t[0]); [void](Expect $t[2] $t[1])
}
Mark; Key (Win $picker) 0x1B; [void](Expect 'Esc cancels it again' '^login\|cancel$')
$s = Win $settingsTitle; if ($s) { Post $s 0x0010 0 0 }

function TrayMenu($label, $pattern, $what) {
    Menu-Pick $label
    Mark; Post (Tray) 0x8001 0 0x007B   # WM_CONTEXTMENU
    Expect $what $pattern
}
$tm = TrayMenu 'nothing at all' '^menu\|' 'a right click on the icon opens the tray menu'
Check 'the tray menu names the app as ChatGPT' ($tm -match (L '\|Open ChatGPT\|' '\|Открыть ChatGPT\|')) $tm
[void](TrayMenu (L 'Refresh limits' 'Обновить лимиты') '^refresh\|all$' 'tray menu: Refresh limits')
[void](TrayMenu (L 'Open ChatGPT' 'Открыть ChatGPT') '^app\|(show|start)' 'tray menu: Open ChatGPT')
[void](TrayMenu (L 'Restart ChatGPT' 'Перезапустить ChatGPT') '^app\|close$' 'tray menu: Restart ChatGPT closes it')
[void](Expect 'and starts it again' '^app\|start$')
[void](TrayMenu (L 'Start 5-hour windows for all' 'Запустить 5-часовые окна у всех') '^warmup\|' 'tray menu: Start 5-hour windows for all')
[void](TrayMenu (L 'Continue conversations stopped' 'Продолжить разговоры') '^continue$' 'tray menu: Continue conversations')
[void](TrayMenu (L 'Show accounts' 'Открыть окно выбора') '^window\|picker$' 'tray menu: Show accounts')
[void](TrayMenu (L 'Settings' 'Настройки') '^window\|settings$' 'tray menu: Settings')
[void](TrayMenu 'trinewalters' "^switch\|$($ids[3])\|open$" 'tray menu: an account switches to it and opens the app')
[void](Answer $ask 0 2500)

# ================================================================== settings
Section 'Settings'
$s = Win $settingsTitle
if (-not $s) { Signal 0; [void](Click (Win $picker) 'settings:0'); $s = Win $settingsTitle }
Check 'Settings is open' ([bool]$s)
Shot $s 'settings'
function Flip($key, $setting, $what) {
    $before = Setting $setting
    [void](Click $s $key); Start-Sleep -Milliseconds 250
    $after = Setting $setting
    Check $what ("$after" -ne "$before") "$setting stayed $before"
}
Check 'the first switch is Open ChatGPT when choosing an account, on' ((Has $s 'open-after:0') -and ((Setting 'OpenAfterSwitch') -ne $false))
Flip 'open-after:0' 'OpenAfterSwitch' 'Open ChatGPT when choosing an account turns off'
$p = Win $picker
Mark; [void](Click $p 'row:1')
[void](Expect 'with it off, a click only switches' "^switch\|$($ids[1])\|stay$")
[void](Answer $ask.Replace('trinewalters', 'jessicamontgomery482+1') 0 2500)
[void](Expect 'and the app restarts as it was open' "^switched\|$($ids[1])\|ok\|closed started")
Flip 'open-after:0' 'OpenAfterSwitch' 'and back on'
Flip 'restart:0' 'RestartCodex' 'Restart ChatGPT when switching'
Flip 'restart:0' 'RestartCodex' '… back'
Flip 'ask:0' 'AskBeforeRestart' 'Ask before closing ChatGPT'
Mark; [void](Click (Win $picker) 'row:2')
ExpectNot 'with the question off, no dialog' '^dialog\|' 800
[void](Expect 'and it switches at once' "^switched\|$($ids[2])\|ok")
Flip 'ask:0' 'AskBeforeRestart' '… back'
Flip 'hide-after:0' 'HideAfterSwitch' 'Hide the window after switching'
Flip 'hide-after:0' 'HideAfterSwitch' '… back'
Flip 'autowarm:0' 'AutoWarmup' 'Start 5-hour windows automatically'
Flip 'autowarm:0' 'AutoWarmup' '… back'
Flip 'timed:0' 'TimedWarmup' 'Start them at set times'
Mark; [void](Click $s 'times:0')
$tt = Win (L 'Times to start the windows' 'Время запуска окон')
Check 'the set-times button asks for times' ([bool]$tt)
if ($tt) { $e = @(Edits $tt); if ($e.Count) { Type-Into $e[0] '9:30, 14:00' }; [void](Click $tt 'dlg:0'); Start-Sleep -Milliseconds 400; Check 'the times are saved' (((Setting 'WarmupTimes') -join ',') -eq '09:30,14:00') ((Setting 'WarmupTimes') -join ',') }
Flip 'usage:1' 'UsageMinutes' 'Refresh limits: 5 min'
Check '… is 5' ((Setting 'UsageMinutes') -eq 5)
Flip 'traymode:1' 'TrayIcon' 'Tray icon: % left'
Check '… is percent' ((Setting 'TrayIcon') -eq 'percent')
Flip 'traymode:0' 'TrayIcon' 'Tray icon: the icon again'
Flip 'mask:0' 'MaskAll' 'Hide emails'
Flip 'mask:0' 'MaskAll' '… back'
Flip 'autostart:0' 'Autostart' 'Start with Windows'
Flip 'keep-tray:0' 'KeepInTray' 'Stay in the tray when closed'
Flip 'keep-tray:0' 'KeepInTray' '… back'
Flip 'theme:2' 'Theme' 'Theme: dark'
Flip 'theme:0' 'Theme' 'Theme: system'
Flip 'acchk:0' 'AccountHotkeys' 'Ctrl + Shift + Alt + number'
Mark; [void](Click $s 'hk-clear:0'); Start-Sleep -Milliseconds 300
Check 'the shortcut can be cleared' ((Setting 'Hotkey') -eq '')
Mark; [void](Click $s 'codexapp:0'); [void](Expect 'the app''s Open/Restart button works' '^app\|')
Mark; [void](Click $s 'authfile:0'); [void](Expect 'Show opens the sign-in file in Explorer' '^open\|explorer\.exe\|/select,')
Mark; [void](Click $s 'folder:0'); [void](Expect 'Data folder opens the folder' ('^open\|' + [regex]::Escape($Folder)))
Mark; [void](Click $s 'add:0'); [void](Expect 'Add account starts a sign-in' '^login\|begin')
Mark; Key (Win $picker) 0x1B; [void](Expect '… cancelled' '^login\|cancel$')

# An auth.json from a file
$claims = @{ email = 'from.file@example.com'; sub = 'auth0|file'; exp = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + 86400; 'https://api.openai.com/auth' = @{ chatgpt_account_id = 'ws-file'; chatgpt_user_id = 'user-file'; chatgpt_plan_type = 'plus' } }
$b64 = { param($t) [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($t)).TrimEnd('=').Replace('+', '-').Replace('/', '_') }
$jwt = 'eyJhbGciOiJub25lIn0.' + (& $b64 ($claims | ConvertTo-Json -Compress -Depth 5)) + '.demo'
$auth = @{ tokens = @{ id_token = $jwt; access_token = $jwt; refresh_token = 'demo-file'; account_id = 'ws-file' }; last_refresh = [DateTime]::UtcNow.ToString('o') }
[IO.File]::WriteAllText((Join-Path $out 'import-auth.json'), ($auth | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding $false))
File-Pick (Join-Path $out 'import-auth.json')
Mark; [void](Click $s 'authjson:0'); [void](Expect 'From an auth.json file asks for the file' '^file-open\|')
Start-Sleep -Milliseconds 1200
Check 'the account from the file is added' (Has (Win $picker) 'row:6')

# Backup as text, then import it back
Mark; [void](Click $s 'backup:1')
[void](Answer (L 'Back up all accounts' 'Резервная копия всех аккаунтов') 1)
$pw = Win (L 'Choose a password' 'Придумайте пароль')
Check 'Copy as text asks for a password' ([bool]$pw)
if ($pw) {
    $e = @(Edits $pw)
    Check 'twice' ($e.Count -eq 2)
    Type-Into $e[0] 'correct horse 1'; Type-Into $e[1] 'wrong horse 1'
    [void](Click $pw 'dlg:0'); Start-Sleep -Milliseconds 300
    Check 'different passwords are refused' ([bool](Win (L 'Choose a password' 'Придумайте пароль') 300))
    Type-Into $e[1] 'correct horse 1'
    [void](Click $pw 'dlg:0')
}
$clip = Expect 'the backup goes to the clipboard as text' '^clipboard\|cas1\.' 8000
[void](Answer (L 'Copied' 'Скопировано') 0)
if ($clip) {
    $text = $clip.Substring('clipboard|'.Length)
    Mark; [void](Click $s 'backup:0')
    [void](Answer (L 'Import accounts' 'Импорт аккаунтов') 1)
    $paste = Win (L 'Paste the backup' 'Вставьте копию')
    if ($paste) { Type-Into (@(Edits $paste)[0]) $text; [void](Click $paste 'dlg:0') }
    $pw = Win (L 'Backup password' 'Пароль копии')
    Check 'importing asks for the password' ([bool]$pw)
    if ($pw) { Type-Into (@(Edits $pw)[0]) 'correct horse 1'; [void](Click $pw 'dlg:0') }
    [void](Answer (L 'Accounts imported' 'Аккаунты импортированы') 0 10000)
}
# Backup to a file and back
File-Pick (Join-Path $out 'all.casbackup')
Mark; [void](Click $s 'backup:1')
[void](Answer (L 'Back up all accounts' 'Резервная копия всех аккаунтов') 0)
$pw = Win (L 'Choose a password' 'Придумайте пароль')
if ($pw) { $e = @(Edits $pw); Type-Into $e[0] 'another pass 2'; Type-Into $e[1] 'another pass 2'; [void](Click $pw 'dlg:0') }
[void](Expect 'Save to a file asks where' '^file-save\|' 8000)
[void](Expect 'and says it is saved' '^notify\|' 4000)
Check 'the backup file is there' (Test-Path (Join-Path $out 'all.casbackup'))
File-Pick (Join-Path $out 'all.casbackup')
Mark; [void](Click $s 'backup:0')
[void](Answer (L 'Import accounts' 'Импорт аккаунтов') 0)
$pw = Win (L 'Backup password' 'Пароль копии')
if ($pw) { Type-Into (@(Edits $pw)[0]) 'wrong'; [void](Click $pw 'dlg:0') }
[void](Answer (L "Couldn't import" 'Не удалось импортировать') 0 10000)

# The browser list
Flip 'web:0' 'WebOn' 'Account list in a browser'
Start-Sleep -Milliseconds 800
$sr = Regions $s
Check 'its link buttons appear' ($sr.ContainsKey('weblink:2'))
Mark; [void](Click $s 'weblink:1'); [void](Expect 'Copy copies the link' '^clipboard\|http://')
Mark; [void](Click $s 'weblink:2'); [void](Expect 'Open opens it' '^open\|http://')
$key = Setting 'WebKey'
[void](Click $s 'weblink:0'); Start-Sleep -Milliseconds 300
Check 'New link makes a new key' ((Setting 'WebKey') -ne $key)
Flip 'web:0' 'WebOn' '… and off'
Post $s 0x0010 0 0
Check 'Settings closes' (Gone $settingsTitle)

# ================================================================== the end
Section 'Quit'
Post (Tray) 0x8001 0 0x400
$f = Win $flyoutTitle
Mark; [void](Click $f 'fo-exit:0')
[void](Expect 'Quit switcher quits' '^exit$')
$script:proc.WaitForExit(5000) | Out-Null
Check 'the process ends' ($script:proc.HasExited)

# ------------------------------------------------------------------ summary
$failed = @($results | Where-Object { -not $_.Ok })
Write-Host ''
Write-Host ("{0} checks, {1} passed, {2} failed" -f $results.Count, ($results.Count - $failed.Count), $failed.Count) -ForegroundColor $(if ($failed.Count) { 'Red' } else { 'Green' })
foreach ($f in $failed) { Write-Host ("  [{0}] {1}: {2}" -f $f.Section, $f.Check, $f.Detail) -ForegroundColor Red }
if (-not $Keep) {
    Get-Process CodexAccountSwitcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe } | Stop-Process -Force
    Start-Sleep -Milliseconds 300
    Remove-Item $Folder -Recurse -Force -ErrorAction SilentlyContinue
}
exit $failed.Count
