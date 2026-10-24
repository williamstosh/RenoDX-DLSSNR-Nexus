param(
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$HostRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/renodx'),
    [string]$ShaderIncludeDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.include'),
    [string]$BuildProject = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.vcxproj'),
    [string]$OutputDirectory,
    [string]$VcVars64,
    [ValidatePattern('^[A-Za-z_][A-Za-z0-9_]*$')][string]$ScopeSaveFunction = 'SavePresentRestoreScope'
)
. (Join-Path $PSScriptRoot 'test-support.ps1')
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$HostRoot = [IO.Path]::GetFullPath($HostRoot)
$ShaderIncludeDirectory = [IO.Path]::GetFullPath($ShaderIncludeDirectory)
if (-not $OutputDirectory) { $OutputDirectory = Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/tests/' + [guid]::NewGuid().ToString('N')) }
$OutputDirectory = Initialize-TestOutput $OutputDirectory

$ErrorActionPreference = 'Stop'
$renodx = $HostRoot
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $OutputDirectory) -and
    (Get-ChildItem -LiteralPath $OutputDirectory -Force | Select-Object -First 1)) {
    throw "Refusing nonempty output directory: $OutputDirectory"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Write-Host "Lifecycle artifacts: $OutputDirectory"
$inputs = @('fg_lifecycle_sdk.cpp', 'fg_lifecycle_fixture.cpp', 'fg_lifecycle_tests.cpp', 'run-fg-lifecycle-tests.ps1') |
    ForEach-Object { Join-Path $PSScriptRoot $_ }
$inputs += @('fg_xefg.hpp', 'fg_frame.hpp', 'detour_guard.hpp') |
    ForEach-Object { Join-Path $SourceRoot $_ }
$detours = Join-Path $renodx 'external/Detours/lib.X64/detours.lib'
$inputs += $detours
$hashes = @($inputs | ForEach-Object {
    [ordered]@{ path = $_; sha256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }
})
$hashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'inputs.json') -Encoding utf8
Initialize-TestBuild $VcVars64
$common = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/O2', '/MT', '/utf-8', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN')
$includes = @("/I$SourceRoot", "/I$HostRoot/src", "/I$HostRoot/src/addons/dlss5", "/I$renodx/external/Detours/src", "/I$renodx/external/reshade", "/I$renodx/external/Streamline/include")
$libraries = @($detours, 'uuid.lib', 'advapi32.lib', 'd3d11.lib', 'd3d12.lib', 'dxgi.lib', 'dxguid.lib')
Push-Location -LiteralPath $OutputDirectory
try {
    foreach ($target in @(
        @{ source = 'fg_lifecycle_sdk'; binary = 'libxess_fg.dll'; dll = $true },
        @{ source = 'fg_lifecycle_fixture'; binary = 'fg_lifecycle_fixture.dll'; dll = $true },
        @{ source = 'fg_lifecycle_tests'; binary = 'fg_lifecycle_tests.exe'; dll = $false }
    )) {
        $arguments = $common + $includes
        if ($target.dll) { $arguments += '/LD' }
        $arguments += @((Join-Path $PSScriptRoot ($target.source + '.cpp')), "/Fo$($target.source).obj",
            "/Fe$($target.binary)", '/sourceDependencies', ($target.source + '.dependencies.json'),
            '/link', '/INCREMENTAL:NO', '/OPT:NOICF') + $libraries
        $arguments | ConvertTo-Json | Set-Content -LiteralPath ($target.source + '.arguments.json') -Encoding utf8
        & cl.exe @arguments *> ($target.source + '.compile.log')
        if ($LASTEXITCODE -ne 0) {
            Get-Content -LiteralPath ($target.source + '.compile.log') -Tail 50
            throw "Compilation/link failed: $($target.source); $OutputDirectory"
        }
    }
    # Preserve every compiler-reported transitive header hash before running the host.
    $dependencies = Get-ChildItem -LiteralPath $OutputDirectory -Filter '*.dependencies.json' |
        ForEach-Object { (Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json).Data.Includes } |
        Sort-Object -Unique
    $dependencyHashes = @($dependencies | ForEach-Object {
        [ordered]@{ path = $_; sha256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }
    })
    $dependencyHashes | ConvertTo-Json | Set-Content -LiteralPath 'dependency-inputs.json' -Encoding utf8
    $process = Start-Process -FilePath (Join-Path $OutputDirectory 'fg_lifecycle_tests.exe') -WorkingDirectory $OutputDirectory `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $OutputDirectory 'host.stdout.log') `
        -RedirectStandardError (Join-Path $OutputDirectory 'host.stderr.log')
    $null = $process.Handle
    if (-not $process.WaitForExit(45000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "Host timed out: $OutputDirectory"
    }
    $process.WaitForExit()
    [ordered]@{ exit_code = $process.ExitCode; utc = [DateTime]::UtcNow.ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath 'host-exit.json' -Encoding utf8
    Get-Content -LiteralPath 'host.stdout.log'
    Get-Content -LiteralPath 'host.stderr.log'
    foreach ($inputHash in @($hashes) + @($dependencyHashes)) {
        if ((Get-FileHash -LiteralPath $inputHash.path).Hash -ne $inputHash.sha256) {
            throw "Input changed during test: $($inputHash.path)"
        }
    }
    Get-ChildItem -LiteralPath $OutputDirectory -File | Where-Object { $_.Extension -in '.dll', '.exe' } |
        ForEach-Object { [ordered]@{ path = $_.FullName; sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash } } |
        ConvertTo-Json | Set-Content -LiteralPath 'outputs.json' -Encoding utf8
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) {
        throw "Lifecycle host failed (exit=$($process.ExitCode)): $OutputDirectory"
    }
    Write-Host "DLL lifecycle host passed: $OutputDirectory"
} finally {
    Pop-Location
}
