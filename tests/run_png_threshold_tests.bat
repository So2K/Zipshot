@echo off
setlocal
if defined VCToolsInstallDir goto toolchain_ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZIPSHOT_VS=%%i"
if not defined ZIPSHOT_VS exit /b 1
call "%ZIPSHOT_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:toolchain_ready
cd /d "%~dp0.."
if not exist build\tests-png-threshold mkdir build\tests-png-threshold
cl /nologo /O2 /Gy /W4 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tests\png_threshold_tests.cpp src\png_encode_fast.cpp third_party\fpng\fpng.cpp /Fobuild\tests-png-threshold\ /Fe:build\tests-png-threshold\png_threshold_tests.exe /link /OPT:REF user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib windowscodecs.lib winmm.lib advapi32.lib bcrypt.lib || exit /b 1
build\tests-png-threshold\png_threshold_tests.exe "build\tests-png-threshold\run-%RANDOM%-%RANDOM%"
exit /b %errorlevel%
