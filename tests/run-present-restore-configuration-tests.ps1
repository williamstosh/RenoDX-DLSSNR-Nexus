param(
    [string[]]$Case = @(),
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
$testSource = Join-Path $PSScriptRoot 'present_restore_configuration_tests.cpp'
$projectPath = $BuildProject
$addonDirectory = $SourceRoot

$runLog = Join-Path $outputDirectory 'run.log'
$compileLog = Join-Path $outputDirectory 'compile.log'
$linkLog = Join-Path $outputDirectory 'link.log'
$dependencyLog = Join-Path $outputDirectory 'source-dependencies.json'
$hashLog = Join-Path $outputDirectory 'input-hashes.json'
$stdoutLog = Join-Path $outputDirectory 'test.stdout.log'
$stderrLog = Join-Path $outputDirectory 'test.stderr.log'
$objectPath = Join-Path $outputDirectory 'configuration-tests.obj'
$executable = Join-Path $outputDirectory 'configuration-tests.exe'
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Add-RunLine([string]$line) {
    [IO.File]::AppendAllText($runLog, "$line`r`n", $utf8NoBom)
}

Add-RunLine "started_utc=$([DateTime]::UtcNow.ToString('o'))"
Add-RunLine 'scope=production LoadConfiguration/save + disk-backed ReShade boundary; no GPU or installation'
Write-Host "Artifacts: $outputDirectory"

try {
    $buildInputs = Get-TestProjectInputs $BuildProject $SourceRoot $HostRoot $ShaderIncludeDirectory
    $includes = $buildInputs.Includes
    $libraries = $buildInputs.Libraries
    $inputs = @($testSource, $PSCommandPath, $projectPath,
        (Join-Path $addonDirectory 'dlssnr.hpp'),
        (Join-Path $addonDirectory 'capability.hpp'),
        (Join-Path $addonDirectory 'stage_plan.hpp'),
        (Join-Path $addonDirectory 'gpu_timers.hpp'),
        (Join-Path $addonDirectory 'gpu_comparison_ui.hpp'),
        (Join-Path $PSScriptRoot 'capability_runtime_cases.hpp'),
        (Join-Path $addonDirectory 'present_path.hpp'),
        (Join-Path $addonDirectory 'present_hdr_pedestal_policy.hpp'),
        (Join-Path $addonDirectory 'diagnostic_policy.hpp'),
        (Join-Path $addonDirectory 'present_restore_scope.hpp'),
        (Join-Path $addonDirectory 'present_sdr_restore_policy.hpp'),
        (Join-Path $addonDirectory 'screenshot.hpp'),
        (Join-Path $addonDirectory 'd3d11_bridge.hpp'),
        (Join-Path $addonDirectory 'codec_gain.hpp'),
        (Join-Path $addonDirectory 'preset_store.hpp'),
        (Join-Path $addonDirectory 'preset_fields.hpp'),
        (Join-Path $addonDirectory 'preset_runtime.hpp'),
        (Join-Path $addonDirectory 'profile_runtime.hpp'),
        (Join-Path $addonDirectory 'profile_store.hpp'),
        (Join-Path $addonDirectory 'profile_templates.hpp'),
        (Join-Path $PSScriptRoot 'profile_runtime_cases.hpp'),
        (Join-Path $PSScriptRoot 'profile_first_install_cases.hpp'),
        (Join-Path $PSScriptRoot 'profile_reset_cases.hpp'),
        (Join-Path $ShaderIncludeDirectory 'embed/shaders.h'))
    $hashes = @{}
    foreach ($inputPath in $inputs) {
        $hashes[$inputPath] = (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash
    }
    $hashes | ConvertTo-Json | Set-Content -LiteralPath $hashLog -Encoding utf8
    $production = Get-Content -LiteralPath (Join-Path $addonDirectory 'dlssnr.hpp') -Raw
    $loadStart = $production.IndexOf('inline void LoadConfiguration()')
    if ($loadStart -lt 0) { throw 'Production LoadConfiguration entry point was not found.' }
    $loadBody = $production.Substring($loadStart)
    $loadEnd = $loadBody.IndexOf('inline bool Shutdown(')
    if ($loadEnd -gt 0) { $loadBody = $loadBody.Substring(0, $loadEnd) }
    if ($production -notmatch ("\b" + [regex]::Escape($ScopeSaveFunction) + "\s*\(") -or
        $loadBody -notmatch 'present_restore_configuration|NRPresentRestoreScope') {
        $expectation = @(
            'PENDING: production scope configuration I/O integration is not ready.',
            'Required: LoadConfiguration discovers any persistent add-on setting before writes,',
            'loads the scope pair, and verifies recommended defaults before stamping global schema v8.',
            "Save adapter expects: bool internal::$ScopeSaveFunction(present_restore_scope::Scope).",
            'Select a compatible actual function name with -ScopeSaveFunction; no synthetic implementation.',
            'State observed: internal::present_restore_configuration.scope (Configuration).',
            'Failure injection drops one ReShadeSetConfigValue call; writer must verify readback.',
            'Rerun once after production endpoints are stable. No compilation attempted.'
        )
        $expectation | Set-Content -LiteralPath (Join-Path $outputDirectory 'integration-api-expectation.txt') -Encoding utf8
        throw ($expectation -join ' ')
    }
    # One bounded quiet-window check; never retry builds while core is changing.
    Start-Sleep -Seconds 5
    foreach ($inputPath in $inputs) {
        if ((Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash -ne $hashes[$inputPath]) {
            throw "PENDING: input changed during quiet-window check: $inputPath; no compilation attempted."
        }
    }
    Initialize-TestBuild $VcVars64
    Add-RunLine "compiler=$((Get-Command cl.exe).Source)"
    $compileArguments = @('/nologo', '/std:c++20', '/EHsc', '/W3', '/MT', '/O2', '/Gy', '/Gw',
        '/bigobj', '/utf-8', '/DNOMINMAX', '/DNDEBUG', '/DRESHADE_ADDON=2',
        "/DCONFIG_TEST_SCOPE_SAVE=$ScopeSaveFunction") +
        @($includes | ForEach-Object { "/I$_" }) +
        @('/c', $testSource, "/Fo$objectPath", '/sourceDependencies', $dependencyLog)
    Register-TestCompilerOutput -Directory $OutputDirectory -ObjectPath $objectPath -ExecutablePath $executable
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
    $cases = @('old-arbitrary', 'old-registry', 'new', 'new-reload', 'empty-section', 'scope0', 'scope1',
        'malformed-scope', 'malformed-version', 'incomplete-pair', 'future-scope', 'future-version',
        'future-global', 'malformed-global', 'future-policy', 'fail-scope', 'fail-version',
        'select', 'save-fail-scope', 'save-fail-version',
        'pre8-explicit', 'pre8-legacy-key', 'pre8-future-scope', 'pre8-future-policy',
        'pre8-fail-scope', 'pre8-fail-sdr', 'pre8-fail-hdr', 'pre8-fail-bypass',
        'fail-sdr', 'fail-hdr', 'fail-bypass', 'fail-sdr-version', 'fail-hdr-version',
        'pre8-malformed-sdr-version', 'pre8-future-sdr-version', 'pre8-fail-sdr-bypass', 'v8-legacy-key', 'reset',
        'nr-model-default', 'nr-model-explicit', 'nr-model-invalid', 'nr-model-legacy',
        'nr-stage-default', 'nr-stage-explicit', 'nr-stage-invalid',
        'pass-result-default', 'pass-result-custom', 'pass-result-invalid', 'presets',
        'nr-resolution-default', 'nr-resolution-explicit', 'nr-resolution-invalid', 'native-prefg-resolution-config', 'profiles', 'capability-ui')
    if ($Case.Count) {
        foreach ($requested in $Case) {
            if ($requested -notin $cases) { throw "Unknown configuration case: $requested" }
        }
        $cases = $Case
    }
    $totalChecks = 0
    $totalFailures = 0
    $failedCases = @()
    foreach ($case in $cases) {
        $fixtureName = $case
        if ($case -eq 'new-reload') { $fixtureName = 'new' }
        $fixtureDirectory = Join-Path $outputDirectory $fixtureName
        New-Item -ItemType Directory -Path $fixtureDirectory -Force | Out-Null
        $stdoutLog = Join-Path $outputDirectory "$case.stdout.log"
        $stderrLog = Join-Path $outputDirectory "$case.stderr.log"
        $process = Start-Process -FilePath $executable -ArgumentList $case -WorkingDirectory $fixtureDirectory `
            -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog
        $null = $process.Handle
        if (-not $process.WaitForExit(45000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "Configuration test timed out: $case"
        }
        $process.WaitForExit()
        $testExitCode = $process.ExitCode
        if ($null -eq $testExitCode) { throw 'Process.ExitCode unavailable; cannot report a pass.' }
        Add-RunLine "case=$case test_exit_code=$testExitCode"
        $summary = Get-Content -LiteralPath $stdoutLog |
            Where-Object { $_ -match '^\d+ checks, \d+ failures$' } | Select-Object -Last 1
        if ($summary -match '^(\d+) checks, (\d+) failures$') {
            $totalChecks += [int]$matches[1]
            $totalFailures += [int]$matches[2]
            Write-Host "$case`: $summary"
        } else {
            $failedCases += $case
        }
        if ($testExitCode -ne 0) {
            $failedCases += $case
            Get-Content -LiteralPath $stderrLog
        }
    }
    foreach ($inputPath in $inputs) {
        if ((Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash -ne $hashes[$inputPath]) {
            throw "PENDING: input changed during runtime checks: $inputPath; results invalidated."
        }
    }
    if ($cases -contains 'presets' -and $failedCases -notcontains 'presets') {
        $inventoryPath = Join-Path $outputDirectory 'presets/preset-field-inventory.json'
        $inventory = Get-Content -LiteralPath $inventoryPath -Raw | ConvertFrom-Json
        if (-not $inventory.fields.Count) { throw 'Preset field inventory is empty.' }
        foreach ($field in $inventory.fields) {
            foreach ($name in @('key', 'type', 'default', 'range', 'category', 'pass', 'effect', 'environment')) {
                if ($field.PSObject.Properties.Name -notcontains $name) { throw "Missing inventory field: $name" }
            }
        }
        Add-RunLine "preset_field_inventory=$inventoryPath fields=$($inventory.fields.Count)"
    }
    Add-RunLine "check_summary=$totalChecks checks, $totalFailures failures"
    if ($failedCases.Count -gt 0 -or $totalFailures -gt 0) {
        throw "Actual configuration checks failed: $($failedCases -join ', '); $totalFailures assertion failures."
    }
    Add-RunLine 'status=passed'
    Write-Host "Compile=$compileExitCode Link=$linkExitCode Process.ExitCode=$testExitCode"
} catch {
    if ($_.Exception.Message.StartsWith('PENDING:')) {
        Add-RunLine 'status=pending'
    } else {
        Add-RunLine 'status=failed'
    }
    Add-RunLine "error=$($_.Exception.Message)"
    throw
}
Complete-TestOutput $OutputDirectory -Outcome Passed
