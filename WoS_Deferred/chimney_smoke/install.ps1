# Installs the chimney smoke build: backs up the live winmm.dll, copies the new one.
# The shader (chimney_smoke.cso) is read from this folder at runtime, nothing else moves.
#   powershell -ExecutionPolicy Bypass -File install.ps1          (dry run)
#   powershell -ExecutionPolicy Bypass -File install.ps1 -Apply
param([switch]$Apply)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$game = Resolve-Path (Join-Path $here '..\..')
$built = Join-Path $here '..\full_deferred_staged\build\winmm.dll'
$live = Join-Path $game 'winmm.dll'
$cso = Join-Path $here 'chimney_smoke.cso'

if (Get-Process -Name 'Spider-Man Web of Shadows' -ErrorAction SilentlyContinue) { throw 'The game is running: save, close it, then run again.' }
foreach ($p in @($built, $live, $cso)) { if (-not (Test-Path $p)) { throw "Missing $p" } }
$marker = [Text.Encoding]::ASCII.GetBytes('Chimney smoke')
$bytes = [IO.File]::ReadAllBytes($built)
$found = $false
for ($i = 0; $i -le $bytes.Length - $marker.Length -and -not $found; $i++) {
    if ($bytes[$i] -ne $marker[0]) { continue }
    $ok = $true; for ($k = 1; $k -lt $marker.Length; $k++) { if ($bytes[$i + $k] -ne $marker[$k]) { $ok = $false; break } }
    $found = $ok
}
if (-not $found) { throw 'Built DLL has no chimney smoke; rebuild with build_integration.ps1' }
python (Join-Path $here '..\full_deferred_staged\verify_install.py') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'verify_install.py failed' }

$newHash = (Get-FileHash $built -Algorithm SHA256).Hash.ToLower()
$oldHash = (Get-FileHash $live -Algorithm SHA256).Hash.ToLower()
$backup = Join-Path $here ('..\backups\winmm.pre_chimney_smoke_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '.dll')
Write-Host "live   $oldHash"
Write-Host "new    $newHash"
if ($newHash -eq $oldHash) { Write-Host 'Already installed.'; exit 0 }
if (-not $Apply) { Write-Host "Dry run. Would back up to $backup and install. Re-run with -Apply."; exit 0 }
Copy-Item $live $backup
Copy-Item $built $live -Force
if ((Get-FileHash $live -Algorithm SHA256).Hash.ToLower() -ne $newHash) { Copy-Item $backup $live -Force; throw 'Copy mismatch; restored the backup' }
@{ installed = (Get-Date -Format s); dll_sha256 = $newHash; previous_sha256 = $oldHash; backup = (Resolve-Path $backup).Path;
   shader_sha256 = (Get-FileHash $cso -Algorithm SHA256).Hash.ToLower(); in_game_tested = $false } |
    ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $here 'install.json')
Write-Host "Installed. Backup: $backup"
