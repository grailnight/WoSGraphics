$ErrorActionPreference='Stop'
$waterDir=Split-Path -Parent $MyInvocation.MyCommand.Path
$gameDir=Split-Path -Parent $waterDir
if(Get-Process -Name 'Spider-Man Web of Shadows' -ErrorAction SilentlyContinue){throw 'Close the game before rollback.'}
$receipt=Get-Content -LiteralPath (Join-Path $waterDir 'preflight.json') -Raw | ConvertFrom-Json
$target=Join-Path $gameDir 'winmm.dll'
$backup=Join-Path $waterDir $receipt.backup
if((Get-FileHash -LiteralPath $target).Hash -ne $receipt.candidate){throw 'Installed DLL has changed; automatic rollback stopped.'}
if((Get-FileHash -LiteralPath $backup).Hash -ne $receipt.baseline){throw 'Backup mismatch.'}
Copy-Item -LiteralPath $backup -Destination $target
Write-Output 'Previous DLL restored. Water source files retained.'
