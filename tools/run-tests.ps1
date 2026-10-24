[CmdletBinding()]
param(
    [ValidateSet('all','branding','native','sdr-policy','hdr-policy','scope','config','constants','fg-xefg','fg-native','lifecycle','bridge-motion')]
    [string[]]$Suite = @('all'),
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$HostRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/renodx'),
    [string]$ShaderIncludeDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.include'),
    [string]$BuildProject = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.vcxproj'),
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/tests/' + [guid]::NewGuid().ToString('N'))),
    [string]$VcVars64,
    [string]$DetoursLibrary,
    [ValidateSet('native-required-menu','native-owner-idle','native-options-resume','native-options-delayed','native-guide-contract','present-guide-models','native-present-decision','trace-metadata')]
    [string[]]$NativeCase = @(),
    [switch]$BuildOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = Split-Path $PSScriptRoot -Parent
$testRoot = Join-Path $repositoryRoot 'tests'
. (Join-Path $testRoot 'test-support.ps1')
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$HostRoot = [IO.Path]::GetFullPath($HostRoot)
$ShaderIncludeDirectory = [IO.Path]::GetFullPath($ShaderIncludeDirectory)
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$sources = [ordered]@{
    branding='branding_tests'
    native='native_contract_tests'; 'sdr-policy'='present_sdr_restore_policy_tests'
    'hdr-policy'='present_hdr_pedestal_policy_tests'; scope='present_restore_scope_tests'
    config='present_restore_configuration_tests'; constants='present_hdr_runtime_constants_tests'
    'fg-xefg'='fg_xefg_tests'; 'fg-native'='fg_native_tests'
    lifecycle='fg_lifecycle_tests'; 'bridge-motion'='d3d11_bridge_motion_tests'
}
if ($Suite -contains 'all') { $Suite = @($sources.Keys) }
if ($BuildOnly -and @($Suite | Where-Object { $_ -in @('config','constants','lifecycle') }).Count) {
    throw '-BuildOnly requires explicitly selecting native/policy/scope/fg-xefg/fg-native/bridge-motion suites.'
}
if ($NativeCase.Count -and $Suite -notcontains 'fg-native') { throw '-NativeCase requires -Suite fg-native (or all).' }
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
    if ($item -in @('config','constants','lifecycle')) {
        if ($BuildOnly) { throw '-BuildOnly is supported for native/policy/scope/fg-xefg/fg-native/bridge-motion; select these explicitly.' }
        $entry = switch ($item) {
            config { 'run-present-restore-configuration-tests.ps1' }
            constants { 'run-present-hdr-runtime-constants-tests.ps1' }
            lifecycle { 'run-fg-lifecycle-tests.ps1' }
        }
        & (Join-Path $testRoot $entry) @parameters -OutputDirectory $directory
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
    if ($item -in @('branding','sdr-policy','hdr-policy','scope')) { $link = @() }
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
    & cl.exe @arguments *> "$directory/compile.log"
    if ($LASTEXITCODE -ne 0) { throw "Compile/link failed: $item; see $directory/compile.log" }
    $dependencies = (Get-Content -LiteralPath "$directory/dependencies.json" -Raw | ConvertFrom-Json).Data.Includes
    $hashes = @(@((Join-Path $testRoot "$name.cpp"), $PSCommandPath) + $dependencies | Sort-Object -Unique | ForEach-Object {
        @{path=$_; sha256=(Get-FileHash -LiteralPath $_).Hash}
    })
    $hashes | ConvertTo-Json | Set-Content -LiteralPath "$directory/inputs.json" -Encoding utf8
    if (-not $BuildOnly) {
        if ($item -eq 'bridge-motion') {
            Invoke-TestProcess $exe $directory 'hardware'
            Invoke-TestProcess $exe $directory 'warp' @('--warp')
        } elseif ($item -eq 'fg-native' -and $NativeCase.Count) {
            foreach ($case in $NativeCase) { Invoke-TestProcess $exe $directory $case @("--$case") }
        } else { Invoke-TestProcess $exe $directory 'test' }
        foreach ($inputHash in $hashes) {
            if ((Get-FileHash -LiteralPath $inputHash.path).Hash -ne $inputHash.sha256) { throw "Test input changed: $($inputHash.path)" }
        }
    }
    @{suite=$item; build='passed'; run=$(if ($BuildOnly) { 'not_run' } else { 'passed' })} |
        ConvertTo-Json | Set-Content -LiteralPath "$directory/result.json" -Encoding utf8
}
Write-Host "Test outputs: $OutputDirectory"
