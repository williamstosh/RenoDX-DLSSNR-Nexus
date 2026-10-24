[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CandidateManifest,
    [Parameter(Mandatory)][string]$GameBinaryDirectory,
    [Parameter(Mandatory)][string]$Python,
    [string]$TargetPath = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/game-test/witcher3-steam.json'),
    [string]$OutputRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) 'artifacts/game-tests'),
    [ValidateSet('smoke','menu')][string]$Scenario = 'smoke',
    [ValidateRange(0,120)][int]$CaptureHoldSeconds = 90,
    [switch]$GpuTimers,
    [switch]$SdrGameOutput,
    [switch]$StartNrDisabled,
    [switch]$CleanEnvironment,
    [switch]$BlockOptionalHudless,
    [switch]$ShowGameHud,
    [switch]$IsolateOverlayKey,
    [string]$NrRuntimePath,
    [ValidateRange(-1,3)][int]$NrPreset = -1,
    [ValidateRange(-1,2)][int]$NrPass2Preset = -1,
    [ValidateRange(0,4)][int]$NrPasses = 0,
    [switch]$InstallOnly,
    [switch]$RestoreInstallation,
    [switch]$AddonAbsentControl
)
$ErrorActionPreference = 'Stop'
if ($AddonAbsentControl -and $InstallOnly) { throw 'An absent-add-on control cannot be retained without a test' }
if (($NrRuntimePath -or $NrPreset -ge 0 -or $NrPass2Preset -ge 0 -or $NrPasses -gt 0) -and
    (-not $RestoreInstallation -or $InstallOnly)) {
    throw 'Model tests require a protected session and -RestoreInstallation; -InstallOnly is not supported.'
}
function Set-FgPresentSection([string]$Text, [bool]$BlockHudless) {
    if ($Text -notmatch '(?m)^NRHookPoint=') { throw 'NRHookPoint is missing' }
    $Text = [regex]::Replace($Text, '(?m)^NRHookPoint=[^\r\n]*', 'NRHookPoint=2')
    $taskPolicy = 'NRPresentBlockHudless=' + [int]$BlockHudless
    if ($Text -match '(?m)^NRPresentBlockHudless=') {
        return [regex]::Replace($Text, '(?m)^NRPresentBlockHudless=[^\r\n]*', $taskPolicy)
    }
    return $Text.TrimEnd() + "`r`n$taskPolicy`r`n`r`n"
}
$taskRoot = Split-Path -Parent $PSScriptRoot
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (-not (@('tmp','artifacts') | Where-Object { $OutputRoot.StartsWith((Join-Path $taskRoot $_) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) })) {
    throw 'Acceptance output must remain below repository tmp/ or artifacts/.'
}
if (-not (Test-Path -LiteralPath $Python -PathType Leaf)) { throw 'Provide -Python containing Pillow.' }
if (-not (Test-Path -LiteralPath $TargetPath -PathType Leaf)) { throw 'Missing local target configuration.' }
$taskTarget = Get-Content -LiteralPath $TargetPath -Raw | ConvertFrom-Json
$taskPerformanceTarget = [double]$taskTarget.performance_target_fps
if ($taskPerformanceTarget -le 0) { throw 'Configure performance_target_fps in the local target.' }
$taskGame = (Resolve-Path -LiteralPath $GameBinaryDirectory).Path
$taskManifestPath = (Resolve-Path -LiteralPath $CandidateManifest).Path
$taskManifest = Get-Content -LiteralPath $taskManifestPath -Raw | ConvertFrom-Json
$taskCandidate = $taskManifest.candidate_dll.copy
$taskDll = Join-Path $taskGame 'renodx-dlss5.addon64'
$taskConfig = Join-Path $taskGame 'ReShade.ini'
$taskGameConfig = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'The Witcher 3/dx12user.settings'
$taskBackupFiles = @($taskDll, $taskConfig)
if ($NrRuntimePath) {
    $taskRuntimeVersion = (Get-Item -LiteralPath $NrRuntimePath).VersionInfo
    if ($taskRuntimeVersion.FileMajorPart -ne 310 -or $taskRuntimeVersion.FileMinorPart -ne 9 -or
        $taskRuntimeVersion.FileBuildPart -ne 16 -or $taskRuntimeVersion.FilePrivatePart -ne 0) {
        throw 'The dual-model test requires the identified 310.9.16 runtime.'
    }
    $taskBackupFiles += Join-Path $taskGame 'nvngx_dlssnr.dll'
}
if ($CleanEnvironment) {
    $taskBackupFiles += Join-Path $taskGame 'OptiScaler.ini'
    $taskBackupFiles += Join-Path $taskGame 'ReShadePreset.ini'
}
if ($SdrGameOutput -or $ShowGameHud) {
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
New-Item -ItemType Directory -Path $taskRun | Out-Null
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
    nr_model_test=@{runtime=$NrRuntimePath; main=$NrPreset; pass2=$NrPass2Preset; passes=$NrPasses}
    temporary_game_sdr=[bool]$SdrGameOutput
    start_nr_disabled=[bool]$StartNrDisabled
    clean_environment=[bool]$CleanEnvironment
    block_optional_hudless=[bool]$BlockOptionalHudless
    temporary_game_hud_visible=[bool]$ShowGameHud
    temporary_overlay_key=$(if ($IsolateOverlayKey) { 'F7' } else { $null })
    install_only=[bool]$InstallOnly
    candidate_retained=$false; test_changes_restored=$false
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
    if ($NrRuntimePath) {
        Copy-Item -LiteralPath $NrRuntimePath -Destination (Join-Path $taskGame 'nvngx_dlssnr.dll')
        if ((Get-FileHash -LiteralPath $NrRuntimePath).Hash -ne
            (Get-FileHash -LiteralPath (Join-Path $taskGame 'nvngx_dlssnr.dll')).Hash) {
            throw 'Installed NR runtime hash mismatch'
        }
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
    $taskModelSettings = @{}
    if ($NrPreset -ge 0) { $taskModelSettings.NRPreset = $NrPreset }
    if ($NrPass2Preset -ge 0) { $taskModelSettings.NRPass2Preset = $NrPass2Preset }
    if ($NrPasses -gt 0) { $taskModelSettings.NRPasses = $NrPasses }
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
    $taskReceipt.installation_verified = $true
    $taskReceipt.test_config_sha256 = (Get-FileHash -LiteralPath $taskConfig).Hash
    $taskReceipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $taskReceiptPath -Encoding utf8
    Write-Output "ACCEPTANCE_RECEIPT=$taskReceiptPath"
    if ($InstallOnly) {
        $taskReceipt.test_exit_code = 0
    } else {
        & (Join-Path $PSScriptRoot 'run-game-test.ps1') -Python $Python -TargetPath $TargetPath -OutputRoot $OutputRoot -Scenario $Scenario -CaptureHoldSeconds $CaptureHoldSeconds -Retries 0 -AddonAbsentControl:$AddonAbsentControl
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
            $taskRetainedText = $taskRetainedText.Substring(0,$taskRetainedSection.Index) + $taskRetainedBody + $taskRetainedText.Substring($taskRetainedSection.Index+$taskRetainedSection.Length)
            [IO.File]::WriteAllText($taskConfig, $taskRetainedText, [Text.UTF8Encoding]::new($false))
            $taskReceipt.retained_files = @($taskDll,$taskConfig).ForEach({[ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash}})
            $taskReceipt.candidate_retained = $true
            $taskReceipt.restored = $false
        }
    } elseif ($taskChanged) {
        $taskReceipt.restore_pending = 'Game/crash reporter remains alive; preserve backup and restore after safe exit'
    }
    $taskReceipt.finished_utc = [DateTimeOffset]::UtcNow.ToString('o')
    $taskReceipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $taskReceiptPath -Encoding utf8
    Write-Output "ACCEPTANCE_RECEIPT=$taskReceiptPath"
}
if ($taskReceipt.test_exit_code -ne 0) { exit 1 }
