$ErrorActionPreference = 'Stop'

# Compiles the low-resolution cloud passes. The DLL reads both .cso files from
# this folder at runtime. The bloom composite that consumes the buffer is built
# by WoS_VolumetricClouds\native\build_gta_lighting.ps1; the two share
# wos_clouds.hlsli, so rebuild both after editing it.

$fxc = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\fxc.exe'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

foreach ($name in @('cloud_march', 'cloud_resolve')) {
    & $fxc /nologo /T ps_3_0 /E main /O3 /Fo (Join-Path $here "$name.cso") /Fc (Join-Path $here "$name.asm") (Join-Path $here "$name.hlsl")
    if ($LASTEXITCODE -ne 0) { throw "$name failed to compile" }
}
Write-Host "Cloud low-resolution shaders built in $here"
