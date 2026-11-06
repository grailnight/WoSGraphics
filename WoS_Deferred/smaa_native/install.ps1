$ErrorActionPreference='Stop'
$root=Split-Path -Parent $MyInvocation.MyCommand.Path
$game=[IO.Path]::GetFullPath((Join-Path $root '..\..'))
$stage=Join-Path $game 'WoS_Deferred\full_deferred_staged'
if(Get-Process -Name 'Spider-Man Web of Shadows' -ErrorAction SilentlyContinue){throw 'Close the game before replacing winmm.dll.'}
$test=Get-Content -LiteralPath "$root\build\test_receipt.json" -Raw | ConvertFrom-Json
if(-not $test.valid -or -not $test.gpu_hal_executed){throw 'SMAA HAL proof missing'}
foreach($p in $test.input_hashes.PSObject.Properties){
    if((Get-FileHash -LiteralPath (Join-Path $root $p.Name)).Hash.ToLowerInvariant() -ne $p.Value){throw "SMAA input changed: $($p.Name)"}
}
& python "$stage\verify_primary_install.py"
if($LASTEXITCODE -ne 0){throw 'Primary integration validation failed'}
$validation=Get-Content -LiteralPath "$stage\primary_release\install_validation.json" -Raw | ConvertFrom-Json
$backup=Join-Path $root ('install_backup_'+(Get-Date -Format yyyyMMdd_HHmmss))
New-Item -ItemType Directory -Path $backup | Out-Null
$dll=Join-Path $game 'winmm.dll'
Copy-Item -LiteralPath $dll -Destination "$backup\winmm.dll"
$oldHash=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant()
$oldReceipt=Join-Path $game 'WoS_Deferred\full_deferred_install.json'
Copy-Item -LiteralPath $oldReceipt -Destination "$backup\full_deferred_install.json"
$assets=Join-Path $game 'WoS_Deferred\full_deferred'
$assetHashes=@{}
foreach($f in Get-ChildItem -LiteralPath $assets -File -Recurse){$assetHashes[$f.FullName]=(Get-FileHash -LiteralPath $f.FullName).Hash}
if(Get-Process -Name 'Spider-Man Web of Shadows' -ErrorAction SilentlyContinue){throw 'Game started during preflight; installation stopped'}
try {
    Copy-Item -LiteralPath "$stage\build\winmm.dll" -Destination $dll -Force
    if((Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant() -ne $validation.dll_sha256){throw 'Installed DLL hash mismatch'}
    foreach($p in $assetHashes.GetEnumerator()){if((Get-FileHash -LiteralPath $p.Key).Hash -ne $p.Value){throw 'Existing renderer asset changed'}}
    $receipt=Get-Content -LiteralPath $oldReceipt -Raw | ConvertFrom-Json
    $receipt.previous_dll_sha256=$oldHash
    $receipt.installed_dll_sha256=$validation.dll_sha256
    $receipt.backup_directory=$backup
    $receipt.installed_at=Get-Date -Format o
    $receipt.live_gameplay_verified=$false
    $receipt.validation=$validation
    $receipt | Add-Member -NotePropertyName smaa_native -NotePropertyValue ([ordered]@{enabled_default=$true;quality='SMAA 1x High';location='after recognized native bloom';independent_of_reshade=$true;hal_checks=$test.checks;existing_assets_unchanged=$true}) -Force
    $receipt | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $oldReceipt -Encoding UTF8
    $receipt | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath "$root\install_receipt.json" -Encoding UTF8
    Write-Host "Installed native SMAA. Backup: $backup"
} catch {
    Copy-Item -LiteralPath "$backup\winmm.dll" -Destination $dll -Force
    Copy-Item -LiteralPath "$backup\full_deferred_install.json" -Destination $oldReceipt -Force
    throw
}
