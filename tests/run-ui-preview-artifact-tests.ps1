$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'test-support.ps1')
$snapshotRoot = Join-Path $repositoryRoot ('tmp/preview-lifecycle-' + [guid]::NewGuid().ToString('N'))
$runId = [guid]::NewGuid().ToString('N')
$RetentionMode = 'Standard'
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $repositoryRoot 'tools/preview-ui.ps1'),[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count) { throw $parseErrors[0] }
foreach ($name in @('Save-PreviewOwnership','Remove-OwnedPreview')) {
    $function = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name },$true)
    . ([scriptblock]::Create($function.Extent.Text))
}
$checks = 0
foreach ($case in @('standard','changed','extra','acceptance','running')) {
    $directory = Join-Path $snapshotRoot $case
    [void][IO.Directory]::CreateDirectory($directory)
    Set-Content -LiteralPath (Join-Path $directory 'image.webp') -Value 'synthetic image bytes'
    $RetentionMode = $(if ($case -eq 'acceptance') { 'Acceptance' } else { 'Standard' })
    Save-PreviewOwnership $directory
    if ($case -eq 'changed') { Add-Content -LiteralPath (Join-Path $directory 'image.webp') -Value 'modified by another owner' }
    if ($case -eq 'extra') { Set-Content -LiteralPath (Join-Path $directory 'keep.txt') -Value 'unregistered evidence' }
    if ($case -eq 'running') {
        $receipt = Get-Content -LiteralPath (Join-Path $directory 'snapshot-ownership.json') -Raw | ConvertFrom-Json
        $receipt.status = 'Running'
        $receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $directory 'snapshot-ownership.json')
    }
    $rejected = $false
    try { Remove-OwnedPreview $directory } catch { $rejected = $true }
    if ($case -eq 'standard') {
        if ($rejected -or (Test-Path -LiteralPath $directory)) { throw 'Completed owned Standard output was not cleaned.' }
    } elseif (-not $rejected -or -not (Test-Path -LiteralPath (Join-Path $directory 'image.webp'))) {
        throw "Protected/modified output was not preserved: $case"
    }
    $checks++
}
# Intentionally retain rejection fixtures as evidence; never relax their receipts.
Write-Host "Preview lifecycle checks=$checks passed; protected fixtures: $snapshotRoot"
