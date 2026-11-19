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
if not exist build\gallery-qa mkdir build\gallery-qa
rc /nologo /fo build\gallery-qa\mainwindow-gallery.res res\zipshot.rc || exit /b 1
cl /nologo /O2 /Gy /W4 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tests\mainwindow_gallery_tests.cpp build\gallery-qa\mainwindow-gallery.res /Fobuild\gallery-qa\ /Fe:build\gallery-qa\mainwindow_gallery_tests.exe /link /OPT:REF /MANIFEST:EMBED /MANIFESTINPUT:src\zipshot.manifest user32.lib gdi32.lib dwmapi.lib ole32.lib oleaut32.lib shell32.lib shlwapi.lib windowscodecs.lib msimg32.lib comctl32.lib uuid.lib || exit /b 1
set "ZIPSHOT_GALLERY_TEST_RUN=build\gallery-qa\run-%RANDOM%-%RANDOM%"
build\gallery-qa\mainwindow_gallery_tests.exe "%ZIPSHOT_GALLERY_TEST_RUN%"
exit /b %errorlevel%
