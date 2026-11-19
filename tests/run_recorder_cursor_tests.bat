@echo off
setlocal
if defined VCToolsInstallDir goto toolchain_ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZIPSHOT_VS=%%i"
if not defined ZIPSHOT_VS (
    echo Visual Studio C++ build tools were not found.
    exit /b 1
)
call "%ZIPSHOT_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:toolchain_ready
cd /d "%~dp0.."
if not exist build\cursor-qa mkdir build\cursor-qa
set "ZIPSHOT_CURSOR_RECORDER_DEFINE="
if /i "%~1"=="--staged" set "ZIPSHOT_CURSOR_RECORDER_DEFINE=/DZIPSHOT_STAGED_CURSOR_RECORDER"
cl /nologo /O2 /Gy /W4 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE %ZIPSHOT_CURSOR_RECORDER_DEFINE% /Isrc tests\recorder_cursor_tests.cpp /Fobuild\cursor-qa\ /Fe:build\cursor-qa\recorder_cursor_tests.exe /link /OPT:REF user32.lib gdi32.lib dxgi.lib ole32.lib windowscodecs.lib || exit /b 1
set "ZIPSHOT_RECORDER_TEST_RUN=build\cursor-qa\recorder-run-%RANDOM%-%RANDOM%"
build\cursor-qa\recorder_cursor_tests.exe "%ZIPSHOT_RECORDER_TEST_RUN%"
exit /b %errorlevel%
