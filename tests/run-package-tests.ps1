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

function Read-ZipText([string]$Name) {
    $reader = [IO.StreamReader]::new($taskZip.GetEntry($Name).Open(), [Text.UTF8Encoding]::new($false, $true))
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}

function Assert-DocumentLinks([string]$Name, [string]$Text, [hashtable]$Documents, [string[]]$Paths, [bool]$Source = $false) {
    $chineseStart = $Text.IndexOf('## 简体中文', [StringComparison]::Ordinal)
    Assert-Test ($chineseStart -ge 0) "Chinese section missing in $Name"
    $englishImages = @()
    $chineseImages = @()
    foreach ($image in [regex]::Matches($Text, '!\[[^\]]*\]\(([^\s)]+)\)')) {
        $file = [IO.Path]::GetFileName($image.Groups[1].Value)
        $chinese = $image.Index -gt $chineseStart
        Assert-Test (($file -cmatch '-zh\.webp$') -eq $chinese) "Image language mismatch in ${Name}: $file"
        if ($chinese) { $chineseImages += $file -creplace '-zh\.webp$', '.webp' }
        else { $englishImages += $file }
    }
    Assert-Test ($englishImages.Count -gt 0 -and $englishImages.Count -eq $chineseImages.Count -and
        -not (Compare-Object $englishImages $chineseImages -CaseSensitive)) "Unpaired language images in $Name"
    foreach ($link in [regex]::Matches($Text, '\]\(([^\s)]+)\)')) {
        $target = $link.Groups[1].Value
        if ($target -match '^(?:[a-zA-Z][a-zA-Z0-9+.-]*:|//)') { continue }
        $parts = $target -split '#', 2
        $path = if ($parts[0]) {
            [IO.Path]::GetRelativePath($taskRoot, [IO.Path]::GetFullPath(
                (Join-Path (Split-Path (Join-Path $taskRoot $Name) -Parent) $parts[0]))) -replace '\\', '/'
        } else { $Name }
        $exists = if ($Source) { Test-Path -LiteralPath (Join-Path $taskRoot $path) } else { $Paths -ccontains $path }
        Assert-Test $exists "Broken relative link in ${Name}: $target"
        if ($parts.Count -ne 2 -or -not $parts[1]) { continue }
        Assert-Test ($Documents.ContainsKey($path)) "Unverified anchor target in ${Name}: $target"
        $anchors = [Collections.Generic.List[string]]::new()
        $duplicates = @{}
        foreach ($heading in [regex]::Matches($Documents[$path], '(?m)^#{1,6}\s+(.+?)\s*#*\r?$')) {
            $slug = $heading.Groups[1].Value.ToLowerInvariant()
            $slug = [regex]::Replace($slug, '\[([^\]]+)\]\([^)]*\)', '$1')
            $slug = [regex]::Replace($slug, '<[^>]+>', '')
            $slug = [regex]::Replace($slug, '[^\p{L}\p{M}\p{N}_\-\s]', '') -replace '\s', '-'
            $base = $slug
            if ($duplicates.ContainsKey($base)) { $duplicates[$base]++; $slug += '-' + $duplicates[$base] }
            else { $duplicates[$base] = 0 }
            $anchors.Add($slug)
        }
        foreach ($anchor in [regex]::Matches($Documents[$path], '<(?:a|h[1-6])\b[^>]*(?:id|name)=["'']([^"'']+)["'']')) {
            $anchors.Add($anchor.Groups[1].Value)
        }
        Assert-Test ($anchors -ccontains [Uri]::UnescapeDataString($parts[1])) "Broken anchor in ${Name}: $target"
    }
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
    $taskExpected = @('renodx-dlss5.addon64', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md', 'USER_GUIDE.md', 'VERSION.txt',
        'licenses/RenoDX/LICENSE', 'licenses/RenoDX/NOTICE.txt', 'licenses/Detours/LICENSE.md',
        'licenses/ReShade/LICENSE.md', 'licenses/DearImGui/LICENSE.txt', 'licenses/nlohmann-json/LICENSE.MIT',
        'licenses/nlohmann-json/NOTICE.txt', 'licenses/Streamline/license.txt', 'licenses/Streamline/NOTICE.txt',
        'licenses/NVIDIA-DLSS/LICENSE.txt')
    $taskImageNames = @('quality', 'multipass', 'results', 'diagnostics', 'advanced', 'themes',
        'quality-zh', 'multipass-zh', 'results-zh', 'diagnostics-zh', 'advanced-zh', 'themes-zh')
    $taskExpected += @($taskImageNames | ForEach-Object { "images/ui/$_.webp" })
    Assert-Test ($taskNames.Count -eq $taskExpected.Count -and
        -not (Compare-Object $taskExpected $taskNames -CaseSensitive)) 'Complete ZIP whitelist mismatch.'
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
    $taskCommit = & git --no-optional-locks -C $taskRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify README source commit.' }
    Assert-Test ((Read-ZipText 'VERSION.txt') -ceq "Version: $($taskVersion.Version)`nTag: $($taskVersion.Tag)`nSource: $taskCommit`n") 'Package source identity mismatch.'
    $taskMapping = @{'README.md'='README.md'; 'docs/USER_GUIDE.md'='USER_GUIDE.md';
        'LICENSE'='LICENSE'; 'THIRD_PARTY_NOTICES.md'='THIRD_PARTY_NOTICES.md'}
    foreach ($taskImage in $taskImageNames) {
        $taskMapping["docs/images/ui/$taskImage.webp"] = "images/ui/$taskImage.webp"
        $taskStream = $taskZip.GetEntry("images/ui/$taskImage.webp").Open()
        $taskMemory = [IO.MemoryStream]::new()
        try { $taskStream.CopyTo($taskMemory); $taskImageBytes = $taskMemory.ToArray() }
        finally { $taskStream.Dispose(); $taskMemory.Dispose() }
        Assert-Test ([Convert]::ToBase64String($taskImageBytes) -ceq [Convert]::ToBase64String(
            [IO.File]::ReadAllBytes((Join-Path $taskRoot "docs/images/ui/$taskImage.webp")))) "Image source mismatch: $taskImage"
    }
    $taskSourceDocuments = @{}
    $taskPackedDocuments = @{}
    foreach ($taskSourceDocument in @('README.md', 'docs/USER_GUIDE.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md')) {
        $taskSourceDocuments[$taskSourceDocument] = [IO.File]::ReadAllText(
            (Join-Path $taskRoot $taskSourceDocument), [Text.UTF8Encoding]::new($false, $true))
        $taskPackedDocuments[$taskMapping[$taskSourceDocument]] = Read-ZipText $taskMapping[$taskSourceDocument]
    }
    foreach ($taskSourceDocument in @('README.md', 'docs/USER_GUIDE.md')) {
        $taskSourceText = $taskSourceDocuments[$taskSourceDocument]
        $taskPackedText = $taskPackedDocuments[$taskMapping[$taskSourceDocument]]
        Assert-Test (-not [string]::IsNullOrWhiteSpace($taskPackedText)) "Empty document: $taskSourceDocument"
        Assert-Test ($taskSourceText -match '[\u4e00-\u9fff]' -and $taskPackedText -match '[\u4e00-\u9fff]') "Chinese UTF-8 content missing: $taskSourceDocument"
        $taskSourceLinks = @([regex]::Matches($taskSourceText, '\]\(([^\s)]+)\)'))
        $taskPackageLinks = @([regex]::Matches($taskPackedText, '\]\(([^\s)]+)\)'))
        Assert-Test ($taskSourceLinks.Count -eq $taskPackageLinks.Count) "Link count changed: $taskSourceDocument"
        for ($taskIndex = 0; $taskIndex -lt $taskSourceLinks.Count; $taskIndex++) {
            $taskTarget = $taskSourceLinks[$taskIndex].Groups[1].Value
            $taskExpectedTarget = $taskTarget
            if (-not ($taskTarget.StartsWith('#') -or $taskTarget -match '^(?:[a-zA-Z][a-zA-Z0-9+.-]*:|//)')) {
                $taskParts = $taskTarget -split '#', 2
                $taskPath = [IO.Path]::GetRelativePath($taskRoot, [IO.Path]::GetFullPath(
                    (Join-Path (Split-Path (Join-Path $taskRoot $taskSourceDocument) -Parent) $taskParts[0]))) -replace '\\', '/'
                $taskExpectedTarget = if ($taskMapping.ContainsKey($taskPath)) { $taskMapping[$taskPath] }
                    else { "https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/blob/$taskCommit/$taskPath" }
                if ($taskParts.Count -eq 2) { $taskExpectedTarget += '#' + $taskParts[1] }
            }
            Assert-Test ($taskPackageLinks[$taskIndex].Groups[1].Value -ceq $taskExpectedTarget) "Wrong packaged link: $taskTarget"
        }
        # 除链接目标外，文档正文必须与本次打包源码完全一致。
        $taskLinkPattern = '(?<=\]\()[^\s)]+(?=\))'
        Assert-Test ([regex]::Replace($taskPackedText, $taskLinkPattern, '<link>') -ceq
            [regex]::Replace($taskSourceText, $taskLinkPattern, '<link>')) "Document source mismatch: $taskSourceDocument"
        Assert-DocumentLinks $taskSourceDocument $taskSourceText $taskSourceDocuments @($taskMapping.Keys) $true
        Assert-DocumentLinks $taskMapping[$taskSourceDocument] $taskPackedText $taskPackedDocuments $taskNames
        # 原问题复现：链接仍有效，但中文章节误用了英文图片，必须拒绝。
        $taskWrongLanguage = $taskSourceText -creplace '-zh\.webp', '.webp'
        $taskRejectedLanguage = $false
        try { Assert-DocumentLinks $taskSourceDocument $taskWrongLanguage $taskSourceDocuments @($taskMapping.Keys) $true }
        catch {
            if ($_.Exception.Message -notmatch 'Image language mismatch') { throw }
            $taskRejectedLanguage = $true
        }
        Assert-Test $taskRejectedLanguage "Mixed-language screenshots were accepted in $taskSourceDocument"
    }
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

# 复制最小源码夹具以验证文档门禁；不删除或改写生产指南。
$taskFixture = Join-Path $taskOutput 'missing-guide-source'
foreach ($taskDirectory in @('tools', 'host', 'docs')) {
    [IO.Directory]::CreateDirectory((Join-Path $taskFixture $taskDirectory)) | Out-Null
}
foreach ($taskFile in @('tools/package.ps1', 'tools/check-version.ps1', 'host/lock.json',
    'version.h', 'brand.h', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md')) {
    Copy-Item -LiteralPath (Join-Path $taskRoot $taskFile) -Destination (Join-Path $taskFixture $taskFile)
}
[IO.File]::WriteAllText((Join-Path $taskFixture 'dlssnr.hpp'), '#include "version.h"' + "`n" + 'constexpr char kAddonVersion[] = "v" NR_NEXUS_VERSION;')
$taskOriginalPackage = $taskPackage
try {
    $taskPackage = Join-Path $taskFixture 'tools/package.ps1'
    $taskNegative.BinaryPath = $taskBinary
    Assert-PackageRejected $taskNegative 'Missing package input: docs/USER_GUIDE.md'
    foreach ($taskEmptyGuide in @('', " `r`n`t")) {
        [IO.File]::WriteAllText((Join-Path $taskFixture 'docs/USER_GUIDE.md'), $taskEmptyGuide)
        Assert-PackageRejected $taskNegative 'Empty package input: docs/USER_GUIDE.md'
    }
    Assert-Test (-not (Test-Path -LiteralPath $taskNegative.OutputDirectory)) 'Missing/empty guide left package output.'
} finally { $taskPackage = $taskOriginalPackage }

[pscustomobject]@{ TestsPassed = $taskPassed; ArchivePath = $taskResult.ArchivePath; OutputDirectory = $taskOutput }
