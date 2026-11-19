@echo off
setlocal
if defined VCToolsInstallDir goto ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZIPSHOT_VS=%%i"
if not defined ZIPSHOT_VS exit /b 1
call "%ZIPSHOT_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:ready
cd /d "%~dp0..\.."
if not exist build\perf\video mkdir build\perf\video
cl /nologo /O2 /W4 /WX /EHsc /std:c++20 /utf-8 /MT tools\perf\video_commands.cpp /Fobuild\perf\video\ /Fe:build\perf\video\video_commands.exe
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tools\perf\video_pipeline.cpp /Fobuild\perf\video\ /Fe:build\perf\video\video_pipeline.exe /link user32.lib advapi32.lib
if errorlevel 1 exit /b 1
python tools\perf\video_prepare_control.py
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE /Isrc tools\perf\video_prepared.cpp /Fobuild\perf\video\ /Fe:build\perf\video\video_prepared.exe /link user32.lib advapi32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE /DZIPSHOT_PERF_CURRENT_API /Isrc tools\perf\video_prepared.cpp /Fobuild\perf\video\video_prepared_actual.obj /Fe:build\perf\video\video_prepared_actual.exe /link user32.lib advapi32.lib
exit /b %errorlevel%
