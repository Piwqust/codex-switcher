<#
.SYNOPSIS
    Saves one of the demo switcher's windows to a PNG.

.DESCRIPTION
    Asks that single window to draw its content into a picture, so nothing else on the screen
    can end up in it and the window can sit off screen. The title bar Windows draws is left
    out, and the plain background stands in for Mica. Start the demo first with tools\demo.ps1.

.EXAMPLE
    .\tools\capture.ps1 -Window picker -Out picker.png
    .\tools\capture.ps1 -Window flyout -OpenFlyout -Out tray.png
    .\tools\capture.ps1 -Window settings -ClickSettings -Scale 1.25 -Out settings.png
#>
param(
    [Parameter(Mandatory)][string]$Out,
    [ValidateSet('picker', 'settings', 'flyout', 'details', 'manager')][string]$Window = 'picker',
    [string]$Exe = (Join-Path $PSScriptRoot '..\build\x64\CodexAccountSwitcher.exe'),
    [double]$Scale = 1,        # the display scale, used only when the window can't say its own (a demo started with -Visible)
    [switch]$OpenFlyout,       # click the demo's tray icon first
    [switch]$ClickSettings,    # click the picker's settings button first
    [switch]$OpenManager,      # click the picker's account manager button first
    [switch]$OpenDetails,      # press I in the picker first: details of the account in use
    [int]$Scroll = 0,          # turn the mouse wheel this many notches down in the window first
    [double[]]$Click,          # x, y in DIPs: click there in the window first (after scrolling)
    [string]$Control,          # click this control of the window first, by its name in the checks, e.g. nav:1
    [string]$Then,             # then capture the window with this title instead, e.g. a dialog the click opened
    [string]$Title,            # work on the window with this title rather than -Window's, e.g. a dialog
    [string]$Folder = (Join-Path $env:TEMP 'cas-demo')   # where tools\demo.ps1 put the demo
)
$ErrorActionPreference = 'Stop'
$titles = @{
    picker   = @('Codex Accounts', 'Аккаунты Codex')
    settings = @('Settings — Codex Accounts', 'Настройки — Аккаунты Codex')
    flyout   = @('Account list', 'Список аккаунтов')
    details  = @('* — Codex Accounts', '* — Аккаунты Codex')   # "<account> — Codex Accounts"
    manager  = @('Account manager — Codex Accounts', 'Управление аккаунтами — Аккаунты Codex')
}
Add-Type -AssemblyName System.Drawing
Add-Type -Name W -Namespace CasCapture -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
[DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
public struct RECT { public int Left, Top, Right, Bottom; }
public static IntPtr Find(uint pid, string[] titles) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
        uint p; GetWindowThreadProcessId(h, out p);
        if (p != pid || !IsWindowVisible(h)) return true;
        var sb = new System.Text.StringBuilder(256); GetWindowText(h, sb, 256);
        var t = sb.ToString();
        foreach (var want in titles)
            if (want.StartsWith("*") ? t.EndsWith(want.Substring(1)) && !t.StartsWith("Settings") && !t.StartsWith("Настройки") && !t.StartsWith("Account manager") && !t.StartsWith("Управление") : t == want) { found = h; return false; }
        return true;
    }, IntPtr.Zero);
    return found;
}
public static IntPtr Tray(uint pid) {
    for (IntPtr h = IntPtr.Zero; (h = FindWindowEx(IntPtr.Zero, h, "CodexAccountSwitcherTray", null)) != IntPtr.Zero;) {
        uint p; GetWindowThreadProcessId(h, out p); if (p == pid) return h;
    }
    return IntPtr.Zero;
}
'@
# Per-monitor aware, so window sizes come in real pixels rather than scaled down to 96 DPI.
[void][CasCapture.W]::SetThreadDpiAwarenessContext([IntPtr]-4)
$exePath = (Resolve-Path $Exe).Path
$proc = Get-Process CodexAccountSwitcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exePath } | Select-Object -First 1
if (-not $proc) { throw "The demo switcher is not running. Start it with tools\demo.ps1." }
$id = [uint32]$proc.Id

# The window itself says where its controls are and at what scale it draws (off screen, the
# demo answers this), so a click lands on its button whatever the monitor's scale. -Scale is
# only what is used when it doesn't answer, as with -Visible.
function Layout($h) {
    $n = Get-Random -Minimum 1000 -Maximum 100000000
    $file = Join-Path $Folder "regions-$n.txt"
    [void][CasCapture.W]::PostMessage($h, 0x803E, [IntPtr]$n, [IntPtr]::Zero)
    for ($i = 0; $i -lt 25 -and -not (Test-Path $file); $i++) { Start-Sleep -Milliseconds 60 }
    if (-not (Test-Path $file)) { return $null }
    Start-Sleep -Milliseconds 40
    $lines = [IO.File]::ReadAllLines($file); Remove-Item $file -Force
    $out = @{ _scale = [double]($lines[0].Split(' ')[1]) }
    foreach ($line in $lines | Select-Object -Skip 1) {
        $q = $line.Split("`t")[0].Split(' ')
        if ($q.Count -ge 5 -and -not $out.ContainsKey($q[0])) { $out[$q[0]] = @(([double]$q[1] + [double]$q[3] / 2), ([double]$q[2] + [double]$q[4] / 2)) }
    }
    $out
}
function ClickAt($h, [double]$x, [double]$y) {
    $lp = [IntPtr](([int]$y -shl 16) -bor ([int]$x -band 0xFFFF))
    [void][CasCapture.W]::PostMessage($h, 0x0200, [IntPtr]::Zero, $lp)   # move
    [void][CasCapture.W]::PostMessage($h, 0x0201, [IntPtr]1, $lp)        # down
    [void][CasCapture.W]::PostMessage($h, 0x0202, [IntPtr]::Zero, $lp)   # up
}
# A click on a control by its name; from the right edge when the window doesn't say where it is.
function ClickControl($h, $name, [double]$fromRight, [double]$down) {
    $l = Layout $h
    if ($l -and $l.ContainsKey($name)) { ClickAt $h ($l[$name][0] * $l._scale) ($l[$name][1] * $l._scale); return }
    $c = New-Object CasCapture.W+RECT; [void][CasCapture.W]::GetClientRect($h, [ref]$c)
    ClickAt $h ($c.Right - $fromRight * $Scale) ($down * $Scale)
}

if ($OpenFlyout) {
    # NIN_SELECT, what a left click on the icon sends.
    [void][CasCapture.W]::PostMessage([CasCapture.W]::Tray($id), 0x8001, [IntPtr]::Zero, [IntPtr]0x400)
    Start-Sleep -Milliseconds 700
}
if ($ClickSettings) {
    # The gear: 36 DIPs in from the right edge and 30 down.
    ClickControl ([CasCapture.W]::Find($id, $titles.picker)) 'settings:0' 36 30
    Start-Sleep -Milliseconds 900
}
if ($OpenManager) {
    # The people button: 108 DIPs in from the right edge and 30 down.
    ClickControl ([CasCapture.W]::Find($id, $titles.picker)) 'manage:0' 108 30
    Start-Sleep -Milliseconds 900
}
if ($OpenDetails) {
    $picker = [CasCapture.W]::Find($id, $titles.picker)
    [void][CasCapture.W]::PostMessage($picker, 0x0100, [IntPtr]0x49, [IntPtr]0x00170001)   # WM_KEYDOWN, I
    Start-Sleep -Milliseconds 1200   # its statistics arrive on another thread
}

$h = [CasCapture.W]::Find($id, $(if ($Title) { @($Title) } else { $titles[$Window] }))
if ($h -eq [IntPtr]::Zero) { throw "The $Window window is not visible." }
Start-Sleep -Milliseconds 500   # let it finish appearing
for ($i = 0; $i -lt $Scroll; $i++) {
    [void][CasCapture.W]::PostMessage($h, 0x020A, [IntPtr](-120 * 65536), [IntPtr]::Zero)   # WM_MOUSEWHEEL, one notch down
    Start-Sleep -Milliseconds 60
}
if ($Scroll) { Start-Sleep -Milliseconds 600 }
if ($Control) {
    ClickControl $h $Control 0 0
    Start-Sleep -Milliseconds 900
}
if ($Click) {
    $l = Layout $h
    $k = if ($l) { $l._scale } else { $Scale }
    ClickAt $h ($Click[0] * $k) ($Click[1] * $k)
    Start-Sleep -Milliseconds 900
}
if ($Then) {
    $h = [CasCapture.W]::Find($id, @($Then))
    if ($h -eq [IntPtr]::Zero) { throw "No window titled $Then." }
    Start-Sleep -Milliseconds 400
}
# The window draws itself into a PNG in the demo's snapshots folder (CAS_SNAPSHOT), so the
# picture doesn't depend on where it is or on what Windows composes.
$n = Get-Random -Minimum 1000 -Maximum 1000000
$png = Join-Path $Folder "snapshots\snapshot-$n.png"
[void][CasCapture.W]::PostMessage($h, 0x803D, [IntPtr]$n, [IntPtr]::Zero)
for ($i = 0; $i -lt 50 -and -not (Test-Path $png); $i++) { Start-Sleep -Milliseconds 100 }
if (-not (Test-Path $png)) { throw "The $Window window didn't save its picture. Is the demo running from $Folder?" }
Start-Sleep -Milliseconds 150
$full = [IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force (Split-Path $full) | Out-Null
Move-Item $png $full -Force
Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Image]::FromFile($full); "Saved $($img.Width)x$($img.Height) to $full"; $img.Dispose()
