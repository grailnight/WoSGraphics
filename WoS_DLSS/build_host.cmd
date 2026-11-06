@echo off
rem Builds WoS_DLSS_Host.exe (x64) into WoS_DLSS\bin. Run from anywhere.
setlocal
set "HERE=%~dp0"
set "EXT=%HERE%..\WoS_Research\external"
set "VKH=%EXT%\vulkan_headers_1.4.363.0\include"
set "NGX=%EXT%\dlss_sdk_310.9.1"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
if not exist "%HERE%bin" mkdir "%HERE%bin"
if not exist "%HERE%host\obj" mkdir "%HERE%host\obj"
cl /nologo /MT /EHsc /std:c++17 /utf-8 /O2 /W3 /DUNICODE /D_UNICODE /I"%VKH%" /I"%NGX%\include" "%HERE%host\wos_dlss_host.cpp" /Fo:"%HERE%host\obj\\" /Fe:"%HERE%bin\WoS_DLSS_Host.exe" /link /SUBSYSTEM:CONSOLE "%NGX%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib" kernel32.lib user32.lib advapi32.lib version.lib
exit /b %errorlevel%
