$ErrorActionPreference='Stop'
$waterDir=Split-Path -Parent $MyInvocation.MyCommand.Path
$gameDir=Split-Path -Parent $waterDir
if(Get-Process -Name 'Spider-Man Web of Shadows' -ErrorAction SilentlyContinue){throw 'Close the game before installing.'}
$preflight=Get-Content -LiteralPath (Join-Path $waterDir 'preflight.json') -Raw | ConvertFrom-Json
$target=Join-Path $gameDir 'winmm.dll'
$candidate=Join-Path $gameDir 'WoS_Deferred\full_deferred_staged\build\winmm.dll'
if((Get-FileHash -LiteralPath $target).Hash -ne $preflight.baseline){throw 'Installed DLL changed since preflight.'}
if((Get-FileHash -LiteralPath $candidate).Hash -ne $preflight.candidate){throw 'Candidate DLL differs from validated build.'}
if((Get-FileHash -LiteralPath (Join-Path $waterDir $preflight.backup)).Hash -ne $preflight.baseline){throw 'Backup mismatch.'}
Copy-Item -LiteralPath $candidate -Destination $target
if((Get-FileHash -LiteralPath $target).Hash -ne $preflight.candidate){throw 'Installed hash mismatch.'}
@{version=$preflight.version;installed=$true;time=(Get-Date).ToString('o');dll_sha256=$preflight.candidate;backup=$preflight.backup;gpu_validation='passed';game_validation='pending'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $waterDir 'install_receipt.json')
Write-Output 'Water integration installed and verified.'
