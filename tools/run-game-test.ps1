[CmdletBinding()]
param(
    [string]$Game = 'witcher3',
    [string]$Platform = 'steam',
    [ValidateSet('smoke', 'menu', 'calibrate')][string]$Scenario = 'smoke',
    [string]$Save,
    [string]$Python,
    [string]$TargetPath = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/game-test/witcher3-steam.json'),
    [string]$OutputRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) 'artifacts/game-tests'),
    [int]$CaptureHoldSeconds = 0,
    [ValidateRange(0,2)][int]$Retries = 2,
    [switch]$Preflight,
    [switch]$AddonAbsentControl
)
$ErrorActionPreference = 'Stop'
if ($CaptureHoldSeconds -ne 0 -and ($CaptureHoldSeconds -lt 10 -or $CaptureHoldSeconds -gt 120)) {
    throw '-CaptureHoldSeconds must be 0 or between 10 and 120.'
}
if ($CaptureHoldSeconds -gt 0 -and $Scenario -ne 'smoke') {
    throw '-CaptureHoldSeconds is only supported for the smoke scenario.'
}
if (-not $Python -or -not (Test-Path -LiteralPath $Python -PathType Leaf)) { throw 'Provide -Python with a Python 3 interpreter containing Pillow.' }
$taskArguments = @('-B', (Join-Path $PSScriptRoot 'game-test/runner.py'), '--game', $Game, '--platform', $Platform,
    '--scenario', $Scenario, '--capture-hold-seconds', "$CaptureHoldSeconds", '--retries', "$Retries")
$taskArguments += @('--target', $TargetPath, '--output-root', $OutputRoot)
if ($Save) { $taskArguments += @('--save', $Save) }
if ($Preflight) { $taskArguments += '--preflight' }
if ($AddonAbsentControl) { $taskArguments += '--addon-absent-control' }
& $Python @taskArguments
exit $LASTEXITCODE
