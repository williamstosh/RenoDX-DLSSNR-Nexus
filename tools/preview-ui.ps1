[CmdletBinding()]
param(
    [ValidateSet('Quick','Full')][string]$Mode = 'Quick',
    [string[]]$Scenario = @(),
    [ValidateSet('None','Check','Update')][string]$Documentation = 'None',
    [string[]]$DocumentationImage = @(),
    [string]$PythonExecutable,
    [string]$HostRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/renodx'),
    [string]$ShaderIncludeDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.include'),
    [string]$BuildProject = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.vcxproj'),
    [string]$VcVars64,
    [ValidateSet('Standard','Full','Acceptance')][string]$RetentionMode = $(if ($env:NR_NEXUS_TEST_RETENTION) { $env:NR_NEXUS_TEST_RETENTION } else { 'Standard' }),
    [ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9._-]{0,63}$')][string]$ArchiveName
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = Split-Path $PSScriptRoot -Parent
. (Join-Path $repositoryRoot 'tests/test-support.ps1')
$snapshotRoot = Join-Path $repositoryRoot 'artifacts/ui-snapshots'
Assert-TestArtifactPath $snapshotRoot
[void][IO.Directory]::CreateDirectory($snapshotRoot)
# One writer owns compilation, rotation and cleanup; a second invocation exits.
$lock = [IO.File]::Open((Join-Path $snapshotRoot '.preview.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
if ($Scenario.Count) {
    $snapshotRoot = Join-Path $snapshotRoot 'selected'
    Assert-TestArtifactPath $snapshotRoot
    [void][IO.Directory]::CreateDirectory($snapshotRoot)
}
$runId = [guid]::NewGuid().ToString('N')
$work = Join-Path $snapshotRoot ".work-$runId"
$candidate = Join-Path $snapshotRoot ".pending-$runId"
$latest = Join-Path $snapshotRoot 'latest'
$previous = Join-Path $snapshotRoot 'previous'
$environmentBefore = @{}
foreach ($name in @('NR_NEXUS_UI_SCENARIOS','NR_NEXUS_UI_DOCS','NR_NEXUS_DOC_CAPTURES','NR_NEXUS_UX_REPRO')) {
    $environmentBefore[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

function Get-PreviewSourceState {
    $paths = @(& git --no-optional-locks -C $repositoryRoot -c core.quotepath=false ls-files --cached --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify workspace inputs.' }
    $inputs = @($paths | Sort-Object -Unique | ForEach-Object {
        $path = Join-Path $repositoryRoot $_
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            @{path=$_; sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
        } else { @{path=$_; sha256='deleted'} }
    })
    $identity = ($inputs | ForEach-Object { $_.path + '|' + $_.sha256 }) -join "`n"
    $digest = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($identity)))
    $versionText = Get-Content -LiteralPath (Join-Path $repositoryRoot 'version.h') -Raw
    $version = [regex]::Match($versionText, '#define\s+NR_NEXUS_VERSION\s+"([^"]+)"').Groups[1].Value
    return @{commit=(& git --no-optional-locks -C $repositoryRoot rev-parse HEAD)
        branch=(& git --no-optional-locks -C $repositoryRoot branch --show-current)
        changes=@(& git --no-optional-locks -C $repositoryRoot -c core.quotepath=false status --porcelain=v1 --untracked-files=all)
        fingerprint=$digest; inputs=$inputs; version=$version}
}

function Save-PreviewOwnership {
    param([string]$Directory)
    Assert-TestArtifactPath $Directory
    $files = @(Get-ChildItem -LiteralPath $Directory -Recurse -File -Force | ForEach-Object {
        Assert-TestArtifactPath $_.FullName
        @{path=[IO.Path]::GetRelativePath($Directory,$_.FullName); sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
    })
    @{schema='nr-nexus-preview-owned/1';runId=$runId;status='Passed';mode=$RetentionMode;files=$files} |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Directory 'snapshot-ownership.json') -Encoding utf8
}

function Remove-OwnedPreview {
    param([string]$Directory, [string]$Marker = 'snapshot-ownership.json')
    # Only this system's completed, explicitly inventoried Standard outputs.
    Assert-TestArtifactPath $Directory
    $directoryPath = [IO.Path]::GetFullPath($Directory)
    if (-not $directoryPath.StartsWith($snapshotRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Preview cleanup target escapes snapshot root.'
    }
    $receiptPath = Join-Path $directoryPath $Marker
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    if ($receipt.schema -ne 'nr-nexus-preview-owned/1' -or $receipt.status -ne 'Passed' -or $receipt.mode -ne 'Standard') {
        throw 'Preview output is unowned, incomplete or protected.'
    }
    $items = @(Get-ChildItem -LiteralPath $directoryPath -Recurse -Force)
    foreach ($item in $items) { Assert-TestArtifactPath $item.FullName }
    $files = @($items | Where-Object { -not $_.PSIsContainer -and $_.FullName -ne $receiptPath })
    if ($files.Count -ne $receipt.files.Count) { throw 'Preview inventory changed; retaining output.' }
    foreach ($entry in $receipt.files) {
        $path = [IO.Path]::GetFullPath((Join-Path $directoryPath $entry.path))
        if (-not $path.StartsWith($directoryPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Invalid preview inventory path.'
        }
        Assert-TestArtifactPath $path
        if ((Get-FileHash -LiteralPath $path).Hash -ne $entry.sha256) { throw 'Preview file changed; retaining output.' }
        $stream = [IO.File]::Open($path,'Open','Read','None'); $stream.Dispose()
    }
    foreach ($entry in $receipt.files) { Remove-Item -LiteralPath (Join-Path $directoryPath $entry.path) }
    Remove-Item -LiteralPath $receiptPath
    foreach ($item in @($items | Where-Object PSIsContainer | Sort-Object { $_.FullName.Length } -Descending)) {
        Remove-Item -LiteralPath $item.FullName
    }
    Remove-Item -LiteralPath $directoryPath
}

try {
    if ($Documentation -ne 'None' -and $Scenario.Count) { throw 'Documentation requires the complete bilingual scene set; omit -Scenario.' }
    if ($DocumentationImage.Count -and $Documentation -ne 'Update') { throw '-DocumentationImage requires -Documentation Update.' }
    if (-not $PythonExecutable) {
        $PythonExecutable = $env:NR_NEXUS_PYTHON
    }
    if (-not $PythonExecutable) {
        $python = Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue
        if ($python -and $python.Source -notmatch 'WindowsApps') { $PythonExecutable = $python.Source }
        else { throw 'Pass -PythonExecutable with a Python containing NumPy and Pillow.' }
    }
    & $PythonExecutable -B -c 'import numpy, PIL'
    if ($LASTEXITCODE -ne 0) { throw 'Python requires NumPy and Pillow.' }
    $source = Get-PreviewSourceState
    $source['selection'] = @($Scenario)
    $env:NR_NEXUS_UI_SCENARIOS = $Scenario -join ','
    $env:NR_NEXUS_UI_DOCS = $(if ($Documentation -ne 'None') { '1' } else { $null })
    $env:NR_NEXUS_DOC_CAPTURES = $null
    $env:NR_NEXUS_UX_REPRO = $null
    & (Join-Path $repositoryRoot 'tests/run-ui-layout-tests.ps1') -SnapshotMode $Mode -HostRoot $HostRoot `
        -ShaderIncludeDirectory $ShaderIncludeDirectory -BuildProject $BuildProject -VcVars64 $VcVars64 `
        -OutputDirectory $work -RetentionMode $RetentionMode
    if ($LASTEXITCODE -ne 0) { throw 'Snapshot fixture failed.' }
    if ((Get-PreviewSourceState).fingerprint -ne $source.fingerprint) { throw 'Workspace changed during capture; regenerate snapshots.' }
    $source['build'] = Get-Content -LiteralPath (Join-Path $work 'inputs.json') -Raw | ConvertFrom-Json
    $sourcePath = Join-Path $work 'source-state.json'
    $source | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $sourcePath -Encoding utf8
    $arguments = @('-B',(Join-Path $repositoryRoot 'tests/ui_snapshot_artifacts.py'), '--captures',(Join-Path $work 'menu/captures.json'),
        '--output',$candidate,'--source',$sourcePath,'--mode',$Mode)
    if (Test-Path -LiteralPath $latest) { $arguments += @('--previous',$latest) }
    if ($Documentation -ne 'None') {
        $arguments += @('--documentation',(Join-Path $candidate 'documentation'),'--existing-docs',(Join-Path $repositoryRoot 'docs/images/ui'))
    }
    & $PythonExecutable @arguments
    if ($LASTEXITCODE -ne 0) { throw "Partial/failed preview retained at $candidate; previous snapshots preserved." }
    if ((Get-PreviewSourceState).fingerprint -ne $source.fingerprint) { throw 'Workspace changed during rasterization; regenerate snapshots.' }
    $manifest = Get-Content -LiteralPath (Join-Path $candidate 'manifest.json') -Raw | ConvertFrom-Json
    if ($manifest.status -ne 'complete') { throw 'Incomplete snapshot cannot become latest.' }
    if ($Documentation -eq 'Update') {
        $report = Get-Content -LiteralPath (Join-Path $candidate 'documentation/docs-report.json') -Raw | ConvertFrom-Json
        $available = @($report.images | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_.file) })
        foreach ($name in $DocumentationImage) {
            if ($name -notin $available) { throw "Unknown documentation image: $name" }
        }
    }
    Copy-Item -LiteralPath (Join-Path $work 'menu/menu.stdout.log') -Destination (Join-Path $candidate 'capture.log')
    Copy-Item -LiteralPath (Join-Path $work 'result.json') -Destination (Join-Path $candidate 'test-result.json')
    Save-PreviewOwnership $candidate
    if ($ArchiveName) {
        $archive = Join-Path $snapshotRoot "history/$ArchiveName"
        Assert-TestArtifactPath $archive
        if (Test-Path -LiteralPath $archive) { throw 'Archive name already exists.' }
        [void][IO.Directory]::CreateDirectory((Split-Path $archive -Parent))
        Copy-Item -LiteralPath $candidate -Destination $archive -Recurse
    }
    $retired = Join-Path $snapshotRoot ".retired-$runId"
    foreach ($path in @($latest,$previous,$candidate,$retired)) { Assert-TestArtifactPath $path }
    if (Test-Path -LiteralPath $previous) { Move-Item -LiteralPath $previous -Destination $retired }
    try {
        if (Test-Path -LiteralPath $latest) { Move-Item -LiteralPath $latest -Destination $previous }
        try { Move-Item -LiteralPath $candidate -Destination $latest }
        catch {
            if (Test-Path -LiteralPath $previous) { Move-Item -LiteralPath $previous -Destination $latest }
            throw
        }
    } catch {
        if (Test-Path -LiteralPath $retired) { Move-Item -LiteralPath $retired -Destination $previous }
        throw
    }
    # Documentation candidates are reviewable in latest/documentation. Update
    # copies only meaningful changes after the same geometry/language checks.
    if ($Documentation -eq 'Update') {
        foreach ($entry in $report.images) {
            if ($entry.changed -and (-not $DocumentationImage.Count -or [IO.Path]::GetFileNameWithoutExtension($entry.file) -in $DocumentationImage)) {
                Copy-Item -LiteralPath (Join-Path $latest ('documentation/' + $entry.file)) `
                    -Destination (Join-Path $repositoryRoot ('docs/images/ui/' + $entry.file))
                Write-Host "Updated documentation: $($entry.file)"
            }
        }
        $manifest | Add-Member -NotePropertyName documentation_workspace -NotePropertyValue (Get-PreviewSourceState)
        $manifest | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $latest 'manifest.json') -Encoding utf8
        # Document copying changes tracked files after capture, without changing
        # the captured UI input identity. Record both states explicitly.
        Remove-Item -LiteralPath (Join-Path $latest 'snapshot-ownership.json')
        Save-PreviewOwnership $latest
    }
    if (Test-Path -LiteralPath $retired) {
        try { Remove-OwnedPreview $retired } catch { Write-Warning "Old preview retained: $retired : $_" }
    }
    if ($RetentionMode -eq 'Standard') {
        # The fixture has exited, its Standard lifecycle succeeded, and final
        # images/input identity/logs now live in latest. Raw meshes are disposable.
        $lifecycle = Get-Content -LiteralPath (Join-Path $work 'artifact-lifecycle.json') -Raw | ConvertFrom-Json
        if ($lifecycle.cleanupErrors.Count) { Write-Warning "Fixture cleanup was refused; work directory preserved: $work" }
        elseif ($lifecycle.status -eq 'Passed' -and $lifecycle.ownerPid -eq $PID -and $lifecycle.mode -eq 'Standard') {
            Save-PreviewOwnership $work
            try { Remove-OwnedPreview $work } catch { Write-Warning "Current intermediate output retained: $work : $_" }
        }
    }
    Write-Host "UI Snapshot: $(Join-Path $latest 'index.html')"
    Write-Host "Images: $(Join-Path $latest 'images')"
    if ($Documentation -eq 'Check') { Write-Host "Documentation candidates: $(Join-Path $latest 'documentation/docs-report.json')" }
} catch {
    @{status='failed';runId=$runId;mode=$Mode;reason=$_.Exception.Message
        work=$work;candidate=$candidate;generatedUtc=[DateTime]::UtcNow.ToString('o')} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $snapshotRoot 'last-failure.json') -Encoding utf8
    throw
} finally {
    foreach ($name in $environmentBefore.Keys) { [Environment]::SetEnvironmentVariable($name,$environmentBefore[$name],'Process') }
    $lock.Dispose()
}
