[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Tag,
    [string]$Repository = $env:GITHUB_REPOSITORY,
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$SourceCommit = 'HEAD',
    [string]$NotesPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'A GitHub owner/repository is required.' }
if ($Tag -notmatch '^v[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$') { throw 'Invalid release tag.' }
$notesRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
if (-not $NotesPath) { $NotesPath = Join-Path $notesRoot "docs/releases/$Tag.md" }
if (-not (Test-Path -LiteralPath $NotesPath -PathType Leaf)) {
    throw "Release notes missing: $NotesPath. Add reviewed highlights, validation, limitations and installation before publishing."
}
$notesText = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $NotesPath).Path).Replace("`r`n", "`n").Trim()
$notesTitle = [regex]::Match($notesText, '\A# (NR Nexus ' + [regex]::Escape($Tag) + '(?: — .+)?)\n')
if (-not $notesTitle.Success) { throw "Release notes title must start with '# NR Nexus $Tag'." }
if ($notesText -match '(?im)\b(TODO|TBD|PLACEHOLDER)\b' -or $notesText -match '(?m)^## (Source|References)\s*$') {
    throw 'Release notes contain placeholders or reserved generated Source/References sections.'
}
foreach ($notesHeading in @('Highlights', 'Validation', 'Known Limitations', 'Installation')) {
    $notesSection = [regex]::Matches($notesText, '(?ms)^## ' + [regex]::Escape($notesHeading) + '\n(.*?)(?=^## |\z)')
    if ($notesSection.Count -ne 1) { throw "Release notes require exactly one $notesHeading section." }
    # Link lists alone do not describe a feature, fix, verification result or limit.
    $notesContent = [regex]::Replace($notesSection[0].Groups[1].Value, '\[[^\]]*\]\([^)]*\)|https?://\S+|<!--.*?-->', '', 'Singleline')
    if ([regex]::Replace($notesContent, '[^\p{L}]', '').Length -lt 20) {
        throw "Release notes $notesHeading needs substantive reviewed text, not only links or headings."
    }
}
$notesPrevious = [regex]::Matches($notesText, '(?m)^<!-- Previous: (none|v[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?) -->$')
if ($notesPrevious.Count -ne 1) { throw 'Release notes require one <!-- Previous: v<version> --> (or none for the first release).' }
$notesPreviousTag = $notesPrevious[0].Groups[1].Value
$notesSource = (& git -C $notesRoot rev-parse --verify "$SourceCommit^{commit}").Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve release source.' }
$notesRange = $notesSource
if ($notesPreviousTag -ne 'none') {
    $notesPreviousCommit = & git -C $notesRoot rev-parse --verify "refs/tags/$notesPreviousTag^{commit}"
    if ($LASTEXITCODE -ne 0) { throw 'Previous release tag is missing; fetch tags before generating notes.' }
    & git -C $notesRoot merge-base --is-ancestor $notesPreviousCommit $notesSource
    if ($LASTEXITCODE -ne 0 -or $notesPreviousTag -ceq $Tag) { throw 'Previous tag must precede the release source.' }
    $notesRange = "$notesPreviousCommit..$notesSource"
}
$notesLog = @(& git -C $notesRoot log --reverse '--format=%H%x09%s' $notesRange)
if ($LASTEXITCODE -ne 0) { throw 'Cannot read release commits.' }
$notesPulls = [Collections.Generic.HashSet[string]]::new()
$notesCommits = [Collections.Generic.List[string]]::new()
foreach ($notesLine in $notesLog) {
    $notesParts = $notesLine -split "`t", 2
    if ($notesParts.Count -ne 2) { throw 'Unexpected release commit format.' }
    $notesPull = [regex]::Match($notesParts[1], '(?:Merge pull request #([0-9]+)|\(#([0-9]+)\)$)')
    if ($notesPull.Success) {
        $notesNumber = $notesPull.Groups[1].Value + $notesPull.Groups[2].Value
        [void]$notesPulls.Add($notesNumber)
    } else {
        $notesSubject = [regex]::Replace($notesParts[1], '([\\`*\[\]<>])', '\$1')
        $notesCommits.Add("- [$($notesParts[0].Substring(0,8))](https://github.com/$Repository/commit/$($notesParts[0])) $notesSubject")
    }
}
$notesBody = [regex]::Replace($notesText.Substring($notesTitle.Length), '(?m)^<!-- Previous: .*? -->\n?', '').Trim()
$notesBody += "`n`n## Source`n`nTag: [$Tag](https://github.com/$Repository/tree/$Tag)`n`nSource: https://github.com/$Repository/commit/$notesSource"
if ($notesPreviousTag -ne 'none') {
    $notesBody += "`n`n[Full Changelog](https://github.com/$Repository/compare/$notesPreviousTag...$Tag)"
} else {
    $notesBody += "`n`n[Full Changelog](https://github.com/$Repository/commits/$Tag)"
}
if ($notesPulls.Count -or $notesCommits.Count) {
    $notesBody += "`n`n## References"
    if ($notesPulls.Count) {
        $notesLinks = @($notesPulls | Sort-Object { [int]$_ } | ForEach-Object { "[#$_](https://github.com/$Repository/pull/$_)" })
        $notesBody += "`n`nPull requests: " + ($notesLinks -join ', ')
    }
    if ($notesCommits.Count) { $notesBody += "`n`nCommits:`n" + ($notesCommits -join "`n") }
}
[pscustomobject]@{ Title=$notesTitle.Groups[1].Value; Body=$notesBody + "`n"; Source=$notesSource; PreviousTag=$notesPreviousTag }
