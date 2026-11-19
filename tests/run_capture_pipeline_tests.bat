@echo off
setlocal
if defined VCToolsInstallDir goto toolchain_ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZIPSHOT_VS=%%i"
if not defined ZIPSHOT_VS exit /b 1
call "%ZIPSHOT_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:toolchain_ready
cd /d "%~dp0.."
if not exist build\tests-capture-pipeline mkdir build\tests-capture-pipeline
rem Unreachable code is intentional in the fail-closed stubs for unrelated app paths.
cl /nologo /O2 /Gy /W4 /wd4702 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tests\capture_pipeline_tests.cpp /Fobuild\tests-capture-pipeline\ /Fe:build\tests-capture-pipeline\capture_pipeline_tests.exe /link /OPT:REF user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib psapi.lib dwmapi.lib windowscodecs.lib winmm.lib advapi32.lib || exit /b 1
build\tests-capture-pipeline\capture_pipeline_tests.exe
exit /b %errorlevel%
