param(
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/tests/preset-dialog-' + [guid]::NewGuid().ToString('N'))),
    [string]$VcVars64
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'test-support.ps1')
$OutputDirectory = Initialize-TestOutput $OutputDirectory
Initialize-TestBuild $VcVars64
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$testSource = Join-Path $PSScriptRoot 'preset_dialog_tests.cpp'
$executable = Join-Path $OutputDirectory 'preset-dialog-tests.exe'
$object = Join-Path $OutputDirectory 'preset-dialog-tests.obj'
$compileLog = Join-Path $OutputDirectory 'compile.log'
$inputs = @($testSource, (Join-Path $sourceRoot 'preset_dialog.hpp'), $PSCommandPath)
$hashes = @{}
foreach ($inputPath in $inputs) {
    $hashes[$inputPath] = (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash
}
$hashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'input-hashes.json') -Encoding utf8
& cl.exe /nologo /std:c++20 /EHsc /W4 /WX /MT /O2 /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN "/I$sourceRoot" $testSource "/Fo$object" "/Fe$executable" /link ole32.lib shell32.lib uuid.lib *> $compileLog
if ($LASTEXITCODE -ne 0) {
    Get-Content -LiteralPath $compileLog
    throw 'Preset dialog compilation failed.'
}
Invoke-TestProcess -Executable $executable -Directory $OutputDirectory -Name 'preset-dialog'
foreach ($inputPath in $inputs) {
    if ((Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash -ne $hashes[$inputPath]) {
        throw "Input changed during test: $inputPath"
    }
}
Write-Host "Preset dialog tests passed: $OutputDirectory"
