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

[简体中文](#简体中文) · [English](#english)

<!-- Previous: v1.2.2 -->

## 简体中文

### 版本亮点

支持独立模型。

### 验证情况

定向测试通过。

### 已知限制

实机画质未测。

### 安装说明

解压安装附件。

## English

### Highlights

Choose independent models for each processing pass without changing other settings.

### Validation

Targeted tests passed; the tested inputs and remaining visual scope are documented.

### Known Limitations

Cross-game image quality and long-duration stability have not yet been verified.

### Installation

Install the release ZIP with compatible ReShade and a separately obtained NVIDIA runtime.
'@
foreach ($notesTestTag in @('v1.2.3-dev.1', 'v1.2.3-rc.1', 'v1.2.3')) {
    [IO.File]::WriteAllText($notesTestPath, $notesTestTemplate.Replace('TAG', $notesTestTag))
    $notesTestResult = & $notesTestTool -Tag $notesTestTag -Repository fixture/repository -SourceRoot $notesTestDirectory -NotesPath $notesTestPath
    Assert-NotesTest ($notesTestResult.Source -ceq $notesTestSource) 'Wrong source commit'
    Assert-NotesTest ($notesTestResult.Body.Contains('independent models without a pull request')) 'Direct feature commit was omitted'
    Assert-NotesTest ($notesTestResult.Body.Contains("/commit/$notesTestDirect")) 'Direct commit link missing'
    Assert-NotesTest ([regex]::Matches($notesTestResult.Body, '/pull/7\)').Count -eq 1) 'Duplicate pull request reference'
    Assert-NotesTest ($notesTestResult.Body.StartsWith('[简体中文](#简体中文) · [English](#english)')) 'Top navigation missing'
    Assert-NotesTest ($notesTestResult.Body.IndexOf('## 简体中文') -lt $notesTestResult.Body.IndexOf('## English')) 'Language order changed'
    Assert-NotesTest ($notesTestResult.Body.IndexOf('### Installation') -lt $notesTestResult.Body.IndexOf('## Source / 源码')) 'Generated content displaced language blocks'
    Assert-NotesTest ([regex]::Matches($notesTestResult.Body, '(?m)^## Source / 源码$').Count -eq 1) 'Source section must occur once'
    Assert-NotesTest ([regex]::Matches($notesTestResult.Body, '(?m)^## References / PR 与 Commit 记录$').Count -eq 1) 'References section must occur once'
    Assert-NotesTest ([regex]::Matches($notesTestResult.Body, '(?m)^Source: https://github.com/fixture/repository/commit/' + $notesTestSource + '$').Count -eq 1) 'Publisher source identity line changed'
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
    @{name='missing validation';text=$notesTestValid.Replace('### Validation', '### Other');error='Validation'},
    @{name='wrong version';text=$notesTestValid.Replace('v1.2.3', 'v9.9.9');error='title'},
    @{name='duplicate generated source';text=$notesTestValid + "`n`n## Source`nExisting source";error='reserved'}
)
$notesTestNavigation = '[简体中文](#简体中文) · [English](#english)'
$notesTestChineseStart = $notesTestValid.IndexOf('## 简体中文')
$notesTestEnglishStart = $notesTestValid.IndexOf('## English')
$notesTestChinese = $notesTestValid.Substring($notesTestChineseStart, $notesTestEnglishStart - $notesTestChineseStart)
$notesTestEnglish = $notesTestValid.Substring($notesTestEnglishStart)
$notesTestPrefix = $notesTestValid.Substring(0, $notesTestChineseStart)
$notesTestFailures += @(
    @{name='missing Chinese';text=$notesTestPrefix + $notesTestEnglish;error='block'},
    @{name='missing English';text=$notesTestPrefix + $notesTestChinese;error='block'},
    @{name='English first';text=$notesTestPrefix + $notesTestEnglish + "`n`n" + $notesTestChinese;error='block'},
    @{name='duplicate Chinese';text=$notesTestValid + "`n`n" + $notesTestChinese;error='block'},
    @{name='duplicate English';text=$notesTestValid + "`n`n" + $notesTestEnglish;error='block'},
    @{name='missing navigation';text=$notesTestValid.Replace($notesTestNavigation, '');error='navigation'},
    @{name='wrong navigation order';text=$notesTestValid.Replace($notesTestNavigation, '[English](#english) · [简体中文](#简体中文)');error='navigation'},
    @{name='wrong navigation anchor';text=$notesTestValid.Replace('](#english)', '](#English)');error='navigation'},
    @{name='duplicate navigation';text=$notesTestValid + "`n`n$notesTestNavigation";error='navigation'},
    @{name='navigation below prose';text=$notesTestValid.Replace($notesTestNavigation, "Reviewed introductory prose.`n`n$notesTestNavigation");error='navigation'}
)
$notesTestSections = @(
    @{heading='版本亮点';body='支持独立模型。'},
    @{heading='验证情况';body='定向测试通过。'},
    @{heading='已知限制';body='实机画质未测。'},
    @{heading='安装说明';body='解压安装附件。'},
    @{heading='Highlights';body='Choose independent models for each processing pass without changing other settings.'},
    @{heading='Validation';body='Targeted tests passed; the tested inputs and remaining visual scope are documented.'},
    @{heading='Known Limitations';body='Cross-game image quality and long-duration stability have not yet been verified.'},
    @{heading='Installation';body='Install the release ZIP with compatible ReShade and a separately obtained NVIDIA runtime.'}
)
foreach ($section in $notesTestSections) {
    $heading = '### ' + $section.heading
    $notesTestFailures += @{name="missing $heading";text=$notesTestValid.Replace($heading, '### Other');error=$section.heading}
    $notesTestFailures += @{name="duplicate $heading";text=$notesTestValid.Replace($heading, "$heading`n`n$($section.body)`n`n$heading");error=$section.heading}
    $notesTestFailures += @{name="wrong level $heading";text=$notesTestValid.Replace($heading, '#' + $heading);error=$section.heading}
    $notesTestFailures += @{name="comment heading $heading";text=$notesTestValid.Replace($heading, "<!-- $heading -->");error=$section.heading}
    $notesTestFailures += @{name="inline heading $heading";text=$notesTestValid.Replace($heading, "Text $heading");error=$section.heading}
    foreach ($content in @('', '- [A sufficiently long link label](https://example.invalid)', 'https://example.invalid/long-description', '#### A sufficiently long subsection title', '<!-- A sufficiently long review comment -->', '[Long reference label][ref]', '[ref]: https://example.invalid', ('```text' + "`nExample text is not a reviewed result`n" + '```'))) {
        $notesTestFailures += @{name="no prose $heading : $content";text=$notesTestValid.Replace($section.body, $content);error='substantive'}
    }
    foreach ($placeholder in @('TODO', 'TBD', 'PLACEHOLDER', '待补充', '待填写', '占位符')) {
        $notesTestFailures += @{name="placeholder $heading : $placeholder";text=$notesTestValid.Replace($section.body, $placeholder);error='placeholders'}
    }
    if ($section.heading -match '^[A-Za-z]') {
        $notesTestFailures += @{name="short English $heading";text=$notesTestValid.Replace($section.body, 'None');error='substantive'}
    }
}
foreach ($reserved in @('Source', 'Source / 源码', '源码', 'References', 'References / PR 与 Commit 记录', 'PR 与 Commit 记录', 'Changelog', 'Full Changelog', '更新日志')) {
    $notesTestFailures += @{name="reserved $reserved";text=$notesTestValid + "`n`n## $reserved`n`nReviewed text.";error='reserved'}
}
$notesTestFailures += @{name='manual changelog link';text=$notesTestValid + "`n`n[Full Changelog](https://example.invalid)";error='reserved'}
$notesTestFailures += @{name='manual short changelog link';text=$notesTestValid + "`n`n[Changelog](https://example.invalid)";error='reserved'}
$notesTestFailures += @{name='manual source line';text=$notesTestValid + "`n`nSource: https://example.invalid";error='reserved'}
$notesTestFailures += @{name='missing Previous';text=$notesTestValid.Replace('<!-- Previous: v1.2.2 -->', '');error='Previous'}
$notesTestFailures += @{name='duplicate Previous';text=$notesTestValid + "`n<!-- Previous: v1.2.2 -->";error='Previous'}
foreach ($optional in @('新增功能与改进', '修复内容', 'Features & Improvements', 'Fixes')) {
    $insertBefore = if ($optional -match '[\p{IsCJKUnifiedIdeographs}]') { '## English' } else { '### Installation' }
    $optionalBody = if ($optional -match '[\p{IsCJKUnifiedIdeographs}]') { '修复切换问题。' } else { 'Fixed model switching for independent passes.' }
    $withOptional = $notesTestValid.Replace($insertBefore, "### $optional`n`n$optionalBody`n`n$insertBefore")
    [IO.File]::WriteAllText($notesTestPath, $withOptional)
    & $notesTestTool -Tag v1.2.3 -Repository fixture/repository -SourceRoot $notesTestDirectory -NotesPath $notesTestPath | Out-Null
    $notesTestCount++
    $notesTestFailures += @{name="duplicate optional $optional";text=$withOptional.Replace("### $optional", "### $optional`n`n$optionalBody`n`n### $optional");error=$optional}
    $notesTestFailures += @{name="empty optional $optional";text=$withOptional.Replace($optionalBody, '');error='substantive'}
}
foreach ($extra in @(
    @{heading='迁移说明';body='无需迁移配置。';before='## English'},
    @{heading='Compatibility';body='Existing configuration remains compatible.';before='### Installation'}
)) {
    $withExtra = $notesTestValid.Replace($extra.before, "### $($extra.heading)`n`n$($extra.body)`n`n$($extra.before)")
    [IO.File]::WriteAllText($notesTestPath, $withExtra)
    & $notesTestTool -Tag v1.2.3 -Repository fixture/repository -SourceRoot $notesTestDirectory -NotesPath $notesTestPath | Out-Null
    $notesTestCount++
    $notesTestFailures += @{name="duplicate extra $($extra.heading)";text=$withExtra.Replace("### $($extra.heading)", "### $($extra.heading)`n`n$($extra.body)`n`n### $($extra.heading)");error='Duplicate'}
    $notesTestFailures += @{name="empty extra $($extra.heading)";text=$withExtra.Replace($extra.body, '');error='substantive'}
}
foreach ($foreign in @(
    @{heading='Highlights';before='## English'},
    @{heading='版本亮点';before='### Installation'}
)) {
    $notesTestFailures += @{name="wrong language required $($foreign.heading)";text=$notesTestValid.Replace($foreign.before, "### $($foreign.heading)`n`nReviewed change description text.`n`n$($foreign.before)");error='Unexpected'}
}
# Short Chinese prose is valid; headings in comments/examples do not create duplicates.
foreach ($validText in @(
    $notesTestValid.Replace('支持独立模型。', '独立模型。'),
    $notesTestValid.Replace('实机画质未测。', '无已知新增限制。'),
    $notesTestValid.Replace($notesTestNavigation, "<!-- Fill both language blocks. -->`n<!-- Keep generated sections out. -->`n`n$notesTestNavigation"),
    $notesTestValid.Replace($notesTestNavigation, '').Replace('<!-- Previous: v1.2.2 -->', "<!-- Previous: v1.2.2 -->`n`n$notesTestNavigation"),
    ($notesTestValid + "`n`n<!-- ## English`n### Highlights -->"),
    ($notesTestValid + "`n`n" + '```markdown' + "`n## English`n### Highlights`n" + '```')
)) {
    [IO.File]::WriteAllText($notesTestPath, $validText)
    & $notesTestTool -Tag v1.2.3 -Repository fixture/repository -SourceRoot $notesTestDirectory -NotesPath $notesTestPath | Out-Null
    $notesTestCount++
}
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
Assert-NotesTest ($notesTestResult.Body.Contains('/commits/v1.2.3') -and -not $notesTestResult.Body.Contains('/compare/')) 'First release must link its history without inventing a previous tag'
$notesTestCount++
# An empty range has no references, but still has source identity and history.
[IO.File]::WriteAllText($notesTestPath, $notesTestValid)
$notesTestBaseline = (Invoke-NotesTestGit rev-parse v1.2.2).Trim()
$notesTestResult = & $notesTestTool -Tag v1.2.3 -Repository fixture/repository -SourceRoot $notesTestDirectory -SourceCommit $notesTestBaseline -NotesPath $notesTestPath
Assert-NotesTest (-not $notesTestResult.Body.Contains('## References')) 'Empty commit range must not invent references'
$notesTestCount++
foreach ($previous in @('v9.9.9', 'v1.2.3')) {
    if ($previous -eq 'v1.2.3') { Invoke-NotesTestGit tag v1.2.3 }
    [IO.File]::WriteAllText($notesTestPath, $notesTestValid.Replace('Previous: v1.2.2', "Previous: $previous"))
    $caught = $false
    try { & $notesTestTool -Tag v1.2.3 -Repository fixture/repository -SourceRoot $notesTestDirectory -NotesPath $notesTestPath | Out-Null }
    catch { Assert-NotesTest ($_.Exception.Message.Contains('Previous')) "Unexpected Previous error: $_"; $caught=$true }
    Assert-NotesTest $caught 'Invalid Previous accepted'
    $notesTestCount++
}
# A real divergent tag cannot be used as the previous release.
Invoke-NotesTestGit checkout --quiet --orphan divergent
Invoke-NotesTestGit commit --quiet --allow-empty -m divergent
Invoke-NotesTestGit tag v1.1.0
[IO.File]::WriteAllText($notesTestPath, $notesTestValid.Replace('Previous: v1.2.2', 'Previous: v1.1.0'))
$caught = $false
try { & $notesTestTool -Tag v1.2.3 -Repository fixture/repository -SourceRoot $notesTestDirectory -SourceCommit $notesTestSource -NotesPath $notesTestPath | Out-Null }
catch { Assert-NotesTest ($_.Exception.Message.Contains('must precede')) "Unexpected ancestry error: $_"; $caught=$true }
Assert-NotesTest $caught 'Non-ancestor Previous accepted'
$notesTestCount++
Write-Host "Release notes tests passed: $notesTestCount; real synthetic commit ranges, no network or project-tag mutation"
