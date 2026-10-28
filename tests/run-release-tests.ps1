[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$releaseTestRoot = Split-Path $PSScriptRoot -Parent
$releaseTestVersion = & (Join-Path $releaseTestRoot 'tools/check-version.ps1')
$releaseTestSource = (& git -C $releaseTestRoot rev-parse HEAD).Trim()
$releaseTestDirectory = Join-Path $releaseTestRoot ('tmp/release-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $releaseTestDirectory | Out-Null
$releaseTestArchive = Join-Path $releaseTestDirectory ($releaseTestVersion.ArchiveStem + '-windows-x64.zip')
# Publishing consumes a package already verified by the build job. These tests
# exercise the HTTP publication boundary, never a real GitHub endpoint or tag.
[IO.File]::WriteAllBytes($releaseTestArchive, [byte[]]@(1,2,3))
$releaseTestHash = (Get-FileHash -LiteralPath $releaseTestArchive).Hash.ToLowerInvariant()
$releaseTestChecksum = $releaseTestArchive + '.sha256'
$releaseTestSourceRoot = Join-Path $releaseTestDirectory 'tag-source'
New-Item -ItemType Directory -Path (Join-Path $releaseTestSourceRoot 'tools') | Out-Null
Copy-Item -LiteralPath (Join-Path $releaseTestRoot 'tools/check-version.ps1') -Destination (Join-Path $releaseTestSourceRoot 'tools/check-version.ps1')
Copy-Item -LiteralPath (Join-Path $releaseTestRoot 'version.h') -Destination (Join-Path $releaseTestSourceRoot 'version.h')
[IO.File]::WriteAllText((Join-Path $releaseTestSourceRoot 'dlssnr.hpp'), '#include "version.h"' + "`n" + 'constexpr char kAddonVersion[] = "v" NR_NEXUS_VERSION;')
$releaseTestNotesPath = Join-Path $releaseTestDirectory 'reviewed-notes.md'
$releaseTestNotes = @'
# NR Nexus TAG

[简体中文](#简体中文) · [English](#english)

<!-- Previous: none -->

## 简体中文

### 版本亮点

支持独立模型。

### 验证情况

模拟发布通过。

### 已知限制

尚未验证实机。

### 安装说明

解压安装附件。

## English

### Highlights

Select independent models per pass.

### Validation

Synthetic publication tests passed.

### Known Limitations

Game visuals remain unverified.

### Installation

Extract the release archive.
'@
$releaseTestNotes = $releaseTestNotes.Replace('TAG', $releaseTestVersion.Tag)
[IO.File]::WriteAllText($releaseTestNotesPath, $releaseTestNotes)
function Assert-TestNotes([string]$Body) {
    Assert-Test ($Body.StartsWith("[简体中文](#user-content-nr-nexus-$($releaseTestVersion.Tag)-zh) · [English](#user-content-nr-nexus-$($releaseTestVersion.Tag)-en)")) 'Reviewed navigation missing'
    Assert-Test ($Body.IndexOf('## 简体中文') -lt $Body.IndexOf('## English')) 'Language blocks reordered'
    Assert-Test ($Body.Contains('### 版本亮点') -and $Body.Contains('### Highlights') -and $Body.Contains('direct feature: add independent per-pass models')) 'Reviewed features or direct commits missing'
    foreach ($heading in @('Source / 源码', 'References / PR 与 Commit 记录')) {
        Assert-Test ([regex]::Matches($Body, '(?m)^## ' + [regex]::Escape($heading) + '$').Count -eq 1) "Generated section must occur once: $heading"
    }
    Assert-Test ([regex]::Matches($Body, '\[Full Changelog\]').Count -eq 1) 'Generated changelog must occur once'
    Assert-Test ([regex]::Matches($Body, '(?m)^Source: https://github.com/fixture/repository/commit/' + $releaseTestSource + '$').Count -eq 1) 'Publisher source identity changed'
}
function Reset-TestChecksum {
    [IO.File]::WriteAllText($releaseTestChecksum, "$releaseTestHash  $([IO.Path]::GetFileName($releaseTestArchive))`n")
}
function New-TestAsset([string]$Path) {
    $item = Get-Item -LiteralPath $Path
    [pscustomobject]@{name=$item.Name;state='uploaded';size=$item.Length;digest=('sha256:'+(Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant())}
}
function Assert-Test([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
$releaseTestOldToken = $env:GH_TOKEN
$env:GH_TOKEN = 'synthetic-test-token'
$releaseTestResults = @()
try {
    foreach ($releaseTestScenario in @('new','empty-array-response','unrelated-array-response','separate-source-root','annotated-tag','existing-exact','existing-different','existing-wrong-source','draft-wrong-source','draft-resume','draft-empty','draft-array-response','multiple-release-array','draft-page-two','duplicate-draft','draft-starter','upload-failure','wrong-remote-tag','forbidden','incomplete-assets','unexpected-draft-asset','checksum-mismatch','tag-version-mismatch','notes-missing','notes-pr-only')) {
        Reset-TestChecksum
        & {
            $releaseTestState = @{calls=[Collections.Generic.List[object]]::new();release=$null}
            if ($releaseTestScenario -in @('existing-exact','existing-different','existing-wrong-source','draft-wrong-source','draft-resume','draft-empty','draft-array-response','multiple-release-array','draft-page-two','duplicate-draft','draft-starter','unexpected-draft-asset')) {
                $releaseTestState.release = [pscustomobject]@{
                    id=1;tag_name=$releaseTestVersion.Tag;target_commitish=$releaseTestSource;name='Previous draft title'
                    draft=($releaseTestScenario -in @('draft-wrong-source','draft-resume','draft-empty','draft-array-response','multiple-release-array','draft-page-two','duplicate-draft','draft-starter','unexpected-draft-asset'))
                    assets=@((New-TestAsset $releaseTestArchive),(New-TestAsset $releaseTestChecksum))
                    html_url='https://github.com/fixture/repository/releases/example'
                    body="Source: https://github.com/fixture/repository/commit/$releaseTestSource"
                }
                if ($releaseTestScenario -eq 'existing-different') { $releaseTestState.release.assets[0].digest='sha256:wrong' }
                if ($releaseTestScenario -in @('existing-wrong-source','draft-wrong-source')) { $releaseTestState.release.body='Source: different' }
                if ($releaseTestScenario -in @('draft-resume','draft-array-response','draft-page-two')) { $releaseTestState.release.assets=@($releaseTestState.release.assets[0]) }
                if ($releaseTestScenario -eq 'draft-empty') { $releaseTestState.release.assets=@() }
                if ($releaseTestScenario -eq 'draft-starter') {
                    $releaseTestState.release.assets[0]=[pscustomobject]@{id=123;name=([IO.Path]::GetFileName($releaseTestArchive));state='starter';size=0;digest=$null}
                }
                if ($releaseTestScenario -eq 'unexpected-draft-asset') {
                    $releaseTestState.release.assets += [pscustomobject]@{name='unreviewed.dll';state='uploaded';size=1;digest='sha256:wrong'}
                }
            }
            if ($releaseTestScenario -eq 'checksum-mismatch') { [IO.File]::WriteAllText($releaseTestChecksum,'invalid') }
            function git {
                $global:LASTEXITCODE = 0
                $sourceIndex = [array]::IndexOf($args, '-C')
                $expectedRoot = if ($releaseTestScenario -eq 'separate-source-root') { $releaseTestSourceRoot } else { $releaseTestRoot }
                Assert-Test ($sourceIndex -ge 0 -and $args[$sourceIndex+1] -ceq $expectedRoot) 'Git identity must use the selected source checkout'
                if ($args -contains '--porcelain') { return }
                if ($args -contains '--short=8') { return $releaseTestSource.Substring(0,8) }
                if ($args -contains 'rev-parse') { return $releaseTestSource }
                if ($args -contains 'merge-base') { return }
                if ($args -contains 'log') {
                    return @("$releaseTestSource`tdirect feature: add independent per-pass models", "$releaseTestSource`tMerge pull request #3 from fixture/publisher")
                }
                throw "Unexpected Git call in publication test: $args"
            }
            function Invoke-RestMethod {
                param([string]$Uri, [object]$Headers, [string]$Method='GET', [string]$ContentType, [string]$Body, [string]$InFile, [int]$TimeoutSec)
                $payload = if ($Body) { $Body | ConvertFrom-Json } else { $null }
                $releaseTestState.calls.Add([pscustomobject]@{uri=$Uri;method=$Method;payload=$payload})
                if ($releaseTestScenario -eq 'forbidden') { throw 'Synthetic HTTP 403; must not be treated as missing release' }
                if ($Uri -like 'https://api.github.com/repos/fixture/repository/git/ref/tags/*') {
                    if ($releaseTestScenario -eq 'annotated-tag') { return [pscustomobject]@{object=[pscustomobject]@{type='tag';sha='annotation'}} }
                    return [pscustomobject]@{object=[pscustomobject]@{type='commit';sha=$(if ($releaseTestScenario -eq 'wrong-remote-tag') { 'different' } else { $releaseTestSource })}}
                }
                if ($Uri -eq 'https://api.github.com/repos/fixture/repository/git/tags/annotation') {
                    return [pscustomobject]@{object=[pscustomobject]@{type='commit';sha=$releaseTestSource}}
                }
                if ($Uri -like 'https://api.github.com/repos/fixture/repository/releases/tags/*') {
                    if ($releaseTestState.release -and -not $releaseTestState.release.draft) { return $releaseTestState.release }
                    $response = [Net.Http.HttpResponseMessage]::new([Net.HttpStatusCode]::NotFound)
                    throw [Microsoft.PowerShell.Commands.HttpResponseException]::new('Synthetic not found', $response)
                }
                if ($Uri -like 'https://api.github.com/repos/fixture/repository/releases?per_page=100&page=*') {
                    if ($releaseTestScenario -eq 'empty-array-response') { return ,@() }
                    if ($releaseTestScenario -eq 'unrelated-array-response') { return ,@([pscustomobject]@{tag_name='other-tag'}) }
                    if ($releaseTestScenario -in @('draft-array-response','draft-empty')) { return ,@($releaseTestState.release) }
                    if ($releaseTestScenario -eq 'multiple-release-array') {
                        return ,@([pscustomobject]@{tag_name='earlier'},$releaseTestState.release,[pscustomobject]@{tag_name='later'})
                    }
                    if ($releaseTestScenario -eq 'draft-page-two' -and $Uri.EndsWith('page=1')) {
                        return @(1..100 | ForEach-Object { [pscustomobject]@{tag_name="other-$_"} })
                    }
                    if ($releaseTestScenario -eq 'duplicate-draft') { return @($releaseTestState.release,$releaseTestState.release) }
                    if ($releaseTestState.release) { return $releaseTestState.release }
                    return @()
                }
                if ($Uri -eq 'https://api.github.com/repos/fixture/repository/releases' -and $Method -eq 'POST') {
                    Assert-Test $payload.draft 'Creation must remain private until assets pass'
                    Assert-Test (-not $payload.generate_release_notes) 'GitHub PR-only notes must not replace reviewed highlights'
                    Assert-TestNotes $payload.body
                    Assert-Test ($payload.PSObject.Properties.Name -notcontains 'target_commitish') 'An existing tag needs no target_commitish/workflow-write permission'
                    $releaseTestState.release = [pscustomobject]@{
                        id=1;tag_name=$payload.tag_name;target_commitish='main';body=$payload.body;name=$payload.name;draft=$true;assets=@()
                        html_url='https://github.com/fixture/repository/releases/example'
                    }
                    return $releaseTestState.release
                }
                if ($Uri -like 'https://uploads.github.com/repos/fixture/repository/releases/1/assets?name=*') {
                    if ($releaseTestScenario -eq 'upload-failure') { throw 'Synthetic upload failure' }
                    $asset = New-TestAsset $InFile
                    $releaseTestState.release.assets += $asset
                    return $asset
                }
                if ($Uri -eq 'https://api.github.com/repos/fixture/repository/releases/assets/123' -and $Method -eq 'DELETE') {
                    $releaseTestState.release.assets=@($releaseTestState.release.assets | Where-Object state -NE 'starter')
                    return
                }
                if ($Uri -eq 'https://api.github.com/repos/fixture/repository/releases/1' -and $Method -eq 'GET') {
                    if ($releaseTestScenario -eq 'incomplete-assets') { $releaseTestState.release.assets=@() }
                    return $releaseTestState.release
                }
                if ($Uri -eq 'https://api.github.com/repos/fixture/repository/releases/1' -and $Method -eq 'PATCH') {
                    if ($payload.PSObject.Properties.Name -notcontains 'draft') {
                        Assert-Test $releaseTestState.release.draft 'Retry must not edit published release notes'
                        Assert-TestNotes $payload.body
                        $releaseTestState.release.body=$payload.body
                        $releaseTestState.release.name=$payload.name
                        return $releaseTestState.release
                    }
                    Assert-Test (-not $payload.draft) 'Final transition must publish the verified draft'
                    Assert-Test ($releaseTestState.release.assets.Count -eq 2) 'Missing assets at publish boundary'
                    $releaseTestState.release.draft=$false
                    return $releaseTestState.release
                }
                throw "Unmocked publication request: $Method $Uri"
            }
            $failed=$false
            try {
                $tag = if ($releaseTestScenario -eq 'tag-version-mismatch') { 'v999.0.0' } else { $releaseTestVersion.Tag }
                $publishArguments = @{Tag=$tag;ArchivePath=$releaseTestArchive;Repository='fixture/repository'}
                if ($releaseTestScenario -eq 'separate-source-root') { $publishArguments.SourceRoot=$releaseTestSourceRoot }
                $publishArguments.NotesPath=$releaseTestNotesPath
                if ($releaseTestScenario -eq 'notes-missing') { $publishArguments.NotesPath=Join-Path $releaseTestDirectory 'absent.md' }
                if ($releaseTestScenario -eq 'notes-pr-only') {
                    $publishArguments.NotesPath=Join-Path $releaseTestDirectory 'pr-only.md'
                    [IO.File]::WriteAllText($publishArguments.NotesPath, $releaseTestNotes.Replace('Select independent models per pass.', '- [#3](https://github.com/fixture/repository/pull/3)'))
                }
                & (Join-Path $releaseTestRoot 'tools/publish-release.ps1') @publishArguments
            } catch { $failed=$true }
            $success = $releaseTestScenario -in @('new','empty-array-response','unrelated-array-response','separate-source-root','annotated-tag','existing-exact','draft-resume','draft-empty','draft-array-response','multiple-release-array','draft-page-two','draft-starter')
            Assert-Test ($failed -ne $success) "Unexpected outcome: $releaseTestScenario"
            $published=@($releaseTestState.calls | Where-Object { $_.method -eq 'PATCH' -and $_.payload.PSObject.Properties.Name -contains 'draft' })
            Assert-Test ($published.Count -eq $(if ($success -and $releaseTestScenario -ne 'existing-exact') { 1 } else { 0 })) "Publication boundary failed: $releaseTestScenario"
            if ($releaseTestScenario -in @('draft-empty','draft-array-response','multiple-release-array')) {
                Assert-Test (@($releaseTestState.calls | Where-Object { $_.method -eq 'POST' -and $_.uri -eq 'https://api.github.com/repos/fixture/repository/releases' }).Count -eq 0) 'Existing drafts must be resumed, not recreated'
            }
            if ($releaseTestScenario -in @('existing-exact','existing-different','existing-wrong-source','draft-wrong-source','duplicate-draft','wrong-remote-tag','forbidden','checksum-mismatch','tag-version-mismatch','notes-missing','notes-pr-only')) {
                Assert-Test (@($releaseTestState.calls | Where-Object method -NE 'GET').Count -eq 0) "Unexpected mutation: $releaseTestScenario"
            }
            if ($releaseTestScenario -in @('notes-missing','notes-pr-only')) {
                Assert-Test ($releaseTestState.calls.Count -eq 0) 'Invalid notes must fail before remote publication calls'
            }
            if ($published.Count) {
                $expectedPrerelease=$releaseTestVersion.Version.Split('+')[0].Contains('-')
                Assert-Test ($published[0].payload.prerelease -eq $expectedPrerelease) 'Incorrect prerelease classification'
                Assert-Test ($published[0].payload.make_latest -ceq $(if ($expectedPrerelease) { 'false' } else { 'legacy' })) 'Incorrect latest policy'
            }
            Write-Host "PASS publication boundary: $releaseTestScenario"
        }
        $releaseTestResults += [pscustomobject]@{scenario=$releaseTestScenario;result='passed'}
    }
} finally { $env:GH_TOKEN = $releaseTestOldToken }
$releaseTestResults | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $releaseTestDirectory 'result.json') -Encoding utf8
Write-Host "Publication boundary tests passed: $($releaseTestResults.Count); no real network or tag mutation"
