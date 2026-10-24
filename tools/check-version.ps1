[CmdletBinding()]
param(
    [string]$ExpectedTag,
    [switch]$VerifyTag,
    [string[]]$BinaryPath = @()
)

$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskHeader = Get-Content -LiteralPath (Join-Path $taskRoot 'version.h') -Raw
$taskDefinitions = [regex]::Matches($taskHeader, '(?m)^#define NR_NEXUS_VERSION "([^"]+)"\r?$')
if ($taskDefinitions.Count -ne 1) { throw 'version.h must define NR_NEXUS_VERSION exactly once.' }
$taskVersion = $taskDefinitions[0].Groups[1].Value
$taskSemver = '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$'
$taskMatch = [regex]::Match($taskVersion, $taskSemver)
if (-not $taskMatch.Success) { throw "Invalid SemVer: $taskVersion" }
foreach ($taskPart in $taskMatch.Groups[4].Value.Split('.')) {
    if ($taskPart -match '^0[0-9]+$') { throw 'Numeric prerelease identifiers cannot have leading zeros.' }
}
foreach ($taskIndex in 1..3) {
    if ([decimal]$taskMatch.Groups[$taskIndex].Value -gt 65535) { throw 'PE numeric version components must fit in 16 bits.' }
}
if ($ExpectedTag -and $ExpectedTag -cne "v$taskVersion") { throw "Tag must equal v$taskVersion" }
$taskSource = Get-Content -LiteralPath (Join-Path $taskRoot 'dlssnr.hpp') -Raw
if (-not $taskSource.Contains('#include "version.h"') -or
    -not $taskSource.Contains('constexpr char kAddonVersion[] = "v" NR_NEXUS_VERSION;')) {
    throw 'The displayed add-on version must use version.h.'
}
if ($VerifyTag) {
    if (-not $ExpectedTag) { throw '-VerifyTag requires -ExpectedTag.' }
    $taskTagCommit = & git -C $taskRoot rev-parse --verify "refs/tags/$ExpectedTag^{commit}"
    if ($LASTEXITCODE -ne 0) { throw 'The expected tag does not exist.' }
    $taskHead = & git -C $taskRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $taskTagCommit -cne $taskHead) { throw 'The version tag must point to HEAD.' }
    $taskDirty = & git -C $taskRoot status --porcelain --untracked-files=normal
    if ($LASTEXITCODE -ne 0 -or $taskDirty) { throw 'Tag verification requires a clean working tree.' }
}
if ($BinaryPath.Count) {
    $taskBrand = Get-Content -LiteralPath (Join-Path $taskRoot 'brand.h') -Raw
    $taskName = [regex]::Match($taskBrand, '(?m)^#define NR_NEXUS_NAME "([^"]+)"').Groups[1].Value
    $taskDescription = [regex]::Match($taskBrand, '(?m)^#define NR_NEXUS_DESCRIPTION "([^"]+)"').Groups[1].Value
    foreach ($taskBinary in $BinaryPath) {
        $taskInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo((Resolve-Path -LiteralPath $taskBinary).Path)
        if ($taskInfo.FileVersion -cne $taskVersion -or $taskInfo.ProductVersion -cne $taskVersion -or
            $taskInfo.ProductName -cne $taskName -or $taskInfo.FileDescription -cne $taskDescription -or
            $taskInfo.OriginalFilename -cne 'renodx-dlss5.addon64' -or
            $taskInfo.FileMajorPart -ne [int]$taskMatch.Groups[1].Value -or
            $taskInfo.FileMinorPart -ne [int]$taskMatch.Groups[2].Value -or
            $taskInfo.FileBuildPart -ne [int]$taskMatch.Groups[3].Value -or $taskInfo.FilePrivatePart -ne 0) {
            throw "PE brand/version metadata mismatch: $taskBinary"
        }
        Write-Host "PE brand/version verified: $taskBinary"
    }
}
$taskRevision = & git -C $taskRoot rev-parse --short=8 HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source commit.' }
[pscustomobject]@{
    Version = $taskVersion
    Tag = "v$taskVersion"
    FileVersion = $taskVersion
    NumericFileVersion = ($taskMatch.Groups[1..3].Value -join '.') + '.0'
    ArchiveStem = "NR-Nexus-v$taskVersion-$taskRevision"
}
