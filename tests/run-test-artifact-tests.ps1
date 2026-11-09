param([string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/tests/artifact-policy-' + [guid]::NewGuid().ToString('N'))))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'test-support.ps1')
$env:NR_NEXUS_TEST_KEEP_RAW = '0'
$env:NR_NEXUS_TEST_BUDGET_BYTES = '0'
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$checks = 0
function Assert-ArtifactTest([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
$sizeTree = Join-Path $OutputDirectory 'size-tree'
$sizeNested = Join-Path $sizeTree 'nested'
[void][IO.Directory]::CreateDirectory($sizeNested)
[IO.File]::WriteAllBytes((Join-Path $sizeTree 'root.bin'), [byte[]]::new(5))
[IO.File]::WriteAllBytes((Join-Path $sizeNested 'nested.bin'), [byte[]]::new(3))
[IO.File]::WriteAllText((Join-Path $sizeNested 'artifact-lifecycle.json'), '{"schema":"nr-nexus-test-output/1","keepRawArtifacts":true,"ownerPid":0,"retainedPaths":[]}')
$sizeOutside = Join-Path $OutputDirectory 'size-outside'
[void][IO.Directory]::CreateDirectory($sizeOutside)
[IO.File]::WriteAllBytes((Join-Path $sizeOutside 'outside.bin'), [byte[]]::new(7))
$sizeLink = Join-Path $sizeTree 'linked'
try {
    New-Item -ItemType Junction -Path $sizeLink -Target $sizeOutside | Out-Null
    $size = Get-TestOutputSize $sizeTree
    Assert-ArtifactTest ($size.files -eq 2 -and $size.bytes -eq 8) 'Size scan count/bytes, marker exclusion or reparse exclusion'
} finally {
    if (Test-Path -LiteralPath $sizeLink) { (Get-Item -LiteralPath $sizeLink -Force).Delete() }
}
Remove-Item -LiteralPath (Join-Path $sizeNested 'nested.bin')
$size = Get-TestOutputSize $sizeTree
Assert-ArtifactTest ($size.files -eq 1 -and $size.bytes -eq 5) 'Size scan count/bytes after file removal'
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
        Assert-ArtifactTest (-not (Test-Path -LiteralPath $object)) "$mode/$outcome intermediate cleanup"
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
Assert-ArtifactTest (Test-Path -LiteralPath $object) 'Incomplete output remains until orphan maintenance'

$orphaned = Initialize-TestOutput ($OutputDirectory + '-orphaned')
$orphanObject = Join-Path $orphaned 'orphan.obj'
Register-TestDisposable -Directory $orphaned -Path $orphanObject -Kind 'raw-readback'
[IO.File]::WriteAllText($orphanObject, 'orphaned intermediate')
$orphanMarkerPath = Join-Path $orphaned 'artifact-lifecycle.json'
$orphanMarker = Get-Content -LiteralPath $orphanMarkerPath -Raw | ConvertFrom-Json
$orphanMarker.ownerPid = 2147483647
$orphanMarker | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $orphanMarkerPath -Encoding utf8
Invoke-TestOutputMaintenance -Root $orphaned
$orphanState = Get-Content -LiteralPath $orphanMarkerPath -Raw | ConvertFrom-Json
Assert-ArtifactTest ($orphanState.status -eq 'Failed' -and -not (Test-Path -LiteralPath $orphanObject)) 'Orphaned output was safely cleaned'

$active = Initialize-TestOutput (Join-Path $OutputDirectory 'active')
$activeObject = Join-Path $active 'active.obj'
Register-TestDisposable -Directory $active -Path $activeObject -Kind 'raw-readback'
[IO.File]::WriteAllText($activeObject, 'active intermediate')
Invoke-TestOutputMaintenance -Root $OutputDirectory
$activeState = Get-Content -LiteralPath (Join-Path $active 'artifact-lifecycle.json') -Raw | ConvertFrom-Json
Assert-ArtifactTest ($activeState.status -eq 'Running' -and (Test-Path -LiteralPath $activeObject)) 'Active output was protected'
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
Assert-ArtifactTest (-not (Test-Path -LiteralPath $timeoutObject)) 'Timed out intermediate was cleaned'
$timeoutState = Get-Content -LiteralPath (Join-Path $timedOut 'artifact-lifecycle.json') -Raw | ConvertFrom-Json
Assert-ArtifactTest ($timeoutState.status -eq 'Failed') 'Timeout did not mark incomplete run failed'
$keepRawBefore = $env:NR_NEXUS_TEST_KEEP_RAW
try {
    $env:NR_NEXUS_TEST_KEEP_RAW = '1'
    $kept = Initialize-TestOutput (Join-Path $OutputDirectory 'explicit-raw-retention') -RetentionMode Acceptance
    $keptRaw = Join-Path $kept 'raw.bin'
    Register-TestDisposable -Directory $kept -Path $keptRaw -Kind 'raw-readback'
    [IO.File]::WriteAllText($keptRaw, 'explicitly retained raw data')
    Complete-TestOutput $kept
    Assert-ArtifactTest (Test-Path -LiteralPath $keptRaw) 'Explicit raw retention was ignored'
    Assert-ArtifactTest ((Get-Content (Join-Path $kept 'artifact-lifecycle.json') -Raw | ConvertFrom-Json).keepRawArtifacts) 'Explicit raw retention was not recorded'
} finally { $env:NR_NEXUS_TEST_KEEP_RAW = $keepRawBefore }
$budgetRoot = Join-Path (Split-Path $OutputDirectory -Parent) ('budget-policy-' + [guid]::NewGuid().ToString('N'))
foreach ($mode in @('Standard','Full','Acceptance')) {
    $run = Initialize-TestOutput (Join-Path $budgetRoot $mode) -RetentionMode $mode
    $bulk = Join-Path $run 'unregistered.png'
    [IO.File]::WriteAllBytes($bulk, [byte[]]::new(1MB))
    [IO.File]::WriteAllText((Join-Path $run 'critical.log'), 'keep log')
    [IO.File]::WriteAllText((Join-Path $run 'repro.json'), '{"seed":7}')
    Complete-TestOutput $run
    $path = Join-Path $run 'artifact-lifecycle.json'
    $s = Get-Content $path -Raw | ConvertFrom-Json -AsHashtable
    $s.ownerPid = 2147483647
    $s.Remove('keepRawArtifacts')
    $s | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding utf8
}
$protectedRun = Initialize-TestOutput (Join-Path $budgetRoot 'active-parent')
$protectedBulk = Join-Path $protectedRun 'parent.png'
[IO.File]::WriteAllBytes($protectedBulk, [byte[]]::new(1024))
$activeChild = Initialize-TestOutput (Join-Path $protectedRun 'active-child') -RetentionMode Full
$activeBulk = Join-Path $activeChild 'raw.f32'
[IO.File]::WriteAllText($activeBulk, 'active')
Complete-TestOutput $protectedRun
$parentMarker = Join-Path $protectedRun 'artifact-lifecycle.json'
$s = Get-Content $parentMarker -Raw | ConvertFrom-Json -AsHashtable
$s.ownerPid = 2147483647
$s | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $parentMarker -Encoding utf8
Invoke-TestOutputMaintenance -Root $budgetRoot -CapacityBudgetBytes 16384
foreach ($mode in @('Standard','Full','Acceptance')) {
    $run = Join-Path $budgetRoot $mode
    Assert-ArtifactTest (-not (Test-Path -LiteralPath (Join-Path $run 'unregistered.png'))) "$mode budget did not reclaim unregistered bulk"
    Assert-ArtifactTest (Test-Path -LiteralPath (Join-Path $run 'critical.log')) "$mode budget deleted key log"
    Assert-ArtifactTest (Test-Path -LiteralPath (Join-Path $run 'repro.json')) "$mode budget deleted repro config"
}
Assert-ArtifactTest ((Test-Path -LiteralPath $protectedBulk) -and (Test-Path -LiteralPath $activeBulk)) 'Budget cleanup crossed an active child'
$child = Initialize-TestOutput (Join-Path $active 'finished-child')
$childRaw = Join-Path $child 'still-used.bin'
Register-TestDisposable -Directory $child -Path $childRaw -Kind 'raw-readback'
[IO.File]::WriteAllText($childRaw, 'parent still uses this output')
$childMarker = Join-Path $child 'artifact-lifecycle.json'
$s = Get-Content $childMarker -Raw | ConvertFrom-Json -AsHashtable
$s.ownerPid = 2147483647
$s | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $childMarker -Encoding utf8
Invoke-TestOutputMaintenance -Root $active -CapacityBudgetBytes 1
Assert-ArtifactTest (Test-Path -LiteralPath $childRaw) 'Maintenance crossed an active parent'
$workspace = Join-Path $OutputDirectory 'shared-budget'
foreach ($rootName in @('tmp','artifacts')) {
    $run = Initialize-TestOutput (Join-Path $workspace "$rootName/run")
    [IO.File]::WriteAllBytes((Join-Path $run 'bulk.png'), [byte[]]::new(65536))
    [void][IO.Directory]::CreateDirectory((Join-Path $run 'backup'))
    [IO.File]::WriteAllText((Join-Path $run 'backup/runtime.dll'), 'recovery')
    Complete-TestOutput $run
    $path = Join-Path $run 'artifact-lifecycle.json'
    $s = Get-Content $path -Raw | ConvertFrom-Json -AsHashtable
    $s.ownerPid = 2147483647
    $s | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding utf8
}
# The enclosing synthetic run is active; isolate only its workspace markers.
$workspaceMarker = Join-Path $OutputDirectory 'artifact-lifecycle.json'
$workspaceState = Get-Content $workspaceMarker -Raw
try {
    $s = $workspaceState | ConvertFrom-Json -AsHashtable
    $s.ownerPid = 2147483647
    $s | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $workspaceMarker -Encoding utf8
    Invoke-TestWorkspaceMaintenance -RepositoryRoot $workspace -CapacityBudgetBytes 100000
} finally { [IO.File]::WriteAllText($workspaceMarker, $workspaceState) }
Assert-ArtifactTest ((Get-TestOutputSize $workspace).bytes -le 100000) 'tmp/artifacts did not share a budget'
foreach ($capacity in @(0L, 100000L)) {
    $orphan = Join-Path $workspace 'artifacts/run'
    $orphanRaw = Join-Path $orphan 'orphan.bin'
    [IO.File]::WriteAllText($orphanRaw, 'orphaned')
    $path = Join-Path $orphan 'artifact-lifecycle.json'
    $s = Get-Content $path -Raw | ConvertFrom-Json -AsHashtable
    $s.status = 'Running'
    $s.disposables = @(@{path='orphan.bin';kind='raw-readback'})
    $s | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding utf8
    try {
        $s = $workspaceState | ConvertFrom-Json -AsHashtable
        $s.ownerPid = 2147483647
        $s | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $workspaceMarker -Encoding utf8
        Invoke-TestWorkspaceMaintenance -RepositoryRoot $workspace -CapacityBudgetBytes $capacity
    } finally { [IO.File]::WriteAllText($workspaceMarker, $workspaceState) }
    Assert-ArtifactTest (-not (Test-Path -LiteralPath $orphanRaw)) 'Orphan cleanup depended on exceeding the budget'
}
foreach ($rootName in @('tmp','artifacts')) {
    Assert-ArtifactTest (Test-Path -LiteralPath (Join-Path $workspace "$rootName/run/backup/runtime.dll")) 'Budget removed a recovery backup'
}
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
