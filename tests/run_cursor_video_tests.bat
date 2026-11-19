@echo off
setlocal
if defined VCToolsInstallDir goto toolchain_ready
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZIPSHOT_VS=%%i"
if not defined ZIPSHOT_VS exit /b 1
call "%ZIPSHOT_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:toolchain_ready
cd /d "%~dp0.."
if not exist build\tests-cursor-video mkdir build\tests-cursor-video
cl /nologo /O2 /W4 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tests\cursor_video_tests.cpp /Fobuild\tests-cursor-video\ /Fe:build\tests-cursor-video\cursor_video_tests.exe /link user32.lib advapi32.lib || exit /b 1
for /f "delims=" %%i in ('where ffmpeg.exe') do if not defined ZIPSHOT_FFMPEG set "ZIPSHOT_FFMPEG=%%i"
if not defined ZIPSHOT_FFMPEG exit /b 1
build\tests-cursor-video\cursor_video_tests.exe "build\tests-cursor-video\run-%RANDOM%-%RANDOM%" "%ZIPSHOT_FFMPEG%"
exit /b %errorlevel%
