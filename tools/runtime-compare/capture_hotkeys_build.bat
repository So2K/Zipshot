@echo off
setlocal
cd /d "%~dp0\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build\runtime-compare mkdir build\runtime-compare
cl /nologo /O2 /GL /W3 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tools\runtime-compare\capture_hotkeys.cpp /Fobuild\runtime-compare\ /Fe:build\runtime-compare\capture_hotkeys.exe /link /LTCG user32.lib gdi32.lib ole32.lib windowscodecs.lib advapi32.lib || exit /b 1
