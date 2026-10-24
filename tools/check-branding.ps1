[CmdletBinding()]
param(
    [string[]]$Path = @(),
    [switch]$All
)

$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if ($All -and $Path.Count) { throw 'Use either -All or -Path.' }
if ($All) {
    $taskFiles = @(& git -C $taskRoot ls-files --cached --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot list branding inputs.' }
} elseif ($Path.Count) {
    $taskFiles = $Path
} else {
    # Daily use checks only staged/unstaged changes and new source files.
    $taskFiles = @(& git -C $taskRoot diff --name-only HEAD --diff-filter=ACMR)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot list changed files.' }
    $taskFiles += @(& git -C $taskRoot ls-files --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot list new branding inputs.' }
}
$taskFiles = @($taskFiles | ForEach-Object {
    [IO.Path]::GetRelativePath($taskRoot, [IO.Path]::GetFullPath($_, $taskRoot)).Replace('\', '/')
})
$taskFiles = @($taskFiles | Sort-Object -Unique | Where-Object {
    $_ -match '\.(cpp|hpp|h|rc|ps1|py|json|cmake)$' -and
    $_ -notmatch '^(tests/|tools/game-test/test_|host/)' -and
    $_ -ne 'tools/check-branding.ps1'
})
# Inspect literals, not comments/history. Preserve strings before stripping comments.
$taskCppTokens = '(?s)//[^\r\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"'
$taskScriptTokens = '(?m)#[^\r\n]*|"(?:\\.|`.|[^"\\])*"|''(?:''''|[^''])*'''
$taskOldBrand = '(?i)\b(?:RenoDX[ _-]*DLSS[ _-]*5(?:[ _-]*Generic)?|DLSS[ _-]*5[ _-]*(?:Generic|Neural[ _-]+Rendering))\b'
# Old algorithm labels (for example Slew v6.1.2) and dependency paths are technical history.
$taskOldVersion = '(?i)\b(?:v?8\.5\.0(?:-[0-9A-Za-z.-]+)?|v[0-9]+\.[0-9]+\.[0-9]+-(?:rc|dev|alpha|beta)[0-9A-Za-z.-]*)\b'
# Existing loader, trace/report paths, schema and HTTP agent are compatibility contracts.
$taskCompatibility = '(?i)renodx-dlss5(?=\.addon(?:32|64)\b|-crash\b|-debug\b|-edittrace\b|-normtrace\b|-report\b|\.workset\b|/|-$)'
$taskErrors = @()
foreach ($taskFile in $taskFiles) {
    $taskFullPath = Join-Path $taskRoot $taskFile
    if (-not (Test-Path -LiteralPath $taskFullPath -PathType Leaf)) {
        if ($Path.Count) { throw "Branding input does not exist: $taskFile" }
        continue
    }
    $taskText = Get-Content -LiteralPath $taskFullPath -Raw
    $taskTokens = if ($taskFile -match '\.(cpp|hpp|h|rc)$') { $taskCppTokens } else { $taskScriptTokens }
    foreach ($taskToken in [regex]::Matches($taskText, $taskTokens)) {
        if ($taskToken.Value[0] -notin @('"', "'")) { continue }
        $taskLiteral = $taskToken.Value.Substring(1, $taskToken.Length - 2)
        $taskLiteral = [regex]::Replace($taskLiteral, $taskCompatibility, '')
        if ($taskLiteral -match $taskOldBrand -or $taskLiteral -match $taskOldVersion) {
            $taskLine = 1 + [regex]::Matches($taskText.Substring(0, $taskToken.Index), '\n').Count
            $taskErrors += "${taskFile}:${taskLine}: legacy display literal"
        }
        if ($taskFile -ne 'brand.h' -and $taskLiteral -cmatch 'NR Nexus|DLSS Neural Rendering Add-on for RenoDX') {
            $taskLine = 1 + [regex]::Matches($taskText.Substring(0, $taskToken.Index), '\n').Count
            # Build status messages are prose; compiled UI/log/resource text shares brand.h.
            if ($taskFile -match '\.(cpp|hpp|h|rc)$') {
                $taskErrors += "${taskFile}:${taskLine}: use brand.h for shared display text"
            }
        }
    }
}
if ($taskErrors.Count) { throw ($taskErrors -join "`n") }
Write-Host "Branding check passed ($($taskFiles.Count) source files; comments/history and compatibility identifiers excluded)."
