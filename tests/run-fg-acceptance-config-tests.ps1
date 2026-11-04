$ErrorActionPreference = 'Stop'
# Load only pure configuration functions, never the game entry.
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot '../tools/run-fg-acceptance.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw $parseErrors[0] }
foreach ($name in @('Set-FgPresentSection','Set-XeFgUiComposition','Get-ControlMaskSettings','Set-OptiFgDisabled','Copy-ControlMaskGpuEvidence')) {
    $definition = $ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name}, $true)
    if ($null -eq $definition) { throw "Missing production transform: $name" }
    . ([scriptblock]::Create($definition.Extent.Text))
}
$fixture = "[Other]`nUIComposition = true`n[XeFG]`nUIComposition = true`nInterpolationCount = 3`n[After]`nUIComposition = true`n"
$fgFixture = "[Other]`nEnabled = true`n[FrameGen]`nEnabled = true`nFGOutput = XeFG`n[After]`nEnabled = true`n"
if ((Set-OptiFgDisabled $fgFixture) -cne $fgFixture.Replace("[FrameGen]`nEnabled = true", "[FrameGen]`nEnabled = false")) {
    throw 'P0 FG disable changed unrelated settings'
}
foreach ($invalid in @('[Other]', "[FrameGen]`n", "[FrameGen]`nEnabled=true`nEnabled=false", "[FrameGen]`nEnabled=true`n[FrameGen]`nEnabled=true")) {
    $rejected = $false
    try { Set-OptiFgDisabled $invalid | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw 'Ambiguous OptiScaler FrameGen configuration accepted' }
}
$disabled = Set-XeFgUiComposition $fixture $false
if ($disabled -cne $fixture.Replace("[XeFG]`nUIComposition = true", "[XeFG]`nUIComposition = false")) {
    throw 'XeFG override changed unrelated sections or settings'
}
if ((Set-XeFgUiComposition $disabled $true) -cne $fixture) { throw 'XeFG override cannot round-trip' }
foreach ($invalid in @('[Other]', "[XeFG]`n", "[XeFG]`nUIComposition=true`n[XeFG]`nUIComposition=false", "[XeFG]`nUIComposition=true`nUIComposition=false")) {
    $rejected = $false
    try { Set-XeFgUiComposition $invalid $false | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw 'Ambiguous configuration accepted' }
}
foreach ($initialMarker in @('', "NRPresentBlockHudlessConfigVersion=0`n")) {
    foreach ($single in @($true,$false)) {
        $section = Set-FgPresentSection ("[RenoDX.DLSS5]`nNRHookPoint=0`nNRPresentBlockHudless=1`n" + $initialMarker) $single
        if ($section -notmatch '(?m)^NRPresentBlockHudlessConfigVersion=1\r?$' -or
            $section -notmatch ('(?m)^NRPresentBlockHudless=' + [int]$single + '\r?$') -or
            $section -notmatch '(?m)^NRHookPoint=2\r?$') { throw 'Explicit test policy lost to migration' }
    }
}
foreach ($case in @('T0','T1','T2','T3','T4','T5','T6','GRect','BRect')) {
    $settings = Get-ControlMaskSettings $case
    if ($settings.NRHookPoint -ne 0 -or $settings.NRPasses -ne 1 -or
        $settings.NRFollowInputRes -ne 0 -or $settings.NRResolutionScale -ne 1 -or
        $settings.NRAutoMask -ne 0 -or $settings.NRControlMaskBlendMode -ne 0 -or $settings.NRScreenshotSource -ne 2 -or
        $settings.NRControlMaskEnabled -ne [int]($case -ne 'T0') -or
        $settings.NRLocalTone -ne 1 -or $settings.NRLocalStructure -ne 1 -or
        $settings.NRStyle -ne 2 -or $settings.NRIntensity -ne 1 -or
        [double]$settings.NRSkinStructure -ne 0.8 -or $settings.NRSkinIndependent -ne 1 -or
        $settings.NRUICorrection -ne 0) { throw "Unsafe P0 baseline: $case" }
    $expected = switch ($case) {
        T0 { @(0,1,1,1) }; T1 { @(1,1,1,1) }; T2 { @(0,0,1,1) }; T3 { @(0,0.5,1,1) }
        T4 { @(0,1,0,1) }; T5 { @(0,1,1,0) }; T6 { @(2,1,1,1) }
        GRect { @(3,1,1,1) }; BRect { @(4,1,1,1) }
    }
    $actual = @($settings.NRControlMaskMode, $settings.NRControlMaskRed,
                $settings.NRControlMaskGreen, $settings.NRControlMaskBlue)
    for ($i = 0; $i -lt 4; ++$i) {
        if ([double]$actual[$i] -ne $expected[$i]) { throw "Incorrect P0 probe: $case" }
    }
}
$captureGuard = $ast.Find({param($node)
    $node -is [Management.Automation.Language.IfStatementAst] -and
    $node.Extent.Text.StartsWith('if ($ControlMaskGpuSamples -gt')
}, $true)
if ($null -eq $captureGuard) { throw 'Missing raw capture guard' }
$validateCapture = [scriptblock]::Create("param(`$ControlMaskGpuSamples,`$ControlMaskCase,`$GpuTimers)`n" + $captureGuard.Extent.Text)
& $validateCapture 1000 'T0' $true
& $validateCapture 0 '' $false
foreach ($probe in @(@(1000,'',$true), @(1000,'GRect',$false))) {
    $rejected = $false
    try { & $validateCapture @probe } catch { $rejected = $true }
    if (-not $rejected) { throw 'Raw capture accepted without the protected case or existing timers' }
}
$controlMaskGuard = $ast.Find({param($node)
    $node -is [Management.Automation.Language.IfStatementAst] -and
    $node.Extent.Text.StartsWith('if ($ControlMaskCase -and')
}, $true)
if ($null -eq $controlMaskGuard) { throw 'Missing protected ControlMask entry guard' }
$validateControlMask = [scriptblock]::Create(@'
param($RestoreInstallation, $InstallOnly, $SrOnly, $NrRuntimePath, $NrPreset, $NrPasses, $NrHookPoint)
$ControlMaskCase = 'T0'
'@ + "`n" + $controlMaskGuard.Extent.Text)
& $validateControlMask $true $false $true 'synthetic-runtime.dll' 2 1 0
foreach ($invalid in @(
    @($false,$false,$true,'synthetic-runtime.dll',2,1,0),
    @($true,$true,$true,'synthetic-runtime.dll',2,1,0),
    @($true,$false,$false,'synthetic-runtime.dll',2,1,0),
    @($true,$false,$true,'',2,1,0),
    @($true,$false,$true,'synthetic-runtime.dll',0,1,0),
    @($true,$false,$true,'synthetic-runtime.dll',2,2,0),
    @($true,$false,$true,'synthetic-runtime.dll',2,1,2))) {
    $rejected = $false
    try { & $validateControlMask @invalid } catch { $rejected = $true }
    if (-not $rejected) { throw 'Unprotected ControlMask entry accepted' }
}
$captureFixture = Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/controlmask-capture-config-' + [guid]::NewGuid().ToString('N'))
$captureSource = Join-Path $captureFixture 'source'
[void][IO.Directory]::CreateDirectory($captureSource)
'old evidence' | Set-Content -LiteralPath (Join-Path $captureSource 'control-mask-old.json') -Encoding utf8
'{"schema":"control_mask_capture_v1","status":"incomplete"}' | Set-Content -LiteralPath (Join-Path $captureSource 'control-mask-new.incomplete.json') -Encoding utf8
$terminal = @{schema='control_mask_capture_v1';status='terminal';summary=@{
    reason='target_reached';actual_valid=3;dropped=0;overflow=0;pending_stop=0};events=@(1..3 | ForEach-Object {
        @{status='valid';samples=1;context=@{ngx_accepted=$true};timing_us=@{eval=4000}}
    })}
$terminal | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $captureSource 'control-mask-new.json') -Encoding utf8
$copied = Copy-ControlMaskGpuEvidence $captureSource (Join-Path $captureFixture 'complete') @('control-mask-old.json') 3
if (-not $copied.target_reached -or $copied.files.Count -ne 2 -or (Test-Path (Join-Path $captureFixture 'complete/control-mask-old.json'))) {
    throw 'Raw capture did not preserve the new-only evidence boundary'
}
$terminal.events = @($terminal.events[0])
$terminal | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $captureSource 'control-mask-new.json') -Encoding utf8
$inconsistent = Copy-ControlMaskGpuEvidence $captureSource (Join-Path $captureFixture 'inconsistent') @('control-mask-old.json') 3
if ($inconsistent.target_reached) { throw 'A summary with missing raw samples was accepted' }
$rejected = $false
try { Copy-ControlMaskGpuEvidence $captureSource (Join-Path $captureFixture 'complete') @('control-mask-old.json') 3 | Out-Null } catch { $rejected = $true }
if (-not $rejected) { throw 'Existing evidence was overwritten' }
Write-Host "FG acceptance config, T0-T6/GRect/BRect and raw-capture guards/export passed; synthetic evidence: $captureFixture"
