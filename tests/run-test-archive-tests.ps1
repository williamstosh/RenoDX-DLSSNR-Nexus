#requires -Version 7.0
# Synthetic local tests only. No build tools, installation or historical evidence.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tool = Join-Path $repo 'tools/archive-test-output.ps1'
$id = 'archive-tests-' + [Guid]::NewGuid().ToString('N')
$root = Join-Path $repo "tmp/$id"
$archives = Join-Path $repo ".local/archives/$id"
$null = [IO.Directory]::CreateDirectory($root)
$null = [IO.Directory]::CreateDirectory($archives)
$script:checks = 0
function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "Assertion failed: $Message" }
    $script:checks++
}
function Expect-Failure([scriptblock]$Action, [string]$Pattern) {
    $message = ''
    try { & $Action } catch { $message = $_.Exception.Message }
    Assert ($message -match $Pattern) "Expected '$Pattern', got '$message'"
}
function New-Batch([string]$Name, [string]$Mode = 'Standard', [string]$Status = 'Passed') {
    $path = Join-Path $root $Name
    # Use the real helper in an exited owner process, without depending on any
    # in-process helper globals or modifying its lifecycle contract.
    $helper = (Join-Path $PSScriptRoot 'test-support.ps1').Replace("'", "''")
    $quoted = $path.Replace("'", "''")
    $code = "`$ErrorActionPreference = 'Stop'; . '$helper'; `$output = Initialize-TestOutput -Path '$quoted' -RetentionMode '$Mode'; Complete-TestOutput `$output -Outcome '$Status'"
    $start = [Diagnostics.ProcessStartInfo]::new((Get-Process -Id $PID).Path)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    foreach ($argument in @('-NoProfile', '-EncodedCommand', [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code)))) {
        $start.ArgumentList.Add($argument)
    }
    $owner = [Diagnostics.Process]::Start($start)
    try {
        if (-not $owner.WaitForExit(15000) -or $owner.ExitCode -ne 0) { throw "Fixture helper failed: $path" }
    } finally { $owner.Dispose() }
    return $path
}
function Update-Checksums([string]$Path) {
    $checks = @(Get-ChildItem -LiteralPath $Path -File | Where-Object Name -ne 'checksums.json' | ForEach-Object {
        @{ Name = $_.Name; Length = $_.Length; SHA256 = (Get-FileHash -LiteralPath $_.FullName).Hash }
    })
    ConvertTo-Json -InputObject $checks | Set-Content -LiteralPath (Join-Path $Path 'checksums.json') -Encoding utf8
}
$batch = New-Batch 'roundtrip'
$nested = Join-Path $batch 'nested/unicode-数据'
$null = [IO.Directory]::CreateDirectory($nested)
$null = [IO.Directory]::CreateDirectory((Join-Path $batch 'empty-directory'))
$bytes = [byte[]]::new(350000)
[Random]::new(42).NextBytes($bytes)
[IO.File]::WriteAllBytes((Join-Path $nested 'large.bin'), $bytes)
[IO.File]::WriteAllBytes((Join-Path $nested 'empty.bin'), [byte[]]::new(0))
[IO.File]::WriteAllText((Join-Path $batch 'repetitive.txt'), ('lossless archive evidence ' * 18000))
# Several ordinary files exercise packing by uncompressed size; the larger file
# must stay whole in its own volume rather than being byte-split.
foreach ($name in @('pack-a.bin', 'pack-b.bin', 'pack-c.bin')) {
    [IO.File]::WriteAllBytes((Join-Path $batch $name), $bytes[0..59999])
}
[IO.File]::WriteAllText((Join-Path $batch '.hidden-evidence'), 'synthetic evidence')
$destination = Join-Path $archives 'roundtrip'
& $tool -SourceDirectory $batch -ArchiveDirectory $destination -VolumeBytes 131072
Assert (Test-Path -LiteralPath $batch) 'Default retains originals'
$volumes = @(Get-ChildItem -LiteralPath $destination -Filter '*.zip')
Assert ($volumes.Count -gt 1) 'Multiple whole-file volumes'
$manifest = Get-Content -LiteralPath (Join-Path $destination 'manifest.json') -Raw | ConvertFrom-Json
foreach ($volume in $volumes) {
    $zip = [IO.Compression.ZipFile]::OpenRead($volume.FullName)
    try {
        $members = @($zip.Entries | Where-Object { -not $_.FullName.EndsWith('/') })
        $size = ($members | Measure-Object Length -Sum).Sum
        Assert ($size -le 131072 -or ($members.Count -eq 1 -and $members[0].Length -gt 131072)) 'Uncompressed bound or one oversized file'
        foreach ($file in $manifest.Files | Where-Object Volume -eq $volume.Name) {
            $entry = $zip.GetEntry($file.Path)
            Assert ($null -ne $entry -and $entry.Length -eq $file.Length) 'Original relative name accessible by GetEntry'
            Assert (-not ($file.PSObject.Properties.Name -contains 'Parts')) 'No custom chunk parts'
            if ($file.Path -eq 'repetitive.txt') {
                Assert ($entry.CompressedLength -lt $entry.Length / 10) 'Deflate materially shrinks repetitive evidence'
            }
        }
    } finally { $zip.Dispose() }
}
& $tool -ArchiveDirectory $destination -VerifyOnly
$restored = Join-Path $root 'restored'
& $tool -ArchiveDirectory $destination -RestoreDirectory $restored
foreach ($file in Get-ChildItem -LiteralPath $batch -File -Recurse -Force) {
    $relative = [IO.Path]::GetRelativePath($batch, $file.FullName)
    Assert ((Get-FileHash -LiteralPath $file.FullName).Hash -eq
        (Get-FileHash -LiteralPath (Join-Path $restored $relative)).Hash) "Exact restore: $relative"
}
Assert (Test-Path -LiteralPath (Join-Path $restored 'empty-directory') -PathType Container) 'Empty directory preserved'
Expect-Failure { & $tool -SourceDirectory $batch -ArchiveDirectory $destination } 'nonempty'
Expect-Failure { & $tool -ArchiveDirectory $destination -RestoreDirectory $restored } 'nonempty'

$acceptance = New-Batch 'acceptance' 'Acceptance'
Expect-Failure { & $tool -SourceDirectory $acceptance -ArchiveDirectory (Join-Path $archives 'acceptance-denied') -RemoveOriginals } 'Acceptance'
Assert (Test-Path -LiteralPath (Join-Path $acceptance 'artifact-lifecycle.json')) 'Acceptance originals intact'
& $tool -SourceDirectory $acceptance -ArchiveDirectory (Join-Path $archives 'acceptance-kept')

$active = New-Batch 'active'
$activeMarker = Join-Path $active 'artifact-lifecycle.json'
$activeState = Get-Content -LiteralPath $activeMarker -Raw | ConvertFrom-Json
$activeState.status = 'Running'
$activeState | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $activeMarker
Expect-Failure { & $tool -SourceDirectory $active -ArchiveDirectory (Join-Path $archives 'active') -RemoveOriginals } 'Running'
$activeState.status = 'Passed'
$activeState.ownerPid = $PID
$activeState | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $activeMarker
Expect-Failure { & $tool -SourceDirectory $active -ArchiveDirectory (Join-Path $archives 'live-owner') } 'owner is still active'
$nestedSuite = Join-Path $active 'completed-suite'
$null = [IO.Directory]::CreateDirectory($nestedSuite)
'{"build":"passed","run":"passed"}' | Set-Content -LiteralPath (Join-Path $nestedSuite 'result.json')
Expect-Failure { & $tool -SourceDirectory $nestedSuite -ArchiveDirectory (Join-Path $archives 'active-child') -RemoveOriginals } 'managed batch root'
Assert (Test-Path -LiteralPath (Join-Path $nestedSuite 'result.json')) 'Nested result cannot bypass batch lifecycle'
$failed = New-Batch 'failed' 'Full' 'Failed'
& $tool -SourceDirectory $failed -ArchiveDirectory (Join-Path $archives 'failed')
Assert (Test-Path -LiteralPath (Join-Path $failed 'artifact-lifecycle.json')) 'Failed batch archived without deletion'
& $tool -SourceDirectory $failed -ArchiveDirectory (Join-Path $archives 'failed-removed') -RemoveOriginals
Assert (-not (Test-Path -LiteralPath $failed)) 'Explicit removal of completed Full/Failed batch'
$failedRestored = Join-Path $root 'failed-restored'
& $tool -ArchiveDirectory (Join-Path $archives 'failed-removed') -RestoreDirectory $failedRestored
$restoredState = Get-Content -LiteralPath (Join-Path $failedRestored 'artifact-lifecycle.json') -Raw | ConvertFrom-Json
Assert ($restoredState.mode -eq 'Full' -and $restoredState.status -eq 'Failed') 'Failed lifecycle restored exactly'
$parentBatch = New-Batch 'parent'
$nestedAcceptance = New-Batch 'parent/acceptance' 'Acceptance'
Expect-Failure { & $tool -SourceDirectory $parentBatch -ArchiveDirectory (Join-Path $archives 'nested-acceptance') -RemoveOriginals } 'Acceptance'
Assert (Test-Path -LiteralPath (Join-Path $nestedAcceptance 'artifact-lifecycle.json')) 'Nested Acceptance retained'
& $tool -SourceDirectory $parentBatch -ArchiveDirectory (Join-Path $archives 'nested-kept')
$nestedStatePath = Join-Path $nestedAcceptance 'artifact-lifecycle.json'
$nestedState = Get-Content -LiteralPath $nestedStatePath -Raw | ConvertFrom-Json
$nestedState.mode = 'Standard'
$nestedState.status = 'Running'
$nestedState | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $nestedStatePath
Expect-Failure { & $tool -SourceDirectory $parentBatch -ArchiveDirectory (Join-Path $archives 'nested-running') } 'Running'
$legacy = Join-Path $root 'legacy'
$null = [IO.Directory]::CreateDirectory($legacy)
Expect-Failure { & $tool -SourceDirectory $legacy -ArchiveDirectory (Join-Path $archives 'no-result') } 'Missing completed'
'{"build":"passed","run":"not_run"}' | Set-Content -LiteralPath (Join-Path $legacy 'result.json')
Expect-Failure { & $tool -SourceDirectory $legacy -ArchiveDirectory (Join-Path $archives 'not-run') } 'completed'
'{"build":"passed","run":"passed"}' | Set-Content -LiteralPath (Join-Path $legacy 'result.json')
& $tool -SourceDirectory $legacy -ArchiveDirectory (Join-Path $archives 'legacy') -RemoveOriginals
Assert (-not (Test-Path -LiteralPath $legacy)) 'Explicit removal after verification'
& $tool -ArchiveDirectory (Join-Path $archives 'legacy') -RestoreDirectory (Join-Path $root 'legacy-restored')

Expect-Failure { & $tool -SourceDirectory (Join-Path $repo 'tmp') -ArchiveDirectory (Join-Path $archives 'broad') } 'below repository'
Expect-Failure { & $tool -SourceDirectory $repo -ArchiveDirectory (Join-Path $archives 'outside') } 'below repository'
Expect-Failure { & $tool -SourceDirectory $batch -ArchiveDirectory (Join-Path $batch 'archive') } 'Archive must'
Expect-Failure { & $tool -SourceDirectory "$root/../escape" -ArchiveDirectory (Join-Path $archives 'escape') } 'Unsafe path'

# Junctions require no symbolic-link privilege on Windows. Check both descendants
# and source/destination ancestors without ever traversing or deleting their target.
$junctionBatch = New-Batch 'junction'
$link = Join-Path $junctionBatch 'linked'
$null = New-Item -ItemType Junction -Path $link -Target $batch
try {
    Expect-Failure { & $tool -SourceDirectory $junctionBatch -ArchiveDirectory (Join-Path $archives 'junction') } 'Reparse child'
    Expect-Failure { & $tool -SourceDirectory (Join-Path $link 'nested') -ArchiveDirectory (Join-Path $archives 'ancestor') } 'Reparse path'
} finally { Remove-Item -LiteralPath $link -ErrorAction Stop }
$archiveLink = Join-Path $archives 'linked'
$null = New-Item -ItemType Junction -Path $archiveLink -Target $root
try {
    Expect-Failure { & $tool -SourceDirectory $batch -ArchiveDirectory (Join-Path $archiveLink 'escape') } 'Reparse path'
} finally { Remove-Item -LiteralPath $archiveLink -ErrorAction Stop }

# Corrupt a volume, then restore its bytes. Original evidence remains untouched.
$zipPath = $volumes[0].FullName
$saved = [IO.File]::ReadAllBytes($zipPath)
$corrupted = $saved.Clone()
$corrupted[45] = $corrupted[45] -bxor 1
[IO.File]::WriteAllBytes($zipPath, $corrupted)
Expect-Failure { & $tool -ArchiveDirectory $destination -VerifyOnly } 'checksum mismatch'
Assert ((Get-FileHash -LiteralPath (Join-Path $nested 'large.bin')).Hash -eq
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))) 'Tamper leaves originals intact'
[IO.File]::WriteAllBytes($zipPath, $saved)

# Recomputed outer checksums cannot hide wrong file bytes or traversal paths.
$manifestPath = Join-Path $destination 'manifest.json'
$manifestBytes = [IO.File]::ReadAllBytes($manifestPath)
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$manifest.Files[0].SHA256 = '0' * 64
$manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath
Update-Checksums $destination
Expect-Failure { & $tool -ArchiveDirectory $destination -VerifyOnly } 'Full file validation'
[IO.File]::WriteAllBytes($manifestPath, $manifestBytes)
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$manifest.Files[0].Path = '../outside.txt'
$manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath
Update-Checksums $destination
Expect-Failure { & $tool -ArchiveDirectory $destination -RestoreDirectory (Join-Path $root 'unsafe-restore') } 'Unsafe relative'
Assert (-not (Test-Path -LiteralPath (Join-Path $root 'outside.txt'))) 'No traversal output'
[IO.File]::WriteAllBytes($manifestPath, $manifestBytes)
Update-Checksums $destination

$zip = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Update)
try { $null = $zip.CreateEntry('../escape.txt') } finally { $zip.Dispose() }
Update-Checksums $destination
Expect-Failure { & $tool -ArchiveDirectory $destination -VerifyOnly } 'Unsafe relative'
[IO.File]::WriteAllBytes($zipPath, $saved)
Update-Checksums $destination
& $tool -ArchiveDirectory $destination -VerifyOnly

# Read-sharing denies writing/removing while the batch is being copied.
$lockedBatch = New-Batch 'locked'
$lockedPath = Join-Path $lockedBatch 'locked.bin'
[IO.File]::WriteAllText($lockedPath, 'keep')
$handle = [IO.File]::Open($lockedPath, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    Expect-Failure { & $tool -SourceDirectory $lockedBatch -ArchiveDirectory (Join-Path $archives 'locked') -RemoveOriginals } 'used by another process|being used|sharing|process cannot access'
} finally { $handle.Dispose() }
Assert ([IO.File]::ReadAllText($lockedPath) -eq 'keep') 'Copy failure preserves original'

$deleteBatch = New-Batch 'deletion-error'
$blocked = Join-Path $deleteBatch '000-readonly.bin'
$later = Join-Path $deleteBatch '001-retain.bin'
[IO.File]::WriteAllText($blocked, 'synthetic read-only fixture')
[IO.File]::WriteAllText($later, 'must survive earlier deletion error')
[IO.File]::SetAttributes($blocked, [IO.FileAttributes]::ReadOnly)
try {
    # Provider permission errors are localized; the verified archive below proves
    # this reached deletion rather than failing during archive construction.
    Expect-Failure { & $tool -SourceDirectory $deleteBatch -ArchiveDirectory (Join-Path $archives 'deletion-error') -RemoveOriginals } '.+'
    Assert (Test-Path -LiteralPath $later) 'Deletion stops at first error'
    Assert (Test-Path -LiteralPath (Join-Path $deleteBatch 'artifact-lifecycle.json')) 'No subsequent metadata deletion'
    & $tool -ArchiveDirectory (Join-Path $archives 'deletion-error') -VerifyOnly
} finally { [IO.File]::SetAttributes($blocked, [IO.FileAttributes]::Normal) }

@{ assertions = $script:checks; status = 'Passed'; output = $root; archives = $archives } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'archive-tests-result.json') -Encoding utf8
Write-Host "PASS: $script:checks archive assertions. Synthetic outputs retained: $root"
Write-Host "Synthetic archives retained: $archives"
