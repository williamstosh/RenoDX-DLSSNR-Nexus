param([string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/tests/artifact-policy-' + [guid]::NewGuid().ToString('N'))))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'test-support.ps1')
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$checks = 0
function Assert-ArtifactTest([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
$canonical = Initialize-TestOutput (Join-Path $OutputDirectory 'lowercase') -RetentionMode 'full'
Assert-ArtifactTest ($global:NexusTestOutputs[$canonical].mode -ceq 'Full') 'Mode spelling not canonical'
Complete-TestOutput $canonical
foreach ($mode in @('Standard','Full','Acceptance')) {
    foreach ($outcome in @('Passed','Failed')) {
        $directory = Initialize-TestOutput (Join-Path $OutputDirectory "$mode-$outcome") -RetentionMode $mode
        $object = Join-Path $directory 'generated.obj'
        Register-TestCompilerOutput $directory -ObjectPath $object
        [IO.File]::WriteAllBytes($object, [byte[]]@(0x64,0x86,0,0))
        [IO.File]::WriteAllText((Join-Path $directory 'evidence.json'), '{"keep":true}')
        [IO.File]::WriteAllText((Join-Path $directory 'unregistered.obj'), 'not owned by compiler registration')
        Complete-TestOutput $directory -Outcome $outcome
        $expected = $mode -ne 'Standard' -or $outcome -ne 'Passed'
        Assert-ArtifactTest ((Test-Path -LiteralPath $object) -eq $expected) "$mode/$outcome intermediate retention"
        Assert-ArtifactTest (Test-Path -LiteralPath (Join-Path $directory 'evidence.json')) 'Evidence removed'
        Assert-ArtifactTest (Test-Path -LiteralPath (Join-Path $directory 'unregistered.obj')) 'Unregistered file removed'
        $state = Get-Content -LiteralPath (Join-Path $directory 'artifact-lifecycle.json') -Raw | ConvertFrom-Json
        Assert-ArtifactTest ($state.status -eq $outcome) 'Wrong completion status'
    }
}
$interrupted = Initialize-TestOutput (Join-Path $OutputDirectory 'interrupted')
$object = Join-Path $interrupted 'generated.obj'
Register-TestCompilerOutput $interrupted -ObjectPath $object
[IO.File]::WriteAllText($object, 'unfinished')
Assert-ArtifactTest (Test-Path -LiteralPath $object) 'Incomplete output must be retained'
$rejected = $false
try { Initialize-TestOutput $interrupted | Out-Null } catch { $rejected = $true }
Assert-ArtifactTest $rejected 'Nonempty output reused'
$rejected = $false
try { Register-TestCompilerOutput $interrupted -ObjectPath (Join-Path $OutputDirectory 'escape.obj') } catch { $rejected = $true }
Assert-ArtifactTest $rejected 'Compiler output escaped owner'
$markerPath = Join-Path $interrupted 'artifact-lifecycle.json'
$marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
$marker.runId = 'changed-owner'
$marker | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $markerPath -Encoding utf8
$rejected = $false
try { Complete-TestOutput $interrupted } catch { $rejected = $true }
Assert-ArtifactTest $rejected 'Changed ownership accepted'
Assert-ArtifactTest (Test-Path -LiteralPath $object) 'Changed owner data removed'
$locked = Initialize-TestOutput (Join-Path $OutputDirectory 'locked')
$lockedObject = Join-Path $locked 'generated.obj'
Register-TestCompilerOutput $locked -ObjectPath $lockedObject
[IO.File]::WriteAllText($lockedObject, 'in use')
$lock = [IO.File]::Open($lockedObject, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
try { Complete-TestOutput $locked } finally { $lock.Dispose() }
Assert-ArtifactTest (Test-Path -LiteralPath $lockedObject) 'Occupied file was removed'
$lockedState = Get-Content -LiteralPath (Join-Path $locked 'artifact-lifecycle.json') -Raw | ConvertFrom-Json
Assert-ArtifactTest (@($lockedState.cleanupErrors).Count -eq 1) 'Cleanup error was hidden'
$timedOut = Initialize-TestOutput (Join-Path $OutputDirectory 'timeout')
$timeoutObject = Join-Path $timedOut 'generated.obj'
Register-TestCompilerOutput $timedOut -ObjectPath $timeoutObject
[IO.File]::WriteAllText($timeoutObject, 'unfinished')
$timeoutObserved = $false
try {
    Invoke-TestProcess (Join-Path $PSHOME 'pwsh.exe') $timedOut 'sleep' @('-NoProfile','-Command','"Start-Sleep -Seconds 5"') -TimeoutMilliseconds 200
} catch { $timeoutObserved = $_.Exception.Message -like 'Test timeout:*' }
Assert-ArtifactTest $timeoutObserved 'Timeout was not reported'
Assert-ArtifactTest (Test-Path -LiteralPath $timeoutObject) 'Timed out evidence removed'
$timeoutState = Get-Content -LiteralPath (Join-Path $timedOut 'artifact-lifecycle.json') -Raw | ConvertFrom-Json
Assert-ArtifactTest ($timeoutState.status -eq 'Failed') 'Timeout did not mark incomplete run failed'
$target = Join-Path $OutputDirectory 'outside'
[void][IO.Directory]::CreateDirectory($target)
$junction = Join-Path $OutputDirectory 'linked-output'
try {
    New-Item -ItemType Junction -Path $junction -Target $target | Out-Null
    $rejected = $false
    try { Initialize-TestOutput (Join-Path $junction 'new-output') | Out-Null } catch { $rejected = $true }
    Assert-ArtifactTest $rejected 'Reparse ancestor accepted'
    Assert-ArtifactTest (-not (Test-Path -LiteralPath (Join-Path $target 'new-output'))) 'Initialization wrote through junction'
} finally {
    if (Test-Path -LiteralPath $junction) { (Get-Item -LiteralPath $junction -Force).Delete() }
}
@{ checks=$checks; result='passed'; scope='synthetic local ownership, mode, failure and reparse cases' } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
Complete-TestOutput $OutputDirectory
Write-Host "Artifact lifecycle: $checks checks passed. $OutputDirectory"
