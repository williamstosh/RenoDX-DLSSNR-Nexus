[CmdletBinding()]
param(
    [string]$HostRoot = (Join-Path $PSScriptRoot '../.local/renodx')
)

$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskHost = [IO.Path]::GetFullPath($HostRoot)
$taskLock = Get-Content -LiteralPath (Join-Path $taskRoot 'host/lock.json') -Raw | ConvertFrom-Json
if (Test-Path -LiteralPath $taskHost) {
    throw "Choose a new host directory; existing directories are not overwritten: $taskHost"
}

function Initialize-PinnedRepository {
    param([string]$Directory, [string]$Repository, [string]$Commit, [string[]]$Paths)
    & git init --quiet $Directory
    if ($LASTEXITCODE -ne 0) { throw "Cannot initialize $Directory" }
    & git -C $Directory config core.autocrlf false
    & git -C $Directory remote add origin $Repository
    & git -C $Directory sparse-checkout init --cone
    & git -C $Directory sparse-checkout set -- @Paths
    if ($LASTEXITCODE -ne 0) { throw "Cannot configure sparse checkout: $Directory" }
    & git -C $Directory -c filter.lfs.smudge= -c filter.lfs.required=false fetch --quiet --depth=1 --filter=blob:none origin $Commit
    if ($LASTEXITCODE -ne 0) { throw "Cannot fetch pinned commit $Commit from $Repository" }
    & git -C $Directory -c filter.lfs.smudge= -c filter.lfs.required=false checkout --quiet --detach FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { throw "Cannot check out $Commit" }
    if ((& git -C $Directory rev-parse HEAD) -cne $Commit) { throw "Unexpected checkout: $Directory" }
}

Initialize-PinnedRepository $taskHost $taskLock.host.repository $taskLock.host.commit $taskLock.host.paths
foreach ($taskDependency in $taskLock.dependencies) {
    Initialize-PinnedRepository (Join-Path $taskHost $taskDependency.path) $taskDependency.repository $taskDependency.commit $taskDependency.paths
}
$taskPatch = Join-Path $taskRoot ('host/' + $taskLock.compatibility.patch)
& git -C $taskHost apply --check -- $taskPatch
if ($LASTEXITCODE -ne 0) { throw 'Pinned RenoDX host does not accept the compatibility patch.' }
& git -C $taskHost apply -- $taskPatch
if ($LASTEXITCODE -ne 0) { throw 'Host compatibility patch failed.' }
foreach ($taskHeader in $taskLock.compatibility.headers.PSObject.Properties) {
    if ((Get-FileHash -LiteralPath (Join-Path $taskHost $taskHeader.Name)).Hash -cne $taskHeader.Value) {
        throw "Patched host header does not match the qualified input: $($taskHeader.Name)"
    }
}
New-Item -ItemType Directory -Path (Join-Path $taskHost 'src/addons') -Force | Out-Null
New-Item -ItemType Junction -Path (Join-Path $taskHost 'src/addons/dlss5') -Target $taskRoot | Out-Null
[pscustomobject]@{
    HostRoot = $taskHost
    HostCommit = $taskLock.host.commit
    CompatibilityPatchSHA256 = (Get-FileHash -LiteralPath $taskPatch).Hash
    AddonSource = $taskRoot
    DependencyCount = @($taskLock.dependencies).Count
}
