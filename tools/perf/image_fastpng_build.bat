@echo off
setlocal
if defined VCToolsInstallDir goto toolchain_ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZIPSHOT_VS=%%i"
if not defined ZIPSHOT_VS exit /b 1
call "%ZIPSHOT_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:toolchain_ready
cd /d "%~dp0../.."
if not exist build\perf\images\vendor\fpng\fpng.cpp exit /b 2
cl /nologo /c /O2 /EHsc /std:c++20 /MT build\perf\images\vendor\fpng\fpng.cpp /Fobuild\perf\images\fpng.obj || exit /b 1
cl /nologo /c /O2 /EHsc /std:c++20 /MT /DFPNGE_USE_PEXT=0 build\perf\images\vendor\fpnge\fpnge.cc /Fobuild\perf\images\fpnge_sse.obj || exit /b 1
cl /nologo /c /O2 /EHsc /std:c++20 /MT /arch:AVX2 /DFPNGEEncode=FPNGEEncodeAvx2 build\perf\images\vendor\fpnge\fpnge.cc /Fobuild\perf\images\fpnge_avx2.obj || exit /b 1
cl /nologo /O2 /Gy /W4 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tools\perf\image_fastpng.cpp src\png_encode_fast.cpp build\perf\images\fpng.obj build\perf\images\fpnge_sse.obj build\perf\images\fpnge_avx2.obj /Fobuild\perf\images\ /Fe:build\perf\images\image_fastpng.exe /link /OPT:REF user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib windowscodecs.lib winmm.lib advapi32.lib bcrypt.lib || exit /b 1
