[CmdletBinding()]
param(
    [string]$BinaryPath = (Join-Path $PSScriptRoot '../build/Release/renodx-dlss5.addon64'),
    [string]$HostRoot = (Join-Path $PSScriptRoot '../.local/renodx'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../dist'),
    [string]$ExpectedTag
)

$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskBinary = (Resolve-Path -LiteralPath $BinaryPath).Path
$taskHost = (Resolve-Path -LiteralPath $HostRoot).Path
$taskOutput = [IO.Path]::GetFullPath($OutputDirectory)

# 不执行 DLL；直接检查 PE 头及版本资源。Debug 资源不可作为发布输入。
$taskBytes = [IO.File]::ReadAllBytes($taskBinary)
if ($taskBytes.Length -lt 64 -or [BitConverter]::ToUInt16($taskBytes, 0) -ne 0x5a4d) {
    throw 'Package input is not a PE DLL.'
}
$taskPeOffset = [BitConverter]::ToInt32($taskBytes, 0x3c)
if ($taskPeOffset -lt 64 -or $taskPeOffset -gt $taskBytes.Length - 26 -or
    [BitConverter]::ToUInt32($taskBytes, $taskPeOffset) -ne 0x4550 -or
    [BitConverter]::ToUInt16($taskBytes, $taskPeOffset + 4) -ne 0x8664 -or
    [BitConverter]::ToUInt16($taskBytes, $taskPeOffset + 24) -ne 0x20b -or
    ([BitConverter]::ToUInt16($taskBytes, $taskPeOffset + 22) -band 0x2000) -eq 0) {
    throw 'Package requires an AMD64 PE32+ DLL.'
}
if ([Diagnostics.FileVersionInfo]::GetVersionInfo($taskBinary).IsDebug) {
    throw 'Debug binaries cannot be packaged for release.'
}
$taskVersion = & (Join-Path $PSScriptRoot 'check-version.ps1') -BinaryPath $taskBinary -ExpectedTag $ExpectedTag
$taskCommit = & git --no-optional-locks -C $taskRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source commit.' }
$taskLock = Get-Content -LiteralPath (Join-Path $taskRoot 'host/lock.json') -Raw | ConvertFrom-Json

function Assert-PinnedRepository {
    param([string]$Repository, [string]$Commit)
    $actual = & git --no-optional-locks -C $Repository rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $actual -cne $Commit) { throw "Unpinned license source: $Repository" }
}

function Read-PinnedFile {
    param([string]$Repository, [string]$Commit, [string]$Path)
    # 直接读固定提交的 Git blob，避免稀疏检出遗漏许可，或工作文件改动影响原文。
    $start = [Diagnostics.ProcessStartInfo]::new('git')
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in @('--no-optional-locks', '-C', $Repository, 'show', "${Commit}:$Path")) {
        $start.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($start)
    $errorTask = $process.StandardError.ReadToEndAsync()
    $memory = [IO.MemoryStream]::new()
    try {
        $process.StandardOutput.BaseStream.CopyTo($memory)
        $process.WaitForExit()
        $message = $errorTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0 -or $memory.Length -eq 0) { throw "Missing license input ${Path}: $message" }
        return ,$memory.ToArray()
    } finally {
        $memory.Dispose()
        $process.Dispose()
    }
}

$taskFiles = [ordered]@{}
$taskFiles['renodx-dlss5.addon64'] = $taskBytes
foreach ($taskFile in @('LICENSE', 'THIRD_PARTY_NOTICES.md')) {
    $taskFiles[$taskFile] = [IO.File]::ReadAllBytes((Join-Path $taskRoot $taskFile))
    if ($taskFiles[$taskFile].Length -eq 0) { throw "Empty package input: $taskFile" }
}
$taskDocuments = [ordered]@{ 'README.md' = 'README.md'; 'docs/USER_GUIDE.md' = 'USER_GUIDE.md' }
$taskDocumentInputs = @{}
foreach ($taskDocument in $taskDocuments.Keys) {
    $taskDocumentPath = Join-Path $taskRoot $taskDocument
    if (-not (Test-Path -LiteralPath $taskDocumentPath -PathType Leaf)) { throw "Missing package input: $taskDocument" }
    $taskDocumentInputs[$taskDocument] = [Text.UTF8Encoding]::new($false, $true).GetString([IO.File]::ReadAllBytes($taskDocumentPath))
    if ([string]::IsNullOrWhiteSpace($taskDocumentInputs[$taskDocument])) { throw "Empty package input: $taskDocument" }
}
$taskImageNames = @('quality', 'multipass', 'results', 'diagnostics', 'advanced', 'themes', 'presets',
    'quality-zh', 'multipass-zh', 'results-zh', 'diagnostics-zh', 'advanced-zh', 'themes-zh', 'presets-zh')
$taskLocalPaths = [ordered]@{
    'LICENSE' = 'LICENSE'; 'THIRD_PARTY_NOTICES.md' = 'THIRD_PARTY_NOTICES.md'
    'README.md' = 'README.md'; 'docs/USER_GUIDE.md' = 'USER_GUIDE.md'
}
foreach ($taskImage in $taskImageNames) {
    $taskSourcePath = "docs/images/ui/$taskImage.webp"
    $taskDestination = "images/ui/$taskImage.webp"
    $taskLocalPaths[$taskSourcePath] = $taskDestination
    $taskFiles[$taskDestination] = [IO.File]::ReadAllBytes((Join-Path $taskRoot $taskSourcePath))
    if ($taskFiles[$taskDestination].Length -eq 0) { throw "Empty package input: $taskSourcePath" }
}
# 文档来自同一源码目录，仅重定位链接；包外源码链接固定到本次完整提交。
foreach ($taskDocument in $taskDocuments.Keys) {
    $taskText = $taskDocumentInputs[$taskDocument]
    $taskPackedText = [regex]::Replace($taskText, '(\]\()([^\s)]+)(\))', {
        param($match)
        $target = $match.Groups[2].Value
        if ($target.StartsWith('#') -or $target -match '^(?:[a-zA-Z][a-zA-Z0-9+.-]*:|//)') {
            return $match.Value
        }
        $parts = $target -split '#', 2
        $sourcePath = [IO.Path]::GetRelativePath($taskRoot,
            [IO.Path]::GetFullPath((Join-Path (Split-Path (Join-Path $taskRoot $taskDocument) -Parent) $parts[0]))) -replace '\\', '/'
        $fragment = if ($parts.Count -eq 2) { '#' + $parts[1] } else { '' }
        if ($sourcePath -cin @($taskLocalPaths.Keys)) {
            return $match.Groups[1].Value + $taskLocalPaths[$sourcePath] + $fragment + $match.Groups[3].Value
        }
        return $match.Groups[1].Value + "https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/blob/$taskCommit/" +
            $sourcePath + $fragment + $match.Groups[3].Value
    })
    $taskFiles[$taskDocuments[$taskDocument]] = [Text.Encoding]::UTF8.GetBytes($taskPackedText)
}
Assert-PinnedRepository $taskHost $taskLock.host.commit
$taskFiles['licenses/RenoDX/LICENSE'] = Read-PinnedFile $taskHost $taskLock.host.commit 'LICENSE'
$taskHostNotices = foreach ($taskHeader in @(
    'src/utils/directx.hpp', 'src/utils/float16.hpp', 'src/utils/path.hpp', 'src/utils/vtable.hpp',
    'src/utils/detour.hpp', 'src/utils/log.hpp', 'src/utils/format.hpp', 'src/utils/png.hpp'
)) {
    $taskText = [Text.Encoding]::UTF8.GetString((Read-PinnedFile $taskHost $taskLock.host.commit $taskHeader))
    $taskNotice = [regex]::Match($taskText, '(?s)\A\s*(/\*.*?\*/)')
    if (-not $taskNotice.Success -or $taskNotice.Value -notmatch 'Copyright') { throw "Missing RenoDX copyright: $taskHeader" }
    "$taskHeader`n$($taskNotice.Groups[1].Value)"
}
$taskFiles['licenses/RenoDX/NOTICE.txt'] = [Text.Encoding]::UTF8.GetBytes(($taskHostNotices -join "`n`n") + "`n")
$taskLicenseInputs = @(
    @('external/Detours', 'LICENSE.md', 'licenses/Detours/LICENSE.md'),
    @('external/reshade', 'LICENSE.md', 'licenses/ReShade/LICENSE.md'),
    @('external/reshade/deps/imgui', 'LICENSE.txt', 'licenses/DearImGui/LICENSE.txt'),
    @('external/json', 'LICENSE.MIT', 'licenses/nlohmann-json/LICENSE.MIT'),
    @('external/Streamline', 'license.txt', 'licenses/Streamline/license.txt'),
    @('external/DLSS', 'LICENSE.txt', 'licenses/NVIDIA-DLSS/LICENSE.txt')
)
foreach ($taskInput in $taskLicenseInputs) {
    $taskDependency = @($taskLock.dependencies | Where-Object path -CEQ $taskInput[0])
    if ($taskDependency.Count -ne 1) { throw "Missing dependency lock: $($taskInput[0])" }
    $taskRepository = Join-Path $taskHost $taskInput[0]
    Assert-PinnedRepository $taskRepository $taskDependency[0].commit
    $taskFiles[$taskInput[2]] = Read-PinnedFile $taskRepository $taskDependency[0].commit $taskInput[1]
}

# 根许可未列全的版权从实际使用的头文件取原文；不改写、翻译法律文本。
$taskJson = $taskLock.dependencies | Where-Object path -CEQ 'external/json'
$taskJsonNotices = foreach ($taskHeader in @(
    'include/nlohmann/json.hpp',
    'include/nlohmann/detail/output/serializer.hpp',
    'include/nlohmann/detail/conversions/to_chars.hpp',
    'include/nlohmann/detail/meta/cpp_future.hpp',
    'include/nlohmann/thirdparty/hedley/hedley.hpp'
)) {
    $taskText = [Text.Encoding]::UTF8.GetString((Read-PinnedFile (Join-Path $taskHost $taskJson.path) $taskJson.commit $taskHeader))
    $taskLines = @([regex]::Matches($taskText, '(?m)^// SPDX-(?:FileCopyrightText|License-Identifier):[^\r\n]+') | ForEach-Object Value)
    if (-not $taskLines.Count) { throw "Missing JSON copyright: $taskHeader" }
    "$taskHeader`n$($taskLines -join "`n")"
}
$taskFiles['licenses/nlohmann-json/NOTICE.txt'] = [Text.Encoding]::UTF8.GetBytes(($taskJsonNotices -join "`n`n") + "`n")
$taskStreamline = $taskLock.dependencies | Where-Object path -CEQ 'external/Streamline'
$taskStreamlineNotices = foreach ($taskHeader in @(
    'sl_core_api.h', 'sl_struct.h', 'sl_consts.h', 'sl_version.h', 'sl_result.h',
    'sl_appidentity.h', 'sl_device_wrappers.h', 'sl_core_types.h', 'sl.h', 'sl_dlss.h', 'sl_dlss_g.h'
)) {
    $taskText = [Text.Encoding]::UTF8.GetString((Read-PinnedFile (Join-Path $taskHost $taskStreamline.path) $taskStreamline.commit "include/$taskHeader"))
    $taskNotice = [regex]::Matches($taskText, '(?s)/\*.*?\*/') | Where-Object Value -Match 'Copyright' | Select-Object -First 1
    # 部分短接口头没有版权块；它们仍由同包的上游根许可覆盖。
    if ($taskNotice) { "include/$taskHeader`n$($taskNotice.Value)" }
}
$taskFiles['licenses/Streamline/NOTICE.txt'] = [Text.Encoding]::UTF8.GetBytes(($taskStreamlineNotices -join "`n`n") + "`n")
$taskFiles['VERSION.txt'] = [Text.Encoding]::UTF8.GetBytes("Version: $($taskVersion.Version)`nTag: $($taskVersion.Tag)`nSource: $taskCommit`n")

# 仅从显式白名单创建归档，不递归复制构建目录或游戏运行环境。
$taskArchive = Join-Path $taskOutput ($taskVersion.ArchiveStem + '-windows-x64.zip')
$taskChecksum = $taskArchive + '.sha256'
if ((Test-Path -LiteralPath $taskArchive) -or (Test-Path -LiteralPath $taskChecksum)) {
    throw 'Refusing to overwrite an existing package or checksum.'
}
[IO.Directory]::CreateDirectory($taskOutput) | Out-Null
$taskCreatedArchive = $false
$taskCreatedChecksum = $false
try {
    $taskStream = [IO.File]::Open($taskArchive, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $taskCreatedArchive = $true
    try {
        $taskZip = [IO.Compression.ZipArchive]::new($taskStream, [IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            foreach ($taskName in $taskFiles.Keys) {
                $taskEntry = $taskZip.CreateEntry($taskName, [IO.Compression.CompressionLevel]::Optimal)
                $taskEntryStream = $taskEntry.Open()
                try { $taskEntryStream.Write($taskFiles[$taskName]) } finally { $taskEntryStream.Dispose() }
            }
        } finally { $taskZip.Dispose() }
    } finally { $taskStream.Dispose() }
    $taskZip = [IO.Compression.ZipFile]::OpenRead($taskArchive)
    try {
        $taskNames = @($taskZip.Entries | ForEach-Object FullName)
        if ($taskNames.Count -ne $taskFiles.Count -or
            (Compare-Object @($taskFiles.Keys) $taskNames -CaseSensitive)) { throw 'ZIP entry whitelist mismatch.' }
        $taskEntryStream = $taskZip.GetEntry('renodx-dlss5.addon64').Open()
        try {
            $taskHasher = [Security.Cryptography.SHA256]::Create()
            try { $taskEmbeddedHash = [Convert]::ToHexString($taskHasher.ComputeHash($taskEntryStream)) }
            finally { $taskHasher.Dispose() }
        } finally { $taskEntryStream.Dispose() }
        if ($taskEmbeddedHash -cne [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($taskBytes))) {
            throw 'ZIP add-on hash mismatch.'
        }
    } finally { $taskZip.Dispose() }
    $taskHash = (Get-FileHash -LiteralPath $taskArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    $taskChecksumStream = [IO.File]::Open($taskChecksum, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $taskCreatedChecksum = $true
    try {
        $taskChecksumBytes = [Text.Encoding]::UTF8.GetBytes("$taskHash  $([IO.Path]::GetFileName($taskArchive))`n")
        $taskChecksumStream.Write($taskChecksumBytes)
    } finally { $taskChecksumStream.Dispose() }
} catch {
    # 失败只撤回本次创建的不完整产物，不触碰已存在的发布包。
    if ($taskCreatedChecksum) { [IO.File]::Delete($taskChecksum) }
    if ($taskCreatedArchive) { [IO.File]::Delete($taskArchive) }
    throw
}
[pscustomobject]@{
    Version = $taskVersion.Version
    Tag = $taskVersion.Tag
    ArchivePath = $taskArchive
    ChecksumPath = $taskChecksum
}
