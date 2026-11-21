<#
.SYNOPSIS
    Redraws the pictures in docs/images from the demo, in both languages.

.DESCRIPTION
    Runs tools\demo.ps1 off screen for each look and saves the picker, the account details,
    the tray list and Settings with tools\capture.ps1, and the browser list with Microsoft
    Edge running headless. Nothing appears on screen. The windows say where their buttons are,
    so the display scale needn't be passed.

.EXAMPLE
    .\tools\make-images.ps1
#>
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\build\x64\CodexAccountSwitcher.exe'),
    [double]$Scale = 1
)
$ErrorActionPreference = 'Stop'
$images = Join-Path $PSScriptRoot '..\docs\images'
$demo = Join-Path $PSScriptRoot 'demo.ps1'
$capture = Join-Path $PSScriptRoot 'capture.ps1'
$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
$edgeProfile = Join-Path $env:TEMP 'cas-demo-edge'
# One scale for every picture. Left to the monitor, an off-screen window takes the scale of
# whichever screen it was created on, and the pictures came out at 100% or 125% by chance.
$shotScale = 1.25

foreach ($lang in 'en', 'ru') {
    $dir = if ($lang -eq 'en') { $images } else { Join-Path $images 'ru' }
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($theme in 'dark', 'light') {
        & $demo -Exe $Exe -Theme $theme -Language $lang -Scale $shotScale -KeepFlyout -CodexOpen -Web:($theme -eq 'dark') | Out-Null
        Start-Sleep -Seconds 2
        & $capture -Exe $Exe -Window picker -Out (Join-Path $dir "picker-$theme.png") | Out-Null
        if ($theme -ne 'dark') { continue }
        # Details first: it fetches the statistics the tray list's top line shows.
        & $capture -Exe $Exe -Window details -OpenDetails -Out (Join-Path $dir 'details.png') | Out-Null
        & $capture -Exe $Exe -Window flyout -OpenFlyout -Out (Join-Path $dir 'tray.png') | Out-Null
        if ($edge) {
            $png = Join-Path $dir 'browser.png'
            Remove-Item $png -ErrorAction SilentlyContinue
            $p = Start-Process -FilePath $edge -WindowStyle Hidden -PassThru -ArgumentList @(
                '--headless=new', '--disable-gpu', "--user-data-dir=$edgeProfile", '--hide-scrollbars', '--force-dark-mode',
                "--lang=$lang", '--window-size=760,660', "--screenshot=$png", 'http://127.0.0.1:3291/?key=demo-key-not-secret')
            [void]$p.WaitForExit(30000)
        }
    }
    # Settings on its Switching page, from a demo without the browser list, so the page ends in view.
    & $demo -Exe $Exe -Theme dark -Language $lang -Scale $shotScale -CodexOpen | Out-Null
    Start-Sleep -Seconds 2
    & $capture -Exe $Exe -Window settings -ClickSettings -Scale $Scale -Control 'nav:1' -Out (Join-Path $dir 'settings.png') | Out-Null
    # The account manager with its first card open: an account from today's order, half secured.
    & $capture -Exe $Exe -Window manager -OpenManager -Scale $Scale -Control 'mrow:0' -Out (Join-Path $dir 'manager.png') | Out-Null
}
Get-Process CodexAccountSwitcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Resolve-Path $Exe).Path } | Stop-Process -Force
Remove-Item $edgeProfile -Recurse -Force -ErrorAction SilentlyContinue

# The repository's social preview (Settings → General → Social preview on GitHub): 1280×640.
Add-Type -AssemblyName System.Drawing
$card = New-Object System.Drawing.Bitmap 1280, 640
$g = [System.Drawing.Graphics]::FromImage($card)
$g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.TextRenderingHint = 'AntiAliasGridFit'
$g.Clear([System.Drawing.Color]::FromArgb(255, 20, 20, 20))
$shot = [System.Drawing.Image]::FromFile((Join-Path $images 'picker-dark.png'))
$w = 700; $h = [int]($shot.Height * $w / $shot.Width)
$x = 1280 - $w - 48; $y = [int]((640 - $h) / 2)
$frame = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 58, 58, 58)), 2
$g.DrawImage($shot, $x, $y, $w, $h); $g.DrawRectangle($frame, $x - 1, $y - 1, $w + 1, $h + 1)
$white = [System.Drawing.Brushes]::White
$grey = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 170, 170, 170))
$green = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 108, 203, 95))
$title = New-Object System.Drawing.Font 'Segoe UI Semibold', 40, ([System.Drawing.FontStyle]::Regular), ([System.Drawing.GraphicsUnit]::Pixel)
$body = New-Object System.Drawing.Font 'Segoe UI', 23, ([System.Drawing.FontStyle]::Regular), ([System.Drawing.GraphicsUnit]::Pixel)
$small = New-Object System.Drawing.Font 'Segoe UI', 19, ([System.Drawing.FontStyle]::Regular), ([System.Drawing.GraphicsUnit]::Pixel)
$g.DrawString("Codex Account`nSwitcher", $title, $white, 56, 150)
$g.DrawString('Several Codex accounts on one Windows PC. Pick one, and ChatGPT opens with it; the CLI and VS Code switch too.', $body, $grey, (New-Object System.Drawing.RectangleF 56, 270, ($x - 56 - 40), 200))
$g.FillEllipse($green, 58, 492, 12, 12)
$g.DrawString('Native C++ · one exe · Windows 10 and 11', $small, $grey, 80, 482)
$g.Dispose(); $shot.Dispose()
$card.Save((Join-Path $images 'social-preview.png'), [System.Drawing.Imaging.ImageFormat]::Png); $card.Dispose()

"Pictures saved to $images"
