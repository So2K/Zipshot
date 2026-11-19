@echo off
setlocal
if defined VCToolsInstallDir goto toolchain_ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZIPSHOT_VS=%%i"
if not defined ZIPSHOT_VS exit /b 1
call "%ZIPSHOT_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:toolchain_ready
cd /d "%~dp0.."
if not exist build\tests-clipboard-shelf mkdir build\tests-clipboard-shelf
cl /nologo /O2 /Gy /W4 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tests\clipboard_shelf_tests.cpp /Fobuild\tests-clipboard-shelf\ /Fe:build\tests-clipboard-shelf\clipboard_shelf_tests.exe /link /OPT:REF user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib windowscodecs.lib winmm.lib advapi32.lib || exit /b 1
build\tests-clipboard-shelf\clipboard_shelf_tests.exe "build\tests-clipboard-shelf\run-%RANDOM%-%RANDOM%"
exit /b %errorlevel%
