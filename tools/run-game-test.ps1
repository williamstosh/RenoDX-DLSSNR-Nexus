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
    [string]$CaptureScript,
    [ValidateRange(0,2)][int]$Retries = 0,
    [switch]$Preflight,
    [switch]$ManualImeSession,
    [switch]$AddonAbsentControl
)
$ErrorActionPreference = 'Stop'
if ($CaptureHoldSeconds -ne 0 -and ($CaptureHoldSeconds -lt 10 -or $CaptureHoldSeconds -gt 900)) {
    throw '-CaptureHoldSeconds must be 0 or between 10 and 900.'
}
if ($CaptureHoldSeconds -gt 0 -and $Scenario -notin @('smoke', 'menu')) {
    throw '-CaptureHoldSeconds is only supported for smoke or menu scenarios.'
}
if ($ManualImeSession -and ($Scenario -ne 'menu' -or $CaptureHoldSeconds -eq 0 -or $Retries -ne 0)) {
    throw '-ManualImeSession requires menu, a nonzero capture hold and -Retries 0.'
}
if ($CaptureScript) {
    if ($CaptureHoldSeconds -eq 0 -or $ManualImeSession) { throw '-CaptureScript requires capture hold without ManualImeSession.' }
    $taskCapturePath = [IO.Path]::GetFullPath($CaptureScript)
    $taskRepository = Split-Path $PSScriptRoot -Parent
    if (-not (Test-Path -LiteralPath $taskCapturePath -PathType Leaf) -or [IO.Path]::GetExtension($taskCapturePath) -ne '.py' -or
        -not (@('tools','tmp') | Where-Object { $taskCapturePath.StartsWith((Join-Path $taskRepository $_) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) })) {
        throw 'Capture script must be a Python file below repository tools/ or tmp/.'
    }
    $CaptureScript = $taskCapturePath
    $PSBoundParameters['CaptureScript'] = $taskCapturePath
}
. (Join-Path $PSScriptRoot 'game-test/normal-context.ps1')
$taskRepository = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'game-test/resolve-python.ps1')
$Python = Resolve-NrNexusPython -RequestedPath $Python -RepositoryRoot $taskRepository
$PSBoundParameters['Python'] = $Python
$taskContextExitCode = Invoke-NormalGameTestContext -EntryScript $PSCommandPath -Parameters $PSBoundParameters -Python $Python
if ($null -ne $taskContextExitCode) { exit $taskContextExitCode }
$taskArguments = @('-B', (Join-Path $PSScriptRoot 'game-test/runner.py'), '--game', $Game, '--platform', $Platform,
    '--scenario', $Scenario, '--capture-hold-seconds', "$CaptureHoldSeconds", '--retries', "$Retries")
$taskArguments += @('--target', $TargetPath, '--output-root', $OutputRoot)
if ($Save) { $taskArguments += @('--save', $Save) }
if ($CaptureScript) { $taskArguments += @('--capture-script', $CaptureScript) }
if ($Preflight) { $taskArguments += '--preflight' }
if ($ManualImeSession) { $taskArguments += '--manual-ime-session' }
if ($AddonAbsentControl) { $taskArguments += '--addon-absent-control' }
& $Python @taskArguments
exit $LASTEXITCODE
