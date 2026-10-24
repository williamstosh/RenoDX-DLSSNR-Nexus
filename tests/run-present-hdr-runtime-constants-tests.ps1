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
Set-StrictMode -Version Latest
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$testSource = Join-Path $PSScriptRoot 'present_hdr_runtime_constants_tests.cpp'
$projectPath = $BuildProject
$addonDirectory = $SourceRoot

$runLog = Join-Path $outputDirectory 'run.log'
$compileLog = Join-Path $outputDirectory 'compile.log'
$linkLog = Join-Path $outputDirectory 'link.log'
$dependencyLog = Join-Path $outputDirectory 'source-dependencies.json'
$hashLog = Join-Path $outputDirectory 'input-hashes.json'
$stdoutLog = Join-Path $outputDirectory 'test.stdout.log'
$stderrLog = Join-Path $outputDirectory 'test.stderr.log'
$objectPath = Join-Path $outputDirectory 'runtime-constants.obj'
$executable = Join-Path $outputDirectory 'runtime-constants.exe'
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Add-RunLine([string]$line) {
    [IO.File]::AppendAllText($runLog, "$line`r`n", $utf8NoBom)
}

Add-RunLine "started_utc=$([DateTime]::UtcNow.ToString('o'))"
Add-RunLine 'scope=actual CPU production function calls; no GPU dispatch or add-on installation'
Write-Host "Artifacts: $outputDirectory"

try {
    $buildInputs = Get-TestProjectInputs $BuildProject $SourceRoot $HostRoot $ShaderIncludeDirectory
    $includes = $buildInputs.Includes
    $libraries = $buildInputs.Libraries
    $inputs = @($testSource, $PSCommandPath, $projectPath,
        (Join-Path $addonDirectory 'dlssnr.hpp'),
        (Join-Path $addonDirectory 'present_path.hpp'),
        (Join-Path $addonDirectory 'present_hdr_pedestal_policy.hpp'),
        (Join-Path $addonDirectory 'diagnostic_policy.hpp'),
        (Join-Path $addonDirectory 'codec_gain.hpp'),
        (Join-Path $ShaderIncludeDirectory 'embed/shaders.h'))
    $hashes = @{}
    foreach ($inputPath in $inputs) {
        $hashes[$inputPath] = (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash
    }
    $hashes | ConvertTo-Json | Set-Content -LiteralPath $hashLog -Encoding utf8
    Initialize-TestBuild $VcVars64
    Add-RunLine "compiler=$((Get-Command cl.exe).Source)"
    $compileArguments = @('/nologo', '/std:c++20', '/EHsc', '/W3', '/MT', '/O2', '/Gy', '/Gw',
        '/bigobj', '/utf-8', '/DNOMINMAX', '/DNDEBUG', '/DRESHADE_ADDON=2') +
        @($includes | ForEach-Object { "/I$_" }) +
        @('/c', $testSource, "/Fo$objectPath", '/sourceDependencies', $dependencyLog)
    & cl.exe @compileArguments *> $compileLog
    $compileExitCode = $LASTEXITCODE
    Add-RunLine "compile_exit_code=$compileExitCode"
    if ($compileExitCode -ne 0) {
        Get-Content -LiteralPath $compileLog -Tail 45
        throw "Production header compilation failed; see $compileLog"
    }
    $linkArguments = @('/NOLOGO', '/OPT:REF', '/OPT:ICF', "/OUT:$executable", $objectPath) + $libraries
    & link.exe @linkArguments *> $linkLog
    $linkExitCode = $LASTEXITCODE
    Add-RunLine "link_exit_code=$linkExitCode"
    if ($linkExitCode -ne 0) {
        Get-Content -LiteralPath $linkLog -Tail 45
        throw "Production header linking failed; see $linkLog"
    }
    foreach ($inputPath in $inputs) {
        if ((Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash -ne $hashes[$inputPath]) {
            throw "Input changed during compilation/linking: $inputPath; rerun against stable source."
        }
    }
    Add-RunLine 'tracked_inputs_stable=true'
    $process = Start-Process -FilePath $executable -WorkingDirectory $outputDirectory -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog
    $null = $process.Handle
    if (-not $process.WaitForExit(45000)) {
        $process.Kill()
        $process.WaitForExit()
        throw 'Runtime constants test timed out.'
    }
    $process.WaitForExit()
    $testExitCode = $process.ExitCode
    if ($null -eq $testExitCode) { throw 'Process.ExitCode unavailable; cannot report a pass.' }
    Add-RunLine "test_exit_code=$testExitCode"
    Get-Content -LiteralPath $stdoutLog
    Get-Content -LiteralPath $stderrLog
    if ($testExitCode -ne 0) { throw "Actual runtime function checks failed with exit code $testExitCode" }
    $summary = Get-Content -LiteralPath $stdoutLog | Where-Object { $_ -match '^\d+ checks, \d+ failures$' } | Select-Object -Last 1
    if (-not $summary) { throw 'Test check summary was missing.' }
    Add-RunLine "check_summary=$summary"
    Add-RunLine 'status=passed'
    Write-Host "Compile=$compileExitCode Link=$linkExitCode Process.ExitCode=$testExitCode"
} catch {
    Add-RunLine 'status=failed'
    Add-RunLine "error=$($_.Exception.Message)"
    throw
}
