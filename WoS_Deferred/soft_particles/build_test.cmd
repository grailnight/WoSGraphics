@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /EHsc /std:c++17 /O2 /W3 gpu_test.cpp /link user32.lib d3d9.lib /OUT:gpu_test.exe
exit /b %errorlevel%
