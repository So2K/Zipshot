@echo off
setlocal
cd /d "%~dp0\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build\benchmark mkdir build\benchmark
cl /nologo /O2 /GL /Gy /W3 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE tools\benchmark\zipshot_capture.cpp /Fobuild\benchmark\ /Fe:build\benchmark\zipshot_capture.exe /link /LTCG /OPT:REF /OPT:ICF user32.lib gdi32.lib dwmapi.lib dxgi.lib d3d11.lib || exit /b 1
