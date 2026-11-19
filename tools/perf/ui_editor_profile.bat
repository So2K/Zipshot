@echo off
setlocal
if defined VCToolsInstallDir goto ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZIPSHOT_UI_PERF_VS=%%i"
if not defined ZIPSHOT_UI_PERF_VS exit /b 1
call "%ZIPSHOT_UI_PERF_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:ready
cd /d "%~dp0..\.."
if not exist build\perf\ui\editor mkdir build\perf\ui\editor
cl /nologo /O2 /Gy /W3 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE /Isrc ^
 tools\perf\ui_editor_profile.cpp src\shapes_ext.cpp src\imageops.cpp src\imagedialogs.cpp src\stickers.cpp src\colorpicker.cpp src\capture.cpp src\output.cpp src\png_encode_fast.cpp third_party\fpng\fpng.cpp src\sharex_import.cpp src\combine.cpp ^
 /Fobuild\perf\ui\editor\ /Fe:build\perf\ui\editor\ui_editor_profile.exe ^
 /link /OPT:REF /OPT:ICF user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib windowscodecs.lib d2d1.lib dwrite.lib dwmapi.lib msimg32.lib d3d11.lib dxgi.lib comdlg32.lib winmm.lib advapi32.lib || exit /b 1
build\perf\ui\editor\ui_editor_profile.exe > build\perf\ui\editor-baseline.json
exit /b %errorlevel%
