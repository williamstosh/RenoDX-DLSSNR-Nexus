$ErrorActionPreference = 'Stop'
# Load only pure configuration functions, never the game entry.
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot '../tools/run-fg-acceptance.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw $parseErrors[0] }
foreach ($name in @('Set-FgPresentSection','Set-XeFgUiComposition','Get-ControlMaskSettings','Set-OptiFgDisabled')) {
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
foreach ($case in @('T0','T1','T2','T3','T4','T5','T6')) {
    $settings = Get-ControlMaskSettings $case
    if ($settings.NRHookPoint -ne 0 -or $settings.NRPasses -ne 1 -or
        $settings.NRFollowInputRes -ne 0 -or $settings.NRResolutionScale -ne 1 -or
        $settings.NRAutoMask -ne 0 -or $settings.NRControlMaskBlendMode -ne 0 -or $settings.NRScreenshotSource -ne 2 -or
        $settings.NRControlMaskEnabled -ne [int]($case -ne 'T0')) { throw "Unsafe P0 baseline: $case" }
    $expected = switch ($case) {
        T0 { @(0,1,1,1) }; T1 { @(1,1,1,1) }; T2 { @(0,0,1,1) }; T3 { @(0,0.5,1,1) }
        T4 { @(0,1,0,1) }; T5 { @(0,1,1,0) }; T6 { @(2,1,1,1) }
    }
    $actual = @($settings.NRControlMaskMode, $settings.NRControlMaskRed,
                $settings.NRControlMaskGreen, $settings.NRControlMaskBlue)
    for ($i = 0; $i -lt 4; ++$i) {
        if ([double]$actual[$i] -ne $expected[$i]) { throw "Incorrect P0 probe: $case" }
    }
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
Write-Host 'FG acceptance config and ControlMask T0-T6 passed; no game or installed files touched.'
