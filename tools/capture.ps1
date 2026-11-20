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
    [ValidateSet('picker', 'settings', 'flyout', 'details')][string]$Window = 'picker',
    [string]$Exe = (Join-Path $PSScriptRoot '..\build\x64\CodexAccountSwitcher.exe'),
    [double]$Scale = 1,        # the display scale (or what the demo was started with)
    [switch]$OpenFlyout,       # click the demo's tray icon first
    [switch]$ClickSettings,    # click the picker's settings button first
    [switch]$OpenDetails,      # press I in the picker first: details of the account in use
    [int]$Scroll = 0,          # turn the mouse wheel this many notches down in the window first
    [double[]]$Click,          # x, y in DIPs: click there in the window first (after scrolling)
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
            if (want.StartsWith("*") ? t.EndsWith(want.Substring(1)) && !t.StartsWith("Settings") && !t.StartsWith("Настройки") : t == want) { found = h; return false; }
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

if ($OpenFlyout) {
    # NIN_SELECT, what a left click on the icon sends.
    [void][CasCapture.W]::PostMessage([CasCapture.W]::Tray($id), 0x8001, [IntPtr]::Zero, [IntPtr]0x400)
    Start-Sleep -Milliseconds 700
}
if ($ClickSettings) {
    # The gear sits 41 DIPs in from the right edge and 30 down.
    $picker = [CasCapture.W]::Find($id, $titles.picker)
    $c = New-Object CasCapture.W+RECT; [void][CasCapture.W]::GetClientRect($picker, [ref]$c)
    $x = [int]($c.Right - 41 * $Scale); $y = [int](30 * $Scale)
    $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    [void][CasCapture.W]::PostMessage($picker, 0x0200, [IntPtr]::Zero, $lp)   # move
    [void][CasCapture.W]::PostMessage($picker, 0x0201, [IntPtr]1, $lp)        # down
    [void][CasCapture.W]::PostMessage($picker, 0x0202, [IntPtr]::Zero, $lp)   # up
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
if ($Click) {
    $x = [int]($Click[0] * $Scale); $y = [int]($Click[1] * $Scale)
    $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    [void][CasCapture.W]::PostMessage($h, 0x0200, [IntPtr]::Zero, $lp)
    [void][CasCapture.W]::PostMessage($h, 0x0201, [IntPtr]1, $lp)
    [void][CasCapture.W]::PostMessage($h, 0x0202, [IntPtr]::Zero, $lp)
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
