$ErrorActionPreference='Stop'
$root=Split-Path -Parent $MyInvocation.MyCommand.Path
$vcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
$command='call "'+$vcvars+'" && cd /d "'+$root+'\build" && cl /nologo /EHsc /std:c++17 /O2 /MT "'+$root+'\test_smaa.cpp" /Fe:test_smaa.exe /link d3d9.lib user32.lib'
cmd.exe /c $command
if($LASTEXITCODE -ne 0){throw 'SMAA test build failed'}
Push-Location "$root\build"
try { & .\test_smaa.exe; if($LASTEXITCODE -ne 0){throw 'SMAA HAL test failed'} } finally {Pop-Location}
