@echo off
setlocal
if defined VCToolsInstallDir goto toolchain_ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZIPSHOT_EDITOR_VS=%%i"
if not defined ZIPSHOT_EDITOR_VS exit /b 1
call "%ZIPSHOT_EDITOR_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:toolchain_ready
cd /d "%~dp0..\.."
if not exist build\runtime-compare\editor-ui-native mkdir build\runtime-compare\editor-ui-native
rc /nologo /fo build\runtime-compare\editor-ui-native\zipshot.res res\zipshot.rc || exit /b 1
cl /nologo /O2 /GL /GS /guard:cf /Gy /W3 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE /Isrc ^
 src\app.cpp src\capture.cpp src\output.cpp src\overlay.cpp tools\runtime-compare\editor_ui_native.cpp src\toast.cpp src\recorder.cpp src\mainwindow.cpp src\i18n.cpp src\sharex_import.cpp src\colorpicker.cpp src\combine.cpp src\imageops.cpp src\imagedialogs.cpp src\shapes_ext.cpp src\stickers.cpp ^
 build\runtime-compare\editor-ui-native\zipshot.res /Fobuild\runtime-compare\editor-ui-native\ /Fe:build\runtime-compare\editor-ui-native\editor_ui_native.exe ^
 /link /LTCG /OPT:REF /OPT:ICF /DYNAMICBASE /NXCOMPAT /guard:cf /SUBSYSTEM:CONSOLE /MANIFEST:EMBED /MANIFESTINPUT:src\zipshot.manifest ^
 user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib windowscodecs.lib d2d1.lib dwrite.lib dwmapi.lib winmm.lib dxgi.lib d3d11.lib msimg32.lib advapi32.lib comdlg32.lib
exit /b %errorlevel%
