@echo off
setlocal
if defined VCToolsInstallDir goto toolchain_ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZIPSHOT_BRUSH_VS=%%i"
if not defined ZIPSHOT_BRUSH_VS exit /b 1
call "%ZIPSHOT_BRUSH_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:toolchain_ready
cd /d "%~dp0.."
if not exist build\tests-editor-brush mkdir build\tests-editor-brush
cl /nologo /O2 /Gy /W4 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE /Isrc ^
 tests\editor_brush_tests.cpp src\shapes_ext.cpp src\imageops.cpp src\imagedialogs.cpp src\stickers.cpp src\colorpicker.cpp src\capture.cpp src\output.cpp src\png_encode_fast.cpp third_party\fpng\fpng.cpp src\sharex_import.cpp src\combine.cpp ^
 /Fobuild\tests-editor-brush\ /Fe:build\tests-editor-brush\editor_brush_tests.exe ^
 /link /OPT:REF /OPT:ICF user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib windowscodecs.lib d2d1.lib dwrite.lib dwmapi.lib msimg32.lib d3d11.lib dxgi.lib comdlg32.lib winmm.lib advapi32.lib || exit /b 1
build\tests-editor-brush\editor_brush_tests.exe
exit /b %errorlevel%
