@echo off
setlocal
cd /d "%~dp0\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
python tools\runtime-compare\capture_api_build.py || exit /b 1
cl /nologo /O2 /GL /W3 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE /DCAPTURE_DIAGNOSTIC_TRACING=0 /Isrc tools\runtime-compare\capture_api_diagnostic.cpp /Fobuild\runtime-compare\capture_api_plain.obj /Fe:build\runtime-compare\capture_api_plain.exe /link /SUBSYSTEM:WINDOWS /LTCG user32.lib gdi32.lib dwmapi.lib dxgi.lib d3d11.lib advapi32.lib shell32.lib || exit /b 1
cl /nologo /O2 /GL /W3 /EHsc /std:c++20 /utf-8 /MT /DUNICODE /D_UNICODE /DCAPTURE_DIAGNOSTIC_TRACING=1 /Ibuild\runtime-compare /Isrc tools\runtime-compare\capture_api_diagnostic.cpp /Fobuild\runtime-compare\capture_api_trace.obj /Fe:build\runtime-compare\capture_api_trace.exe /link /SUBSYSTEM:WINDOWS /LTCG user32.lib gdi32.lib dwmapi.lib dxgi.lib d3d11.lib advapi32.lib shell32.lib || exit /b 1
