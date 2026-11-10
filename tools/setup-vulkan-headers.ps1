[CmdletBinding()]
param(
    [string]$ToolRoot = (Join-Path $PSScriptRoot '../.local/vulkan-headers')
)

$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskInput = (Get-Content -LiteralPath (Join-Path $taskRoot 'host/tools.lock.json') -Raw | ConvertFrom-Json).vulkan_headers
$taskTools = [IO.Path]::GetFullPath($ToolRoot)
$taskArchive = Join-Path $taskTools ($taskInput.commit + '.zip')
$taskExtract = Join-Path $taskTools ('Vulkan-Headers-' + $taskInput.commit)
$taskInclude = Join-Path $taskExtract 'include'

New-Item -ItemType Directory -Path $taskTools -Force | Out-Null
if (-not (Test-Path -LiteralPath $taskArchive -PathType Leaf)) {
    Invoke-WebRequest -Uri $taskInput.url -OutFile $taskArchive
}
if ((Get-FileHash -LiteralPath $taskArchive -Algorithm SHA256).Hash -cne $taskInput.sha256) {
    throw 'Official Vulkan-Headers archive hash mismatch.'
}
if (-not (Test-Path -LiteralPath (Join-Path $taskInclude 'vulkan/vulkan.h') -PathType Leaf)) {
    Expand-Archive -LiteralPath $taskArchive -DestinationPath $taskTools -Force
}
foreach ($taskHeader in @('vulkan/vulkan.h', 'vulkan/vulkan_core.h', 'vk_video/vulkan_video_codecs_common.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $taskInclude $taskHeader) -PathType Leaf)) {
        throw "Missing Vulkan header: $taskHeader"
    }
}
Write-Host "Vulkan-Headers $($taskInput.version) prepared: $taskInclude"
return $taskInclude
