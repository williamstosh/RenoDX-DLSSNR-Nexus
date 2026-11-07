$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-support.ps1')
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot '../tools/run-fg-acceptance.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw $parseErrors[0] }
foreach ($name in @('Remove-FgAcceptanceBinaryBackups','Get-FgAcceptanceRuntimePlan')) {
    $definition = $ast.Find({param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    if (-not $definition) { throw "Missing acceptance helper: $name" }
    . ([scriptblock]::Create($definition.Extent.Text))
}
$backupDecision = $ast.Find({param($node)
    $node -is [Management.Automation.Language.IfStatementAst] -and
        $node.Extent.Text.StartsWith('if (-not $taskRuntimePlan.reused)')
}, $true)
$runtimeInstall = $ast.Find({param($node)
    $node -is [Management.Automation.Language.IfStatementAst] -and
        $node.Extent.Text.StartsWith('if ($NrRuntimePath -and -not $taskRuntimePlan.reused)')
}, $true)
if (-not $backupDecision -or -not $runtimeInstall) { throw 'Missing runtime reuse wiring' }
$selectRuntimeBackup = [scriptblock]::Create($backupDecision.Extent.Text)
$installRuntime = [scriptblock]::Create($runtimeInstall.Extent.Text)
$outer = $ast.Find({param($node)
    $node -is [Management.Automation.Language.TryStatementAst] -and
        $node.Finally -and $node.Finally.Extent.Text -match 'ACCEPTANCE_BACKUP_CLEANUP='
}, $true)
if (-not $outer) { throw 'Missing unconditional wrapper finalizer' }
$finalize = [scriptblock]::Create(($outer.Finally.Statements.Extent.Text -join "`n"))
$root = Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/tests/game-backup-' + [guid]::NewGuid().ToString('N'))
$savedRaw = $env:NR_NEXUS_TEST_KEEP_RAW
$savedBudget = $env:NR_NEXUS_TEST_BUDGET_BYTES
$checks = 0
$script:gameActive = $false
# Workspace sweeping has its own lifecycle tests; isolate wrapper fixtures.
function Invoke-TestWorkspaceMaintenance {}
# Synthetic observations only; no game or desktop operations.
function Get-Process {
    param([string[]]$Name, $ErrorAction)
    if ($script:gameActive) { [pscustomobject]@{Name='CrashReporter';Id=12345} }
}
function Assert-BackupTest([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function New-BackupFixture([string]$Name, [switch]$Legacy) {
    $directory = Join-Path $root ('acceptance-' + $Name)
    if ($Legacy) { [void][IO.Directory]::CreateDirectory($directory) }
    else {
        $directory = Initialize-TestOutput $directory
        $state = $global:NexusTestOutputs[$directory]
        $state.retainedPaths = @('backup')
        $state | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $directory 'artifact-lifecycle.json')
    }
    [void][IO.Directory]::CreateDirectory((Join-Path $directory 'backup'))
    foreach ($name in @('nvngx_dlssnr.dll','original.addon','original.addon64','helper.exe','ReShade.ini','hash.sha256')) {
        [IO.File]::WriteAllText((Join-Path $directory "backup/$name"), 'synthetic ' + $name)
    }
    $receipt = @{restored=$true;candidate_retained=$false;test_exit_code=1;original_files=@(@{sha256='evidence'})}
    $receipt | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $directory 'receipt.json')
    return $directory
}
function Save-FixtureReceipt([string]$Directory, [hashtable]$Overrides) {
    $receipt = Get-Content (Join-Path $Directory 'receipt.json') -Raw | ConvertFrom-Json -AsHashtable
    foreach ($key in $Overrides.Keys) { $receipt[$key] = $Overrides[$key] }
    $receipt | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $Directory 'receipt.json')
}
try {
    $env:NR_NEXUS_TEST_KEEP_RAW = '0'
    $env:NR_NEXUS_TEST_BUDGET_BYTES = '0'
    $taskGame = Join-Path $root 'runtime-fixture'
    [void][IO.Directory]::CreateDirectory($taskGame)
    $installedRuntime = Join-Path $taskGame 'nvngx_dlssnr.dll'
    $requestedRuntime = Join-Path $taskGame 'requested.dll'
    [IO.File]::WriteAllText($installedRuntime, 'synthetic runtime')
    [IO.File]::WriteAllText($requestedRuntime, 'synthetic runtime')
    foreach ($NrRuntimePath in @($installedRuntime,$requestedRuntime)) {
        $taskRuntimePlan = Get-FgAcceptanceRuntimePlan $NrRuntimePath $installedRuntime
        Assert-BackupTest $taskRuntimePlan.reused 'Identical runtime bytes were not reused'
        $taskBackupFiles = @('other-input.ini')
        . $selectRuntimeBackup
        Assert-BackupTest ($installedRuntime -notin $taskBackupFiles) 'Reused runtime was selected for backup'
        $timestamp = [DateTime]::UtcNow.AddDays(-2)
        [IO.File]::SetLastWriteTimeUtc($installedRuntime, $timestamp)
        . $installRuntime
        Assert-BackupTest ([IO.File]::GetLastWriteTimeUtc($installedRuntime) -eq $timestamp) 'Reused runtime was overwritten'
        $identity = $taskRuntimePlan | ConvertTo-Json | ConvertFrom-Json
        Assert-BackupTest ($identity.sha256 -eq (Get-FileHash $NrRuntimePath).Hash -and
            $identity.original_sha256 -eq $identity.sha256 -and
            $identity.requested_path -eq $NrRuntimePath -and $identity.installed_path -eq $installedRuntime -and
            $identity.version -ceq [string](Get-Item $NrRuntimePath).VersionInfo.FileVersion -and
            $identity.original_version -ceq [string](Get-Item $installedRuntime).VersionInfo.FileVersion) 'Runtime receipt identity lost hash/path/version'
    }
    $NrRuntimePath = $requestedRuntime
    [IO.File]::WriteAllText($requestedRuntime, 'different synthetic runtime')
    $taskRuntimePlan = Get-FgAcceptanceRuntimePlan $NrRuntimePath $installedRuntime
    Assert-BackupTest (-not $taskRuntimePlan.reused -and $taskRuntimePlan.sha256 -ne $taskRuntimePlan.original_sha256) 'Different runtime bytes were reused'
    $taskBackupFiles = @('other-input.ini')
    . $selectRuntimeBackup
    Assert-BackupTest ($installedRuntime -in $taskBackupFiles) 'Changed runtime was not selected for restoration backup'
    . $installRuntime
    Assert-BackupTest ((Get-FileHash $installedRuntime).Hash -eq $taskRuntimePlan.sha256) 'Changed runtime was not installed'

    $taskRun = New-BackupFixture 'failed-test-restored'
    $taskReceiptPath = Join-Path $taskRun 'receipt.json'
    $receiptHash = (Get-FileHash $taskReceiptPath).Hash
    $taskOutcome = 'Failed'
    & $finalize | Out-Null
    $marker = Get-Content (Join-Path $taskRun 'artifact-lifecycle.json') -Raw | ConvertFrom-Json
    Assert-BackupTest ($marker.status -eq 'Failed' -and $marker.cleanup.Count -eq 4) 'Failed test did not clean restored backups through wrapper finalizer'
    Assert-BackupTest (-not (Test-Path (Join-Path $taskRun 'backup/nvngx_dlssnr.dll'))) 'Restored runtime survived'
    Assert-BackupTest ((Get-FileHash $taskReceiptPath).Hash -eq $receiptHash) 'Receipt or hashes changed'
    foreach ($name in @('ReShade.ini','hash.sha256')) {
        Assert-BackupTest (Test-Path (Join-Path $taskRun "backup/$name")) 'Configuration/hash evidence removed'
    }
    $again = Remove-FgAcceptanceBinaryBackups $taskReceiptPath
    Assert-BackupTest ($again.status -eq 'protected') 'History call ignored active owner'
    $taskRun = New-BackupFixture 'passed-test'
    $taskReceiptPath = Join-Path $taskRun 'receipt.json'
    $taskOutcome = 'Passed'
    & $finalize | Out-Null
    $marker = Get-Content (Join-Path $taskRun 'artifact-lifecycle.json') -Raw | ConvertFrom-Json
    Assert-BackupTest ($marker.status -eq 'Passed' -and $marker.cleanup.Count -eq 4) 'Passing run did not complete lifecycle cleanup'
    $taskOutcome = 'Failed'
    $taskRun = New-BackupFixture 'missing-receipt'
    $taskReceiptPath = Join-Path $taskRun 'receipt.json'
    Remove-Item -LiteralPath $taskReceiptPath
    & $finalize | Out-Null
    Assert-BackupTest (Test-Path (Join-Path $taskRun 'backup/nvngx_dlssnr.dll')) 'Missing receipt lost recovery backup'

    foreach ($case in @('restore-failed','restore-pending','candidate-retained','raw-env','raw-marker','raw-receipt','active-game')) {
        if ($case -eq 'raw-marker') { $env:NR_NEXUS_TEST_KEEP_RAW = '1' }
        $taskRun = New-BackupFixture $case
        $env:NR_NEXUS_TEST_KEEP_RAW = '0'
        switch ($case) {
            'restore-failed' { Save-FixtureReceipt $taskRun @{restored=$false} }
            'restore-pending' { Save-FixtureReceipt $taskRun @{restore_pending='exit required'} }
            'candidate-retained' { Save-FixtureReceipt $taskRun @{restored=$false;candidate_retained=$true} }
            'raw-env' { $env:NR_NEXUS_TEST_KEEP_RAW = '1' }
            'raw-receipt' { Save-FixtureReceipt $taskRun @{keepRawArtifacts=$true} }
            'active-game' { $script:gameActive = $true }
        }
        $taskReceiptPath = Join-Path $taskRun 'receipt.json'
        & $finalize | Out-Null
        Assert-BackupTest (Test-Path (Join-Path $taskRun 'backup/nvngx_dlssnr.dll')) "Protected backup removed: $case"
        $script:gameActive = $false
        $env:NR_NEXUS_TEST_KEEP_RAW = '0'
        $markerPath = Join-Path $taskRun 'artifact-lifecycle.json'
        $marker = Get-Content $markerPath -Raw | ConvertFrom-Json -AsHashtable
        $marker.ownerPid = 0
        if ($case -eq 'restore-failed') { $marker.status = 'Running' }
        $marker | ConvertTo-Json -Depth 8 | Set-Content $markerPath
        Invoke-TestOutputMaintenance -Root $root -CapacityBudgetBytes 1
        Assert-BackupTest (Test-Path (Join-Path $taskRun 'backup/nvngx_dlssnr.dll')) "Maintenance removed protected backup: $case"
    }

    $legacy = New-BackupFixture 'legacy' -Legacy
    $receiptPath = Join-Path $legacy 'receipt.json'
    $result = Remove-FgAcceptanceBinaryBackups $receiptPath
    Assert-BackupTest ($result.status -eq 'protected') 'Ownerless history accepted without explicit confirmation'
    $script:gameActive = $true
    $result = Remove-FgAcceptanceBinaryBackups $receiptPath -LegacyProcessesStopped
    Assert-BackupTest ($result.status -eq 'protected') 'History call ignored live crash reporter'
    $script:gameActive = $false
    $result = Remove-FgAcceptanceBinaryBackups $receiptPath -LegacyProcessesStopped
    Assert-BackupTest ($result.deleted_files -eq 4 -and $result.deleted_bytes -gt 0) 'Confirmed synthetic history did not clean binaries'
    $result = Remove-FgAcceptanceBinaryBackups $receiptPath -LegacyProcessesStopped
    Assert-BackupTest ($result.deleted_files -eq 0 -and $result.status -eq 'cleaned') 'Cleanup is not repeatable'
    foreach ($path in @((Join-Path $root 'receipt.json'), (Join-Path $PSScriptRoot 'acceptance-outside/receipt.json'))) {
        $rejected = $false
        try { Remove-FgAcceptanceBinaryBackups $path -LegacyProcessesStopped | Out-Null } catch { $rejected = $true }
        Assert-BackupTest $rejected 'Cleanup accepted a path outside its run boundary'
    }
    $linked = New-BackupFixture 'reparse' -Legacy
    $link = Join-Path $linked 'backup/linked'
    New-Item -ItemType Junction -Path $link -Target (Join-Path $legacy 'backup') | Out-Null
    try {
        $rejected = $false
        try { Remove-FgAcceptanceBinaryBackups (Join-Path $linked 'receipt.json') -LegacyProcessesStopped | Out-Null } catch { $rejected = $true }
        Assert-BackupTest $rejected 'Reparse point inside backup accepted'
        Assert-BackupTest (Test-Path (Join-Path $linked 'backup/nvngx_dlssnr.dll')) 'Deleted files before boundary validation'
    } finally { Remove-Item -LiteralPath $link }
    Write-Output "PASS: $checks backup lifecycle checks (synthetic only)"
} finally {
    $env:NR_NEXUS_TEST_KEEP_RAW = $savedRaw
    $env:NR_NEXUS_TEST_BUDGET_BYTES = $savedBudget
    Remove-Item Function:Get-Process
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse }
}
