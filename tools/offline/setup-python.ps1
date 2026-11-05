[CmdletBinding()]
param(
    [string]$PythonRoot = (Join-Path $PSScriptRoot '../../.local/offline-python')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$directory = [IO.Path]::GetFullPath($PythonRoot)
if (Test-Path -LiteralPath $directory) {
    throw "Choose a new Python directory: $directory"
}
$pythonLock = (Get-Content -LiteralPath (Join-Path $root 'host/tools.lock.json') -Raw | ConvertFrom-Json).python
New-Item -ItemType Directory -Path $directory | Out-Null
$archive = Join-Path $directory 'python.tar.gz'
Invoke-WebRequest -Uri $pythonLock.url -OutFile $archive
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -cne $pythonLock.sha256) {
    throw 'Pinned Python archive hash mismatch.'
}
& tar.exe -xzf $archive -C $directory
if ($LASTEXITCODE -ne 0) {
    throw 'Pinned Python archive extraction failed.'
}
$python = Join-Path $directory 'python/python.exe'
$resolvedPython = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test.ps1') -PythonPath $python -ResolveOnly
if ($LASTEXITCODE -ne 0 -or -not $resolvedPython) {
    throw "Prepared Python must be CPython $($pythonLock.version) x64."
}
Write-Host "Prepared pinned CPython $($pythonLock.version) x64; archive SHA-256 $($pythonLock.sha256)."
Write-Output $resolvedPython
