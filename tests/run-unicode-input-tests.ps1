[CmdletBinding()]
param(
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/tests/unicode-input-' + [guid]::NewGuid().ToString('N'))),
    [Alias('Interactive')][switch]$Manual,
    [switch]$SingleLine,
    [string]$VcVars64
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'test-support.ps1')

Initialize-TestBuild $VcVars64
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$source = Join-Path $PSScriptRoot 'unicode_input_tests.cpp'
$executable = Join-Path $OutputDirectory 'unicode_input_tests.exe'
$module = Join-Path $OutputDirectory 'unicode_input_module.dll'
$common = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/utf-8',
    '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', "/I$SourceRoot", $source)
foreach ($kind in @('exe', 'dll')) {
    $arguments = $common + @("/Fo$OutputDirectory/$kind.obj", '/sourceDependencies', "$OutputDirectory/$kind-dependencies.json")
    if ($kind -eq 'dll') {
        $arguments += @('/DUNICODE_INPUT_TEST_MODULE', '/LD', "/Fe$module")
    } else {
        $arguments += "/Fe$executable"
    }
    $arguments += @('/link', 'user32.lib', 'gdi32.lib')
    $arguments | ConvertTo-Json | Set-Content -LiteralPath "$OutputDirectory/$kind-arguments.json" -Encoding utf8
    & cl.exe @arguments *> "$OutputDirectory/$kind-compile.log"
    if ($LASTEXITCODE -ne 0) { throw "Unicode input $kind build failed; see $OutputDirectory/$kind-compile.log" }
}
$dependencies = @('exe', 'dll' | ForEach-Object {
    (Get-Content -LiteralPath "$OutputDirectory/$_-dependencies.json" -Raw | ConvertFrom-Json).Data.Includes
})
$hashes = @(@($source, $PSCommandPath, (Join-Path $PSScriptRoot 'test-support.ps1')) + $dependencies |
    Sort-Object -Unique | ForEach-Object { @{ path = $_; sha256 = (Get-FileHash -LiteralPath $_).Hash } })
$hashes | ConvertTo-Json | Set-Content -LiteralPath "$OutputDirectory/inputs.json" -Encoding utf8
if ($Manual) {
    # Visible native editor, no deadline: use an actual IME or ComputerUse and
    # explicitly Apply/Cancel. Text is returned to stdout; nothing is saved.
    $manualArgument = if ($SingleLine) { '--interactive-single' } else { '--interactive' }
    & $executable $manualArgument
    if ($LASTEXITCODE -ne 0) { throw 'Manual editor failed.' }
    return
}
# Shared runner uses Start-Process -WindowStyle Hidden, redirected logs and a
# 45-second process deadline. Every real native test dialog is also kept hidden.
Invoke-TestProcess $executable $OutputDirectory 'test' @(('"' + $module + '"'))
foreach ($inputHash in $hashes) {
    if ((Get-FileHash -LiteralPath $inputHash.path).Hash -ne $inputHash.sha256) {
        throw "Unicode input test input changed: $($inputHash.path)"
    }
}
@{ suite = 'unicode-input'; build = 'passed'; run = 'passed'; unicode_edit = 'passed'
    bounded_apply = 'passed'; cancellation = 'passed'; cross_module_lifetime = 'passed'
    synthetic_ime_routing = 'passed'
    fault_injection = 'controlled-not-user-trace'; real_os_ime = 'manual-not-tested' } |
    ConvertTo-Json | Set-Content -LiteralPath "$OutputDirectory/result.json" -Encoding utf8
Write-Host "Unicode input outputs: $OutputDirectory"
