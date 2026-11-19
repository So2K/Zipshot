@echo off
setlocal
if defined VCToolsInstallDir goto toolchain_ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZIPSHOT_VS=%%i"
if not defined ZIPSHOT_VS exit /b 1
call "%ZIPSHOT_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:toolchain_ready
cd /d "%~dp0.."
if not exist build\tests-pixel-alpha mkdir build\tests-pixel-alpha
cl /nologo /O2 /W4 /EHsc /std:c++20 /utf-8 /MT tests\pixel_alpha_tests.cpp /Fobuild\tests-pixel-alpha\ /Fe:build\tests-pixel-alpha\pixel_alpha_tests.exe || exit /b 1
build\tests-pixel-alpha\pixel_alpha_tests.exe %*
exit /b %errorlevel%
