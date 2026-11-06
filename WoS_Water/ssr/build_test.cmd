@echo off
setlocal
cd /d "%~dp0"
set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
%FXC% /nologo /T ps_3_0 /E main /O3 /Fo water_ssr.cso /Fc water_ssr.asm water_ssr.hlsl || exit /b 1
python build_mask.py || exit /b 1
%FXC% /nologo /T vs_3_0 /E vs_main /Fo test_scene_vs.cso test_scene.hlsl || exit /b 1
%FXC% /nologo /T ps_3_0 /E ps_solid /Fo test_scene_ps.cso test_scene.hlsl || exit /b 1
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cl /nologo /MT /EHsc /std:c++17 /utf-8 /O2 water_ssr_gpu_test.cpp /Fe:water_ssr_gpu_test.exe /link d3d9.lib user32.lib dxguid.lib || exit /b 1
