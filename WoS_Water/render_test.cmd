@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
cl /nologo /O2 /MT /EHsc /std:c++17 WoS_Water\surface_render_test.cpp /Fo:WoS_Water\surface_render_test.obj /Fe:WoS_Water\surface_render_test.exe /link d3d9.lib d3dcompiler.lib user32.lib
if errorlevel 1 exit /b 1
WoS_Water\surface_render_test.exe
exit /b %errorlevel%
