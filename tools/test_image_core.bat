@echo off
setlocal
if not defined VCToolsInstallDir (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZIPSHOT_TEST_VS=%%i"
)
if not defined VCToolsInstallDir call "%ZIPSHOT_TEST_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0.."
if not exist build\tests-image-core mkdir build\tests-image-core
cl /nologo /O2 /Gy /W4 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tools\test_image_core.cpp src\combine.cpp /Fobuild\tests-image-core\ /Fe:build\tests-image-core\test_image_core.exe /link /OPT:REF /OPT:ICF user32.lib gdi32.lib shell32.lib ole32.lib windowscodecs.lib d2d1.lib dwrite.lib dwmapi.lib msimg32.lib || exit /b 1
build\tests-image-core\test_image_core.exe
exit /b %errorlevel%
