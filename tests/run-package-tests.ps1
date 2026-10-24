[CmdletBinding()]
param(
    [string]$BinaryPath = (Join-Path $PSScriptRoot '../build/Release/renodx-dlss5.addon64'),
    [string]$HostRoot = (Join-Path $PSScriptRoot '../.local/renodx'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot ('../tmp/package-tests-' + [guid]::NewGuid().ToString('N')))
)

$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskPackage = Join-Path $taskRoot 'tools/package.ps1'
$taskBinary = (Resolve-Path -LiteralPath $BinaryPath).Path
$taskHost = (Resolve-Path -LiteralPath $HostRoot).Path
$taskVersion = & (Join-Path $taskRoot 'tools/check-version.ps1') -BinaryPath $taskBinary
# 仅在新项目的隔离输出下生成测试夹具；不替换真实输入或安装环境。
. (Join-Path $PSScriptRoot 'test-support.ps1')
$taskOutput = Initialize-TestOutput $OutputDirectory
$taskPassed = 0

function Assert-Test {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-PackageRejected {
    param([hashtable]$Arguments, [string]$MessagePattern)
    $rejected = $false
    try { & $taskPackage @Arguments | Out-Null }
    catch {
        if (($_.Exception.Message + ' ' + $_.FullyQualifiedErrorId) -notmatch $MessagePattern) { throw "Wrong rejection: $($_.Exception.Message)" }
        $rejected = $true
    }
    Assert-Test $rejected "Package was not rejected: $MessagePattern"
    $script:taskPassed++
}

$taskArguments = @{
    BinaryPath = $taskBinary
    HostRoot = $taskHost
    OutputDirectory = (Join-Path $taskOutput 'valid')
    ExpectedTag = $taskVersion.Tag
}
$taskResult = & $taskPackage @taskArguments
Assert-Test ($taskResult.Version -ceq $taskVersion.Version -and $taskResult.Tag -ceq $taskVersion.Tag) 'Package version mismatch.'
Assert-Test ([IO.Path]::GetFileName($taskResult.ArchivePath) -ceq ($taskVersion.ArchiveStem + '-windows-x64.zip')) 'ArchiveStem was not reused.'
$taskZip = [IO.Compression.ZipFile]::OpenRead($taskResult.ArchivePath)
try {
    $taskNames = @($taskZip.Entries | ForEach-Object FullName)
    foreach ($taskRequired in @('renodx-dlss5.addon64', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md', 'VERSION.txt')) {
        Assert-Test ($taskNames -ccontains $taskRequired) "Missing package entry: $taskRequired"
    }
    $taskBinaries = @($taskNames | Where-Object { $_ -match '(?i)\.(addon64|dll|exe|pdb|lib|exp|ini|bin|onnx|safetensors)$' })
    Assert-Test ($taskBinaries.Count -eq 1 -and $taskBinaries[0] -ceq 'renodx-dlss5.addon64') 'Unexpected binary/config/model in ZIP.'
    foreach ($taskEntry in $taskZip.Entries) {
        Assert-Test ($taskEntry.FullName -notmatch '\\|(^|/)\.\.(/|$)|^/') 'Invalid archive path.'
        Assert-Test ($taskEntry.Length -gt 0) "Empty ZIP entry: $($taskEntry.FullName)"
    }
    $taskAddonStream = $taskZip.GetEntry('renodx-dlss5.addon64').Open()
    $taskHasher = [Security.Cryptography.SHA256]::Create()
    try { $taskAddonHash = [Convert]::ToHexString($taskHasher.ComputeHash($taskAddonStream)) }
    finally { $taskHasher.Dispose(); $taskAddonStream.Dispose() }
    Assert-Test ($taskAddonHash -ceq (Get-FileHash -LiteralPath $taskBinary).Hash) 'Archived add-on differs from real build.'
    foreach ($taskDocument in @('LICENSE', 'THIRD_PARTY_NOTICES.md')) {
        $taskReader = [IO.StreamReader]::new($taskZip.GetEntry($taskDocument).Open())
        try { $taskText = $taskReader.ReadToEnd() } finally { $taskReader.Dispose() }
        Assert-Test ($taskText -ceq [IO.File]::ReadAllText((Join-Path $taskRoot $taskDocument))) "Document changed: $taskDocument"
    }
    $taskReader = [IO.StreamReader]::new($taskZip.GetEntry('README.md').Open())
    try { $taskReadme = $taskReader.ReadToEnd() } finally { $taskReader.Dispose() }
    $taskCommit = & git --no-optional-locks -C $taskRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify README source commit.' }
    $taskSourceReadme = [IO.File]::ReadAllText((Join-Path $taskRoot 'README.md'))
    $taskSourceLinks = @([regex]::Matches($taskSourceReadme, '\]\(([^\s)]+)\)'))
    $taskPackageLinks = @([regex]::Matches($taskReadme, '\]\(([^\s)]+)\)'))
    Assert-Test ($taskSourceLinks.Count -eq $taskPackageLinks.Count) 'README link count changed.'
    for ($taskIndex = 0; $taskIndex -lt $taskSourceLinks.Count; $taskIndex++) {
        $taskTarget = $taskSourceLinks[$taskIndex].Groups[1].Value
        $taskPackedTarget = $taskPackageLinks[$taskIndex].Groups[1].Value
        $taskPath = ($taskTarget -split '#', 2)[0] -replace '^\./', ''
        if ($taskTarget.StartsWith('#') -or $taskTarget -match '^(?:[a-zA-Z][a-zA-Z0-9+.-]*:|//)' -or
            $taskPath -cin @('LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md')) {
            Assert-Test ($taskPackedTarget -ceq $taskTarget) "Package-local/external link changed: $taskTarget"
        } else {
            $taskExpectedUrl = "https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/blob/$taskCommit/" + ($taskTarget -replace '^\./', '')
            Assert-Test ($taskPackedTarget -ceq $taskExpectedUrl) "Source link not pinned: $taskTarget"
        }
    }
    # 除链接目标外，正文必须与唯一来源完全一致。
    $taskLinkPattern = '(?<=\]\()[^\s)]+(?=\))'
    Assert-Test ([regex]::Replace($taskReadme, $taskLinkPattern, '<link>') -ceq
        [regex]::Replace($taskSourceReadme, $taskLinkPattern, '<link>')) 'README prose changed during packaging.'
    $taskReader = [IO.StreamReader]::new($taskZip.GetEntry('licenses/nlohmann-json/NOTICE.txt').Open())
    try { $taskJsonNotice = $taskReader.ReadToEnd() } finally { $taskReader.Dispose() }
    foreach ($taskCopyright in @('Niels Lohmann', 'Björn Hoehrmann', 'Florian Loitsch', 'Evan Nemerson', 'The Abseil Authors')) {
        Assert-Test $taskJsonNotice.Contains($taskCopyright) "Missing JSON copyright: $taskCopyright"
    }
    $taskReader = [IO.StreamReader]::new($taskZip.GetEntry('licenses/Streamline/NOTICE.txt').Open())
    try { $taskStreamlineNotice = $taskReader.ReadToEnd() } finally { $taskReader.Dispose() }
    Assert-Test ($taskStreamlineNotice -match 'Copyright \(c\) 2022-2023 NVIDIA CORPORATION') 'Streamline header copyright lost.'
} finally { $taskZip.Dispose() }
$taskArchiveHash = (Get-FileHash -LiteralPath $taskResult.ArchivePath).Hash.ToLowerInvariant()
$taskChecksumText = [IO.File]::ReadAllText($taskResult.ChecksumPath)
Assert-Test ($taskChecksumText -ceq "$taskArchiveHash  $([IO.Path]::GetFileName($taskResult.ArchivePath))`n") 'SHA256 sidecar mismatch.'
$taskPassed++

Assert-PackageRejected $taskArguments 'Refusing to overwrite'
Assert-Test ((Get-FileHash -LiteralPath $taskResult.ArchivePath).Hash.ToLowerInvariant() -ceq $taskArchiveHash) 'Existing ZIP changed.'
Assert-Test ([IO.File]::ReadAllText($taskResult.ChecksumPath) -ceq $taskChecksumText) 'Existing checksum changed.'
$taskNegative = $taskArguments.Clone()
$taskNegative.OutputDirectory = Join-Path $taskOutput 'rejected'
$taskNegative.ExpectedTag = 'v999.0.0-package-mismatch'
Assert-PackageRejected $taskNegative 'Tag must equal'
$taskNegative.ExpectedTag = $taskVersion.Tag
$taskNegative.BinaryPath = Join-Path $taskOutput 'missing.addon64'
Assert-PackageRejected $taskNegative 'PathNotFound'
$taskNegative.BinaryPath = $taskBinary
$taskNegative.HostRoot = Join-Path $taskOutput 'missing-host'
Assert-PackageRejected $taskNegative 'PathNotFound'
$taskNegative.HostRoot = $taskHost

$taskFakePath = Join-Path $taskOutput 'fake.addon64'
[IO.File]::WriteAllText($taskFakePath, 'This is not a plugin.')
$taskNegative.BinaryPath = $taskFakePath
Assert-PackageRejected $taskNegative 'not a PE DLL'

$taskBytes = [IO.File]::ReadAllBytes($taskBinary)
$taskPeOffset = [BitConverter]::ToInt32($taskBytes, 0x3c)
$taskX86Bytes = [byte[]]$taskBytes.Clone()
[BitConverter]::GetBytes([uint16]0x14c).CopyTo($taskX86Bytes, $taskPeOffset + 4)
$taskX86Path = Join-Path $taskOutput 'wrong-machine.addon64'
[IO.File]::WriteAllBytes($taskX86Path, $taskX86Bytes)
$taskNegative.BinaryPath = $taskX86Path
Assert-PackageRejected $taskNegative 'requires an AMD64'

# VS_FIXEDFILEINFO 的 Debug 标志模拟误传 Debug 构建，不需另编译或加载 DLL。
$taskDebugBytes = [byte[]]$taskBytes.Clone()
$taskVersionOffsets = @(for ($taskOffset = 0; $taskOffset -le $taskDebugBytes.Length - 52; $taskOffset += 4) {
    if ([BitConverter]::ToUInt32($taskDebugBytes, $taskOffset) -eq [uint32]::Parse('FEEF04BD', [Globalization.NumberStyles]::HexNumber) -and
        [BitConverter]::ToUInt32($taskDebugBytes, $taskOffset + 4) -eq 0x10000) { $taskOffset }
})
Assert-Test ($taskVersionOffsets.Count -eq 1) 'Expected one VS_FIXEDFILEINFO for Debug fixture.'
[BitConverter]::GetBytes([uint32]0x3f).CopyTo($taskDebugBytes, $taskVersionOffsets[0] + 24)
[BitConverter]::GetBytes([uint32]1).CopyTo($taskDebugBytes, $taskVersionOffsets[0] + 28)
$taskDebugPath = Join-Path $taskOutput 'debug.addon64'
[IO.File]::WriteAllBytes($taskDebugPath, $taskDebugBytes)
$taskNegative.BinaryPath = $taskDebugPath
Assert-PackageRejected $taskNegative 'Debug binaries cannot'
Assert-Test (-not (Test-Path -LiteralPath $taskNegative.OutputDirectory)) 'Rejected inputs left package output.'

[pscustomobject]@{ TestsPassed = $taskPassed; ArchivePath = $taskResult.ArchivePath; OutputDirectory = $taskOutput }
