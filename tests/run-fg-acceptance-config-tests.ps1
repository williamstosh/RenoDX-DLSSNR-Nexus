$ErrorActionPreference = 'Stop'
# Load only the two pure configuration transforms, never the game entry.
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot '../tools/run-fg-acceptance.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw $parseErrors[0] }
foreach ($name in @('Set-FgPresentSection','Set-XeFgUiComposition')) {
    $definition = $ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name}, $true)
    if ($null -eq $definition) { throw "Missing production transform: $name" }
    . ([scriptblock]::Create($definition.Extent.Text))
}
$fixture = "[Other]`nUIComposition = true`n[XeFG]`nUIComposition = true`nInterpolationCount = 3`n[After]`nUIComposition = true`n"
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
Write-Host 'FG acceptance config: scoped composition override, rejection and explicit policy migration passed; no game or installed files touched.'
