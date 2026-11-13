[CmdletBinding()]
param(
    [byte[]]$GuideBytes,
    [string]$SourceCommit = '0123456789abcdef0123456789abcdef01234567',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot ('../tmp/offline-guide-tests-' + [guid]::NewGuid().ToString('N')))
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskGenerator = Join-Path $taskRoot 'tools/build-offline-guide.ps1'
$taskUtf8 = [Text.UTF8Encoding]::new($false, $true)
. (Join-Path $PSScriptRoot 'test-support.ps1')
$taskOutput = Initialize-TestOutput $OutputDirectory
$taskPassed = 0
function Assert-Guide([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:taskPassed++
}
function Get-Text([string]$Html) {
    $plain = [regex]::Replace($Html, '(?s)<!--.*?-->|<[^>]+>', '')
    return [regex]::Replace([Net.WebUtility]::HtmlDecode($plain), '\s+', ' ').Trim()
}
function Get-Attribute([string]$Tag, [string]$Name) {
    return [Net.WebUtility]::HtmlDecode([regex]::Match($Tag, ('\s' + $Name + '="([^"]*)"')).Groups[1].Value)
}
$taskGenerated = & $taskGenerator -SourceCommit $SourceCommit
if ($GuideBytes) {
    Assert-Guide ([Convert]::ToBase64String($GuideBytes) -ceq [Convert]::ToBase64String($taskGenerated.Bytes)) 'Packaged HTML differs from the source-generated HTML.'
} else { $GuideBytes = $taskGenerated.Bytes }
$taskHtml = $taskUtf8.GetString($GuideBytes)
Assert-Guide ($taskHtml.StartsWith('<!doctype html>') -and $taskHtml.EndsWith('</body></html>')) 'Incomplete HTML document.'
Assert-Guide ($taskHtml.Contains('<meta charset="utf-8">') -and $taskHtml.Contains('<html lang="zh-CN">')) 'Chinese UTF-8 identity missing.'
Assert-Guide ($taskHtml.Contains('<meta name="source-commit" content="' + $SourceCommit + '">')) 'HTML source identity mismatch.'
Assert-Guide ($taskHtml -notmatch [char]0xfffd) 'Replacement characters in UTF-8 HTML.'
Assert-Guide ($taskHtml -notmatch '(?i)<(?:script|iframe|object|embed|link|base|form|video|audio)\b|\son\w+\s*=|\ssrcset\s*=|@import|url\s*\(') 'HTML has active content or an external resource dependency.'
Assert-Guide ($taskHtml.Contains("default-src 'none'; img-src data:; style-src 'unsafe-inline'")) 'Offline content policy missing.'
Assert-Guide ([regex]::Matches($taskHtml, '<style>').Count -eq 1 -and $taskHtml.Contains('@media print') -and $taskHtml.Contains('@media (max-width: 900px)')) 'Embedded responsive/print CSS missing.'
$taskIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($match in [regex]::Matches($taskHtml, '\s(?:id|name)="([^"]+)"')) {
    Assert-Guide ($taskIds.Add([Net.WebUtility]::HtmlDecode($match.Groups[1].Value))) ('Duplicate ID: ' + $match.Groups[1].Value)
}
foreach ($match in [regex]::Matches($taskHtml, '\shref="([^"]+)"')) {
    $target = [Net.WebUtility]::HtmlDecode($match.Groups[1].Value)
    if ($target.StartsWith('#')) {
        Assert-Guide ($taskIds.Contains([Uri]::UnescapeDataString($target.Substring(1)))) "Broken internal navigation: $target"
    } else {
        Assert-Guide ($target -cmatch '^https?://') "Non-independent local link: $target"
    }
}
$taskArticles = @([regex]::Matches($taskHtml, '<article\b[^>]*>.*?</article>', 'Singleline'))
Assert-Guide ($taskArticles.Count -eq 4) 'Expected four complete language/document sections.'
$taskExpectedSections = @('readme', 'guide', 'readme-en', 'guide-en')
for ($index = 0; $index -lt 4; $index++) {
    Assert-Guide ((Get-Attribute $taskArticles[$index].Value 'id') -ceq $taskExpectedSections[$index]) 'Chinese must precede all English content.'
}
$taskDocuments = [ordered]@{ 'README.md' = 'readme'; 'docs/USER_GUIDE.md' = 'guide' }
foreach ($document in $taskDocuments.Keys) {
    # Independent stock Markdown rendering checks the complete visible text, not
    # a hand-maintained selection of paragraphs or implementation snapshots.
    $source = [IO.File]::ReadAllText((Join-Path $taskRoot $document), $taskUtf8)
    if ($document -ceq 'README.md') {
        Assert-Guide ($source.Contains('<h1 align="center"><img src="docs/images/logo.svg" alt="NR-Nexus" width="680"></h1>')) 'GitHub README logo missing.'
        $source = $source.Replace('<h1 align="center"><img src="docs/images/logo.svg" alt="NR-Nexus" width="680"></h1>', "# NR Nexus`n`n**DLSS Neural Rendering Add-on for ReShade**")
        Assert-Guide ($taskHtml.Contains('>NR Nexus</h1>') -and $taskHtml.Contains('<strong>DLSS Neural Rendering Add-on for ReShade</strong>') -and -not $taskHtml.Contains('docs/images/logo.svg')) 'Offline logo text fallback missing.'
    }
    $sourceHtml = (ConvertFrom-Markdown -InputObject $source).Html
    $sourceEn = [regex]::Match($sourceHtml, '<h2\b[^>]*>English</h2>').Index
    Assert-Guide ($sourceEn -gt 0) "Missing source language boundary: $document"
    foreach ($language in @('zh', 'en')) {
        $name = $taskDocuments[$document]
        $id = if ($language -ceq 'zh') { $name } else { $name + '-en' }
        $actual = @($taskArticles | Where-Object { (Get-Attribute $_.Value 'id') -ceq $id })[0].Value
        $expected = if ($language -ceq 'zh') { $sourceHtml.Substring(0, $sourceEn) } else { $sourceHtml.Substring($sourceEn) }
        $withoutChrome = [regex]::Replace($actual, '(?s)<p class="document-label">.*?</p>|<aside class="offline-note">.*?</aside>', '')
        Assert-Guide ((Get-Text $withoutChrome) -ceq (Get-Text $expected)) "Source text lost or changed: $document / $language"
        foreach ($tag in @('table', 'tr', 'pre', 'code', 'img', 'h1', 'h2', 'h3', 'h4', 'h5')) {
            Assert-Guide ([regex]::Matches($actual, ('<' + $tag + '\b')).Count -eq [regex]::Matches($expected, ('<' + $tag + '\b')).Count) "Source $tag structure changed: $document / $language"
        }
        $expectedImages = @([regex]::Matches($expected, '<img\b[^>]+>'))
        $actualImages = @([regex]::Matches($actual, '<img\b[^>]+>'))
        for ($imageIndex = 0; $imageIndex -lt $expectedImages.Count; $imageIndex++) {
            $sourceTag = $expectedImages[$imageIndex].Value
            $actualTag = $actualImages[$imageIndex].Value
            Assert-Guide ((Get-Attribute $sourceTag 'alt') -ceq (Get-Attribute $actualTag 'alt')) 'Image description changed.'
            $src = Get-Attribute $actualTag 'src'
            Assert-Guide ($src -cmatch '^data:image/(webp|jpeg);base64,[A-Za-z0-9+/]+=*$') 'Image is not embedded.'
            $bytes = [Convert]::FromBase64String($src.Split(',')[1])
            $sourcePath = [IO.Path]::GetFullPath((Join-Path (Split-Path (Join-Path $taskRoot $document) -Parent) (Get-Attribute $sourceTag 'src')))
            if ($src.StartsWith('data:image/webp;')) {
                Assert-Guide ([Convert]::ToBase64String($bytes) -ceq [Convert]::ToBase64String([IO.File]::ReadAllBytes($sourcePath))) 'Embedded UI image bytes changed.'
            } else {
                Add-Type -AssemblyName System.Drawing.Common
                $memory = [IO.MemoryStream]::new($bytes, $false)
                $image = [Drawing.Image]::FromStream($memory)
                try {
                    Assert-Guide ($image.Width -eq 1280 -and $image.Height -eq 720) 'Game preview is not a bounded full-frame 16:9 preview.'
                    # GDI exposes JPEG quantization tables as property items.
                    Assert-Guide (@($image.PropertyItems | Where-Object Id -NotIn @(0x5090, 0x5091)).Count -eq 0) 'Game preview retained metadata.'
                } finally { $image.Dispose(); $memory.Dispose() }
                Assert-Guide ($bytes.Length -lt 400000 -and [Text.Encoding]::ASCII.GetString($bytes) -notmatch 'hdrgm:|GainMap|urn:iso:std:iso:ts:21496') 'Preview retained a large HDR payload.'
            }
        }
    }
}
Assert-Guide ($taskIds.Contains('guide-contents') -and $taskIds.Contains('guide-contents-1')) 'Explicit Contents anchor/heading collision was not resolved.'
Assert-Guide ([regex]::Matches($taskHtml, 'href="https://williamstosh.github.io/DLSS5-NR-Nexus/compare/#scene=').Count -eq 4) 'Online comparisons must remain optional links.'
Assert-Guide ([regex]::Matches($taskHtml, '<aside class="offline-note">').Count -eq 2) 'SDR offline preview labels missing.'

# Small source fixtures exercise repeated headings, HTML anchors and failures
# without changing either player document or requiring the RenoDX build.
$taskFixture = Join-Path $taskOutput 'source'
[IO.Directory]::CreateDirectory((Join-Path $taskFixture 'docs/compare')) | Out-Null
[IO.File]::WriteAllText((Join-Path $taskFixture 'docs/compare/catalog.json'), '{"schema":1,"scenes":[]}', $taskUtf8)
$taskFixtureText = @'
# Fixture

[中文](#简体中文) · [English](#english)

## 简体中文

<a id="重复"></a>

### 重复

### 重复

[原始锚点](#重复) · [重复标题](#重复-1) · [编码锚点](#%E9%87%8D%E5%A4%8D)

| 参数 | 用途 |
| --- | --- |
| 中文 | 完整显示 |

~~~powershell
Write-Output '<a id="literal">中文</a>'
~~~

## English

<a name='contents'></a>

### Contents

### Repeat

### Repeat

[Duplicate](#repeat-1) · [Named anchor](#contents)
'@
$taskFixtureReadme = $taskFixtureText + [Environment]::NewLine + '[Guide](docs/USER_GUIDE.md#contents)'
[IO.File]::WriteAllText((Join-Path $taskFixture 'README.md'), $taskFixtureReadme, $taskUtf8)
[IO.File]::WriteAllText((Join-Path $taskFixture 'docs/USER_GUIDE.md'), $taskFixtureText + [Environment]::NewLine + '[README](../README.md#重复)', $taskUtf8)
$taskFixtureResult = & $taskGenerator -SourceRoot $taskFixture -SourceCommit $SourceCommit
$taskFixtureHtml = $taskUtf8.GetString($taskFixtureResult.Bytes)
foreach ($fragment in @('readme-重复', 'readme-重复-1', 'readme-重复-1-1', 'guide-contents', 'guide-contents-1', 'guide-repeat-1')) {
    Assert-Guide ($taskFixtureHtml.Contains('id="' + $fragment + '"') -or $taskFixtureHtml.Contains('name="' + $fragment + '"')) "Missing fixture anchor: $fragment"
}
Assert-Guide ($taskFixtureHtml.Contains('href="#guide-contents"') -and $taskFixtureHtml.Contains('href="#readme-重复"')) 'Fixture cross-document links are not internal.'
Assert-Guide ($taskFixtureHtml.Contains('&lt;a id=&quot;literal&quot;&gt;中文&lt;/a&gt;')) 'Code block interpreted as HTML.'
foreach ($case in @(
    @('[Broken](#missing)', 'Broken offline guide anchor'),
    @('![Missing](missing.webp)', 'Missing guide input'),
    @('![Remote](https://example.invalid/image.webp)', 'Guide image must be a local reviewed asset'),
    @('<script>alert(1)</script>', 'Unsupported raw guide HTML'),
    @('[Active](javascript:alert)', 'Unsupported guide link scheme'),
    @('[Escape](../outside.md)', 'Guide path leaves source root'),
    @('<!-- <a id="comment-only"></a> -->' + [Environment]::NewLine + '[Comment](#comment-only)', 'Broken offline guide anchor')
)) {
    [IO.File]::WriteAllText((Join-Path $taskFixture 'README.md'), $taskFixtureReadme + [Environment]::NewLine + $case[0], $taskUtf8)
    $rejected = $false
    try { & $taskGenerator -SourceRoot $taskFixture -SourceCommit $SourceCommit | Out-Null }
    catch { if ($_.Exception.Message -notmatch $case[1]) { throw }; $rejected = $true }
    Assert-Guide $rejected ('Accepted invalid source: ' + $case[0])
}
# Catalog approval is an input boundary: even a small local image is rejected
# if it is unapproved or marked as a synthetic candidate.
[IO.File]::WriteAllBytes((Join-Path $taskFixture 'docs/compare/preview.jpg'), @(0xff, 0xd8, 0xff, 0xd9))
[IO.File]::WriteAllText((Join-Path $taskFixture 'README.md'), $taskFixtureReadme + [Environment]::NewLine + '![Scene](docs/compare/preview.jpg)', $taskUtf8)
foreach ($catalog in @(
    '{"schema":1,"scenes":[{"readme":"preview.jpg","status":"pending","synthetic":false}]}',
    '{"schema":1,"scenes":[{"readme":"preview.jpg","status":"approved","synthetic":true}]}'
)) {
    [IO.File]::WriteAllText((Join-Path $taskFixture 'docs/compare/catalog.json'), $catalog, $taskUtf8)
    $rejected = $false
    try { & $taskGenerator -SourceRoot $taskFixture -SourceCommit $SourceCommit | Out-Null }
    catch { if ($_.Exception.Message -notmatch 'not an approved README preview') { throw }; $rejected = $true }
    Assert-Guide $rejected 'Unapproved/synthetic scene accepted.'
}
Complete-TestOutput $taskOutput
[pscustomobject]@{ TestsPassed = $taskPassed; HtmlBytes = $GuideBytes.Length; Images = $taskGenerated.ImageCount; OutputDirectory = $taskOutput }
