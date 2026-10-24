[CmdletBinding()]
param(
    [string]$ToolRoot = (Join-Path $PSScriptRoot '../.local/shader-tools')
)

$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskTools = [IO.Path]::GetFullPath($ToolRoot)
if (Test-Path -LiteralPath $taskTools) { throw "Choose a new shader tool directory: $taskTools" }
$taskSdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin/10.0.26100.0/x64'
foreach ($taskFile in @('fxc.exe','d3dcompiler_47.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $taskSdk $taskFile))) {
        throw "Windows SDK 10.0.26100.0 x64 is required: $taskFile"
    }
}
$taskVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVsRoot = & $taskVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $taskVsRoot -or
    -not (Get-ChildItem -LiteralPath (Join-Path $taskVsRoot 'VC/Tools/MSVC') -Directory | Where-Object Name -Like '14.44.*')) {
    throw 'Visual Studio 2022 with MSVC 14.44 is required.'
}

$taskLock = Get-Content -LiteralPath (Join-Path $taskRoot 'host/tools.lock.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Path $taskTools | Out-Null
foreach ($taskName in @('dxc','slang')) {
    $taskInput = $taskLock.$taskName
    $taskArchive = Join-Path $taskTools "$taskName.zip"
    Invoke-WebRequest -Uri $taskInput.url -OutFile $taskArchive
    if ((Get-FileHash -LiteralPath $taskArchive -Algorithm SHA256).Hash -cne $taskInput.sha256) {
        throw "Official shader tool archive hash mismatch: $taskName"
    }
    $taskExtract = Join-Path $taskTools $taskName
    Expand-Archive -LiteralPath $taskArchive -DestinationPath $taskExtract
    $taskBin = if ($taskName -eq 'dxc') { Join-Path $taskExtract 'bin/x64' } else { Join-Path $taskExtract 'bin' }
    foreach ($taskBinary in Get-ChildItem -LiteralPath $taskBin -File | Where-Object Extension -In @('.exe','.dll')) {
        Copy-Item -LiteralPath $taskBinary.FullName -Destination $taskTools
    }
}
foreach ($taskFile in @('fxc.exe','d3dcompiler_47.dll')) {
    Copy-Item -LiteralPath (Join-Path $taskSdk $taskFile) -Destination $taskTools
}
foreach ($taskFile in @('fxc.exe','dxc.exe','dxcompiler.dll','dxil.dll','slangc.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $taskTools $taskFile))) { throw "Missing shader tool: $taskFile" }
}
& (Join-Path $taskTools 'dxc.exe') --version
if ($LASTEXITCODE -ne 0) { throw 'DXC cannot run.' }
& (Join-Path $taskTools 'slangc.exe') -version
if ($LASTEXITCODE -ne 0) { throw 'Slang cannot run.' }
Write-Host "Shader tools prepared: $taskTools"
