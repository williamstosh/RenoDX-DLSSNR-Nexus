[CmdletBinding()]
param(
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$HostRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/renodx'),
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/tests/preset-store-' + [guid]::NewGuid().ToString('N'))),
    [string]$VcVars64
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'test-support.ps1')
Initialize-TestBuild $VcVars64
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$HostRoot = [IO.Path]::GetFullPath($HostRoot)
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$source = Join-Path $PSScriptRoot 'preset_store_tests.cpp'
$common = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/utf-8',
    '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', "/I$SourceRoot", "/I$HostRoot/external/json/include", $source)
$executable = Join-Path $OutputDirectory 'preset_store_tests.exe'
$module = Join-Path $OutputDirectory 'preset_store_module.dll'
foreach ($kind in @('exe', 'dll')) {
    $arguments = $common + @("/Fo$OutputDirectory/$kind.obj", '/sourceDependencies', "$OutputDirectory/$kind-dependencies.json")
    if ($kind -eq 'dll') {
        $arguments += @('/DPRESET_STORE_TEST_MODULE', '/LD', "/Fe$module")
    } else {
        $arguments += "/Fe$executable"
    }
    $arguments | ConvertTo-Json | Set-Content -LiteralPath "$OutputDirectory/$kind-arguments.json" -Encoding utf8
    & cl.exe @arguments *> "$OutputDirectory/$kind-compile.log"
    if ($LASTEXITCODE -ne 0) { throw "Preset $kind build failed; see $OutputDirectory/$kind-compile.log" }
}
$dependencies = @('exe', 'dll' | ForEach-Object {
    (Get-Content -LiteralPath "$OutputDirectory/$_-dependencies.json" -Raw | ConvertFrom-Json).Data.Includes
})
$hashes = @(@($source, $PSCommandPath, (Join-Path $PSScriptRoot 'test-support.ps1')) + $dependencies |
    Sort-Object -Unique | ForEach-Object { @{ path = $_; sha256 = (Get-FileHash -LiteralPath $_).Hash } })
$hashes | ConvertTo-Json | Set-Content -LiteralPath "$OutputDirectory/inputs.json" -Encoding utf8
$data = Join-Path $OutputDirectory 'data'
$outside = Join-Path $data 'outside'
$library = Join-Path $data 'local-appdata/NR Nexus/Presets'
New-Item -ItemType Directory -Path $outside, $library | Out-Null
[IO.File]::WriteAllText((Join-Path $outside 'outside.nrnexus.json'), 'synthetic outside sentinel')
$ancestorLink = Join-Path $data 'ancestor-link'
$fileLink = Join-Path $library 'linked.nrnexus.json'
# Junctions exercise real NTFS reparse behavior without Developer Mode/admin.
New-Item -ItemType Junction -Path $ancestorLink -Target $outside | Out-Null
New-Item -ItemType Junction -Path $fileLink -Target $outside | Out-Null
try {
    Invoke-TestProcess $executable $OutputDirectory 'test' @(('"' + $data + '"'), ('"' + $module + '"'))
    foreach ($inputHash in $hashes) {
        if ((Get-FileHash -LiteralPath $inputHash.path).Hash -ne $inputHash.sha256) {
            throw "Preset test input changed: $($inputHash.path)"
        }
    }
    @{ suite = 'preset-store'; build = 'passed'; run = 'passed'; reparse = 'passed'; cross_module = 'passed' } |
        ConvertTo-Json | Set-Content -LiteralPath "$OutputDirectory/result.json" -Encoding utf8
} finally {
    # Remove only our junction entries; never recurse through their targets.
    foreach ($link in @($fileLink, $ancestorLink)) {
        if (Test-Path -LiteralPath $link) {
            $item = Get-Item -LiteralPath $link -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
                throw "Fixture is no longer a reparse point: $link"
            }
            $item.Delete()
        }
    }
}
Write-Host "Preset store outputs: $OutputDirectory"
