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
if ($notesText -match '(?im)\b(TODO|TBD|PLACEHOLDER)\b|待补充|待填写|占位符') {
    throw 'Release notes contain placeholders.'
}
# Comments and code examples cannot supply document structure or reviewed prose.
$notesReviewed = [regex]::Replace($notesText, '(?s)<!--.*?-->', '')
$notesReviewed = [regex]::Replace($notesReviewed, '(?ms)^\s*(`{3,}|~{3,})[^\n]*\n.*?^\s*\1\s*$', '')
if ($notesReviewed -match '(?im)^\s*#{1,6}\s+(?:Source|源码|References|PR 与 Commit 记录|(?:Full )?Changelog|更新日志)(?:\s|/|$)' -or
    $notesReviewed -match '(?im)^\s*Source:\s|\b(?:Full )?Changelog\b') {
    throw 'Release notes contain reserved generated Source/References/Changelog content.'
}
$notesNavigation = '[简体中文](#简体中文) · [English](#english)'
if (($notesReviewed.Substring($notesTitle.Length).TrimStart() -split "`n", 2)[0] -cne $notesNavigation -or
    [regex]::Matches($notesReviewed, [regex]::Escape($notesNavigation)).Count -ne 1) {
    throw "Release notes require top navigation: $notesNavigation"
}
$notesLanguages = [regex]::Matches($notesReviewed, '(?m)^## ([^\n]+)$')
if ($notesLanguages.Count -ne 2 -or $notesLanguages[0].Groups[1].Value -cne '简体中文' -or
    $notesLanguages[1].Groups[1].Value -cne 'English') {
    throw 'Release notes require exactly one 简体中文 block followed by exactly one English block.'
}
$notesRequired = @(@('版本亮点', '验证情况', '已知限制', '安装说明'), @('Highlights', 'Validation', 'Known Limitations', 'Installation'))
for ($notesLanguageIndex = 0; $notesLanguageIndex -lt 2; $notesLanguageIndex++) {
    $notesStart = $notesLanguages[$notesLanguageIndex].Index + $notesLanguages[$notesLanguageIndex].Length
    $notesEnd = if ($notesLanguageIndex -eq 0) { $notesLanguages[1].Index } else { $notesReviewed.Length }
    $notesBlock = $notesReviewed.Substring($notesStart, $notesEnd - $notesStart)
    $notesSections = [regex]::Matches($notesBlock, '(?ms)^### ([^\n]+)(?:\n|\z)(.*?)(?=^### |\z)')
    foreach ($notesHeading in $notesRequired[$notesLanguageIndex]) {
        if (@($notesSections | Where-Object { $_.Groups[1].Value -ceq $notesHeading }).Count -ne 1) {
            throw "Release notes require exactly one $notesHeading section."
        }
    }
    $notesSeenHeadings = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($notesSection in $notesSections) {
        $notesHeading = $notesSection.Groups[1].Value
        if (-not $notesSeenHeadings.Add($notesHeading)) { throw "Duplicate release notes section: $notesHeading" }
        if ($notesHeading -cin $notesRequired[1 - $notesLanguageIndex]) {
            throw "Unexpected release notes section in language block: $notesHeading"
        }
        # Count only prose, excluding headings, reference definitions and links (including labels).
        $notesContent = [regex]::Replace($notesSection.Groups[2].Value, '(?m)^\s*#{1,6}\s+.*$|^\s*\[[^\]]+\]:.*$', '')
        $notesContent = [regex]::Replace($notesContent, '!?\[[^\]]*\](?:\([^)]*\)|\[[^\]]*\])?|https?://\S+', '')
        $notesMinimumLetters = if ($notesLanguageIndex -eq 0) { 4 } else { 20 }
        if ([regex]::Replace($notesContent, '[^\p{L}]', '').Length -lt $notesMinimumLetters) {
            throw "Release notes $notesHeading needs substantive reviewed text, not only links or headings."
        }
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
$notesBody += "`n`n## Source / 源码`n`nTag: [$Tag](https://github.com/$Repository/tree/$Tag)`n`nSource: https://github.com/$Repository/commit/$notesSource"
if ($notesPreviousTag -ne 'none') {
    $notesBody += "`n`n[Full Changelog](https://github.com/$Repository/compare/$notesPreviousTag...$Tag)"
} else {
    $notesBody += "`n`n[Full Changelog](https://github.com/$Repository/commits/$Tag)"
}
if ($notesPulls.Count -or $notesCommits.Count) {
    $notesBody += "`n`n## References / PR 与 Commit 记录"
    if ($notesPulls.Count) {
        $notesLinks = @($notesPulls | Sort-Object { [int]$_ } | ForEach-Object { "[#$_](https://github.com/$Repository/pull/$_)" })
        $notesBody += "`n`nPull requests: " + ($notesLinks -join ', ')
    }
    if ($notesCommits.Count) { $notesBody += "`n`nCommits:`n" + ($notesCommits -join "`n") }
}
[pscustomobject]@{ Title=$notesTitle.Groups[1].Value; Body=$notesBody + "`n"; Source=$notesSource; PreviousTag=$notesPreviousTag }
