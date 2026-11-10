[CmdletBinding()]
param(
    [ValidateSet('warp','nvidia')][string[]]$Adapter = @('warp'),
    [string]$HostRoot = (Join-Path $PSScriptRoot '../.local/renodx'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot ('../tmp/tests/look-shadow-' + [guid]::NewGuid().ToString('N'))),
    [string]$PythonExecutable,
    [string]$VcVars64
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$HostRoot = [IO.Path]::GetFullPath($HostRoot)
. (Join-Path $PSScriptRoot 'test-support.ps1')
. (Join-Path $root 'tools/game-test/resolve-python.ps1')
$python = Resolve-NrNexusPython -RequestedPath $PythonExecutable -RepositoryRoot $root -RequiredImports @()
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$outcome = 'Failed'
try {
    Initialize-TestBuild $VcVars64
    $source = Join-Path $PSScriptRoot 'look_shadow_tests.cpp'
    $reference = Join-Path $root 'tools/look/shadow_reference.py'
    $fixture = Join-Path $OutputDirectory 'fixture.json'
    $exe = Join-Path $OutputDirectory 'look_shadow_tests.exe'
    $object = Join-Path $OutputDirectory 'look_shadow_tests.obj'
    $dependencies = Join-Path $OutputDirectory 'dependencies.json'
    & $python $reference selftest *> (Join-Path $OutputDirectory 'reference-selftest.log')
    if ($LASTEXITCODE -ne 0) { throw 'Look shadow reference selftest failed.' }
    & $python $reference generate --output $fixture
    if ($LASTEXITCODE -ne 0) { throw 'Look shadow fixture generation failed.' }
    $inputs = @($source, $reference, $PSCommandPath, (Join-Path $root 'look_stage.hpp'),
        (Join-Path $root 'shaders/look_compose.cs_5_1.hlsl'), (Join-Path $root 'shaders/look_common.hlsli'),
        (Join-Path $root 'shaders/codec_math.hlsli'), (Join-Path $root 'shaders/vk_binding.hlsli'), $fixture)
    $before = @{}
    foreach ($path in $inputs) { $before[$path] = (Get-FileHash -LiteralPath $path).Hash }
    $arguments = @('/nologo','/std:c++20','/EHsc','/W4','/O2','/MT','/utf-8','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN',
        "/I$root", "/I$HostRoot/src", "/I$HostRoot/src/addons/dlss5", "/I$HostRoot/external/reshade/include",
        "/I$HostRoot/external/json/include", "/I$HostRoot/external/Detours/src", "/I$HostRoot/external/gtl/include",
        $source, "/Fo$object", "/Fe$exe", '/sourceDependencies', $dependencies,
        '/link','d3d12.lib','dxgi.lib','dxguid.lib','d3dcompiler.lib')
    $arguments | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'arguments.json') -Encoding utf8
    Register-TestCompilerOutput $OutputDirectory -ObjectPath $object -ExecutablePath $exe
    & cl.exe @arguments *> (Join-Path $OutputDirectory 'compile.log')
    if ($LASTEXITCODE -ne 0) { throw "Look shadow fixture compilation failed; see $OutputDirectory/compile.log" }
    $includes = (Get-Content -LiteralPath $dependencies -Raw | ConvertFrom-Json).Data.Includes
    foreach ($path in $includes) {
        if (-not $before.ContainsKey($path)) { $before[$path] = (Get-FileHash -LiteralPath $path).Hash }
    }
    $before | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'inputs.json') -Encoding utf8
    foreach ($backend in ($Adapter | Select-Object -Unique)) {
        $gpu = Join-Path $OutputDirectory "$backend.json"
        & $exe $root $fixture $gpu $backend *> (Join-Path $OutputDirectory "$backend.log")
        if ($LASTEXITCODE -ne 0) { throw "Look shadow $backend execution failed; see $OutputDirectory/$backend.log" }
        & $python $reference compare --fixture $fixture --gpu $gpu --output (Join-Path $OutputDirectory "$backend-comparison.json")
        if ($LASTEXITCODE -ne 0) { throw "Look shadow $backend reference comparison failed." }
    }
    foreach ($path in $before.Keys) {
        if ((Get-FileHash -LiteralPath $path).Hash -cne $before[$path]) { throw "Input changed during experiment: $path" }
    }
    [ordered]@{
        status='passed'; adapters=$Adapter; evidence='WARP or NATIVE_GPU; synthetic neural output; NGX not run'
        compiler=(Get-Command cl.exe).Source
        shaderCompiler=(Get-Item (Join-Path $env:WINDIR 'System32/d3dcompiler_47.dll')).VersionInfo.FileVersion
        python=$python; executableSha256=(Get-FileHash -LiteralPath $exe).Hash
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
    $outcome = 'Passed'
    Write-Host "Look shadow comparison passed: $OutputDirectory"
} finally {
    Complete-TestOutput $OutputDirectory -Outcome $outcome
}
