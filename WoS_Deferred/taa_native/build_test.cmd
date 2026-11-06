@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /MT /EHsc /std:c++17 /O2 /W3 WoS_Deferred\taa_native\test_taa.cpp /Fo:WoS_Deferred\taa_native\test_taa.obj /Fe:WoS_Deferred\taa_native\test_taa.exe /link d3d9.lib user32.lib
