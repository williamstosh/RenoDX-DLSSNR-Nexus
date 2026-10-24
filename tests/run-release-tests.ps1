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
    foreach ($releaseTestScenario in @('new','annotated-tag','existing-exact','existing-different','draft-resume','draft-page-two','duplicate-draft','draft-starter','upload-failure','wrong-remote-tag','forbidden','incomplete-assets','unexpected-draft-asset','checksum-mismatch','tag-version-mismatch')) {
        Reset-TestChecksum
        & {
            $releaseTestState = @{calls=[Collections.Generic.List[object]]::new();release=$null}
            if ($releaseTestScenario -in @('existing-exact','existing-different','draft-resume','draft-page-two','duplicate-draft','draft-starter','unexpected-draft-asset')) {
                $releaseTestState.release = [pscustomobject]@{
                    id=1;tag_name=$releaseTestVersion.Tag;target_commitish=$releaseTestSource
                    draft=($releaseTestScenario -in @('draft-resume','draft-page-two','duplicate-draft','draft-starter','unexpected-draft-asset'))
                    assets=@((New-TestAsset $releaseTestArchive),(New-TestAsset $releaseTestChecksum))
                    html_url='https://github.com/fixture/repository/releases/example'
                }
                if ($releaseTestScenario -eq 'existing-different') { $releaseTestState.release.assets[0].digest='sha256:wrong' }
                if ($releaseTestScenario -in @('draft-resume','draft-page-two')) { $releaseTestState.release.assets=@($releaseTestState.release.assets[0]) }
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
                if ($args -contains '--porcelain') { return }
                if ($args -contains '--short=8') { return $releaseTestSource.Substring(0,8) }
                if ($args -contains 'rev-parse') { return $releaseTestSource }
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
                    if ($releaseTestScenario -eq 'draft-page-two' -and $Uri.EndsWith('page=1')) {
                        return @(1..100 | ForEach-Object { [pscustomobject]@{tag_name="other-$_"} })
                    }
                    if ($releaseTestScenario -eq 'duplicate-draft') { return @($releaseTestState.release,$releaseTestState.release) }
                    if ($releaseTestState.release) { return $releaseTestState.release }
                    return @()
                }
                if ($Uri -eq 'https://api.github.com/repos/fixture/repository/releases' -and $Method -eq 'POST') {
                    Assert-Test $payload.draft 'Creation must remain private until assets pass'
                    Assert-Test $payload.generate_release_notes 'Automatic notes were not requested'
                    $releaseTestState.release = [pscustomobject]@{
                        id=1;tag_name=$payload.tag_name;target_commitish=$payload.target_commitish;draft=$true;assets=@()
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
                & (Join-Path $releaseTestRoot 'tools/publish-release.ps1') -Tag $tag -ArchivePath $releaseTestArchive -Repository fixture/repository
            } catch { $failed=$true }
            $success = $releaseTestScenario -in @('new','annotated-tag','existing-exact','draft-resume','draft-page-two','draft-starter')
            Assert-Test ($failed -ne $success) "Unexpected outcome: $releaseTestScenario"
            $published=@($releaseTestState.calls | Where-Object method -EQ 'PATCH')
            Assert-Test ($published.Count -eq $(if ($success -and $releaseTestScenario -ne 'existing-exact') { 1 } else { 0 })) "Publication boundary failed: $releaseTestScenario"
            if ($releaseTestScenario -in @('existing-exact','existing-different','duplicate-draft','wrong-remote-tag','forbidden','checksum-mismatch','tag-version-mismatch')) {
                Assert-Test (@($releaseTestState.calls | Where-Object method -NE 'GET').Count -eq 0) "Unexpected mutation: $releaseTestScenario"
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
