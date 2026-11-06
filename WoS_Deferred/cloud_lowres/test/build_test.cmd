@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\fxc.exe"
%FXC% /nologo /T ps_3_0 /E main /O3 /Fo composite_test.cso composite_test.hlsl >nul
if errorlevel 1 exit /b 1
rem The ground truth: the same per-pixel march with 240 steps instead of 64.
%FXC% /nologo /T ps_3_0 /E main /O3 /D WOS_CLOUD_STEPS=240 /Fo composite_truth.cso composite_test.hlsl >nul
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /O2 /W3 gpu_test.cpp /link user32.lib d3d9.lib /OUT:gpu_test.exe
exit /b %errorlevel%
