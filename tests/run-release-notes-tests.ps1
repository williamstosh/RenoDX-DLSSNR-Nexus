[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$notesTestRoot = Split-Path $PSScriptRoot -Parent
$notesTestTool = Join-Path $notesTestRoot 'tools/get-release-notes.ps1'
$notesTestDirectory = Join-Path $notesTestRoot ('tmp/release-notes-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $notesTestDirectory | Out-Null
# A tiny synthetic repository supplies real commit ranges; no project tag,
# remote endpoint or installed game is modified.
function Invoke-NotesTestGit {
    & git -C $notesTestDirectory @args
    if ($LASTEXITCODE -ne 0) { throw "Fixture Git failed: $args" }
}
function Assert-NotesTest([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
Invoke-NotesTestGit init --quiet
Invoke-NotesTestGit config user.name 'Release fixture'
Invoke-NotesTestGit config user.email 'fixture@example.invalid'
Invoke-NotesTestGit config commit.gpgsign false
[IO.File]::WriteAllText((Join-Path $notesTestDirectory 'feature.txt'), 'baseline')
Invoke-NotesTestGit add feature.txt
Invoke-NotesTestGit commit --quiet -m baseline
Invoke-NotesTestGit tag v1.2.2
[IO.File]::WriteAllText((Join-Path $notesTestDirectory 'feature.txt'), 'direct feature')
Invoke-NotesTestGit commit --quiet -am 'feat: independent models without a pull request'
$notesTestDirect = (Invoke-NotesTestGit rev-parse HEAD).Trim()
Invoke-NotesTestGit commit --quiet --allow-empty -m 'Merge pull request #7 from fixture/fix'
Invoke-NotesTestGit commit --quiet --allow-empty -m 'follow-up (#7)'
$notesTestSource = (Invoke-NotesTestGit rev-parse HEAD).Trim()
$notesTestCount = 0
$notesTestPath = Join-Path $notesTestDirectory 'notes.md'
$notesTestTemplate = @'
# NR Nexus TAG — User-facing update

<!-- Previous: v1.2.2 -->

## Highlights

Choose independent models for each processing pass without changing other settings.

## Validation

Targeted tests passed; the tested inputs and remaining visual scope are documented.

## Known Limitations

Cross-game image quality and long-duration stability have not yet been verified.

## Installation

Install the release ZIP with compatible ReShade and a separately obtained NVIDIA runtime.
'@
foreach ($notesTestTag in @('v1.2.3-dev.1', 'v1.2.3-rc.1', 'v1.2.3')) {
    [IO.File]::WriteAllText($notesTestPath, $notesTestTemplate.Replace('TAG', $notesTestTag))
    $notesTestResult = & $notesTestTool -Tag $notesTestTag -Repository fixture/repository -SourceRoot $notesTestDirectory -NotesPath $notesTestPath
    Assert-NotesTest ($notesTestResult.Source -ceq $notesTestSource) 'Wrong source commit'
    Assert-NotesTest ($notesTestResult.Body.Contains('independent models without a pull request')) 'Direct feature commit was omitted'
    Assert-NotesTest ($notesTestResult.Body.Contains("/commit/$notesTestDirect")) 'Direct commit link missing'
    Assert-NotesTest ([regex]::Matches($notesTestResult.Body, '/pull/7\)').Count -eq 1) 'Duplicate pull request reference'
    Assert-NotesTest ($notesTestResult.Body.IndexOf('## Highlights') -lt $notesTestResult.Body.IndexOf('## References')) 'Commit list displaced highlights'
    Assert-NotesTest ([regex]::Matches($notesTestResult.Body, '\[Full Changelog\]').Count -eq 1) 'Changelog link must occur once'
    Assert-NotesTest ($notesTestResult.Body.Contains("/compare/v1.2.2...$notesTestTag")) 'Wrong changelog range'
    Assert-NotesTest (-not $notesTestResult.Body.Contains('<!-- Previous:')) 'Internal note metadata leaked'
    $notesTestCount++
}
# A release containing only a direct feature commit must work without any PR.
$notesTestResult = & $notesTestTool -Tag v1.2.3 -Repository fixture/repository -SourceRoot $notesTestDirectory -SourceCommit $notesTestDirect -NotesPath $notesTestPath
Assert-NotesTest ($notesTestResult.Body.Contains('without a pull request') -and -not $notesTestResult.Body.Contains('Pull requests:')) 'No-PR release lost its feature'
$notesTestCount++
$notesTestValid = $notesTestTemplate.Replace('TAG', 'v1.2.3')
$notesTestFailures = @(
    @{name='PR links only';text=$notesTestValid.Replace('Choose independent models for each processing pass without changing other settings.', '- [#7](https://github.com/fixture/repository/pull/7)');error='substantive'},
    @{name='placeholder';text=$notesTestValid.Replace('Choose independent models', 'TODO: choose independent models');error='placeholders'},
    @{name='missing validation';text=$notesTestValid.Replace('## Validation', '## Other');error='Validation'},
    @{name='wrong version';text=$notesTestValid.Replace('v1.2.3', 'v9.9.9');error='title'},
    @{name='duplicate generated source';text=$notesTestValid + "`n`n## Source`nExisting source";error='reserved'}
)
foreach ($notesTestFailure in $notesTestFailures) {
    [IO.File]::WriteAllText($notesTestPath, $notesTestFailure.text)
    $notesTestCaught = $false
    try { & $notesTestTool -Tag v1.2.3 -Repository fixture/repository -SourceRoot $notesTestDirectory -NotesPath $notesTestPath | Out-Null }
    catch {
        Assert-NotesTest ($_.Exception.Message.Contains($notesTestFailure.error)) "Unexpected error for $($notesTestFailure.name): $_"
        $notesTestCaught = $true
    }
    Assert-NotesTest $notesTestCaught "Invalid notes accepted: $($notesTestFailure.name)"
    $notesTestCount++
}
$notesTestCaught = $false
try { & $notesTestTool -Tag v1.2.3 -Repository fixture/repository -SourceRoot $notesTestDirectory | Out-Null }
catch { Assert-NotesTest ($_.Exception.Message.Contains('Release notes missing')) 'Missing-file error is unclear'; $notesTestCaught=$true }
Assert-NotesTest $notesTestCaught 'Missing notes must not fall back to PR-only publication'
$notesTestCount++
[IO.File]::WriteAllText($notesTestPath, $notesTestValid.Replace('Previous: v1.2.2', 'Previous: none'))
$notesTestResult = & $notesTestTool -Tag v1.2.3 -Repository fixture/repository -SourceRoot $notesTestDirectory -NotesPath $notesTestPath
Assert-NotesTest (-not $notesTestResult.Body.Contains('Full Changelog')) 'First release must not invent a previous tag'
$notesTestCount++
Write-Host "Release notes tests passed: $notesTestCount; real synthetic commit ranges, no network or project-tag mutation"
