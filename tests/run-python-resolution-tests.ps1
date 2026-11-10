$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/game-test/resolve-python.ps1')
$local = Join-Path $root '.local/game-compare-venv/Scripts/python.exe'
if (-not (Test-NrNexusPython $local @('PIL'))) { throw 'Expected repository Python environment is unavailable for this test.' }
if ((Resolve-NrNexusPython -RepositoryRoot $root) -ne (Resolve-Path $local).Path) {
    throw 'Repository Python environment was not preferred.'
}
if (-not (Test-NrNexusPython $local @('PIL'))) { throw 'Explicit repository Python path was rejected.' }
$rejected = $false
try { Resolve-NrNexusPython -RequestedPath (Join-Path $env:LOCALAPPDATA 'Microsoft/WindowsApps/python.exe') -RepositoryRoot $root | Out-Null }
catch { $rejected = $true }
if (-not $rejected) { throw 'WindowsApps Python alias was accepted.' }
Write-Output 'PASS: Python resolution prefers a runnable repository interpreter and rejects WindowsApps aliases.'
