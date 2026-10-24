[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Tag,
    [Parameter(Mandatory)][string]$ArchivePath,
    [string]$Repository = $env:GITHUB_REPOSITORY,
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$NotesPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'A GitHub owner/repository is required.' }
if (-not $env:GH_TOKEN) { throw 'GH_TOKEN with contents:write is required.' }
# Recovery may use a newer publisher, but source identity always comes from
# the clean checkout at the existing tag, never from the publisher's commit.
$taskSourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$taskVersion = & (Join-Path $taskSourceRoot 'tools/check-version.ps1') -ExpectedTag $Tag -VerifyTag
$taskSource = (& git -C $taskSourceRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the release source.' }
$taskSourceLine = "Source: https://github.com/$Repository/commit/$taskSource"
$taskNotes = & (Join-Path $PSScriptRoot 'get-release-notes.ps1') -Tag $Tag -Repository $Repository `
    -SourceRoot $taskSourceRoot -SourceCommit $taskSource -NotesPath $NotesPath
$taskArchive = (Resolve-Path -LiteralPath $ArchivePath).Path
$taskName = [IO.Path]::GetFileName($taskArchive)
if ($taskName -cne ($taskVersion.ArchiveStem + '-windows-x64.zip')) { throw 'Archive name does not match the source version and commit.' }
$taskChecksum = $taskArchive + '.sha256'
$taskHash = (Get-FileHash -LiteralPath $taskArchive -Algorithm SHA256).Hash.ToLowerInvariant()
$taskExpectedLine = "$taskHash  $taskName"
if ([IO.File]::ReadAllText($taskChecksum).Trim() -cne $taskExpectedLine) { throw 'Archive SHA-256 sidecar mismatch.' }
$taskFiles = @($taskArchive, $taskChecksum)
$taskHeaders = @{
    Authorization = "Bearer $env:GH_TOKEN"
    Accept = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2022-11-28'
}

function Invoke-ReleaseApi {
    param([string]$Path, [string]$Method = 'GET', [object]$Body, [switch]$AllowMissing)
    $taskRequest = @{
        Uri = "https://api.github.com/repos/$Repository/$Path"
        Headers = $taskHeaders
        Method = $Method
        TimeoutSec = 60
    }
    if ($null -ne $Body) {
        $taskRequest.ContentType = 'application/json; charset=utf-8'
        $taskRequest.Body = $Body | ConvertTo-Json -Depth 8 -Compress
    }
    try {
        # Invoke-RestMethod preserves a JSON array as one pipeline object.
        # Re-emit the value so callers see zero/one/many releases consistently.
        $taskResponse = Invoke-RestMethod @taskRequest
        return $taskResponse
    }
    catch {
        if ($AllowMissing -and $_.Exception -is [Microsoft.PowerShell.Commands.HttpResponseException] -and
            [int]$_.Exception.Response.StatusCode -eq 404) { return $null }
        throw
    }
}

function Assert-ReleaseAsset {
    param([object]$Asset, [string]$Path)
    $taskFile = Get-Item -LiteralPath $Path
    $taskDigest = 'sha256:' + (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($Asset.state -cne 'uploaded' -or $Asset.size -ne $taskFile.Length -or $Asset.digest -cne $taskDigest) {
        throw "Existing/uploaded asset differs; it will not be overwritten: $($taskFile.Name)"
    }
}

# Check the remote tag too: publishing must never create or move a tag.
$taskEscapedTag = [Uri]::EscapeDataString($Tag)
$taskRef = Invoke-ReleaseApi "git/ref/tags/$taskEscapedTag"
$taskObject = $taskRef.object
while ($taskObject.type -ceq 'tag') {
    $taskObject = (Invoke-ReleaseApi "git/tags/$($taskObject.sha)").object
}
if ($taskObject.type -cne 'commit' -or $taskObject.sha -cne $taskSource) { throw 'Remote tag does not match the validated source commit.' }
$taskRelease = Invoke-ReleaseApi "releases/tags/$taskEscapedTag" -AllowMissing
if ($null -eq $taskRelease) {
    # The tag endpoint describes published releases. Authenticated listing is
    # needed to find a private draft left by an interrupted asset upload.
    $taskMatches = @()
    for ($taskPage = 1; ; $taskPage++) {
        $taskPageReleases = @(Invoke-ReleaseApi "releases?per_page=100&page=$taskPage")
        $taskMatches += @($taskPageReleases | Where-Object tag_name -CEQ $Tag)
        if ($taskPageReleases.Count -lt 100) { break }
    }
    if ($taskMatches.Count -gt 1) { throw 'Multiple releases/drafts use this tag; resolve them before retrying.' }
    if ($taskMatches.Count -eq 1) { $taskRelease = $taskMatches[0] }
}
$taskPrerelease = $taskVersion.Version.Split('+')[0].Contains('-')
if ($null -eq $taskRelease) {
    $taskBody = @{
        tag_name = $Tag
        # The tag exists and was checked above. Omitting target_commitish
        # avoids asking GITHUB_TOKEN for workflow-write permission on a dev
        # commit whose workflows differ from the default branch.
        name = $taskNotes.Title
        draft = $true
        prerelease = $taskPrerelease
        generate_release_notes = $false
        body = $taskNotes.Body
    }
    $taskRelease = Invoke-ReleaseApi 'releases' 'POST' $taskBody
}
if ($taskRelease.tag_name -cne $Tag -or
    -not [regex]::IsMatch($taskRelease.body, '(?m)^' + [regex]::Escape($taskSourceLine) + '\r?$')) {
    throw 'An existing release targets different source; no changes were made to it.'
}
if ($taskRelease.draft -and ($taskRelease.body.Replace("`r`n", "`n").TrimEnd() -cne $taskNotes.Body.TrimEnd() -or
    $taskRelease.name -cne $taskNotes.Title)) {
    # Resume matching drafts with reviewed notes. Published metadata is never
    # rewritten by a retry; correcting it is a separate explicit operation.
    $taskRelease = Invoke-ReleaseApi "releases/$($taskRelease.id)" 'PATCH' @{
        name = $taskNotes.Title
        body = $taskNotes.Body
    }
}
foreach ($taskFile in $taskFiles) {
    $taskFileInfo = Get-Item -LiteralPath $taskFile
    $taskAsset = @($taskRelease.assets | Where-Object name -CEQ $taskFileInfo.Name)
    if ($taskAsset.Count -gt 1) { throw 'Duplicate release assets.' }
    if ($taskAsset.Count -eq 1 -and $taskRelease.draft -and
        $taskAsset[0].state -ceq 'starter' -and $taskAsset[0].size -eq 0) {
        # GitHub documents empty starter assets after failed uploads.
        Invoke-ReleaseApi "releases/assets/$($taskAsset[0].id)" 'DELETE' | Out-Null
        $taskAsset = @()
    }
    if ($taskAsset.Count -eq 1) {
        Assert-ReleaseAsset $taskAsset[0] $taskFile
    } elseif (-not $taskRelease.draft) {
        throw 'A published release is missing expected assets; published releases are not modified.'
    } else {
        $taskUpload = "https://uploads.github.com/repos/$Repository/releases/$($taskRelease.id)/assets?name=$([Uri]::EscapeDataString($taskFileInfo.Name))"
        $taskType = if ($taskFileInfo.Extension -eq '.zip') { 'application/zip' } else { 'text/plain' }
        $taskUploaded = Invoke-RestMethod -Uri $taskUpload -Headers $taskHeaders -Method POST -ContentType $taskType -InFile $taskFile -TimeoutSec 120
        Assert-ReleaseAsset $taskUploaded $taskFile
    }
}
if (-not $taskRelease.draft) {
    Write-Host "Release already contains the exact validated assets: $Tag"
    return
}
$taskRelease = Invoke-ReleaseApi "releases/$($taskRelease.id)"
$taskNames = @($taskFiles | ForEach-Object { [IO.Path]::GetFileName($_) })
if (@($taskRelease.assets).Count -ne $taskNames.Count -or
    @($taskRelease.assets | Where-Object { $_.name -cnotin $taskNames }).Count) { throw 'Draft asset list is incomplete or contains unexpected files.' }
foreach ($taskFile in $taskFiles) {
    $taskAsset = @($taskRelease.assets | Where-Object name -CEQ ([IO.Path]::GetFileName($taskFile)))[0]
    Assert-ReleaseAsset $taskAsset $taskFile
}
# A failed upload/verification leaves a draft. Visibility changes only here.
$taskPublished = Invoke-ReleaseApi "releases/$($taskRelease.id)" 'PATCH' @{
    draft = $false
    prerelease = $taskPrerelease
    make_latest = $(if ($taskPrerelease) { 'false' } else { 'legacy' })
}
if ($taskPublished.draft) { throw 'GitHub did not publish the verified draft.' }
Write-Host "Published: $($taskPublished.html_url)"
