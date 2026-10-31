#requires -Version 7.0
<#
.SYNOPSIS
Explicit local byte-for-byte test batch archive; never a backup or automatic cleanup.
.DESCRIPTION
Create: -SourceDirectory <one completed batch> -ArchiveDirectory <new local archive>
Verify: -ArchiveDirectory <archive> -VerifyOnly
Restore: -ArchiveDirectory <archive> -RestoreDirectory <new tmp/artifacts directory>
Managed batches use artifact-lifecycle.json (nr-nexus-test-output/1): status
Passed|Failed, mode Standard|Full|Acceptance, and an exited ownerPid. Running is refused.
Legacy batches require result.json: Status=Completed, or build=passed and run=passed.
Acceptance in either metadata file prohibits -RemoveOriginals. Irreplaceable evidence
should be retained separately; deletion is always an explicit caller decision.
Preserves file bytes, relative names and empty directories, not ACLs, ADS or timestamps.
Volumes contain ordinary Deflate ZIP entries with original relative names. VolumeBytes
limits UNCOMPRESSED file bytes (default 1 GiB); a larger file occupies its own volume.
Files are never split; .NET ZipArchive supplies ZIP64 automatically when needed.
#>
[CmdletBinding(DefaultParameterSetName = 'Create')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Create')][string]$SourceDirectory,
    [Parameter(Mandatory)][string]$ArchiveDirectory,
    [Parameter(ParameterSetName = 'Create')][switch]$RemoveOriginals,
    [Parameter(ParameterSetName = 'Create')][ValidateRange(131072, 1073741824)][long]$VolumeBytes = 1GB,
    [Parameter(Mandatory, ParameterSetName = 'Verify')][switch]$VerifyOnly,
    [Parameter(Mandatory, ParameterSetName = 'Restore')][string]$RestoreDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

function Get-SafePath([string]$Path) {
    if ($Path -match '[\x00-\x1f*?]' -or $Path -match '(^|[\\/])\.\.([\\/]|$)' -or
        $Path -match ':(?![\\/])' -or $Path.StartsWith('\\')) { throw "Unsafe path: $Path" }
    $full = [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $cursor = $full
    while ($cursor) {
        $item = $null
        try { $item = Get-Item -LiteralPath $cursor -Force }
        catch [System.Management.Automation.ItemNotFoundException] { }
        if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Reparse path: $cursor" }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    return $full
}
function Test-Below([string]$Path, [string]$Parent) {
    return $Path.StartsWith($Parent + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
}
function Get-BatchPath([string]$Path) {
    $full = Get-SafePath $Path
    if (-not ((Test-Below $full (Join-Path $repo 'tmp')) -or
        (Test-Below $full (Join-Path $repo 'artifacts')))) { throw 'Batch must be below repository tmp/ or artifacts/.' }
    return $full
}
function Get-Tree([string]$Root) {
    $null = Get-SafePath $Root
    foreach ($item in Get-ChildItem -LiteralPath $Root -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse child: $($item.FullName)" }
        $item
        if ($item.PSIsContainer) { Get-Tree $item.FullName }
    }
}
function New-EmptyDirectory([string]$Path) {
    $null = Get-SafePath $Path
    if (Test-Path -LiteralPath $Path) {
        if (-not (Test-Path -LiteralPath $Path -PathType Container) -or
            @(Get-ChildItem -LiteralPath $Path -Force).Count) { throw "Refusing nonempty destination: $Path" }
    } else { $null = [IO.Directory]::CreateDirectory($Path) }
}
function Get-Hash([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Get-Relative([string]$Path) { return [IO.Path]::GetRelativePath($source, $Path).Replace('\', '/') }
function Assert-Relative([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path -match '[\\:\x00-\x1f*?<>|]' -or
        $Path.StartsWith('/') -or @($Path.Split('/') | Where-Object {
            $_ -in @('', '.', '..') -or $_ -match '[. ]$|^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(\.|$)'
        }).Count) { throw "Unsafe relative path: $Path" }
}
function Read-BatchState {
    # A suite result inside an active/Acceptance managed batch is not a separate
    # legacy batch. Require the caller to select that managed batch's root.
    $parent = [IO.Path]::GetDirectoryName($source)
    while ((Test-Below $parent (Join-Path $repo 'tmp')) -or
        (Test-Below $parent (Join-Path $repo 'artifacts'))) {
        if (Test-Path -LiteralPath (Join-Path $parent 'artifact-lifecycle.json')) {
            throw 'Select the managed batch root, not a nested legacy result.'
        }
        $parent = [IO.Path]::GetDirectoryName($parent)
    }
    $managed = Join-Path $source 'artifact-lifecycle.json'
    $legacy = Join-Path $source 'result.json'
    if (-not (Test-Path -LiteralPath $managed -PathType Leaf)) {
        if (-not (Test-Path -LiteralPath $legacy -PathType Leaf)) { throw 'Missing completed batch metadata.' }
        $data = Get-Content -LiteralPath $legacy -Raw | ConvertFrom-Json -AsHashtable
        if ($data['Status'] -ne 'Completed' -and -not ($data['build'] -eq 'passed' -and $data['run'] -eq 'passed')) {
            throw 'Legacy batch requires a completed result.json.'
        }
        if ($RemoveOriginals -and $data['mode'] -eq 'Acceptance') { throw 'Acceptance batches cannot remove originals.' }
    }
    # Inspect every nested managed batch as well: a completed parent must not hide
    # an active child or permit deletion of Acceptance evidence.
    foreach ($marker in @(Get-Tree $source | Where-Object { $_.Name -eq 'artifact-lifecycle.json' })) {
        $data = Get-Content -LiteralPath $marker.FullName -Raw | ConvertFrom-Json -AsHashtable
        if ($data['schema'] -ne 'nr-nexus-test-output/1' -or
            $data['mode'] -notin @('Standard', 'Full', 'Acceptance')) { throw 'Invalid artifact lifecycle schema/mode.' }
        if ($RemoveOriginals -and $data['mode'] -eq 'Acceptance') { throw 'Acceptance batches cannot remove originals.' }
        if ($data['status'] -notin @('Passed', 'Failed')) { throw 'Batch status must be Passed or Failed; Running is refused.' }
        $ownerId = 0
        if (-not [int]::TryParse([string]$data['ownerPid'], [ref]$ownerId) -or $ownerId -le 0) { throw 'Invalid lifecycle ownerPid.' }
        $owner = $null
        try { $owner = [Diagnostics.Process]::GetProcessById($ownerId) }
        catch [ArgumentException] { } # PID no longer exists.
        if ($owner) {
            try { if (-not $owner.HasExited) { throw "Batch owner is still active: $ownerId" } }
            finally { $owner.Dispose() }
        }
    }
}

# The same streaming verifier is used before deletion and when restoring. It never
# trusts manifest paths, lengths or ZIP membership without checking them first.
function Read-Archive([string]$RestoreRoot = '') {
    $null = @(Get-Tree $archive)
    $checks = Get-Content -LiteralPath (Join-Path $archive 'checksums.json') -Raw | ConvertFrom-Json
    $seenChecks = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($check in $checks) {
        if ($check.Name -notmatch '^(manifest\.json|volume-[0-9]{6}\.zip)$' -or
            -not $seenChecks.Add($check.Name)) { throw 'Unsafe or duplicate checksum name.' }
        $path = Join-Path $archive $check.Name
        if ((Get-Item -LiteralPath $path).Length -ne $check.Length -or (Get-Hash $path) -ne $check.SHA256) {
            throw "Archive checksum mismatch: $($check.Name)"
        }
    }
    if (-not $seenChecks.Contains('manifest.json')) { throw 'Manifest checksum missing.' }
    $manifest = Get-Content -LiteralPath (Join-Path $archive 'manifest.json') -Raw | ConvertFrom-Json
    if ($manifest.Schema -ne 2) { throw 'Unsupported archive schema.' }
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $entries = @{}
    $volumes = @{}
    try {
        foreach ($name in $seenChecks) {
            if ($name -eq 'manifest.json') { continue }
            $zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $archive $name))
            $volumes[$name] = $zip
            foreach ($entry in $zip.Entries) {
                Assert-Relative $entry.FullName.TrimEnd('/')
                if ($entries.ContainsKey($entry.FullName)) { throw 'Duplicate ZIP entry.' }
                $entries[$entry.FullName] = @{ Volume = $name; Entry = $entry }
            }
        }
        foreach ($record in @($manifest.Directories) + @($manifest.Files)) {
            Assert-Relative $record.Path
            if (-not $names.Add($record.Path)) { throw 'Duplicate manifest path.' }
        }
        if ($entries.Count -ne $names.Count) { throw 'Unlisted or missing ZIP entries.' }
        foreach ($dir in $manifest.Directories) {
            $key = $dir.Path + '/'
            if (-not $entries.ContainsKey($key) -or $entries[$key].Volume -ne $dir.Volume -or
                $entries[$key].Entry.Length -ne 0) { throw 'Directory ZIP entry mismatch.' }
        }
        # Reject a file masquerading as an ancestor before creating restore files.
        $fileNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($file in $manifest.Files) { $null = $fileNames.Add($file.Path) }
        foreach ($name in $names) {
            $parent = $name
            while ($parent.Contains('/')) {
                $parent = $parent.Substring(0, $parent.LastIndexOf('/'))
                if ($fileNames.Contains($parent)) { throw 'File/directory path collision.' }
            }
        }
        if ($RestoreRoot) {
            foreach ($dir in $manifest.Directories) {
                $target = Get-SafePath (Join-Path $RestoreRoot $dir.Path)
                $null = [IO.Directory]::CreateDirectory($target)
            }
        }
        $buffer = [byte[]]::new(1MB)
        foreach ($file in $manifest.Files) {
            if (-not $entries.ContainsKey($file.Path) -or $entries[$file.Path].Volume -ne $file.Volume -or
                $entries[$file.Path].Entry.Length -ne $file.Length) { throw 'File ZIP entry mismatch.' }
            $hash = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
            $output = $null
            [long]$length = 0
            try {
                if ($RestoreRoot) {
                    $target = Get-SafePath (Join-Path $RestoreRoot $file.Path)
                    $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
                    $output = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                }
                $input = $entries[$file.Path].Entry.Open()
                try {
                    while (($count = $input.Read($buffer, 0, $buffer.Length)) -gt 0) {
                        $hash.AppendData($buffer, 0, $count)
                        if ($output) { $output.Write($buffer, 0, $count) }
                        $length += $count
                    }
                } finally { $input.Dispose() }
                if ($length -ne $file.Length -or [Convert]::ToHexString($hash.GetHashAndReset()) -ne $file.SHA256) {
                    throw "Full file validation failed: $($file.Path)"
                }
            } finally {
                if ($output) { $output.Dispose() }
                $hash.Dispose()
            }
        }
    } finally { foreach ($zip in $volumes.Values) { $zip.Dispose() } }
    return $manifest
}

$archive = Get-SafePath $ArchiveDirectory
if (-not (Test-Below $archive (Join-Path $repo '.local/archives'))) { throw 'Archive must be below repository .local/archives/.' }
if ($PSCmdlet.ParameterSetName -ne 'Create') {
    $null = Read-Archive
    if ($RestoreDirectory) {
        $restore = Get-BatchPath $RestoreDirectory
        New-EmptyDirectory $restore
        $null = Read-Archive $restore
        Write-Host "Restored and SHA256 verified: $restore"
    } else { Write-Host "All archive entries SHA256 verified: $archive" }
    return
}
$source = Get-BatchPath $SourceDirectory
if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw 'Source batch directory missing.' }
if ((Test-Below $archive $source) -or (Test-Below $source $archive)) { throw 'Archive/source overlap.' }
$tree = @(Get-Tree $source)
Read-BatchState
New-EmptyDirectory $archive
$directories = [Collections.Generic.List[object]]::new()
$records = [Collections.Generic.List[object]]::new()
$zip = $null
$volume = 0
[long]$used = 0
$fileCount = 0
$buffer = [byte[]]::new(1MB)
try {
    foreach ($file in $tree | Sort-Object FullName) {
        $relative = Get-Relative $file.FullName
        Assert-Relative $relative
        [long]$length = if ($file.PSIsContainer) { 0 } else { $file.Length }
        if (-not $zip -or ($fileCount -gt 0 -and $used + $length -gt $VolumeBytes)) {
            if ($zip) { $zip.Dispose() }
            $volume++
            $volumeName = 'volume-{0:d6}.zip' -f $volume
            $zip = [IO.Compression.ZipArchive]::new(
                [IO.File]::Open((Join-Path $archive $volumeName), [IO.FileMode]::CreateNew),
                [IO.Compression.ZipArchiveMode]::Create, $false)
            $used = 0
            $fileCount = 0
        }
        if ($file.PSIsContainer) {
            $null = $zip.CreateEntry($relative + '/')
            $directories.Add(@{ Path = $relative; Volume = $volumeName })
            continue
        }
        # Deny writers/deleters while copying; later compare the whole source again.
        $input = [IO.File]::Open($file.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        $hash = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
        try {
            if ($input.Length -ne $length) { throw "Source changed: $relative" }
            $output = $zip.CreateEntry($relative, [IO.Compression.CompressionLevel]::Optimal).Open()
            try {
                while (($count = $input.Read($buffer, 0, $buffer.Length)) -gt 0) {
                    $output.Write($buffer, 0, $count)
                    $hash.AppendData($buffer, 0, $count)
                }
            } finally { $output.Dispose() }
            $records.Add(@{ Path = $relative; Length = $length; SHA256 = [Convert]::ToHexString($hash.GetHashAndReset()); Volume = $volumeName })
            $used += $length
            $fileCount++
        } finally { $input.Dispose(); $hash.Dispose() }
    }
} finally { if ($zip) { $zip.Dispose() } }
@{ Schema = 2; VolumeBytes = $VolumeBytes; Directories = @($directories.ToArray()); Files = @($records.ToArray()) } |
    ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $archive 'manifest.json') -Encoding utf8
$checks = @(Get-ChildItem -LiteralPath $archive -File | ForEach-Object {
    @{ Name = $_.Name; Length = $_.Length; SHA256 = Get-Hash $_.FullName }
})
ConvertTo-Json -InputObject $checks | Set-Content -LiteralPath (Join-Path $archive 'checksums.json') -Encoding utf8
$manifest = Read-Archive

# A real disk restore of one complete file supplements full streaming validation.
$sample = Join-Path $archive ('.restore-check-' + [Guid]::NewGuid().ToString('N'))
if ($manifest.Files.Count) {
    $first = $manifest.Files | Sort-Object Length -Descending | Select-Object -First 1
    $output = [IO.File]::Open($sample, [IO.FileMode]::CreateNew)
    try {
        $sampleZip = [IO.Compression.ZipFile]::OpenRead((Join-Path $archive $first.Volume))
        try {
            $input = $sampleZip.GetEntry($first.Path).Open()
            try { $input.CopyTo($output) } finally { $input.Dispose() }
        } finally { $sampleZip.Dispose() }
    } finally { $output.Dispose() }
    if ((Get-Item -LiteralPath $sample).Length -ne $first.Length -or (Get-Hash $sample) -ne $first.SHA256) {
        throw 'Sample disk restore failed; originals retained.'
    }
    Remove-Item -LiteralPath $sample -ErrorAction Stop
}

# Finish every validation before the first deletion. A later deletion error stops
# immediately; already deleted files remain recoverable from this verified archive.
$current = @(Get-Tree $source)
if ($current.Count -ne $tree.Count) { throw 'Source membership changed; originals retained.' }
$expectedNames = @($manifest.Directories | ForEach-Object { $_.Path }) + @($manifest.Files | ForEach-Object { $_.Path })
foreach ($item in $current) {
    if ((Get-Relative $item.FullName) -notin $expectedNames) { throw 'Source membership changed; originals retained.' }
}
foreach ($dir in $manifest.Directories) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $dir.Path) -PathType Container)) {
        throw 'Source directory type changed; originals retained.'
    }
}
foreach ($file in $manifest.Files) {
    $path = Get-SafePath (Join-Path $source $file.Path)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
        (Get-Item -LiteralPath $path).Length -ne $file.Length -or (Get-Hash $path) -ne $file.SHA256) {
        throw 'Source changed; originals retained.'
    }
}
Read-BatchState
if ($RemoveOriginals) {
    foreach ($file in $manifest.Files) {
        $path = Get-SafePath (Join-Path $source $file.Path)
        Remove-Item -LiteralPath $path -ErrorAction Stop
    }
    foreach ($dir in @($current | Where-Object PSIsContainer | Sort-Object { $_.FullName.Length } -Descending)) {
        $path = Get-SafePath $dir.FullName
        if (@(Get-ChildItem -LiteralPath $path -Force).Count) { throw "Directory changed; stopping deletion: $path" }
        Remove-Item -LiteralPath $path -ErrorAction Stop
    }
    if (@(Get-ChildItem -LiteralPath $source -Force).Count) { throw 'Source changed; stopping deletion.' }
    Remove-Item -LiteralPath $source -ErrorAction Stop
}
Write-Host "Verified archive: $archive; originals removed: $([bool]$RemoveOriginals)"
$quotedArchive = $archive.Replace("'", "''")
Write-Host "Restore to a NEW empty batch directory: & ./tools/archive-test-output.ps1 -ArchiveDirectory '$quotedArchive' -RestoreDirectory './tmp/restored-<unique-name>'"
Write-Host 'Irreplaceable evidence needs separate retention; this explicit archive is not an automatic backup.'
