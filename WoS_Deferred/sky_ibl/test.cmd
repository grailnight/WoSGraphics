@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b %errorlevel%
cd /d "%~dp0"
cl /nologo /MT /EHsc /std:c++17 /O2 gpu_test.cpp /Fo:gpu_test.obj /Fe:gpu_test.exe /link d3d9.lib user32.lib dxguid.lib
