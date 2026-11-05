[CmdletBinding()]
param(
    [ValidateSet('replay')][string]$Backend = 'replay',
    [string]$HostRoot = (Join-Path $PSScriptRoot '../../.local/renodx'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot ('../../tmp/offline/build-' + [guid]::NewGuid().ToString('N'))),
    [string]$VcVars64,
    [ValidateSet('Standard','Full','Acceptance')][string]$RetentionMode = 'Standard'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $root 'tests/test-support.ps1')
$directory = Initialize-TestOutput $OutputDirectory -RetentionMode $RetentionMode
try {
    $hostDirectory = [IO.Path]::GetFullPath($HostRoot)
    $lockPath = Join-Path $root 'host/lock.json'
    $hostLock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
    $hostCommit = & git --no-optional-locks -C $hostDirectory rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $hostCommit -cne $hostLock.host.commit) { throw 'Offline build requires the pinned host.' }
    foreach ($header in $hostLock.compatibility.headers.PSObject.Properties) {
        if ((Get-FileHash -LiteralPath (Join-Path $hostDirectory $header.Name)).Hash -cne $header.Value) {
            throw "Host compatibility header mismatch: $($header.Name)"
        }
    }
    Initialize-TestBuild $VcVars64
    $source = Join-Path $root 'tests/offline_replay_tests.cpp'
    $exe = Join-Path $directory 'offline_replay_tests.exe'
    $arguments = @('/nologo','/std:c++20','/EHsc','/W4','/O2','/MT','/utf-8',
        '/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/DNDEBUG')
    $includes = @("$hostDirectory/src/addons/dlss5", "$hostDirectory/external/DLSS/include", "$hostDirectory/external/json/include")
    $libraries = @('d3d12.lib','dxgi.lib','dxguid.lib','version.lib')
    $extraInputs = @($source, $PSCommandPath, $lockPath, (Join-Path $root 'tests/test-support.ps1'))
    $arguments += @($includes | ForEach-Object { "/I$_" })
    $arguments += @($source, "/Fo$directory/offline_$Backend.obj", "/Fe$exe", '/sourceDependencies',
        "$directory/dependencies.json", '/link', '/OPT:REF', '/OPT:ICF') + $libraries
    $arguments | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'arguments.json') -Encoding utf8
    Register-TestCompilerOutput $directory -ObjectPath @("$directory/offline_$Backend.obj") -ExecutablePath $exe
    & cl.exe @arguments *> (Join-Path $directory 'compile.log')
    if ($LASTEXITCODE -ne 0) { throw 'Offline backend compile/link failed; see compile.log.' }
    $dependencies = (Get-Content -LiteralPath "$directory/dependencies.json" -Raw | ConvertFrom-Json).Data.Includes
    $compiler = (Get-Command cl.exe).Source
    $sourceInputs = @($extraInputs + $dependencies | Sort-Object -Unique)
    $compileSnapshot = @($sourceInputs + $compiler | Sort-Object -Unique | ForEach-Object {
        [ordered]@{path=$_; sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}
    })
    & cl.exe @arguments *> (Join-Path $directory 'compile.log')
    if ($LASTEXITCODE -ne 0) { throw 'Offline backend final compile/link failed; see compile.log.' }
    $finalDependencies = (Get-Content -LiteralPath "$directory/dependencies.json" -Raw | ConvertFrom-Json).Data.Includes
    $dependencySet = @($dependencies | Sort-Object -Unique)
    $finalDependencySet = @($finalDependencies | Sort-Object -Unique)
    if (Compare-Object -ReferenceObject $dependencySet -DifferenceObject $finalDependencySet) {
        throw 'Compiler dependencies changed during the final identity-bound compilation.'
    }
    foreach ($input in $compileSnapshot) {
        if ((Get-FileHash -LiteralPath $input.path).Hash.ToLowerInvariant() -cne $input.sha256) {
            throw "Build input changed during final compilation: $([IO.Path]::GetFileName($input.path))"
        }
    }
    $inputs = @($sourceInputs | Sort-Object -Unique | ForEach-Object {
        $inputPath = $_
        $hash = (Get-FileHash -LiteralPath $inputPath).Hash.ToLowerInvariant()
        $snapshot = $compileSnapshot | Where-Object { $_.path -ceq $inputPath } | Select-Object -First 1
        if (-not $snapshot -or $snapshot.sha256 -cne $hash) {
            throw "Build input changed before the final manifest was written: $([IO.Path]::GetFileName($inputPath))"
        }
        [ordered]@{path=$inputPath; sha256=$hash}
    })
    [ordered]@{schema='nr-nexus-offline-build/1'; status='built'; backend=$Backend
        source_commit=$global:NexusTestOutputs[$directory].sourceCommit; host_commit=$hostCommit
        executable=[IO.Path]::GetFileName($exe); executable_sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()
        compiler=@{name='MSVC'; file_version=(Get-Item -LiteralPath $compiler).VersionInfo.FileVersion
            sha256=(Get-FileHash -LiteralPath $compiler).Hash.ToLowerInvariant(); sdk=$env:WindowsSDKVersion.TrimEnd('\')}
        inputs=$inputs; runtime_bundled=$false} | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (Join-Path $directory 'build-manifest.json') -Encoding utf8
    Complete-TestOutput $directory
    Write-Output "Offline backend built (not executed): $exe"
} catch {
    Complete-TestOutput $directory -Outcome Failed
    throw
}
