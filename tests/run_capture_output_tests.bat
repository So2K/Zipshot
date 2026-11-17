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
cl /nologo /O2 /Gy /W4 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tests\capture_output_tests.cpp /Fobuild\tests\ /Fe:build\tests\capture_output_tests.exe /link /OPT:REF user32.lib gdi32.lib dwmapi.lib d3d11.lib dxgi.lib ole32.lib oleaut32.lib shell32.lib shlwapi.lib windowscodecs.lib winmm.lib advapi32.lib || exit /b 1
set "ZIPSHOT_TEST_RUN=build\tests\run-%RANDOM%-%RANDOM%"
build\tests\capture_output_tests.exe "%ZIPSHOT_TEST_RUN%"
exit /b %errorlevel%
