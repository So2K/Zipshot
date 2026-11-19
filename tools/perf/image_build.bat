@echo off
setlocal
if defined VCToolsInstallDir goto toolchain_ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZIPSHOT_VS=%%i"
if not defined ZIPSHOT_VS exit /b 1
call "%ZIPSHOT_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:toolchain_ready
cd /d "%~dp0../.."
if not exist build\perf\images mkdir build\perf\images
cl /nologo /c /O2 /Gy /EHsc /std:c++20 /MT third_party\fpng\fpng.cpp /Fobuild\perf\images\fpng_prod.obj || exit /b 1
cl /nologo /c /O2 /Gy /W4 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE src\png_encode_fast.cpp /Fobuild\perf\images\png_encode_fast_prod.obj || exit /b 1
cl /nologo /O2 /Gy /W4 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tools\perf\image_native.cpp build\perf\images\png_encode_fast_prod.obj build\perf\images\fpng_prod.obj /Fobuild\perf\images\ /Fe:build\perf\images\image_native.exe /link /OPT:REF user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib windowscodecs.lib winmm.lib advapi32.lib bcrypt.lib || exit /b 1
cl /nologo /O2 /Gy /W4 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tools\perf\image_prepared_check.cpp build\perf\images\png_encode_fast_prod.obj build\perf\images\fpng_prod.obj /Fobuild\perf\images\ /Fe:build\perf\images\image_prepared_check.exe /link /OPT:REF user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib windowscodecs.lib winmm.lib advapi32.lib bcrypt.lib || exit /b 1
