[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CandidateManifest,
    [Parameter(Mandatory)][string]$GameBinaryDirectory,
    [Parameter(Mandatory)][string]$Python,
    [string]$TargetPath = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/game-test/witcher3-steam.json'),
    [string]$OutputRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) 'artifacts/game-tests'),
    [ValidateSet('smoke','menu')][string]$Scenario = 'smoke',
    [ValidateRange(0,900)][int]$CaptureHoldSeconds = 90,
    [string]$CaptureScript,
    [switch]$GpuTimers,
    [ValidateRange(-1,3600)][int]$TelemetrySeconds = -1,
    [switch]$SdrGameOutput,
    [switch]$StartNrDisabled,
    [switch]$EnableStreamlineFallback,
    [switch]$SrOnly,
    [switch]$CleanEnvironment,
    [switch]$BlockOptionalHudless,
    [ValidateRange(-1,1)][int]$XeFGUiComposition = -1,
    [switch]$DisableXeFGSdkDetours,
    [switch]$ShowGameHud,
    [switch]$IsolateOverlayKey,
    [switch]$ModelDiagnosticCapture,
    [string]$NrRuntimePath,
    [ValidateRange(-1,3)][int]$NrPreset = -1,
    [ValidateRange(-1,2)][int]$NrPass2Preset = -1,
    [ValidateRange(0,4)][int]$NrPasses = 0,
    [ValidateRange(-1,2)][int]$NrHookPoint = -1,
    [ValidateRange(-1,3)][int]$NrPass2Stage = -1,
    [ValidateRange(-1,3)][int]$NrPass3Stage = -1,
    [ValidateRange(-1,3)][int]$NrPass4Stage = -1,
    [ValidateSet('T0','T1','T2','T3','T4','T5','T6','GRect','BRect','DepthFar','DepthFarOff')][string]$ControlMaskCase,
    [switch]$ControlMaskUseCoreContext,
    [ValidateRange(-1,1)][int]$ControlMaskAutoMask = -1,
    [ValidateRange(-1,3)][int]$ControlMaskBlendMode = -1,
    [ValidateRange(0,4096)][int]$ControlMaskGpuSamples = 0,
    [ValidateRange(1,300)][int]$ControlMaskGpuSeconds = 120,
    [ValidateRange(0,60)][int]$ControlMaskGpuWarmupSeconds = 6,
    [switch]$InstallOnly,
    [switch]$ManualImeSession,
    [switch]$RestoreInstallation,
    [switch]$AddonAbsentControl
)
$ErrorActionPreference = 'Stop'
if ($ModelDiagnosticCapture -and ($InstallOnly -or -not $RestoreInstallation)) {
    throw '-ModelDiagnosticCapture requires a restored test session, not InstallOnly.'
}
if ($CaptureScript) {
    if ($CaptureHoldSeconds -eq 0 -or $ManualImeSession -or $InstallOnly) { throw '-CaptureScript requires an automatic capture hold, not ManualImeSession or InstallOnly.' }
    $taskCapturePath = [IO.Path]::GetFullPath($CaptureScript)
    $taskRepository = Split-Path $PSScriptRoot -Parent
    if (-not (Test-Path -LiteralPath $taskCapturePath -PathType Leaf) -or [IO.Path]::GetExtension($taskCapturePath) -ne '.py' -or
        -not (@('tools','tmp') | Where-Object { $taskCapturePath.StartsWith((Join-Path $taskRepository $_) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) })) {
        throw 'Capture script must be a Python file below repository tools/ or tmp/.'
    }
    $CaptureScript = $taskCapturePath
    $PSBoundParameters['CaptureScript'] = $taskCapturePath
}
if (($PSBoundParameters.ContainsKey('ControlMaskAutoMask') -or $PSBoundParameters.ContainsKey('ControlMaskBlendMode')) -and -not $ControlMaskCase) {
    throw 'ControlMask parameter overrides require an explicit protected -ControlMaskCase.'
}
if ($ControlMaskUseCoreContext -and -not $ControlMaskCase) {
    throw '-ControlMaskUseCoreContext requires an explicit protected -ControlMaskCase.'
}
if ($ControlMaskGpuSamples -gt 0 -and (-not $ControlMaskCase -or -not $GpuTimers)) {
    throw 'ControlMask raw capture requires an explicit protected ControlMask case and existing -GpuTimers.'
}
if ($ControlMaskGpuSamples -gt 0 -and $ControlMaskGpuWarmupSeconds -ge $ControlMaskGpuSeconds) {
    throw 'ControlMask GPU warmup must be shorter than the bounded sampling window.'
}
if ($ControlMaskCase -in @('DepthFar','DepthFarOff') -and
    (-not $RestoreInstallation -or $InstallOnly -or $SrOnly -or -not $NrRuntimePath -or
        $NrPreset -notin @(1,2) -or $NrPasses -ne 1 -or $NrHookPoint -ne 2)) {
    throw 'Depth-far FG requires -RestoreInstallation, exact -NrRuntimePath, model A/B, one pass and Present hook point 2.'
}
if ($ControlMaskCase -and $ControlMaskCase -notin @('DepthFar','DepthFarOff') -and
    (-not $RestoreInstallation -or $InstallOnly -or -not $SrOnly -or
        -not $NrRuntimePath -or $NrPreset -notin @(1,2) -or $NrPasses -gt 1 -or $NrHookPoint -gt 0)) {
    throw 'ControlMask requires -RestoreInstallation, -SrOnly, exact -NrRuntimePath, explicit model A/B and single-pass Upscaled.'
}
if ($ControlMaskUseCoreContext -and
    ($ControlMaskCase -in @('DepthFar','DepthFarOff') -or -not $SrOnly -or
        -not $RestoreInstallation -or $InstallOnly -or $NrHookPoint -gt 0)) {
    throw '-ControlMaskUseCoreContext requires a restored, SR-only, single-pass Upscaled ControlMask case.'
}
if ($ManualImeSession -and ($Scenario -ne 'menu' -or $CaptureHoldSeconds -eq 0 -or $InstallOnly -or -not $RestoreInstallation)) {
    throw '-ManualImeSession requires menu, a nonzero hold and -RestoreInstallation; installation-only is not supported.'
}
if ($AddonAbsentControl -and $InstallOnly) { throw 'An absent-add-on control cannot be retained without a test' }
if ($DisableXeFGSdkDetours -and -not $StartNrDisabled) {
    throw '-DisableXeFGSdkDetours requires -StartNrDisabled for the startup-only isolation probe'
}
if ($XeFGUiComposition -ge 0 -and (-not $RestoreInstallation -or $InstallOnly)) {
    throw 'UI Composition tests require a protected session and -RestoreInstallation'
}
if (($TelemetrySeconds -ge 0 -or $SrOnly -or $NrRuntimePath -or $NrPreset -ge 0 -or $NrPass2Preset -ge 0 -or $NrPasses -gt 0 -or
        $EnableStreamlineFallback -or $NrHookPoint -ge 0 -or $NrPass2Stage -ge 0 -or $NrPass3Stage -ge 0 -or $NrPass4Stage -ge 0) -and
    (-not $RestoreInstallation -or $InstallOnly)) {
    throw 'Model tests require a protected session and -RestoreInstallation; -InstallOnly is not supported.'
}
function Get-FgAcceptanceRuntimePlan {
    param([string]$RuntimePath, [string]$InstalledPath)
    $requestedHash = (Get-FileHash -LiteralPath $RuntimePath -Algorithm SHA256).Hash
    $installedHash = (Get-FileHash -LiteralPath $InstalledPath -Algorithm SHA256).Hash
    return [ordered]@{
        requested_path=$RuntimePath; installed_path=$InstalledPath
        sha256=$requestedHash; original_sha256=$installedHash
        version=[string](Get-Item -LiteralPath $RuntimePath).VersionInfo.FileVersion
        original_version=[string](Get-Item -LiteralPath $InstalledPath).VersionInfo.FileVersion
        reused=($requestedHash -eq $installedHash)
    }
}
function Remove-FgAcceptanceBinaryBackups {
    param([Parameter(Mandatory)][string]$ReceiptPath, [switch]$CurrentRun, [switch]$LegacyProcessesStopped)
    # Load test-support.ps1 before calling this function independently. A legacy
    # receipt has no owner; its caller must also confirm the game has exited.
    $receiptPath = [IO.Path]::GetFullPath($ReceiptPath)
    $run = [IO.Path]::GetDirectoryName($receiptPath)
    # AST-extracted callers have no PSScriptRoot; the shared helper retains its
    # source location and therefore the actual repository boundary.
    $supportDirectory = Split-Path -Parent (Get-Command Assert-TestArtifactPath).ScriptBlock.File
    $repo = [IO.Path]::GetFullPath((Join-Path $supportDirectory '..'))
    if ([IO.Path]::GetFileName($receiptPath) -ne 'receipt.json' -or
        [IO.Path]::GetFileName($run) -notlike 'acceptance-*' -or
        -not (@('tmp','artifacts') | Where-Object {
            $run.StartsWith((Join-Path $repo $_) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
        })) { throw 'Backup cleanup requires a repository acceptance run receipt.' }
    Assert-TestArtifactPath $receiptPath
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json -AsHashtable
    $result = [ordered]@{status='protected'; deleted_files=0; deleted_bytes=[long]0}
    if ($env:NR_NEXUS_TEST_KEEP_RAW -eq '1' -or $receipt['keepRawArtifacts'] -or
        $receipt['restored'] -isnot [bool] -or -not $receipt['restored'] -or
        $receipt['restore_pending'] -or $receipt['candidate_retained']) { return $result }
    if (@(Get-Process -Name witcher3,CrashReporter -ErrorAction SilentlyContinue).Count) { return $result }
    $markerPath = Join-Path $run 'artifact-lifecycle.json'
    $state = $null
    if (Test-Path -LiteralPath $markerPath) {
        Assert-TestArtifactPath $markerPath
        $marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json -AsHashtable
        if ($marker.schema -ne 'nr-nexus-test-output/1' -or $marker['keepRawArtifacts']) { return $result }
        if ($CurrentRun) {
            $state = $global:NexusTestOutputs[$run]
            if (-not $state -or $state.ownerPid -ne $PID -or $marker.ownerPid -ne $PID -or
                $state.runId -ne $marker.runId -or $state.status -ne 'Running') {
                throw 'Backup cleanup ownership changed.'
            }
        } elseif (Test-TestOutputOwnerActive ([int]$marker.ownerPid)) { return $result }
    } elseif ($CurrentRun -or -not $LegacyProcessesStopped) { return $result }
    $backup = Join-Path $run 'backup'
    Assert-TestArtifactPath $backup
    if (-not (Test-Path -LiteralPath $backup -PathType Container)) { return $result }
    $items = @(Get-ChildItem -LiteralPath $backup -Force)
    foreach ($item in $items) { Assert-TestArtifactPath $item.FullName }
    $binaries = @($items | Where-Object { -not $_.PSIsContainer -and $_.Extension -in @('.dll','.addon','.addon64','.exe') })
    if ($CurrentRun) {
        foreach ($item in $binaries) {
            Register-TestDisposable -Directory $run -Path $item.FullName -Kind 'restored-game-binary-backup'
        }
        # Release only while disposing verified, redundant binary copies. Keep
        # the backup directory protected from later generic budget compaction.
        $retained = $state.retainedPaths
        try {
            $state.retainedPaths = @($retained | Where-Object { $_ -ne 'backup' })
            Remove-TestRegisteredDisposables $run $state
        } finally {
            $state.retainedPaths = $retained
            $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $markerPath -Encoding utf8
        }
    } else {
        foreach ($item in $binaries) {
            Remove-Item -LiteralPath $item.FullName -ErrorAction Stop
        }
    }
    foreach ($item in $binaries) {
        if (-not (Test-Path -LiteralPath $item.FullName)) {
            $result.deleted_files++; $result.deleted_bytes += $item.Length
        }
    }
    $result.status = if ($result.deleted_files -eq $binaries.Count) { 'cleaned' } else { 'incomplete' }
    return $result
}
function Get-ControlMaskSettings([ValidateSet('T0','T1','T2','T3','T4','T5','T6','GRect','BRect','DepthFar','DepthFarOff')][string]$Case) {
    $settings = @{
        NRHookPoint=0; NRPasses=1; NRFollowInputRes=0; NRResolutionScale=1; NRScreenshotSource=2
        NRAutoMask=0; NRControlMaskEnabled=[int]($Case -ne 'T0')
        NRControlMaskMode=0; NRControlMaskBlendMode=0
        NRControlMaskRed=1; NRControlMaskGreen=1; NRControlMaskBlue=1
        NRLocalTone=1; NRLocalStructure=1; NRStyle=2; NRIntensity=1
        NRSkinStructure='0.8'; NRSkinIndependent=1; NRUICorrection=0
    }
    switch ($Case) {
        T1 { $settings.NRControlMaskMode = 1 }
        T2 { $settings.NRControlMaskRed = 0 }
        T3 { $settings.NRControlMaskRed = '0.5' }
        T4 { $settings.NRControlMaskGreen = 0 }
        T5 { $settings.NRControlMaskBlue = 0 }
        T6 { $settings.NRControlMaskMode = 2 }
        GRect { $settings.NRControlMaskMode = 3 }
        BRect { $settings.NRControlMaskMode = 4 }
        DepthFar {
            $settings.NRHookPoint = 2
            $settings.NRControlMaskMode = 5
            $settings.NRControlMaskUseCoreContext = 0
            $settings.NRControlMaskDepthNear = '0.7'
            $settings.NRControlMaskDepthFar = '0.9'
        }
        DepthFarOff {
            $settings.NRHookPoint = 2
            $settings.NRControlMaskEnabled = 0
            $settings.NRControlMaskMode = 5
            $settings.NRControlMaskUseCoreContext = 0
            $settings.NRControlMaskDepthNear = '0.7'
            $settings.NRControlMaskDepthFar = '0.9'
        }
    }
    return $settings
}
function Set-ModelDiagnosticSection([string]$Text) {
    if ($Text -match '(?m)^NRToggleKey=121\r?$') {
        throw 'F10 is already the NR toggle; choose another profile before model diagnostics.'
    }
    foreach ($entry in @{NRScreenshotKey=121; NRScreenshotSource=2; NRDebugCapturePlanes=1}.GetEnumerator()) {
        $pattern = '(?m)^' + $entry.Key + '=[^\r\n]*'
        $line = $entry.Key + '=' + $entry.Value
        if ($Text -match $pattern) {
            $Text = [regex]::Replace($Text, $pattern, $line)
        } else {
            $Text = $Text.TrimEnd() + "`r`n$line`r`n"
        }
    }
    return $Text
}
function Set-OptiFgDisabled([string]$Text) {
    $sections = [regex]::Matches($Text, '(?ms)^\[FrameGen\]\r?\n(?:(?!^\[).)*')
    if ($sections.Count -ne 1) { throw 'Expected one OptiScaler FrameGen section' }
    $section = $sections[0]
    if ([regex]::Matches($section.Value, '(?m)^Enabled\s*=[^\r\n]*').Count -ne 1) {
        throw 'Expected one explicit OptiScaler FrameGen Enabled setting'
    }
    $updated = [regex]::Replace($section.Value, '(?m)^Enabled\s*=[^\r\n]*', 'Enabled = false')
    return $Text.Substring(0,$section.Index) + $updated + $Text.Substring($section.Index+$section.Length)
}
function Copy-ControlMaskGpuEvidence([string]$Directory, [string]$OutputDirectory, [string[]]$BeforeNames, [int]$Target) {
    $files = @(Get-ChildItem -LiteralPath $Directory -Filter 'control-mask-*.json' -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notin $BeforeNames })
    if ($files.Count -gt 32) { throw 'Unexpected number of new ControlMask GPU evidence files' }
    [void][IO.Directory]::CreateDirectory($OutputDirectory)
    $records = @()
    $complete = @()
    foreach ($file in $files) {
        if ($file.Length -gt 32MB) { throw 'ControlMask GPU evidence exceeds the bounded export size' }
        $copy = Join-Path $OutputDirectory $file.Name
        if (Test-Path -LiteralPath $copy) { throw 'Refusing to replace ControlMask GPU evidence' }
        Copy-Item -LiteralPath $file.FullName -Destination $copy -ErrorAction Stop
        $record = [ordered]@{file=$file.Name;sha256=(Get-FileHash -LiteralPath $copy).Hash;status='unreadable'}
        try {
            $json = Get-Content -LiteralPath $copy -Raw | ConvertFrom-Json
            $record.status = $json.status
            if ($json.status -eq 'terminal' -and $json.schema -eq 'control_mask_capture_v1') {
                $record.summary = $json.summary
                $validEvents = @($json.events | Where-Object { $_.status -eq 'valid' })
                if ($validEvents.Count -ne $json.summary.actual_valid -or
                    @($validEvents | Where-Object { $_.samples -ne 1 -or -not $_.context.ngx_accepted -or $null -eq $_.timing_us }).Count) {
                    throw 'GPU summary does not match the actual valid raw records'
                }
                if ($json.summary.reason -eq 'target_reached' -and $json.summary.actual_valid -ge $Target -and
                    $json.summary.dropped -eq 0 -and $json.summary.overflow -eq 0 -and $json.summary.pending_stop -eq 0) {
                    $complete += $file.Name
                }
            }
        } catch { $record.error = $_.Exception.Message }
        $records += $record
    }
    return [ordered]@{target_valid_samples=$Target;target_reached=($complete.Count -eq 1);files=$records
        measurement='Original per-pass GPU Evaluate interval includes mask generation; not independent mask cost.'}
}
function Set-FgPresentSection([string]$Text, [bool]$BlockHudless) {
    if ($Text -notmatch '(?m)^NRHookPoint=') { throw 'NRHookPoint is missing' }
    $Text = [regex]::Replace($Text, '(?m)^NRHookPoint=[^\r\n]*', 'NRHookPoint=2')
    # Explicit test choices must survive the production one-time migration.
    if ($Text -match '(?m)^NRPresentBlockHudlessConfigVersion=') {
        $Text = [regex]::Replace($Text, '(?m)^NRPresentBlockHudlessConfigVersion=[^\r\n]*', 'NRPresentBlockHudlessConfigVersion=1')
    } else {
        $Text = $Text.TrimEnd() + "`r`nNRPresentBlockHudlessConfigVersion=1`r`n"
    }
    $taskPolicy = 'NRPresentBlockHudless=' + [int]$BlockHudless
    if ($Text -match '(?m)^NRPresentBlockHudless=') {
        return [regex]::Replace($Text, '(?m)^NRPresentBlockHudless=[^\r\n]*', $taskPolicy)
    }
    return $Text.TrimEnd() + "`r`n$taskPolicy`r`n`r`n"
}
function Set-StreamlineFallback([string]$Text) {
    $pattern = '(?m)^EnableHooks=[^\r\n]*'
    $matches = [regex]::Matches($Text, $pattern)
    if ($matches.Count -gt 1) { throw 'Ambiguous EnableHooks setting' }
    if ($matches.Count -eq 1) { return [regex]::Replace($Text, $pattern, 'EnableHooks=1') }
    $lineEnding = if ($Text.Contains("`r`n")) { "`r`n" } else { "`n" }
    return $Text.TrimEnd() + $lineEnding + 'EnableHooks=1' + $lineEnding
}
function Set-XeFgUiComposition([string]$Text, [bool]$Enabled) {
    $sections = [regex]::Matches($Text, '(?ms)^\[XeFG\]\r?\n(?:(?!^\[).)*')
    if ($sections.Count -ne 1) { throw 'Expected one XeFG configuration section' }
    $section = $sections[0]
    if ([regex]::Matches($section.Value, '(?m)^UIComposition\s*=').Count -ne 1) {
        throw 'Expected one explicit XeFG UIComposition setting'
    }
    $value = ('UIComposition = ' + $Enabled.ToString().ToLowerInvariant())
    $updated = [regex]::Replace($section.Value, '(?m)^UIComposition\s*=[^\r\n]*', $value)
    return $Text.Substring(0,$section.Index) + $updated + $Text.Substring($section.Index+$section.Length)
}
function Set-XeFgDetourSetting([string]$Text, [bool]$Enabled) {
    $taskPolicy = 'NRXeFGSdkDetours=' + [int]$Enabled
    if ($Text -match '(?m)^NRXeFGSdkDetours=') {
        return [regex]::Replace($Text, '(?m)^NRXeFGSdkDetours=[^\r\n]*', $taskPolicy)
    }
    return $Text.TrimEnd() + [Environment]::NewLine + $taskPolicy + [Environment]::NewLine + [Environment]::NewLine
}
$taskRoot = Split-Path -Parent $PSScriptRoot
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (-not (@('tmp','artifacts') | Where-Object { $OutputRoot.StartsWith((Join-Path $taskRoot $_) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) })) {
    throw 'Acceptance output must remain below repository tmp/ or artifacts/.'
}
if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) { throw 'Provide -Python containing Pillow.' }
. (Join-Path $PSScriptRoot 'game-test/normal-context.ps1')
$taskContextExitCode = Invoke-NormalGameTestContext -EntryScript $PSCommandPath -Parameters $PSBoundParameters -Python $Python
if ($null -ne $taskContextExitCode) { exit $taskContextExitCode }
if (-not (Test-Path -LiteralPath $TargetPath -PathType Leaf)) { throw 'Missing local target configuration.' }
$taskTarget = Get-Content -LiteralPath $TargetPath -Raw | ConvertFrom-Json
$taskPerformanceTarget = [double]$taskTarget.performance_target_fps
if ($taskPerformanceTarget -le 0) { throw 'Configure performance_target_fps in the local target.' }
$taskGame = (Resolve-Path -LiteralPath $GameBinaryDirectory).Path
$taskGpuDirectory = Join-Path $taskGame 'NR Diagnostics'
$taskGpuBefore = @(Get-ChildItem -LiteralPath $taskGpuDirectory -Filter 'control-mask-*.json' -File -ErrorAction SilentlyContinue | ForEach-Object Name)
$taskManifestPath = (Resolve-Path -LiteralPath $CandidateManifest).Path
$taskManifest = Get-Content -LiteralPath $taskManifestPath -Raw | ConvertFrom-Json
$taskCandidate = $taskManifest.candidate_dll.copy
$taskDll = Join-Path $taskGame 'renodx-dlss5.addon64'
$taskConfig = Join-Path $taskGame 'ReShade.ini'
$taskGameConfig = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'The Witcher 3/dx12user.settings'
$taskBackupFiles = @($taskDll, $taskConfig)
$taskRuntimePlan = $null
if ($NrRuntimePath) {
    $taskRuntimeVersion = (Get-Item -LiteralPath $NrRuntimePath).VersionInfo
    if ($taskRuntimeVersion.FileMajorPart -ne 310 -or $taskRuntimeVersion.FileMinorPart -ne 9 -or
        $taskRuntimeVersion.FileBuildPart -ne 16 -or $taskRuntimeVersion.FilePrivatePart -ne 0) {
        throw 'The dual-model test requires the identified 310.9.16 runtime.'
    }
    $taskRuntimePlan = Get-FgAcceptanceRuntimePlan $NrRuntimePath (Join-Path $taskGame 'nvngx_dlssnr.dll')
    if (-not $taskRuntimePlan.reused) { $taskBackupFiles += Join-Path $taskGame 'nvngx_dlssnr.dll' }
}
if ($CleanEnvironment -or $XeFGUiComposition -ge 0 -or
    ($ControlMaskCase -and $ControlMaskCase -notin @('DepthFar','DepthFarOff'))) {
    $taskBackupFiles += Join-Path $taskGame 'OptiScaler.ini'
}
if ($CleanEnvironment) {
    $taskBackupFiles += Join-Path $taskGame 'ReShadePreset.ini'
}
if ($SdrGameOutput -or $ShowGameHud -or $SrOnly) {
    if (-not (Test-Path -LiteralPath $taskGameConfig -PathType Leaf)) { throw 'Missing game HDR settings file' }
    $taskBackupFiles += $taskGameConfig
}
if ($SdrGameOutput) {
    $taskHdrMatches = [regex]::Matches([IO.File]::ReadAllText($taskGameConfig), '(?m)^HdrEnabled=(true|false)\r?$')
    if ($taskHdrMatches.Count -ne 1) { throw 'Expected one explicit HdrEnabled game setting' }
}
if ($ShowGameHud) {
    $taskHudText = [IO.File]::ReadAllText($taskGameConfig)
    $taskHudMatches = [regex]::Matches($taskHudText, '(?m)^HudVisibility=[^\r\n]*')
    if ($taskHudMatches.Count -gt 1 -or
        ($taskHudMatches.Count -eq 1 -and $taskHudMatches[0].Value -notmatch '^HudVisibility=(true|false)$') -or
        [regex]::Matches($taskHudText, '(?m)^\[Hud\]\r?$').Count -gt 1) {
        throw 'Ambiguous or invalid HudVisibility game setting'
    }
}
foreach ($taskFile in @($taskBackupFiles) + @($taskCandidate.path)) {
    if (-not (Test-Path -LiteralPath $taskFile -PathType Leaf)) { throw "Missing acceptance input: $taskFile" }
}
if ((Get-FileHash -LiteralPath $taskCandidate.path).Hash -ne $taskCandidate.sha256) { throw 'Frozen candidate hash mismatch' }
$taskRunning = @(Get-Process -Name witcher3,CrashReporter -ErrorAction SilentlyContinue)
if ($taskRunning.Count) { throw 'Game or crash reporter is already running; installation was not changed' }
$taskRun = Join-Path $OutputRoot ('acceptance-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
. (Join-Path $PSScriptRoot '../tests/test-support.ps1')
Invoke-TestWorkspaceMaintenance
$taskRun = Initialize-TestOutput $taskRun
$taskOutputState = $global:NexusTestOutputs[$taskRun]
# This protection survives wrapper failure and orphan/budget maintenance.
$taskOutputState.retainedPaths = @('backup')
$taskOutputState | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskRun 'artifact-lifecycle.json') -Encoding utf8
$taskReceiptPath = Join-Path $taskRun 'receipt.json'
$taskOutcome = 'Failed'
try {
$taskBackup = Join-Path $taskRun 'backup'
New-Item -ItemType Directory -Path $taskBackup | Out-Null
$taskOriginal = @()
foreach ($taskFile in $taskBackupFiles) {
    $taskCopy = Join-Path $taskBackup (Split-Path -Leaf $taskFile)
    Copy-Item -LiteralPath $taskFile -Destination $taskCopy
    $taskHash = (Get-FileHash -LiteralPath $taskFile).Hash
    if ((Get-FileHash -LiteralPath $taskCopy).Hash -ne $taskHash) { throw 'Acceptance backup hash mismatch' }
    $taskOriginal += [ordered]@{path=$taskFile;backup=$taskCopy;sha256=$taskHash}
}
$taskReceipt = [ordered]@{
    started_utc=[DateTimeOffset]::UtcNow.ToString('o'); manifest=$taskManifestPath
    candidate_sha256=$taskCandidate.sha256; original_files=$taskOriginal
    installation_verified=$false; runtime_verified=$false; restored=$false
    test_exit_code=$null; visual_quality='not_tested'; performance_target_fps=$taskPerformanceTarget
    gpu_timers_requested=[bool]$GpuTimers
    requested_telemetry_seconds=$TelemetrySeconds
    nr_model_test=@{runtime=$NrRuntimePath; main=$NrPreset; pass2=$NrPass2Preset; passes=$NrPasses}
    nr_runtime_test=$taskRuntimePlan
    nr_stage_test=@{hook=$NrHookPoint; pass2=$NrPass2Stage; pass3=$NrPass3Stage; pass4=$NrPass4Stage}
    control_mask_case=$ControlMaskCase
    requested_control_mask_use_core_context=[bool]$ControlMaskUseCoreContext
    requested_control_mask_auto_mask=$ControlMaskAutoMask
    requested_control_mask_blend_mode=$ControlMaskBlendMode
    control_mask_gpu_capture=@{target_valid_samples=$ControlMaskGpuSamples; seconds=$ControlMaskGpuSeconds; warmup_seconds=$ControlMaskGpuWarmupSeconds}
    temporary_game_sdr=[bool]$SdrGameOutput
    start_nr_disabled=[bool]$StartNrDisabled
    requested_sr_only=[bool]$SrOnly
    clean_environment=[bool]$CleanEnvironment
    block_optional_hudless=[bool]$BlockOptionalHudless
    requested_xefg_ui_composition=$XeFGUiComposition
    disable_xefg_sdk_detours=[bool]$DisableXeFGSdkDetours
    temporary_game_hud_visible=[bool]$ShowGameHud
    temporary_overlay_key=$(if ($IsolateOverlayKey) { 'F7' } else { $null })
    model_diagnostic_capture=[bool]$ModelDiagnosticCapture
    enable_streamline_fallback_requested=[bool]$EnableStreamlineFallback
    install_only=[bool]$InstallOnly
    manual_ime_session=[bool]$ManualImeSession
    candidate_retained=$false; test_changes_restored=$false
    keepRawArtifacts=($env:NR_NEXUS_TEST_KEEP_RAW -eq '1')
    isolated_addons=@()
}
$taskReceiptPath = Join-Path $taskRun 'receipt.json'
$taskReceipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $taskReceiptPath -Encoding utf8
$taskChanged = $false
try {
    $taskChanged = $true
    if ($CleanEnvironment) {
        $taskOtherAddons = @(Get-ChildItem -LiteralPath $taskGame -File | Where-Object {
            $_.Extension -in @('.addon', '.addon64') -and $_.FullName -ne $taskDll
        })
        foreach ($taskAddon in $taskOtherAddons) {
            $taskResolved = (Resolve-Path -LiteralPath $taskAddon.FullName).Path
            if ([IO.Path]::GetDirectoryName($taskResolved) -ne $taskGame) { throw 'Add-on outside the game directory' }
            $taskIsolated = Join-Path $taskBackup $taskAddon.Name
            $taskAddonHash = (Get-FileHash -LiteralPath $taskResolved).Hash
            Copy-Item -LiteralPath $taskResolved -Destination $taskIsolated
            if ((Get-FileHash -LiteralPath $taskIsolated).Hash -ne $taskAddonHash) { throw 'Add-on backup mismatch' }
            $taskIsolation = [ordered]@{path=$taskResolved;backup=$taskIsolated;sha256=$taskAddonHash;isolated=$false}
            $taskReceipt.isolated_addons += $taskIsolation
            $taskReceipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $taskReceiptPath -Encoding utf8
            $taskDisabledPath = $taskResolved + '.fg-clean-disabled'
            if (Test-Path -LiteralPath $taskDisabledPath) { throw 'Existing isolated add-on target' }
            Move-Item -LiteralPath $taskResolved -Destination $taskDisabledPath
            $taskIsolation.isolated = $true
            $taskIsolation.disabled_path = $taskDisabledPath
            $taskReceipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $taskReceiptPath -Encoding utf8
        }
    }
    if ($AddonAbsentControl) {
        # A verified backup is already in original_files. Keep the control
        # absence explicit; finally restores the original project candidate.
        Remove-Item -LiteralPath $taskDll
        $taskReceipt.addon_absent_control = $true
    } else {
        Copy-Item -LiteralPath $taskCandidate.path -Destination $taskDll
        if ((Get-FileHash -LiteralPath $taskDll).Hash -ne $taskCandidate.sha256) { throw 'Installed candidate hash mismatch' }
    }
    if ($NrRuntimePath -and -not $taskRuntimePlan.reused) {
        Copy-Item -LiteralPath $NrRuntimePath -Destination (Join-Path $taskGame 'nvngx_dlssnr.dll')
        if ((Get-FileHash -LiteralPath $NrRuntimePath).Hash -ne
            (Get-FileHash -LiteralPath (Join-Path $taskGame 'nvngx_dlssnr.dll')).Hash) {
            throw 'Installed NR runtime hash mismatch'
        }
    }
    if ($ControlMaskCase -and $ControlMaskCase -notin @('DepthFar','DepthFarOff')) {
        $taskOptiPath = Join-Path $taskGame 'OptiScaler.ini'
        [IO.File]::WriteAllText($taskOptiPath,
            (Set-OptiFgDisabled ([IO.File]::ReadAllText($taskOptiPath))),
            [Text.UTF8Encoding]::new($false))
        $taskReceipt.test_optiscaler_fg_config_sha256 = (Get-FileHash -LiteralPath $taskOptiPath).Hash
    }
    if ($XeFGUiComposition -ge 0) {
        $taskOptiPath = Join-Path $taskGame 'OptiScaler.ini'
        [IO.File]::WriteAllText($taskOptiPath,
            (Set-XeFgUiComposition ([IO.File]::ReadAllText($taskOptiPath)) ($XeFGUiComposition -eq 1)),
            [Text.UTF8Encoding]::new($false))
        $taskReceipt.test_xefg_config_sha256 = (Get-FileHash -LiteralPath $taskOptiPath).Hash
    }
    $taskConfigText = [IO.File]::ReadAllText($taskConfig)
    if ($IsolateOverlayKey) {
        # Home also toggles the game's HUD in some profiles. Only the backed-up
        # ReShade input section is changed; the whole file is restored in finally.
        $taskInputSections = [regex]::Matches($taskConfigText, '(?ms)^\[INPUT\]\r?\n(?:(?!^\[).)*')
        if ($taskInputSections.Count -ne 1) { throw 'Expected one ReShade INPUT section' }
        $taskInput = $taskInputSections[0]
        if ([regex]::Matches($taskInput.Value, '(?m)^KeyOverlay=').Count -ne 1) { throw 'Expected one overlay shortcut' }
        $taskInputText = [regex]::Replace($taskInput.Value, '(?m)^KeyOverlay=[^\r\n]*', 'KeyOverlay=118,0,0,0')
        $taskConfigText = $taskConfigText.Substring(0,$taskInput.Index) + $taskInputText + $taskConfigText.Substring($taskInput.Index+$taskInput.Length)
    }
    $taskSection = [regex]::Match($taskConfigText, '(?ms)^\[RenoDX\.DLSS5\]\r?\n(?:(?!^\[).)*')
    if (-not $taskSection.Success) { throw 'RenoDX configuration section is missing' }
    $taskSectionText = $taskSection.Value
    $taskSectionText = Set-FgPresentSection $taskSectionText ([bool]$BlockOptionalHudless)
    $taskSectionText = Set-XeFgDetourSetting $taskSectionText (-not [bool]$DisableXeFGSdkDetours)
    if ($EnableStreamlineFallback) { $taskSectionText = Set-StreamlineFallback $taskSectionText }
    $taskModelSettings = @{}
    if ($TelemetrySeconds -ge 0) { $taskModelSettings.NRTelemetrySeconds = $TelemetrySeconds }
    if ($NrPreset -ge 0) { $taskModelSettings.NRPreset = $NrPreset }
    if ($NrPass2Preset -ge 0) { $taskModelSettings.NRPass2Preset = $NrPass2Preset }
    if ($NrPasses -gt 0) { $taskModelSettings.NRPasses = $NrPasses }
    if ($NrHookPoint -ge 0) { $taskModelSettings.NRHookPoint = $NrHookPoint }
    if ($NrPass2Stage -ge 0) { $taskModelSettings.NRPass2Stage = $NrPass2Stage }
    if ($NrPass3Stage -ge 0) { $taskModelSettings.NRPass3Stage = $NrPass3Stage }
    if ($NrPass4Stage -ge 0) { $taskModelSettings.NRPass4Stage = $NrPass4Stage }
    if ($ControlMaskCase) {
        foreach ($taskSetting in (Get-ControlMaskSettings $ControlMaskCase).GetEnumerator()) {
            $taskModelSettings[$taskSetting.Key] = $taskSetting.Value
        }
        if ($ControlMaskUseCoreContext) { $taskModelSettings.NRControlMaskUseCoreContext = 1 }
        if ($ControlMaskAutoMask -ge 0) { $taskModelSettings.NRAutoMask = $ControlMaskAutoMask }
        if ($ControlMaskBlendMode -ge 0) { $taskModelSettings.NRControlMaskBlendMode = $ControlMaskBlendMode }
        $taskModelSettings.NRControlMaskGpuCaptureSamples = $ControlMaskGpuSamples
        $taskModelSettings.NRControlMaskGpuCaptureSeconds = $ControlMaskGpuSeconds
        $taskModelSettings.NRControlMaskGpuCaptureWarmupSeconds = $ControlMaskGpuWarmupSeconds
    }
    foreach ($taskEntry in $taskModelSettings.GetEnumerator()) {
        $taskPattern = '(?m)^' + $taskEntry.Key + '=[^\r\n]*'
        $taskLine = $taskEntry.Key + '=' + $taskEntry.Value
        if ($taskSectionText -match $taskPattern) {
            $taskSectionText = [regex]::Replace($taskSectionText, $taskPattern, $taskLine)
        } else {
            $taskSectionText = $taskSectionText.TrimEnd() + "`r`n$taskLine`r`n"
        }
    }
    if ($CleanEnvironment) {
        foreach ($taskKey in @('NRDebugXeFgObserve','NRDebugXeFgService','NRDebugCapturePlanes','NRDebugXeFgReadbackGuides','NRGpuTimers','NRCostMeter')) {
            $taskSectionText = [regex]::Replace($taskSectionText, '(?m)^' + $taskKey + '=[^\r\n]*', $taskKey + '=0')
        }
        $taskPresetPath = Join-Path $taskGame 'ReShadePreset.ini'
        $taskPresetText = [IO.File]::ReadAllText($taskPresetPath)
        if ([regex]::Matches($taskPresetText, '(?m)^Techniques=').Count -ne 1) { throw 'Expected one ReShade effects list' }
        $taskPresetText = [regex]::Replace($taskPresetText, '(?m)^Techniques=[^\r\n]*', 'Techniques=')
        [IO.File]::WriteAllText($taskPresetPath, $taskPresetText, [Text.UTF8Encoding]::new($false))
        $taskReceipt.clean_config_hashes = @($taskPresetPath).ForEach({
            [ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash}
        })
        $taskReceipt.retained_overlays = @('NVIDIA game overlay (user requested)', 'OptiScaler FPS overlay (user requested)')
    }
    if ($ModelDiagnosticCapture) {
        # F5 also quicksaves in Witcher 3. This backed-up session uses F10
        # for the existing model diagnostic and enables its optional planes.
        $taskSectionText = Set-ModelDiagnosticSection $taskSectionText
    }
    if ($StartNrDisabled) {
        if ($taskSectionText -notmatch '(?m)^NeuralUplift=') { throw 'NeuralUplift is missing' }
        $taskSectionText = [regex]::Replace($taskSectionText, '(?m)^NeuralUplift=[^\r\n]*', 'NeuralUplift=0')
    }
    if ($GpuTimers) {
        if ($taskSectionText -notmatch '(?m)^NRGpuTimers=') { throw 'NRGpuTimers is missing' }
        $taskSectionText = [regex]::Replace($taskSectionText, '(?m)^NRGpuTimers=[^\r\n]*', 'NRGpuTimers=1')
    }
    $taskConfigText = $taskConfigText.Substring(0,$taskSection.Index) + $taskSectionText + $taskConfigText.Substring($taskSection.Index+$taskSection.Length)
    [IO.File]::WriteAllText($taskConfig, $taskConfigText, [Text.UTF8Encoding]::new($false))
    if ($SdrGameOutput) {
        $taskGameConfigText = [IO.File]::ReadAllText($taskGameConfig)
        $taskGameConfigText = [regex]::Replace($taskGameConfigText, '(?m)^HdrEnabled=(true|false)(?=\r?$)', 'HdrEnabled=false')
        [IO.File]::WriteAllText($taskGameConfig, $taskGameConfigText, [Text.UTF8Encoding]::new($false))
        $taskReceipt.test_game_config_sha256 = (Get-FileHash -LiteralPath $taskGameConfig).Hash
    }
    if ($ShowGameHud) {
        $taskGameConfigText = [IO.File]::ReadAllText($taskGameConfig)
        if ($taskHudMatches.Count -eq 1) {
            $taskGameConfigText = [regex]::Replace($taskGameConfigText, '(?m)^HudVisibility=(true|false)(?=\r?$)', 'HudVisibility=true')
        } elseif ($taskGameConfigText -match '(?m)^\[Hud\]\r?$') {
            $taskGameConfigText = [regex]::Replace($taskGameConfigText, '(?m)^\[Hud\]\r?$', "[Hud]`r`nHudVisibility=true")
        } else {
            # The game may omit default-valued HUD settings on save. The
            # temporary section belongs to the same whole-file restore guard.
            $taskGameConfigText = $taskGameConfigText.TrimEnd() + "`r`n[Hud]`r`nHudVisibility=true`r`n"
        }
        [IO.File]::WriteAllText($taskGameConfig, $taskGameConfigText, [Text.UTF8Encoding]::new($false))
        $taskReceipt.test_game_config_sha256 = (Get-FileHash -LiteralPath $taskGameConfig).Hash
    }
    if ($SrOnly) {
        $taskGameConfigText = [IO.File]::ReadAllText($taskGameConfig)
        foreach ($taskMode in @{EnableDLSSRR='false'; DLSSGMode='0'; XessFrameGeneration='false'; FSRFramegen='0'}.GetEnumerator()) {
            $taskModePattern = '(?m)^' + $taskMode.Key + '=[^\r\n]*'
            if ([regex]::Matches($taskGameConfigText, $taskModePattern).Count -ne 1) {
                throw "Expected one game mode setting: $($taskMode.Key)"
            }
            $taskGameConfigText = [regex]::Replace($taskGameConfigText, $taskModePattern,
                $taskMode.Key + '=' + $taskMode.Value)
        }
        [IO.File]::WriteAllText($taskGameConfig, $taskGameConfigText, [Text.UTF8Encoding]::new($false))
        $taskReceipt.test_game_config_sha256 = (Get-FileHash -LiteralPath $taskGameConfig).Hash
    }
    $taskReceipt.installation_verified = $true
    $taskReceipt.test_config_sha256 = (Get-FileHash -LiteralPath $taskConfig).Hash
    $taskReceipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $taskReceiptPath -Encoding utf8
    Write-Output "ACCEPTANCE_RECEIPT=$taskReceiptPath"
    if ($InstallOnly) {
        $taskReceipt.test_exit_code = 0
    } else {
        & (Join-Path $PSScriptRoot 'run-game-test.ps1') -Python $Python -TargetPath $TargetPath -OutputRoot $OutputRoot -Scenario $Scenario -CaptureHoldSeconds $CaptureHoldSeconds -CaptureScript $CaptureScript -Retries 0 -AddonAbsentControl:$AddonAbsentControl -ManualImeSession:$ManualImeSession
        $taskReceipt.test_exit_code = $LASTEXITCODE
    }
} finally {
    if (-not $InstallOnly) {
        foreach ($taskLog in @('ReShade.log','OptiScaler.log','RenoDX-DLSS5-crash.log')) {
            $taskPath = Join-Path $taskGame $taskLog
            if (Test-Path -LiteralPath $taskPath -PathType Leaf) { Copy-Item -LiteralPath $taskPath -Destination (Join-Path $taskRun $taskLog) }
        }
    }
    $taskRunning = @(Get-Process -Name witcher3,CrashReporter -ErrorAction SilentlyContinue)
    if ($taskChanged -and $taskRunning.Count -eq 0) {
        foreach ($taskFile in $taskOriginal) {
            Copy-Item -LiteralPath $taskFile.backup -Destination $taskFile.path
            if ((Get-FileHash -LiteralPath $taskFile.path).Hash -ne $taskFile.sha256) { throw 'Acceptance restoration hash mismatch' }
        }
        foreach ($taskAddon in $taskReceipt.isolated_addons) {
            if (-not $taskAddon.isolated) { continue }
            if (Test-Path -LiteralPath $taskAddon.path) { throw 'Refusing to overwrite add-on created during the test' }
            if ((Get-FileHash -LiteralPath $taskAddon.disabled_path).Hash -ne $taskAddon.sha256) { throw 'Isolated add-on changed; preserve backups' }
            Move-Item -LiteralPath $taskAddon.disabled_path -Destination $taskAddon.path
            if ((Get-FileHash -LiteralPath $taskAddon.path).Hash -ne $taskAddon.sha256) { throw 'Add-on restoration mismatch' }
        }
        $taskReceipt.restored = $true
        $taskReceipt.test_changes_restored = $true
        # The user wants each new project build left installed. Restore all
        # temporary test settings first, then retain only the DLL and its
        # selected Present input policy. External references still restore.
        if ($taskReceipt.installation_verified -and -not $RestoreInstallation -and -not $AddonAbsentControl -and $null -ne $taskManifest.addon_repository) {
            if ((Get-FileHash -LiteralPath $taskCandidate.path).Hash -ne $taskCandidate.sha256) { throw 'Frozen candidate changed before retention' }
            Copy-Item -LiteralPath $taskCandidate.path -Destination $taskDll
            if ((Get-FileHash -LiteralPath $taskDll).Hash -ne $taskCandidate.sha256) { throw 'Retained candidate hash mismatch' }
            $taskRetainedText = [IO.File]::ReadAllText($taskConfig)
            $taskRetainedSections = [regex]::Matches($taskRetainedText, '(?ms)^\[RenoDX\.DLSS5\]\r?\n(?:(?!^\[).)*')
            if ($taskRetainedSections.Count -ne 1) { throw 'Expected one RenoDX section after restoration' }
            $taskRetainedSection = $taskRetainedSections[0]
            $taskRetainedBody = Set-FgPresentSection $taskRetainedSection.Value ([bool]$BlockOptionalHudless)
            $taskRetainedBody = Set-XeFgDetourSetting $taskRetainedBody (-not [bool]$DisableXeFGSdkDetours)
            $taskRetainedText = $taskRetainedText.Substring(0,$taskRetainedSection.Index) + $taskRetainedBody + $taskRetainedText.Substring($taskRetainedSection.Index+$taskRetainedSection.Length)
            [IO.File]::WriteAllText($taskConfig, $taskRetainedText, [Text.UTF8Encoding]::new($false))
            $taskReceipt.retained_files = @($taskDll,$taskConfig).ForEach({[ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash}})
            $taskReceipt.candidate_retained = $true
            $taskReceipt.restored = $false
        }
    } elseif ($taskChanged) {
        $taskReceipt.restore_pending = 'Game/crash reporter remains alive; preserve backup and restore after safe exit'
    }
    if ($ControlMaskGpuSamples -gt 0 -and $taskRunning.Count -eq 0) {
        try {
            $taskReceipt.control_mask_gpu_result = Copy-ControlMaskGpuEvidence $taskGpuDirectory (Join-Path $taskRun 'raw-gpu') $taskGpuBefore $ControlMaskGpuSamples
            if (-not $taskReceipt.control_mask_gpu_result.target_reached) { $taskReceipt.test_exit_code = 1 }
        } catch {
            $taskReceipt.control_mask_gpu_result = @{target_reached=$false;error=$_.Exception.Message}
            $taskReceipt.test_exit_code = 1
        }
    }
    $taskReceipt.finished_utc = [DateTimeOffset]::UtcNow.ToString('o')
    $taskReceipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $taskReceiptPath -Encoding utf8
    Write-Output "ACCEPTANCE_RECEIPT=$taskReceiptPath"
}
if ($taskReceipt.test_exit_code -eq 0 -and ($taskReceipt.restored -or $taskReceipt.candidate_retained)) { $taskOutcome = 'Passed' }
} finally {
    try {
        if (Test-Path -LiteralPath $taskReceiptPath -PathType Leaf) {
            $taskCleanup = Remove-FgAcceptanceBinaryBackups -ReceiptPath $taskReceiptPath -CurrentRun
            Write-Output ("ACCEPTANCE_BACKUP_CLEANUP=" + ($taskCleanup | ConvertTo-Json -Compress))
        }
    } catch {
        Write-Warning "Acceptance backup cleanup stopped: $($_.Exception.Message)"
    }
    Complete-TestOutput -Directory $taskRun -Outcome $taskOutcome
    Invoke-TestWorkspaceMaintenance
}
if ($taskReceipt.test_exit_code -ne 0) { exit 1 }
