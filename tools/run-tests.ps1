[CmdletBinding()]
param(
    [ValidateSet('all','report','branding','ui','presets','unicode-input','native','sdr-policy','hdr-policy','scope','config','constants','fg-xefg','fg-native','lifecycle','bridge-motion','stage-plan','copy-slots')]
    [string[]]$Suite = @('all'),
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$HostRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/renodx'),
    [string]$ShaderIncludeDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.include'),
    [string]$BuildProject = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.vcxproj'),
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/tests/' + [guid]::NewGuid().ToString('N'))),
    [string]$VcVars64,
    [string]$DetoursLibrary,
    [string]$PythonExecutable,
    [ValidateSet('Auto','Quick','Full','None')][string]$UiPreview = 'Auto',
    [ValidateSet('native-required-menu','native-owner-idle','native-options-resume','native-options-delayed','native-guide-contract','present-guide-models','native-present-decision','trace-metadata','stage-chain','source-lifecycle','feature-switch','feature-switch-hardware','f5-mixed','present-chain','present-chain-hardware','present-chain-scrgb','present-chain-hdr10','native-full','native-full-hardware')]
    [string[]]$NativeCase = @(),
    [ValidateSet('dlaa','sr')][string[]]$BridgeCase = @(),
    [switch]$BuildOnly,
    [ValidateSet('Standard','Full','Acceptance')][string]$RetentionMode = $(if ($env:NR_NEXUS_TEST_RETENTION) { $env:NR_NEXUS_TEST_RETENTION } else { 'Standard' }),
    [switch]$SoftwareOnly
)
$previousRetention = $env:NR_NEXUS_TEST_RETENTION
try {
$RetentionMode = @('Standard','Full','Acceptance') | Where-Object { $_ -ieq $RetentionMode } | Select-Object -First 1
$env:NR_NEXUS_TEST_RETENTION = $RetentionMode
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = Split-Path $PSScriptRoot -Parent
$testRoot = Join-Path $repositoryRoot 'tests'
. (Join-Path $testRoot 'test-support.ps1')
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if ($SourceRoot.TrimEnd('\','/') -ne $repositoryRoot.TrimEnd('\','/') -and $UiPreview -in @('Quick','Full')) {
    throw 'Explicit UI preview requires this checkout as SourceRoot; run the preview entry in the target checkout.'
}
$HostRoot = [IO.Path]::GetFullPath($HostRoot)
$ShaderIncludeDirectory = [IO.Path]::GetFullPath($ShaderIncludeDirectory)
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$sources = [ordered]@{
    report='report_flow_tests'; ui='ui_layout_tests'
    presets='preset_store_tests'
    'unicode-input'='unicode_input_tests'
    branding='branding_tests'
    native='native_contract_tests'; 'sdr-policy'='present_sdr_restore_policy_tests'
    'hdr-policy'='present_hdr_pedestal_policy_tests'; scope='present_restore_scope_tests'
    config='present_restore_configuration_tests'; constants='present_hdr_runtime_constants_tests'
    'fg-xefg'='fg_xefg_tests'; 'fg-native'='fg_native_tests'
    lifecycle='fg_lifecycle_tests'; 'bridge-motion'='d3d11_bridge_motion_tests'
    'stage-plan'='stage_plan_tests'
    'copy-slots'='copy_slots_tests'
}
if ($Suite -contains 'all') { $Suite = @($sources.Keys) }
if ($BuildOnly -and @($Suite | Where-Object { $_ -in @('config','constants','lifecycle','ui') }).Count) {
    throw '-BuildOnly requires explicitly selecting native/policy/scope/fg-xefg/fg-native/bridge-motion suites.'
}
if ($NativeCase.Count -and $Suite -notcontains 'fg-native') { throw '-NativeCase requires -Suite fg-native (or all).' }
if ($BridgeCase -and $Suite -notcontains 'bridge-motion') { throw '-BridgeCase requires -Suite bridge-motion (or all).' }
if (-not $DetoursLibrary) { $DetoursLibrary = Join-Path $HostRoot 'external/Detours/lib.X64/detours.lib' }
$parameters = @{ SourceRoot=$SourceRoot; HostRoot=$HostRoot; ShaderIncludeDirectory=$ShaderIncludeDirectory
    BuildProject=$BuildProject; VcVars64=$VcVars64 }
Initialize-TestBuild $VcVars64
$common = @('/nologo','/std:c++20','/EHsc','/W4','/O2','/MT','/utf-8','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN')
$includes = @($SourceRoot, "$HostRoot/src", "$HostRoot/src/addons/dlss5",
    "$HostRoot/external/Detours/src", "$HostRoot/external/reshade", "$HostRoot/external/Streamline/include")
$libraries = @($DetoursLibrary,'uuid.lib','advapi32.lib','d3d11.lib','d3d12.lib','dxgi.lib','dxguid.lib')
foreach ($item in $Suite) {
    $directory = Join-Path $OutputDirectory $item
    if ($item -eq 'unicode-input') {
        if ($BuildOnly) { throw '-BuildOnly is not supported for unicode-input.' }
        & (Join-Path $testRoot 'run-unicode-input-tests.ps1') -SourceRoot $SourceRoot -OutputDirectory $directory -VcVars64 $VcVars64
        continue
    }
    if ($item -eq 'presets') {
        & (Join-Path $testRoot 'run-preset-store-tests.ps1') -SourceRoot $SourceRoot -HostRoot $HostRoot -OutputDirectory $directory -VcVars64 $VcVars64
        & (Join-Path $testRoot 'run-profile-store-tests.ps1') -SourceRoot $SourceRoot -HostRoot $HostRoot -OutputDirectory (Join-Path $OutputDirectory 'profiles') -VcVars64 $VcVars64
        & (Join-Path $testRoot 'run-preset-dialog-tests.ps1') -OutputDirectory (Join-Path $OutputDirectory 'preset-dialog') -VcVars64 $VcVars64
        continue
    }
    if ($item -in @('report','config','constants','lifecycle','ui')) {
        if ($BuildOnly) { throw '-BuildOnly is supported for native/policy/scope/fg-xefg/fg-native/bridge-motion; select these explicitly.' }
        $entry = switch ($item) {
            report { 'run-report-tests.ps1' }
            config { 'run-present-restore-configuration-tests.ps1' }
            constants { 'run-present-hdr-runtime-constants-tests.ps1' }
            lifecycle { 'run-fg-lifecycle-tests.ps1' }
            ui { 'run-ui-layout-tests.ps1' }
        }
        if ($item -eq 'report') {
            & (Join-Path $testRoot $entry) @parameters -OutputDirectory $directory -PythonExecutable $PythonExecutable
        } else {
            & (Join-Path $testRoot $entry) @parameters -OutputDirectory $directory
        }
        continue
    }
    $directory = Initialize-TestOutput $directory
    if ($item -eq 'branding') {
        & (Join-Path $testRoot 'run-branding-check-tests.ps1') -OutputDirectory $directory
    }
    $name = $sources[$item]
    $exe = Join-Path $directory "$name.exe"
    $arguments = $common + @($includes | ForEach-Object { "/I$_" })
    $link = $libraries
    if ($item -in @('branding','sdr-policy','hdr-policy','scope','stage-plan')) { $link = @() }
    if ($item -in @('fg-native','bridge-motion')) {
        if (-not (Test-Path -LiteralPath "$ShaderIncludeDirectory/embed/shaders.h")) { throw 'Build new shaders first.' }
        $arguments += @('/bigobj','/Gy','/Gw','/DNDEBUG','/DRESHADE_ADDON=1',
            "/I$ShaderIncludeDirectory", "/I$HostRoot/external/DLSS/include",
            "/I$HostRoot/external/json/include", "/I$HostRoot/external/frozen/include", "/I$HostRoot/external/gtl/include")
        $link += @('user32.lib','gdi32.lib','ole32.lib','psapi.lib')
        if ($item -eq 'bridge-motion') { $link += 'd3dcompiler.lib' }
    }
    if ($item -eq 'branding') { $arguments += '/DRENODX_DLSS5_DEBUG' }
    $arguments += @((Join-Path $testRoot "$name.cpp"), "/Fo$directory/$name.obj", "/Fe$exe",
        '/sourceDependencies', "$directory/dependencies.json", '/link', '/OPT:REF', '/OPT:ICF') + $link
    $arguments | ConvertTo-Json | Set-Content -LiteralPath "$directory/arguments.json" -Encoding utf8
    Register-TestCompilerOutput $directory -ObjectPath @(Join-Path $directory "$name.obj") -ExecutablePath $exe
    & cl.exe @arguments *> "$directory/compile.log"
    if ($LASTEXITCODE -ne 0) { throw "Compile/link failed: $item; see $directory/compile.log" }
    $dependencies = (Get-Content -LiteralPath "$directory/dependencies.json" -Raw | ConvertFrom-Json).Data.Includes
    $hashes = @(@((Join-Path $testRoot "$name.cpp"), $PSCommandPath) + $dependencies | Sort-Object -Unique | ForEach-Object {
        @{path=$_; sha256=(Get-FileHash -LiteralPath $_).Hash}
    })
    $hashes | ConvertTo-Json | Set-Content -LiteralPath "$directory/inputs.json" -Encoding utf8
    if (-not $BuildOnly) {
        if ($item -eq 'bridge-motion') {
            $bridgeCases = if ($BridgeCase.Count) { $BridgeCase } else { @('') }
            foreach ($case in $bridgeCases) {
                $bridgeArguments = @()
                $suffix = ''
                if ($case) { $bridgeArguments += "--$case"; $suffix = "-$case" }
                if (-not $SoftwareOnly) { Invoke-TestProcess $exe $directory "hardware$suffix" $bridgeArguments }
                Invoke-TestProcess $exe $directory "warp$suffix" (@('--warp') + $bridgeArguments)
            }
        } elseif ($item -eq 'fg-native' -and $NativeCase.Count) {
            foreach ($case in $NativeCase) { Invoke-TestProcess $exe $directory $case @("--$case") }
        } else {
            Invoke-TestProcess $exe $directory 'test'
            if ($item -eq 'fg-native') { Invoke-TestProcess $exe $directory 'f5-mixed' @('--f5-mixed') }
        }
        foreach ($inputHash in $hashes) {
            if ((Get-FileHash -LiteralPath $inputHash.path).Hash -ne $inputHash.sha256) { throw "Test input changed: $($inputHash.path)" }
        }
    }
    @{suite=$item; build='passed'; run=$(if ($BuildOnly) { 'not_run' } else { 'passed' })} |
        ConvertTo-Json | Set-Content -LiteralPath "$directory/result.json" -Encoding utf8
    if (-not $BuildOnly) { Complete-TestOutput $directory }
}
if (-not $BuildOnly) { Complete-TestOutput $OutputDirectory }
Write-Host "Test outputs: $OutputDirectory"
if ($Suite -contains 'ui' -and -not $BuildOnly -and $UiPreview -ne 'None') {
    $previewMode = $UiPreview
    if ($previewMode -eq 'Auto') {
        $uiChanges = @(& git --no-optional-locks -C $SourceRoot diff --name-only HEAD -- ui)
        $uiChanges += @(& git --no-optional-locks -C $SourceRoot ls-files --others --exclude-standard -- ui)
        # These headers also contain rendering/business code. Trigger on UI
        # hunks rather than forcing captures for every edit to the monolith.
        $mixedDiff = @(& git --no-optional-locks -C $SourceRoot diff --unified=0 HEAD -- dlssnr.hpp preset_runtime.hpp profile_runtime.hpp brand.h)
        $uiChanges += @($mixedDiff | Where-Object {
            $_ -match '^@@.*\b(Draw[A-Z]\w*|OnOverlay|\w*Ui\w*)\b' -or
            $_ -match '^[+-](?![+-]).*(ImGui::|ui::|\bui_\w+|\bNR_NEXUS_NAME\b)'
        })
        $previewMode = $(if ($uiChanges.Count) { 'Quick' } else { 'None' })
        if ($SourceRoot.TrimEnd('\','/') -ne $repositoryRoot.TrimEnd('\','/')) {
            $previewMode = 'None'
            Write-Host 'Custom SourceRoot tested; invoke preview-ui.ps1 from that checkout to preview the same source.'
        }
    }
    if ($previewMode -ne 'None') {
        & (Join-Path $PSScriptRoot 'preview-ui.ps1') -Mode $previewMode -PythonExecutable $PythonExecutable `
            -HostRoot $HostRoot -ShaderIncludeDirectory $ShaderIncludeDirectory -BuildProject $BuildProject -VcVars64 $VcVars64 -RetentionMode $RetentionMode
    }
}

} finally {
    $env:NR_NEXUS_TEST_RETENTION = $previousRetention
}
