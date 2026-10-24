[CmdletBinding()]
param(
    [string]$OutputDirectory = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'artifacts/game-tests/ocr-tool'),
    [string]$WindowsSdkRoot = (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10')
)

$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'ocr.cs'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$outputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (-not (@('tmp','artifacts') | Where-Object { $outputDirectory.StartsWith((Join-Path $root $_) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) })) {
    throw 'OCR output must remain below repository tmp/ or artifacts/.'
}
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$metadata = Join-Path $WindowsSdkRoot 'UnionMetadata/10.0.26100.0/Windows.winmd'
$gac = Join-Path $env:WINDIR 'Microsoft.NET/assembly/GAC_MSIL'

foreach ($required in @($source, $compiler, $metadata)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required OCR build input is missing: $required"
    }
}

# Use installed Framework facades when the SDK reference-assembly pack is absent.
$references = @('System.Runtime', 'System.Runtime.WindowsRuntime',
    'System.Runtime.InteropServices.WindowsRuntime', 'System.ObjectModel',
    'System.Collections', 'System.Threading.Tasks') | ForEach-Object {
    $assembly = Get-ChildItem -LiteralPath (Join-Path $gac $_) -Filter "$_.dll" -Recurse |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $assembly) { throw "Required installed assembly is missing: $_" }
    $assembly.FullName
}

$null = New-Item -ItemType Directory -Path $outputDirectory -Force
$executable = Join-Path $outputDirectory 'ocr.exe'
$arguments = @('/nologo', '/target:exe', '/platform:x64', '/optimize+', '/utf8output',
    "/out:$executable", "/reference:$metadata")
$arguments += $references | ForEach-Object { "/reference:$_" }
$arguments += $source
$diagnostics = & $compiler @arguments 2>&1
if ($LASTEXITCODE -ne 0) {
    throw "OCR compilation failed (exit $LASTEXITCODE):`n$($diagnostics -join "`n")"
}
if ($diagnostics) { Write-Verbose ($diagnostics -join "`n") }
$executable
