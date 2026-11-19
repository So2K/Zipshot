@echo off
setlocal
if defined VCToolsInstallDir goto toolchain_ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZIPSHOT_VS=%%i"
if not defined ZIPSHOT_VS exit /b 1
call "%ZIPSHOT_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:toolchain_ready
cd /d "%~dp0../.."
if not exist build\perf\images mkdir build\perf\images
set "ZIPSHOT_FPNG_SCALAR="
set "ZIPSHOT_FPNG_TAG=simd"
if /i "%~1"=="--scalar" set "ZIPSHOT_FPNG_SCALAR=/DFPNG_NO_SSE=1"
if /i "%~1"=="--scalar" set "ZIPSHOT_FPNG_TAG=scalar"
cl /nologo /c /O2 /Gy /EHsc /std:c++20 /MT %ZIPSHOT_FPNG_SCALAR% third_party\fpng\fpng.cpp /Fobuild\perf\images\fpng_quality_%ZIPSHOT_FPNG_TAG%.obj || exit /b 1
cl /nologo /O2 /Gy /W4 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tools\perf\image_fpng_quality.cpp src\png_encode_fast.cpp build\perf\images\fpng_quality_%ZIPSHOT_FPNG_TAG%.obj /Fobuild\perf\images\ /Fe:build\perf\images\image_fpng_quality_%ZIPSHOT_FPNG_TAG%.exe /link /OPT:REF user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib windowscodecs.lib winmm.lib advapi32.lib bcrypt.lib || exit /b 1
build\perf\images\image_fpng_quality_%ZIPSHOT_FPNG_TAG%.exe "build\perf\images\quality-%ZIPSHOT_FPNG_TAG%-%RANDOM%-%RANDOM%"
exit /b %errorlevel%
