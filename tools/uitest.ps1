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
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
public struct RECT { public int Left, Top, Right, Bottom; }
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

function Start-Demo([switch]$Empty) {
    Get-Process CodexAccountSwitcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe } | Stop-Process -Force
    Start-Sleep -Milliseconds 300
    & (Join-Path $PSScriptRoot 'demo.ps1') -Exe $Exe -Folder $Folder -Stress:(-not $Empty) -Empty:$Empty -Theme dark -Language $Language | Out-Null
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
    # "scale 1.25 client 720 616 scroll 0 0 focus row:1", then a line per control:
    # "row:1 20 84 680 76 1<tab>what a screen reader says<tab>the text shown on hover".
    $top = $lines[0].Split(' ')
    $out = @{ _scale = [double]$top[1]; _clientH = [double]$top[4]; _scroll = [double]$top[6]; _scrollMax = [double]$top[7]; _focus = $top[9] }
    foreach ($line in $lines | Select-Object -Skip 1) {
        $t = $line.Split("`t"); $p = $t[0].Split(' ')
        if ($p.Count -ge 6 -and -not $out.ContainsKey($p[0])) {
            $out[$p[0]] = [pscustomobject]@{ X = [double]$p[1]; Y = [double]$p[2]; W = [double]$p[3]; H = [double]$p[4]; Focusable = $p[5] -eq '1'; Name = $t[1]; Tip = $t[2] }
        }
    }
    $out
}
# Every control the keyboard reaches must have a name a screen reader can say.
function Named($h, $where) {
    $r = Regions $h
    $bad = @($r.Keys | Where-Object { $_ -notlike '_*' -and $r[$_].Focusable -and -not $r[$_].Name })
    Check "$where`: every control the keyboard reaches has a name for screen readers" ($r -and $bad.Count -eq 0) ($bad -join ', ')
}
# The window as UI Automation gives it to Narrator: its children, by the control's name in the checks.
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
function Uia($h, $id) {
    $all = [System.Windows.Automation.AutomationElement]::FromHandle($h).FindAll('Children', [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($e in $all) { if ($e.Current.AutomationId -eq $id) { return $e } }
    $null
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
Named $p 'the list'
Check 'the icon buttons carry the text shown on hover' ($r['manage:0'].Tip -and $r['refresh:0'].Tip -and $r['settings:0'].Tip) "$($r['manage:0'].Tip)"
Check 'a row is named with its account, status and what is left' ($r['row:1'].Name -match 'jessicamontgomery482\+1.*%') $r['row:1'].Name
Mark; [void](Hover $p 'settings:0'); [void](Expect 'resting the pointer on an icon button shows what it does' '^tip\|.+' 2500)
$e = Uia $p 'row:1'
Check 'UI Automation gives Narrator the rows as list items with those names' ($e -and $e.Current.ControlType.ProgrammaticName -eq 'ControlType.ListItem' -and $e.Current.Name -match '%') $(if ($e) { $e.Current.ControlType.ProgrammaticName + ' ' + $e.Current.Name })
$e = Uia $p 'autostart:0'
$on = if ($e) { "$($e.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState)" }
Check '… and the switch says whether it is on' ($on -eq $(if (Setting 'Autostart') { 'On' } else { 'Off' })) $on

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
Named $d 'account details'
$dr = Regions $d
$day = @($dr.Keys | Where-Object { $_ -like 'day:*' } | Select-Object -First 1)
Check 'each day of the chart shows its figure on hover' ($day.Count -eq 1 -and $dr[$day[0]].Tip -match '\d') $(if ($day.Count) { $dr[$day[0]].Tip })
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

# Sign in again: in the default browser, shown in its row
[void](MenuItem 5 (L 'Sign in again' 'Войти заново') "^login\|begin\|$($ids[5])$" '⋯ Sign in again starts a sign-in')
[void](Expect 'the sign-in page opens in the default browser' '^open\|https://auth\.openai\.com/')
ExpectNot '… not in a browser profile of its own' '^browser\|' 300
$p = Win $picker
Start-Sleep -Milliseconds 300
Shot $p 'signing-in'
$r = Regions $p
Check 'the row itself shows the waiting sign-in: open again, copy the link, cancel' ($r.ContainsKey('signin-open:5') -and $r.ContainsKey('signin-link:5') -and $r.ContainsKey('signin-cancel:5') -and -not $r.ContainsKey('banner:0'))
Mark; [void](Click $p 'signin-open:5'); [void](Expect 'Open again opens the page once more' '^login\|reopen$')
[void](Expect '… in the default browser too' '^open\|https://auth\.openai\.com/')
Mark; [void](Click $p 'signin-link:5'); [void](Expect 'the link can be copied for another browser' '^clipboard\|https://auth\.openai\.com/')
[void](Expect '… and the window says so' '^toast\|')
Mark; [void](Click $p 'signin-cancel:5'); [void](Expect 'Cancel in the row stops it' '^login\|cancel$')
Start-Sleep -Milliseconds 400
Check 'the row is back to its Sign in again button' ((Has (Win $picker) 'signin:5') -and -not (Has (Win $picker) 'signin-cancel:5'))
Mark; [void](Click (Win $picker) 'signin:5'); [void](Expect 'the row''s Sign in again button starts a sign-in' "^login\|begin\|$($ids[5])$")
Mark; Key (Win $picker) 0x1B; [void](Expect 'Esc cancels the sign-in' '^login\|cancel$')

# Signing in as somebody else is asked about, not added behind one's back
Start-Sleep -Milliseconds 400
Mark; [void](Click (Win $picker) 'signin:5'); [void](Expect 'a sign-in for the signed-out account starts' "^login\|begin\|$($ids[5])$")
[IO.File]::WriteAllText((Join-Path $Folder 'test-login.txt'), 'someone.else@example.com', (New-Object Text.UTF8Encoding $false))
[void](Expect 'another account coming back is noticed' "^login\|other\|$($ids[5])$" 8000)
[void](Answer (L 'You signed in as someone.else@example.com, not “old.account”' 'Вы вошли как someone.else@example.com, а не «old.account»') 2)
Start-Sleep -Milliseconds 600
Check 'Cancel adds nothing' (-not (Has (Win $picker) 'row:6'))

# Every way of adding, behind the chevron of the add row; the clipboard and dropped files are taken for what they are
$p = Win $picker
$r = Regions $p
Check 'the add row has its other ways' ($r.ContainsKey('addmore:0') -and $r['addmore:0'].Tip)
$paste = L 'Paste an order' 'Вставить заказ'
[IO.File]::WriteAllText((Join-Path $Folder 'test-clipboard.txt'), 'pasted@example.com|pw|GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ', (New-Object Text.UTF8Encoding $false))
Menu-Pick $paste; Mark; [void](Click $p 'addmore:0')
[void](Expect 'a pasted line email|password|key is an account with its details' '^take\|order$')
[void](Answer (L 'Add pasted@example.com?' 'Добавить pasted@example.com?') 1)
[IO.File]::WriteAllText((Join-Path $Folder 'test-clipboard.txt'), (Get-Content (Join-Path $out 'auth.json') -Raw), (New-Object Text.UTF8Encoding $false))
Menu-Pick $paste; Mark; [void](Click (Win $picker) 'addmore:0')
[void](Expect 'a pasted auth.json is a sign-in' '^take\|auth$')
Start-Sleep -Milliseconds 700
if (Has (Win $picker) 'banner:0') { [void](Click (Win $picker) 'banner:0'); Start-Sleep -Milliseconds 300 }
[IO.File]::WriteAllText((Join-Path $Folder 'test-clipboard.txt'), 'just some words', (New-Object Text.UTF8Encoding $false))
Menu-Pick $paste; Mark; [void](Click (Win $picker) 'addmore:0')
[void](Expect 'anything else is refused' '^take\|nothing$')
[void](Expect '… with a line in the window' '^toast\|')
[IO.File]::WriteAllText((Join-Path $Folder 'test-drop.txt'), (Join-Path $out 'auth.json'), (New-Object Text.UTF8Encoding $false))
Mark; Post (Win $picker) 0x8040 0 0
[void](Expect 'a file dropped on the window is read' '^drop\|auth\.json$')
[void](Expect '… and taken as the sign-in it is' '^take\|auth$')
Start-Sleep -Milliseconds 700
if (Has (Win $picker) 'banner:0') { [void](Click (Win $picker) 'banner:0'); Start-Sleep -Milliseconds 300 }

# Dragging a row to another place
function Drag($h, $from, $to) {
    $r = Regions $h; $k = $r._scale
    $x = [int](($r[$from].X + 60) * $k); $y1 = [int](($r[$from].Y + $r[$from].H / 2) * $k); $y2 = [int](($r[$to].Y + $r[$to].H / 2) * $k)
    Post $h 0x200 0 (LParam $x $y1); Post $h 0x201 1 (LParam $x $y1)
    foreach ($t in 0.3, 0.7, 1.0) { Post $h 0x200 1 (LParam $x ([int]($y1 + ($y2 - $y1) * $t))); Start-Sleep -Milliseconds 70 }
    Post $h 0x202 0 (LParam $x $y2); Start-Sleep -Milliseconds 300
}
$p = Win $picker
$first = (Regions $p)['row:1'].Name
Mark; Drag $p 'row:1' 'row:3'
[void](Expect 'dragging the first row onto the third moves it two places down' '^move\|[0-9a-f]+\|2$')
ExpectNot '… without switching to it, as a click would' '^switch\|' 500
Check 'it is the third row now' ((Regions (Win $picker))['row:3'].Name -eq $first) (Regions (Win $picker))['row:3'].Name
Mark; Drag (Win $picker) 'row:3' 'row:1'
[void](Expect 'and dragging it back puts it first again' '^move\|[0-9a-f]+\|-2$')
Start-Sleep -Milliseconds 500

# The sign-in link alone, for another browser: nothing is opened
Menu-Pick (L 'Copy the sign-in link' 'Скопировать ссылку входа'); Mark; [void](Click (Win $picker) 'addmore:0')
[void](Expect 'the add menu gives just the sign-in link' '^login\|link$')
[void](Expect '… on the clipboard' '^clipboard\|https://auth\.openai\.com/')
ExpectNot '… and opens no browser' '^(open|browser)\|' 400
Check 'the new account waits in a row of its own' (Has (Win $picker) 'signin-cancel:0')
Mark; [void](Click (Win $picker) 'signin-cancel:0'); [void](Expect '… until it is cancelled' '^login\|cancel$')

# Add an account through the (simulated) browser sign-in
Start-Sleep -Milliseconds 400
Mark; [void](Click (Win $picker) 'addrow:0')
[void](Expect 'Add account starts a sign-in' '^login\|begin\|$')
[void](Expect '… in the default browser' '^open\|https://auth\.openai\.com/')
Start-Sleep -Milliseconds 300
Check 'the account being added has a row of its own while it waits' (Has (Win $picker) 'signin-cancel:0')
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

# Delete: nothing is asked, and Undo brings the account back whole
[void](MenuItem 6 (L 'Delete' 'Удалить') '^delete\|' '⋯ Delete removes it at once')
[void](Expect '… and offers Undo in the window instead of asking first' (L '^toast\|.*deleted\|Undo$' '^toast\|.*удалён\|Отменить$'))
Start-Sleep -Milliseconds 800
$p = Win $picker
Shot $p 'deleted-undo'
Check 'the row is gone' (-not (Has $p 'row:6'))
Mark; [void](Click $p 'toast-action:0'); [void](Expect 'Undo restores it' '^restore\|')
Start-Sleep -Milliseconds 1200
$back = (Regions (Win $picker))['row:6']
Check 'it is the sixth row again, under its name' ($back -and $back.Name -match 'Spare') $(if ($back) { $back.Name })
[void](MenuItem 6 (L 'Delete' 'Удалить') '^delete\|' 'deleted again, it stays deleted')
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
    Named $f 'the tray list'
    $fr = Regions $f
    Check 'its icon buttons carry the text shown on hover' ($fr['fo-refresh:0'].Tip -and $fr['fo-open:0'].Tip -and $fr['fo-settings:0'].Tip)
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
function Flip($key, $setting, $what) {
    $before = Setting $setting
    [void](Click $s $key); Start-Sleep -Milliseconds 250
    $after = Setting $setting
    Check $what ("$after" -ne "$before") "$setting stayed $before"
}
function Page([int]$n, $what) {
    Mark; [void](Click $s "nav:$n")
    [void](Expect "the $what page opens" "^settings-page\|$n$")
    Start-Sleep -Milliseconds 300
    Shot $s "settings-$n"
    Named $s "the $what page"
}
$sr = Regions $s
Check 'five pages on the left' ($sr -and $sr.ContainsKey('nav:0') -and $sr.ContainsKey('nav:4') -and -not $sr.ContainsKey('nav:5'))

# ---- Switching
Page 1 'Switching'
$sr = Regions $s
Check 'only the switching page''s controls are there' ($sr.ContainsKey('open-after:0') -and -not $sr.ContainsKey('autostart:0') -and -not $sr.ContainsKey('usage:0') -and -not $sr.ContainsKey('mask:0'))
Check 'Open ChatGPT with it is on' ((Setting 'OpenAfterSwitch') -ne $false)
Flip 'open-after:0' 'OpenAfterSwitch' 'Open ChatGPT with it turns off'
$p = Win $picker
Mark; [void](Click $p 'row:1')
[void](Expect 'with it off, a click only switches' "^switch\|$($ids[1])\|stay$")
[void](Answer $ask.Replace('trinewalters', 'jessicamontgomery482+1') 0 2500)
[void](Expect 'and the app restarts as it was open' "^switched\|$($ids[1])\|ok\|closed started")
Flip 'open-after-row:0' 'OpenAfterSwitch' 'a click anywhere on the row turns it back on'
Flip 'restart:0' 'RestartCodex' 'Restart ChatGPT if it''s open'
Check 'with restarting off, Ask first is gone' (-not (Has $s 'ask:0'))
Flip 'restart:0' 'RestartCodex' '… back'
Check 'and back with it' (Has $s 'ask:0')
Flip 'ask:0' 'AskBeforeRestart' 'Ask first'
Mark; [void](Click (Win $picker) 'row:2')
ExpectNot 'with the question off, no dialog' '^dialog\|' 800
[void](Expect 'and it switches at once' "^switched\|$($ids[2])\|ok")
Flip 'ask:0' 'AskBeforeRestart' '… back'
Flip 'hide-after:0' 'HideAfterSwitch' 'Hide the account list'
Flip 'hide-after-row:0' 'HideAfterSwitch' '… back, from its row'

# The browser list
Flip 'web:0' 'WebOn' 'Account list in a browser'
Start-Sleep -Milliseconds 800
$sr = Regions $s
Check 'Let other devices in and the link appear under it' ($sr.ContainsKey('weblan:0') -and $sr.ContainsKey('weblink:2'))
Shot $s 'settings-1-web'
Mark; [void](Click $s 'weblink:1'); [void](Expect 'Copy copies the link' '^clipboard\|http://')
Mark; [void](Click $s 'weblink:2'); [void](Expect 'Open opens it' '^open\|http://')
$key = Setting 'WebKey'
[void](Click $s 'weblink:0'); Start-Sleep -Milliseconds 300
Check 'New link makes a new key' ((Setting 'WebKey') -ne $key)
Flip 'web:0' 'WebOn' '… and off'
Check 'which hides them again' (-not (Has $s 'weblan:0'))

# ---- Limits
Page 2 'Limits'
Flip 'usage:1' 'UsageMinutes' 'Refresh limits: 5 min'
Check '… is 5' ((Setting 'UsageMinutes') -eq 5)
Flip 'traymode:1' 'TrayIcon' 'Tray icon: % left'
Check '… is percent' ((Setting 'TrayIcon') -eq 'percent')
Flip 'traymode:0' 'TrayIcon' 'Tray icon: the icon again'
Flip 'autowarm:0' 'AutoWarmup' 'When a window is idle'
Flip 'autowarm:0' 'AutoWarmup' '… back'
Flip 'timed:0' 'TimedWarmup' 'Every day at set times'
Mark; [void](Click $s 'times:0')
$tt = Win (L 'Times to start the windows' 'Время запуска окон')
Check 'the set-times button asks for times' ([bool]$tt)
if ($tt) { $e = @(Edits $tt); if ($e.Count) { Type-Into $e[0] '9:30, 14:00' }; [void](Click $tt 'dlg:0'); Start-Sleep -Milliseconds 400; Check 'the times are saved' (((Setting 'WarmupTimes') -join ',') -eq '09:30,14:00') ((Setting 'WarmupTimes') -join ',') }

# ---- General
Page 0 'General'
Flip 'autostart:0' 'Autostart' 'Start with Windows'
Flip 'keep-tray:0' 'KeepInTray' 'Stay in the tray when the window closes'
Flip 'keep-tray-row:0' 'KeepInTray' '… back, from its row'
Flip 'acchk:0' 'AccountHotkeys' 'Ctrl + Shift + Alt + digit'
Mark; [void](Click $s 'hk-clear:0'); Start-Sleep -Milliseconds 300
Check 'the shortcut can be cleared' ((Setting 'Hotkey') -eq '')
Flip 'theme:2' 'Theme' 'Theme: dark'
Flip 'theme:0' 'Theme' 'Theme: system'

# ---- About
Page 4 'About'
Mark; [void](Click $s 'codexapp:0'); [void](Expect 'the app''s Open/Restart button works' '^app\|')
Mark; [void](Click $s 'authfile:0'); [void](Expect 'Show opens the sign-in file in Explorer' '^open\|explorer\.exe\|/select,')
Mark; [void](Click $s 'folder:0'); [void](Expect 'Data folder opens the folder' ('^open\|' + [regex]::Escape($Folder)))

# ---- Accounts
Page 3 'Accounts'
Mark; [void](Click $s 'manage:0'); [void](Expect 'Account manager opens the manager' '^window\|manager$')
Post (Win (L 'Account manager — Codex Accounts' 'Управление аккаунтами — Аккаунты Codex')) 0x0010 0 0
Flip 'mask:0' 'MaskAll' 'Hide emails'
Flip 'mask:0' 'MaskAll' '… back'
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

Post $s 0x0010 0 0
Check 'Settings closes' (Gone $settingsTitle)

# ================================================================== account manager
Section 'Account manager'
$managerTitle = L 'Account manager — Codex Accounts' 'Управление аккаунтами — Аккаунты Codex'
$utf8 = New-Object Text.UTF8Encoding $false
function Logins { @((Get-Content (Join-Path $Folder 'logins.dat') -Raw -Encoding utf8 | ConvertFrom-Json).logins | Where-Object { $_.password -or $_.key -or $_.order }) }
function Login($email) { Logins | Where-Object { $_.email -eq $email } | Select-Object -First 1 }
# The 2FA codes a key gives around now, as authenticator apps make them, to compare with the switcher's.
function Codes([string]$b32) {
    $alpha = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'; $bits = ''
    foreach ($ch in $b32.ToUpper().Replace(' ', '').ToCharArray()) { $bits += [Convert]::ToString($alpha.IndexOf($ch), 2).PadLeft(5, '0') }
    $bytes = [byte[]]@(for ($i = 0; $i + 8 -le $bits.Length; $i += 8) { [Convert]::ToByte($bits.Substring($i, 8), 2) })
    $h = New-Object Security.Cryptography.HMACSHA1 (, $bytes)
    $t = [Math]::Floor([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() / 30)
    foreach ($n in ($t - 1), $t, ($t + 1)) {
        $c = [BitConverter]::GetBytes([int64]$n); [Array]::Reverse($c)
        $hash = $h.ComputeHash($c); $o = $hash[19] -band 15
        $v = (([int]$hash[$o] -band 0x7f) -shl 24) -bor ([int]$hash[$o + 1] -shl 16) -bor ([int]$hash[$o + 2] -shl 8) -bor [int]$hash[$o + 3]
        ($v % 1000000).ToString('000000')
    }
}
function Hotkey { Post $mg 0x0312 0x4A01 0 }   # what Ctrl+Alt+V sends

Signal 0
Mark; [void](Click (Win $picker) 'manage:0')
[void](Expect 'the people button in the list opens the manager' '^window\|manager$')
$mg = Win $managerTitle
Check 'the manager is open' ([bool]$mg)
Shot $mg 'manager-accounts'
$r = Regions $mg
Check 'it shows the accounts already on the list, with the two ways in' ($r -and $r.ContainsKey('mrow:0') -and $r.ContainsKey('paste-order:0') -and $r.ContainsKey('bykey:0') -and $r.ContainsKey('filter:4'))
Named $mg 'the manager'
$e = Uia $mg 'bykey:0'
Mark; if ($e) { $e.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
$d = Win (L 'A 2FA code from a key' 'Код 2FA по ключу')
Check 'Narrator can press a button: Code from a key opens its dialog' ([bool]$d)
if ($d) { Post $d 0x0010 0 0; [void](Gone (L 'A 2FA code from a key' 'Код 2FA по ключу')) }

# A code from a key, like 2fa.fb.tools
Mark; [void](Click $mg 'bykey:0')
$d = Win (L 'A 2FA code from a key' 'Код 2FA по ключу')
Check 'Code from a key asks for the key' ([bool]$d)
if ($d) { Type-Into (@(Edits $d)[0]) 'gezd gnbv gy3t qojq gezd gnbv gy3t qojq'; [void](Click $d 'dlg:0') }
$hit = Expect 'its code is copied' '^clipboard\|\d{6}$'
[void](Expect '… and the window says so, as a silent copy leaves one guessing' '^toast\|.+')
if ($hit) { Check 'and it is the right code for that key' ((Codes 'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ') -contains $hit.Substring(10)) $hit }
Check 'it stays on view with its countdown' (Has $mg 'scode:0')
[void](Click $mg 'sdel:0'); Start-Sleep -Milliseconds 300
Check '✕ removes it' (-not (Has $mg 'scode:0'))

# The shop's message
$order = @'
✅ Заказ выдан

Заказ: STORE-170000000000000-12ab1c
Товар: ChatGPT plus | Apple Pay | 1M | W24H
Количество в этой выдаче: 2

🔐 Данные товара:
username|pass|2fa-code
bought.one@example.com|Shop-Pass-1|GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ
bought.two@example.com|Shop-Pass-2|https://2fa.live/tok/JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP

Инструкция:
Гарантия поставщика
'@
[IO.File]::WriteAllText((Join-Path $Folder 'test-clipboard.txt'), $order, $utf8)
Mark; [void](Click $mg 'paste-order:0')
[void](Answer (L 'Add 2 accounts?' 'Добавить 2 аккаунта?') 0)
[void](Expect 'Paste an order reads two accounts from the message' '^import\|2\|0$')
Start-Sleep -Milliseconds 400
$one = Login 'bought.one@example.com'; $two = Login 'bought.two@example.com'
Check 'with their passwords and 2FA keys' ($one.password -eq 'Shop-Pass-1' -and $one.key -eq 'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ' -and $two.key -eq 'JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP')

# Search: by email, order or note
$r = Regions $mg
Check 'the search field is a control like the others, reached by Tab' ($r.ContainsKey('search:0') -and $r['search:0'].Focusable -and $r['search:0'].Name)
Mark; [void](Click $mg 'search:0'); Start-Sleep -Milliseconds 300
$box = @(Edits $mg)[0]
Check 'a click on it opens a text box' ([bool]$box)
if ($box) {
    Type-Into $box 'BOUGHT.two'
    [void](Expect 'typing searches' '^search\|BOUGHT\.two$')
    Start-Sleep -Milliseconds 300
    $r = Regions $mg
    Check 'only the account with that email is left, whatever the letter case' ($r.ContainsKey('mrow:0') -and -not $r.ContainsKey('mrow:1') -and $r['mrow:0'].Name -match 'bought\.two') $r['mrow:0'].Name
    Check 'the tabs count what the search leaves' ($r['filter:0'].Name -match ' 1$') $r['filter:0'].Name
    Type-Into $box '170000000000000-12ab1c'; Start-Sleep -Milliseconds 300
    $r = Regions $mg
    Check 'an order number finds both accounts of that order' ($r.ContainsKey('mrow:1') -and -not $r.ContainsKey('mrow:2'))
    Type-Into $box 'no-such-account'; Start-Sleep -Milliseconds 300
    Check 'a search that finds nothing leaves no cards' (-not (Has $mg 'mrow:0'))
    Shot $mg 'manager-search-empty'
    [void](Click $mg 'search-clear:0'); Start-Sleep -Milliseconds 300
    $r = Regions $mg
    Check '✕ clears it: every account is back and the field is at rest' ($r.ContainsKey('mrow:6') -and $r.ContainsKey('search:0'))
}

# A window too short for its cards: the scroll bar and the keys
$rc = New-Object CasUiTest.U+RECT; [void][CasUiTest.U]::GetWindowRect($mg, [ref]$rc)
$sc = (Regions $mg)._scale
[void][CasUiTest.U]::SetWindowPos($mg, [IntPtr]::Zero, 0, 0, [int](900 * $sc), [int](480 * $sc), 0x16); Start-Sleep -Milliseconds 300
$r = Regions $mg
Check 'cards that do not fit get a scroll bar' ($r.ContainsKey('scrollbar:0') -and $r._scrollMax -gt 0) "scrollMax $($r._scrollMax)"
Key $mg 0x23; $r = Regions $mg
Check 'End goes to the bottom' ($r._scrollMax -gt 0 -and [math]::Abs($r._scroll - $r._scrollMax) -lt 1) "$($r._scroll) of $($r._scrollMax)"
Key $mg 0x21; $r2 = Regions $mg
Check 'Page Up goes back a page' ($r2._scroll -lt $r._scroll) "$($r2._scroll)"
Key $mg 0x24; $r = Regions $mg
Check 'Home goes to the top' ($r._scroll -eq 0) "$($r._scroll)"
$bad = ''; $seen = @{}
for ($i = 0; $i -lt 90; $i++) {
    Key $mg 0x28   # Down moves focus on like Tab, and doesn't turn back if Shift happens to be held at this PC
    $r = Regions $mg; $f = $r._focus
    if ($seen.ContainsKey($f)) { break }
    $seen[$f] = $true
    $c = $r[$f]
    if (-not $c) { $bad = "$f is not on the window"; break }
    if ($c.Y -lt -1 -or $c.Y + $c.H -gt $r._clientH + 1) { $bad = "$f at $($c.Y)…$($c.Y + $c.H) of $($r._clientH)"; break }
}
Check 'the keyboard brings every control it reaches into view' (-not $bad -and $seen.Count -gt 10) "$bad after $($seen.Count)"
Key $mg 0x24
[void][CasUiTest.U]::SetWindowPos($mg, [IntPtr]::Zero, 0, 0, $rc.Right - $rc.Left, $rc.Bottom - $rc.Top, 0x16); Start-Sleep -Milliseconds 300
$now = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
Check 'the order, a 24-hour warranty and a month paid' ($one.order -eq 'STORE-170000000000000-12ab1c' -and [math]::Abs($one.warrantyUntil - $now - 86400000) -lt 120000 -and [math]::Abs($one.paidUntil - $now - 2592000000) -lt 120000)
Shot $mg 'manager-order'
$r = Regions $mg
Check 'the first one opens with its five steps' ($r.ContainsKey('site:0') -and $r.ContainsKey('make-pass:0') -and $r.ContainsKey('paste-key:0') -and $r.ContainsKey('phone:0') -and $r.ContainsKey('signin:0'))
Mark; [void](Click $mg 'mcode:0')
$hit = Expect 'a click on its code copies it' '^clipboard\|\d{6}$'
if ($hit) { Check '… the right one' ((Codes $one.key) -contains $hit.Substring(10)) $hit }
[IO.File]::WriteAllText((Join-Path $Folder 'test-clipboard.txt'), $order, $utf8)
Mark; [void](Click $mg 'paste-order:0')
[void](Answer (L 'Add 2 accounts?' 'Добавить 2 аккаунта?') 0)
[void](Expect 'the same order again adds nothing' '^import\|0\|0$')
Check 'still two' (@(Logins).Count -eq 2)
$r = Regions $mg
Check 'two cards not signed in yet offer to be signed in one after another' ($r.ContainsKey('lineup:0') -and $r['lineup:0'].Name -match '2$') $r['lineup:0'].Name

# Step 1: the site, in a browser profile of its own, and Ctrl+Alt+V
Mark; [void](Click $mg 'site:0')
[void](Expect 'Open the site opens chatgpt.com in the account''s own browser profile' ('^browser\|' + $one.id + '\|https://chatgpt\.com/$'))
[void](Expect 'and Ctrl+Alt+V gets ready to type the email, the password and the code' ('^arm\|' + $one.id + '\|email,password,code$'))
Start-Sleep -Milliseconds 300
Shot $mg 'manager-site'
Check 'the order of what it types shows' ((Has $mg 'chip:2'))
Mark; Hotkey; [void](Expect 'Ctrl+Alt+V types the email and presses Enter' '^type\|email\|enter$')
Mark; Hotkey; [void](Expect '… then the password' '^type\|password\|enter$')
Mark; Hotkey; [void](Expect '… then the 2FA code' '^type\|code\|enter$')
Mark; [void](Click $mg 'chip:1'); Hotkey; [void](Expect 'a click on a field makes it the next one' '^type\|password\|enter$')

# Step 2: a new password
Mark; [void](Click $mg 'make-pass:0'); Start-Sleep -Milliseconds 300
$one = Login 'bought.one@example.com'
Check 'New password makes a strong one' ($one.newPassword.Length -eq 20 -and $one.password -eq 'Shop-Pass-1')
[void](Expect 'and copies it' ('^clipboard\|' + [regex]::Escape($one.newPassword) + '$'))
[void](Expect 'Ctrl+Alt+V now types the old password, then the new one twice' '^arm\|[0-9a-f]+\|password,new-password,new-password$')
Mark; Hotkey; Hotkey; [void](Expect 'it types the new one' '^type\|new-password$')
$new = $one.newPassword
[void](Click $mg 'pass-done:0'); Start-Sleep -Milliseconds 300
$one = Login 'bought.one@example.com'
Check 'Changed it keeps the new password and the old one aside' ($one.password -eq $new -and $one.oldPassword -eq 'Shop-Pass-1' -and $one.passwordDone -and -not $one.newPassword)

# Step 3: a new 2FA key
Mark; [void](Click $mg 'paste-key:0')
$d = Win (L 'The new 2FA key' 'Новый ключ 2FA')
Check 'Paste the key asks for it' ([bool]$d)
if ($d) { Type-Into (@(Edits $d)[0]) 'otpauth://totp/OpenAI:bought.one%40example.com?secret=MFRGGZDFMZTWQ2LKNNWG23TPOBYXE43U&issuer=OpenAI'; [void](Click $d 'dlg:0') }
Start-Sleep -Milliseconds 400
$one = Login 'bought.one@example.com'
Check 'the key is read from an otpauth link' ($one.newKey -eq 'MFRGGZDFMZTWQ2LKNNWG23TPOBYXE43U')
$hit = Expect 'the code of the new key is copied, to confirm it on the site' '^clipboard\|\d{6}$'
if ($hit) { Check '… the right one' ((Codes $one.newKey) -contains $hit.Substring(10)) $hit }
[void](Expect 'Ctrl+Alt+V types it' '^arm\|[0-9a-f]+\|new-code$')
Shot $mg 'manager-key'
[void](Click $mg 'key-done:0'); Start-Sleep -Milliseconds 300
$one = Login 'bought.one@example.com'
Check 'Changed it switches the codes to the new key' ($one.key -eq 'MFRGGZDFMZTWQ2LKNNWG23TPOBYXE43U' -and $one.oldKey -eq 'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ' -and $one.keyDone)

# Step 4: the phone, by hand
[void](Click $mg 'phone:0'); Start-Sleep -Milliseconds 300
Check 'Done marks the phone' ((Login 'bought.one@example.com').phoneDone)
[void](Click $mg 'step-check:3'); Start-Sleep -Milliseconds 300
Check 'its circle unmarks it' (-not (Login 'bought.one@example.com').phoneDone)
[void](Click $mg 'step-check:3'); Start-Sleep -Milliseconds 300
Check '… and marks it again' ((Login 'bought.one@example.com').phoneDone)

# Step 5: signing in to the switcher, in the default browser, with the link to copy
Mark; [void](Click $mg 'signin:0')
[void](Expect 'Sign in starts a sign-in for this card' ('^login\|begin\|\|card:' + $one.id + '$'))
[void](Expect '… in the default browser' '^open\|https://auth\.openai\.com/oauth/authorize')
Start-Sleep -Milliseconds 300
Check 'the waiting step offers the link and a way out' ((Has $mg 'signin-link:0') -and (Has $mg 'signin-cancel:0'))
Mark; [void](Click $mg 'signin-link:0'); [void](Expect 'the link can be copied into the account''s own browser window' '^clipboard\|https://auth\.openai\.com/')
ExpectNot 'the account list doesn''t pop up for it' '^window\|picker$' 400
[IO.File]::WriteAllText((Join-Path $Folder 'test-login.txt'), 'bought.one@example.com', $utf8)
[void](Expect 'the account it brings is linked to the card' ('^linked\|' + $one.id + '\|') 8000)
[void](Expect 'and a notification says it is ready' '^notify\|' 3000)
Start-Sleep -Milliseconds 800
Check 'it is on the switcher''s list' ([bool](Login 'bought.one@example.com').accountId)
Shot $mg 'manager-ready'

# Filters, the menu, deleting
Mark; [void](Click $mg 'filter:1'); Start-Sleep -Milliseconds 300
$r = Regions $mg
Check 'Need you lists the unfinished one first' ($r.ContainsKey('mrow:0') -and -not $r.ContainsKey('mrow:3'))
Menu-Pick (L 'Copy password' 'Копировать пароль')
Mark; [void](Click $mg 'mrow:0' -Right)
[void](Expect 'a right click opens its menu; Copy password copies it' '^clipboard\|Shop-Pass-2$')
[void](Expect '… and says that it will leave the clipboard' (L '^toast\|Password copied.*45' '^toast\|Пароль скопирован.*45'))
Menu-Pick (L 'Delete' 'Удалить')
Mark; [void](Click $mg 'mmore:0')
[void](Answer (L 'Delete bought.two@example.com?' 'Удалить bought.two@example.com?') 0)
[void](Expect '⋯ → Delete removes the card' '^manager-delete\|')
Start-Sleep -Milliseconds 300
Check 'it is gone' (-not (Login 'bought.two@example.com'))
[void](Click $mg 'filter:0')

# Reminders: a warranty about to end on an unfinished account, and a paid month over
[IO.File]::WriteAllText((Join-Path $Folder 'test-clipboard.txt'), "third@example.com|pw3|GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ", $utf8)
Mark; [void](Click $mg 'paste-order:0')
[void](Answer (L 'Add third@example.com?' 'Добавить third@example.com?') 0)
[void](Expect 'a bare email|password|key line is read too' '^import\|1\|0$')
$all = Get-Content (Join-Path $Folder 'logins.dat') -Raw -Encoding utf8 | ConvertFrom-Json
foreach ($c in $all.logins) {
    if ($c.email -eq 'third@example.com') { $c | Add-Member -NotePropertyName warrantyUntil -NotePropertyValue ($now + 40 * 60000) -Force }
    if ($c.email -eq 'bought.one@example.com') { $c | Add-Member -NotePropertyName paidUntil -NotePropertyValue ($now - 60000) -Force }
}
[IO.File]::WriteAllText((Join-Path $Folder 'logins.dat'), ($all | ConvertTo-Json -Depth 6), $utf8)
Start-Sleep -Milliseconds 300
Mark; Signal 4
[void](Expect 'a warranty ending within two hours is announced' (L '^notify\|The warranty on third@example\.com ends in' '^notify\|Гарантия на third@example\.com кончится через'))
Mark; Signal 4
ExpectNot '… once' '^notify\|' 600
Mark; [void](Click $mg 'filter:4'); Start-Sleep -Milliseconds 300
Check 'Over lists the account whose month ended, with Delete all' ((Has $mg 'mrow:0') -and (Has $mg 'delete-expired:0'))
Shot $mg 'manager-expired'
$acc = (Login 'bought.one@example.com').accountId
[void](Click $mg 'delete-expired:0')
[void](Answer (L 'Delete 1 account whose paid month is over?' 'Удалить 1 аккаунт с кончившейся подпиской?') 0)
[void](Expect 'Delete all removes its card and its account' ('^manager-delete\|[0-9a-f]+\|account$'))
[void](Expect '… from the switcher too' ('^delete\|' + $acc))
Start-Sleep -Milliseconds 800
Check 'only the third is left in the manager''s details' (@(Logins).Count -eq 1)

# Sign in to all: one browser window at a time, the next when the one before is done
[void](Click $mg 'filter:0')
[IO.File]::WriteAllText((Join-Path $Folder 'test-clipboard.txt'), "queue.one@example.com|pw|GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ`nqueue.two@example.com|pw|JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP", $utf8)
Mark; [void](Click $mg 'paste-order:0')
[void](Answer (L 'Add 2 accounts?' 'Добавить 2 аккаунта?') 0)
[void](Expect 'two more accounts are taken in' '^import\|2\|0$')
Start-Sleep -Milliseconds 400
Mark; [void](Click $mg 'lineup:0')
$hit = Expect 'Sign in to all starts with the first card' '^lineup\|[0-9a-f]+$'
if ($hit) {
    $firstCard = $hit.Substring(7)
    [void](Expect '… in the default browser' '^open\|https://auth\.openai\.com/')
    Check 'the button now stops the queue and counts' ((Regions $mg)['lineup:0'].Name -match '1 \S+ 3')
    $email = ((Get-Content (Join-Path $Folder 'logins.dat') -Raw -Encoding utf8 | ConvertFrom-Json).logins | Where-Object { $_.id -eq $firstCard }).email
    Mark; [IO.File]::WriteAllText((Join-Path $Folder 'test-login.txt'), $email, $utf8)
    [void](Expect 'when that sign-in comes back the card is linked' ('^linked\|' + $firstCard + '\|') 8000)
    $next = Expect '… and the next card''s window opens by itself' '^lineup\|[0-9a-f]+$' 4000
    Check '… a different card' ($next -and $next.Substring(7) -ne $firstCard)
    Mark; [void](Click $mg 'lineup:0'); [void](Expect 'Stop ends the queue and the waiting sign-in' '^login\|cancel$')
    Start-Sleep -Milliseconds 500
}
[void](Click $mg 'filter:0')
Post $mg 0x0010 0 0
Check 'the manager closes' (Gone $managerTitle)

# ================================================================== the end
Section 'Quit'
Post (Tray) 0x8001 0 0x400
$f = Win $flyoutTitle
Mark; [void](Click $f 'fo-exit:0')
[void](Expect 'Quit switcher quits' '^exit$')
$script:proc.WaitForExit(5000) | Out-Null
Check 'the process ends' ($script:proc.HasExited)

# ================================================================== a first start
Section 'First start'
$script:proc = Start-Demo -Empty
Start-Sleep -Milliseconds 800
$p = Win $picker
$r = Regions $p
Check 'with no accounts the window says what it is for and offers one button' ($r -and $r.ContainsKey('addrow:0') -and -not $r.ContainsKey('row:1') -and $r['addrow:0'].Name) ($r.Keys -join ', ')
Named $p 'the empty list'
Shot $p 'picker-empty'
Mark; [void](Click $p 'addrow:0'); [void](Expect 'that button starts a sign-in' '^login\|begin')

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
