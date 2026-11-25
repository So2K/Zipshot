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
if not exist build\tests mkdir build\tests
cl /nologo /O2 /Gy /W4 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tests\updater_tests.cpp /Fobuild\tests\ /Fe:build\tests\updater_tests.exe /link /OPT:REF user32.lib shell32.lib shlwapi.lib advapi32.lib winhttp.lib || exit /b 1
build\tests\updater_tests.exe
exit /b %errorlevel%
