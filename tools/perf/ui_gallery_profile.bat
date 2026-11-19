@echo off
setlocal
if defined VCToolsInstallDir goto ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZIPSHOT_UI_PERF_VS=%%i"
if not defined ZIPSHOT_UI_PERF_VS exit /b 1
call "%ZIPSHOT_UI_PERF_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:ready
cd /d "%~dp0..\.."
if not exist build\perf\ui mkdir build\perf\ui
cl /nologo /O2 /Gy /W3 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tools\perf\ui_gallery_profile.cpp /Fobuild\perf\ui\ /Fe:build\perf\ui\ui_gallery_profile.exe /link /OPT:REF user32.lib gdi32.lib dwmapi.lib ole32.lib oleaut32.lib shell32.lib shlwapi.lib windowscodecs.lib msimg32.lib comctl32.lib uuid.lib || exit /b 1
build\perf\ui\ui_gallery_profile.exe > build\perf\ui\gallery-baseline.json
exit /b %errorlevel%
