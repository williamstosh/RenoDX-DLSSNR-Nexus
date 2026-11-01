[CmdletBinding()]
param(
    [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot),
    [Parameter(Mandatory)][ValidatePattern('^[a-f0-9]{40}$')][string]$SourceCommit,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath($SourceRoot)
$taskUtf8 = [Text.UTF8Encoding]::new($false, $true)
$taskRepository = 'https://github.com/williamstosh/RenoDX-DLSSNR-Nexus'
# PowerShell ships Markdig; no package restore or network is needed.
$null = ConvertFrom-Markdown -InputObject '# NR Nexus'
$taskBuilder = [Markdig.MarkdownPipelineBuilder]::new()
$null = [Markdig.MarkdownExtensions]::UsePipeTables($taskBuilder)
$null = [Markdig.MarkdownExtensions]::UseAutoIdentifiers($taskBuilder, [Markdig.Extensions.AutoIdentifiers.AutoIdentifierOptions]::GitHub)
$taskPipeline = $taskBuilder.Build()
$taskDocuments = [ordered]@{ 'README.md' = 'readme'; 'docs/USER_GUIDE.md' = 'guide' }
$taskRendered = @{}
$taskImageCache = @{}
$taskCatalog = Get-Content -LiteralPath (Join-Path $taskRoot 'docs/compare/catalog.json') -Raw -Encoding utf8 | ConvertFrom-Json

function Resolve-GuidePath([string]$Document, [string]$Target) {
    $absolute = [IO.Path]::GetFullPath((Join-Path (Split-Path (Join-Path $taskRoot $Document) -Parent) ([Uri]::UnescapeDataString($Target))))
    $relative = [IO.Path]::GetRelativePath($taskRoot, $absolute).Replace([char]92, [char]47)
    if ($relative -eq '..' -or $relative.StartsWith('../') -or [IO.Path]::IsPathRooted($relative)) {
        throw "Guide path leaves source root: $Target"
    }
    if (-not (Test-Path -LiteralPath $absolute -PathType Leaf)) { throw "Missing guide input: $relative" }
    return $relative
}

function Get-GuideImage([string]$Path) {
    if ($taskImageCache.ContainsKey($Path)) { return $taskImageCache[$Path] }
    $absolute = Join-Path $taskRoot $Path
    if ($Path -cmatch '^docs/images/ui/[a-z-]+\.webp$') {
        $bytes = [IO.File]::ReadAllBytes($absolute)
        if ($bytes.Length -lt 12 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 4) -cne 'RIFF' -or
            [Text.Encoding]::ASCII.GetString($bytes, 8, 4) -cne 'WEBP') { throw "Invalid guide WebP: $Path" }
        $mime = 'image/webp'
    } else {
        # JPEG decoding uses the approved preview's SDR base. Resizing strips the
        # HDR gain map and metadata rather than shipping full-resolution HDR.
        $scenes = @($taskCatalog.scenes | Where-Object { ('docs/compare/' + $_.readme) -ceq $Path })
        if ($scenes.Count -ne 1 -or $scenes[0].status -cne 'approved' -or $scenes[0].synthetic) {
            throw "Guide image is not an approved README preview: $Path"
        }
        Add-Type -AssemblyName System.Drawing.Common
        $image = [Drawing.Image]::FromFile($absolute)
        try {
            $width = [Math]::Min(1280, $image.Width)
            $height = [int][Math]::Round($image.Height * $width / $image.Width)
            $bitmap = [Drawing.Bitmap]::new($width, $height)
            try {
                $graphics = [Drawing.Graphics]::FromImage($bitmap)
                try {
                    $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                    $graphics.DrawImage($image, 0, 0, $width, $height)
                } finally { $graphics.Dispose() }
                $encoder = [Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object MimeType -CEQ 'image/jpeg'
                $parameters = [Drawing.Imaging.EncoderParameters]::new(1)
                $memory = [IO.MemoryStream]::new()
                try {
                    $parameters.Param[0] = [Drawing.Imaging.EncoderParameter]::new([Drawing.Imaging.Encoder]::Quality, [long]85)
                    $bitmap.Save($memory, $encoder, $parameters)
                    $bytes = $memory.ToArray()
                } finally { $memory.Dispose(); $parameters.Dispose() }
            } finally { $bitmap.Dispose() }
        } finally { $image.Dispose() }
        $mime = 'image/jpeg'
    }
    if ($bytes.Length -eq 0) { throw "Empty guide image: $Path" }
    $taskImageCache[$Path] = 'data:' + $mime + ';base64,' + [Convert]::ToBase64String($bytes)
    return $taskImageCache[$Path]
}

foreach ($document in $taskDocuments.Keys) {
    $source = $taskUtf8.GetString([IO.File]::ReadAllBytes((Join-Path $taskRoot $document)))
    if ([string]::IsNullOrWhiteSpace($source)) { throw "Empty guide document: $document" }
    $ast = [Markdig.Markdown]::Parse($source, $taskPipeline)
    $nodes = @([Markdig.Syntax.MarkdownObjectExtensions]::Descendants($ast))
    $rawIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($node in $nodes) {
        $raw = if ($node -is [Markdig.Syntax.HtmlBlock]) { $node.Lines.ToString() }
            elseif ($node -is [Markdig.Syntax.Inlines.HtmlInline]) { $node.Tag } else { $null }
        if ($null -eq $raw) { continue }
        # Source HTML currently consists of comments and named anchors. New HTML
        # must be reviewed instead of silently dropped or made active offline.
        $remaining = [regex]::Replace($raw, '(?s)<!--.*?-->|<a\s+(?:id|name)=["''][^"'']+["'']\s*>|</a\s*>', '')
        if (-not [string]::IsNullOrWhiteSpace($remaining)) { throw "Unsupported raw guide HTML in $($document): $raw" }
        $anchorHtml = [regex]::Replace($raw, '(?s)<!--.*?-->', '')
        foreach ($match in [regex]::Matches($anchorHtml, '(?:id|name)=["'']([^"'']+)["'']')) {
            if (-not $rawIds.Add($match.Groups[1].Value)) { throw "Duplicate explicit guide anchor in $($document): $($match.Groups[1].Value)" }
        }
    }
    $ids = [Collections.Generic.HashSet[string]]::new($rawIds, [StringComparer]::Ordinal)
    foreach ($heading in @($nodes | Where-Object { $_ -is [Markdig.Syntax.HeadingBlock] })) {
        $attributes = [Markdig.Renderers.Html.HtmlAttributesExtensions]::GetAttributes($heading)
        $base = $attributes.Id
        $candidate = $base
        $suffix = 0
        while (-not $ids.Add($candidate)) { $suffix++; $candidate = $base + '-' + $suffix }
        $attributes.Id = $candidate
    }
    $html = [Markdig.Markdown]::ToHtml($ast, $taskPipeline)
    $prefix = $taskDocuments[$document] + '-'
    $html = [regex]::Replace($html, '(\s(?:id|name)=)["'']([^"'']+)["'']', {
        param($m)
        $m.Groups[1].Value + '"' + $prefix + [Net.WebUtility]::HtmlEncode([Net.WebUtility]::HtmlDecode($m.Groups[2].Value)) + '"'
    })
    $html = [regex]::Replace($html, '(?i)(<img\b[^>]*?\ssrc=")([^"]+)(")', {
        param($m)
        $target = [Net.WebUtility]::HtmlDecode($m.Groups[2].Value)
        if ($target -match '^(?:[a-zA-Z][a-zA-Z0-9+.-]*:|//|#)') { throw "Guide image must be a local reviewed asset: $target" }
        $path = Resolve-GuidePath $document $target
        $m.Groups[1].Value + (Get-GuideImage $path) + $m.Groups[3].Value
    })
    $html = [regex]::Replace($html, '(?i)(\shref=")([^"]+)(")', {
        param($m)
        $target = [Net.WebUtility]::HtmlDecode($m.Groups[2].Value)
        $parts = $target -split '#', 2
        if ($target.StartsWith('#')) {
            $resolved = '#' + $prefix + [Uri]::UnescapeDataString($parts[1])
        } elseif ($target -match '^(?:[a-zA-Z][a-zA-Z0-9+.-]*:|//)') {
            if ($target -notmatch '^https?://') { throw "Unsupported guide link scheme: $target" }
            $resolved = $target
        } else {
            $path = Resolve-GuidePath $document $parts[0]
            if ($taskDocuments.Contains($path)) {
                $resolved = '#' + $taskDocuments[$path]
                if ($parts.Count -eq 2 -and $parts[1]) { $resolved += '-' + [Uri]::UnescapeDataString($parts[1]) }
            } else {
                $resolved = $taskRepository + '/blob/' + $SourceCommit + '/' + $path
                if ($parts.Count -eq 2) { $resolved += '#' + $parts[1] }
            }
        }
        $m.Groups[1].Value + [Net.WebUtility]::HtmlEncode($resolved) + $m.Groups[3].Value
    })
    $zhStart = $html.IndexOf('<h2 id="' + $prefix + '简体中文">', [StringComparison]::Ordinal)
    $enStart = $html.IndexOf('<h2 id="' + $prefix + 'english">', [StringComparison]::Ordinal)
    if ($zhStart -lt 0 -or $enStart -le $zhStart) { throw "Expected Chinese then English sections in $document" }
    $taskRendered[$document] = @{ Preamble = $html.Substring(0, $zhStart); Zh = $html.Substring($zhStart, $enStart - $zhStart); En = $html.Substring($enStart) }
}

$taskArticles = [Collections.Generic.List[string]]::new()
$taskNavigation = [Collections.Generic.List[string]]::new()
foreach ($language in @('zh', 'en')) {
    foreach ($document in $taskDocuments.Keys) {
        $name = $taskDocuments[$document]
        $sectionId = if ($language -ceq 'zh') { $name } else { $name + '-en' }
        $title = if ($name -ceq 'readme') { 'README' } elseif ($language -ceq 'zh') { '玩家指南' } else { 'User Guide' }
        $body = if ($language -ceq 'zh') { $taskRendered[$document].Preamble + $taskRendered[$document].Zh } else { $taskRendered[$document].En }
        if ($name -ceq 'readme') {
            $note = if ($language -ceq 'zh') { '离线版游戏图片为已审核原图的 SDR 静态预览。点击图片可联网打开 OFF／ON 交互对比；正文中的下载和外部参考链接也需要网络。' }
                else { 'Game images in this offline edition are static SDR previews of approved originals. Click an image to open the online OFF/ON comparison; downloads and external references also require a connection.' }
            $body = [regex]::Replace($body, '(<p><a id="readme-(?:zh|en)-comparisons"></a></p>)', ('$1<aside class="offline-note">' + $note + '</aside>'))
        }
        $lang = if ($language -ceq 'zh') { 'zh-CN' } else { 'en' }
        $taskArticles.Add("<article id=""$sectionId"" lang=""$lang"" data-source=""$document""><p class=""document-label"">$title</p>$body</article>")
        $links = [Collections.Generic.List[string]]::new()
        foreach ($heading in [regex]::Matches($body, '<h([23]) id="([^"]+)"[^>]*>(.*?)</h[23]>', 'Singleline')) {
            $label = [regex]::Replace($heading.Groups[3].Value, '<[^>]+>', '')
            $links.Add('<li class="toc-level-' + $heading.Groups[1].Value + '"><a href="#' + $heading.Groups[2].Value + '">' + $label + '</a></li>')
        }
        $taskNavigation.Add('<li><a class="toc-document" href="#' + $sectionId + '">' + $title + ' · ' + $lang + '</a><ul>' + ($links -join '') + '</ul></li>')
    }
}
$taskCss = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'offline-guide.css'), $taskUtf8)
$taskHtml = @"
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src data:; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'">
<meta name="source-commit" content="$SourceCommit">
<title>NR Nexus — 离线玩家手册 / Offline Player Guide</title>
<style>$taskCss</style>
</head>
<body>
<a class="skip-link" href="#readme">跳到正文 / Skip to content</a>
<header class="masthead"><a class="brand" href="#readme">NR Nexus</a><span>离线玩家手册 / Offline Player Guide</span><nav aria-label="语言 / Language"><a href="#readme-简体中文">简体中文</a><a href="#readme-english">English</a></nav></header>
<div class="layout"><nav class="contents" aria-label="目录 / Contents"><details open><summary>目录 / Contents</summary><ul>$($taskNavigation -join '')</ul></details></nav>
<main>$($taskArticles -join [Environment]::NewLine)</main></div>
<footer><a href="#readme">NR Nexus</a> · <a href="#guide">玩家指南</a> · <a href="#guide-en">User Guide</a></footer>
</body></html>
"@
# Validate final IDs and internal links after namespacing and reordering.
$taskIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$taskCheckHtml = [regex]::Replace($taskHtml, '(?s)<!--.*?-->', '')
foreach ($match in [regex]::Matches($taskCheckHtml, '\s(?:id|name)="([^"]+)"')) {
    $id = [Net.WebUtility]::HtmlDecode($match.Groups[1].Value)
    if (-not $taskIds.Add($id)) { throw "Duplicate offline guide anchor: $id" }
}
foreach ($match in [regex]::Matches($taskCheckHtml, '\shref="#([^"]+)"')) {
    $id = [Uri]::UnescapeDataString([Net.WebUtility]::HtmlDecode($match.Groups[1].Value))
    if (-not $taskIds.Contains($id)) { throw "Broken offline guide anchor: $id" }
}
$taskBytes = $taskUtf8.GetBytes($taskHtml)
if ($OutputPath) {
    $stream = [IO.File]::Open([IO.Path]::GetFullPath($OutputPath), [IO.FileMode]::CreateNew)
    try { $stream.Write($taskBytes) } finally { $stream.Dispose() }
}
[pscustomobject]@{ Bytes = $taskBytes; ImageCount = $taskImageCache.Count; SourceCommit = $SourceCommit }
