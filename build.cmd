@echo off
rem Builds CodexAccountSwitcher.exe with the Visual Studio C++ tools.
rem Static runtime: the exe needs nothing installed, not even the VC++ redistributable.
rem
rem   build.cmd                     x64   -> build\x64\CodexAccountSwitcher.exe
rem   build.cmd arm64               ARM64 -> build\arm64\CodexAccountSwitcher.exe
rem   build.cmd x64 <out.exe>       to another path
rem   build.cmd x64 <out.exe> test  the unit tests, a console program
setlocal
cd /d "%~dp0"
set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
if /i not "%ARCH%"=="x64" if /i not "%ARCH%"=="arm64" (echo Unknown architecture "%ARCH%", use x64 or arm64. & exit /b 1)
set "OUT=%~2"
if "%OUT%"=="" set "OUT=build\%ARCH%\CodexAccountSwitcher.exe"
set "ENTRY=src\main.cpp"
set "SUBSYSTEM=WINDOWS"
if /i "%~3"=="test" (
    set "ENTRY=tests\tests.cpp"
    set "SUBSYSTEM=CONSOLE"
)
rem   build.cmd x64 <out.exe> compat   the backup decoder's tests (zlib, XChaCha20-Poly1305), alone

rem A Developer Command Prompt (or CI with msvc-dev-cmd) already has cl on PATH for its target.
if defined VSCMD_ARG_TGT_ARCH if /i "%VSCMD_ARG_TGT_ARCH%"=="%ARCH%" goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo Visual Studio with the "Desktop development with C++" workload is required. & exit /b 1)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo The MSVC build tools were not found. & exit /b 1)
set "HOST=x64"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "HOST=arm64"
set "TARGET=%HOST%_%ARCH%"
if /i "%HOST%"=="%ARCH%" set "TARGET=%ARCH%"
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" %TARGET% >nul 2>nul || (echo Could not set up the %ARCH% tools. For ARM64 install "MSVC ARM64 build tools". & exit /b 1)

:build
if /i "%~3"=="compat" (
    if not exist "build\obj\%ARCH%-compat" mkdir "build\obj\%ARCH%-compat"
    for %%d in ("%OUT%") do if not exist "%%~dpd" mkdir "%%~dpd"
    cl /nologo /std:c++17 /O1 /MT /EHsc /utf-8 /W3 /DNDEBUG /Fo"build\obj\%ARCH%-compat\\" tests\compat_tests.cpp src\compat.cpp /link bcrypt.lib /out:"%OUT%" || exit /b 1
    echo Built %OUT% ^(%ARCH%^)
    exit /b 0
)
set "OBJ=build\obj\%ARCH%"
if /i "%~3"=="test" set "OBJ=build\obj\%ARCH%-test"
if not exist "%OBJ%" mkdir "%OBJ%"
for %%d in ("%OUT%") do if not exist "%%~dpd" mkdir "%%~dpd"
rc /nologo /fo "%OBJ%\app.res" src\app.rc || exit /b 1
cl /nologo /std:c++17 /O1 /GL /MT /EHsc /utf-8 /W3 /DNDEBUG /Fo"%OBJ%\\" ^
   %ENTRY% src\app.cpp src\core.cpp src\codex.cpp src\net.cpp src\accounts.cpp src\login.cpp src\compat.cpp src\recovery.cpp ^
   src\web.cpp src\shell.cpp src\gfx.cpp src\picker.cpp src\flyout.cpp src\settings.cpp src\details.cpp src\dialogs.cpp src\update.cpp src\logins.cpp src\manager.cpp src\uia.cpp "%OBJ%\app.res" ^
   /link /OPT:REF /OPT:ICF /LTCG /SUBSYSTEM:%SUBSYSTEM% /MANIFEST:NO /out:"%OUT%" ^
   user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib dwmapi.lib d2d1.lib dwrite.lib windowscodecs.lib ^
   advapi32.lib comctl32.lib comdlg32.lib propsys.lib shlwapi.lib bcrypt.lib crypt32.lib winhttp.lib ws2_32.lib iphlpapi.lib uxtheme.lib uiautomationcore.lib || exit /b 1
echo Built %OUT% (%ARCH%)
