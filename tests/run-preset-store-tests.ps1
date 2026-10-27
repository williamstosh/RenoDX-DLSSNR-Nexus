[CmdletBinding()]
param(
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$HostRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/renodx'),
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/tests/preset-store-' + [guid]::NewGuid().ToString('N'))),
    [string]$VcVars64
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'test-support.ps1')

function Assert-UnicodeFixtures {
    param([string]$DataRoot)
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    $combining = [char]0x0301
    $joiner = [char]0x200d
    $samples = @(
        @{
            filename = '中文画质预设.nrnexus.json'
            metadata = @{
                name = '中文画质预设'; author = '合成测试作者'
                description = '仅用于存储测试的中文描述，保留原始字符。'; source_game = '合成中文游戏'
                created_with = '0.2.0-dev.1'; tags = @('画质', '测试')
            }
            strings = @{ 'future-unicode' = '未知字符串值' }
        },
        @{
            filename = "画质-日本語-한국어-café-e$combining-😀.nrnexus.json"
            metadata = @{
                name = "画质-日本語-한국어-café-e$combining-😀"
                author = "作者-日本人-한국인-Renée-e$combining-🎮"
                description = "中文𠮷 / 日本語 / 한국어 / café / cafe$combining / 👩${joiner}💻`n第二行，字节不归一化。"
                source_game = "合成游戏-日本語-한국어-été-e$combining-🎮"
                created_with = '0.2.0-dev.1'; tags = @('中日韓', "café-e$combining-😀")
            }
            strings = @{ 'future-unicode' = "未知-日本語-한국어-café-e$combining-😀"; '未知设置键-🎮' = "保留字符串-é-e$combining" }
        }
    )
    $library = Join-Path $DataRoot '中文根/本地数据/NR Nexus/Presets'
    $external = Join-Path $DataRoot "导出目录/日本語-한국어-café-e$combining-😀"
    $expectedFiles = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $hashes = @{}
    foreach ($sample in $samples) {
        $renamedFilename = $sample.filename.Replace('.nrnexus.json', '-重命名.nrnexus.json')
        $null = $expectedFiles.Add($sample.filename)
        $null = $expectedFiles.Add($renamedFilename)
        if (Test-Path -LiteralPath (Join-Path $library ('导入-' + $sample.filename))) {
            throw 'Unicode rename left the old imported file.'
        }
        foreach ($artifact in @(
            @{ path = (Join-Path $library $sample.filename); renamed = $false },
            @{ path = (Join-Path $external $sample.filename); renamed = $false },
            @{ path = (Join-Path $library $renamedFilename); renamed = $true }
        )) {
            $bytes = [IO.File]::ReadAllBytes($artifact.path)
            $text = $utf8.GetString($bytes)
            if ([Convert]::ToHexString($utf8.GetBytes($text)) -cne [Convert]::ToHexString($bytes)) {
                throw "Unicode JSON byte round trip failed: $($artifact.path)"
            }
            $root = ConvertFrom-Json -InputObject $text -AsHashtable
            if ($root.schema_version -ne 1 -or $root.format -cne 'nr-nexus-preset' -or
                $root.scope -cne 'quality-multipass' -or $root.metadata.Count -ne $sample.metadata.Count) {
                throw "Unicode fixture changed schema: $($artifact.path)"
            }
            foreach ($key in $sample.metadata.Keys) {
                $expected = @($sample.metadata[$key])
                if ($artifact.renamed -and $key -ceq 'name') { $expected = @($expected[0] + '-重命名') }
                $actual = @($root.metadata[$key])
                if ($actual.Count -ne $expected.Count) { throw "Unicode metadata count mismatch: $key" }
                for ($index = 0; $index -lt $expected.Count; ++$index) {
                    if ($actual[$index] -isnot [string] -or
                        -not [string]::Equals($actual[$index], $expected[$index], [StringComparison]::Ordinal) -or
                        [Convert]::ToHexString($utf8.GetBytes($actual[$index])) -cne [Convert]::ToHexString($utf8.GetBytes($expected[$index]))) {
                        throw "Unicode metadata bytes/codepoints mismatch: $key in $($artifact.path)"
                    }
                }
            }
            foreach ($key in $sample.strings.Keys) {
                if (-not $root.settings.ContainsKey($key) -or $root.settings[$key] -isnot [string] -or
                    [Convert]::ToHexString($utf8.GetBytes($root.settings[$key])) -cne [Convert]::ToHexString($utf8.GetBytes($sample.strings[$key]))) {
                    throw "Unknown Unicode setting bytes changed: $key in $($artifact.path)"
                }
            }
            $hashes[$artifact.path] = (Get-FileHash -LiteralPath $artifact.path).Hash
        }
    }
    $actualFiles = [string[]]@(Get-ChildItem -LiteralPath $library -Force | ForEach-Object { $_.Name })
    if (-not $expectedFiles.SetEquals($actualFiles)) { throw 'Unicode library filename codepoints changed.' }
    return $hashes
}

Initialize-TestBuild $VcVars64
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$HostRoot = [IO.Path]::GetFullPath($HostRoot)
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$source = Join-Path $PSScriptRoot 'preset_store_tests.cpp'
$common = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/utf-8',
    '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', "/I$SourceRoot", "/I$HostRoot/external/json/include", $source)
$executable = Join-Path $OutputDirectory 'preset_store_tests.exe'
$module = Join-Path $OutputDirectory 'preset_store_module.dll'
foreach ($kind in @('exe', 'dll')) {
    $arguments = $common + @("/Fo$OutputDirectory/$kind.obj", '/sourceDependencies', "$OutputDirectory/$kind-dependencies.json")
    if ($kind -eq 'dll') {
        $arguments += @('/DPRESET_STORE_TEST_MODULE', '/LD', "/Fe$module")
    } else {
        $arguments += "/Fe$executable"
    }
    $arguments | ConvertTo-Json | Set-Content -LiteralPath "$OutputDirectory/$kind-arguments.json" -Encoding utf8
    $binary = if ($kind -eq 'dll') { $module } else { $executable }
    Register-TestCompilerOutput -Directory $OutputDirectory -ObjectPath (Join-Path $OutputDirectory "$kind.obj") -ExecutablePath $binary
    & cl.exe @arguments *> "$OutputDirectory/$kind-compile.log"
    if ($LASTEXITCODE -ne 0) { throw "Preset $kind build failed; see $OutputDirectory/$kind-compile.log" }
}
$dependencies = @('exe', 'dll' | ForEach-Object {
    (Get-Content -LiteralPath "$OutputDirectory/$_-dependencies.json" -Raw | ConvertFrom-Json).Data.Includes
})
$hashes = @(@($source, $PSCommandPath, (Join-Path $PSScriptRoot 'test-support.ps1')) + $dependencies |
    Sort-Object -Unique | ForEach-Object { @{ path = $_; sha256 = (Get-FileHash -LiteralPath $_).Hash } })
$hashes | ConvertTo-Json | Set-Content -LiteralPath "$OutputDirectory/inputs.json" -Encoding utf8
$data = Join-Path $OutputDirectory 'data'
$outside = Join-Path $data 'outside'
$library = Join-Path $data 'local-appdata/NR Nexus/Presets'
New-Item -ItemType Directory -Path $outside, $library | Out-Null
[IO.File]::WriteAllText((Join-Path $outside 'outside.nrnexus.json'), 'synthetic outside sentinel')
$ancestorLink = Join-Path $data 'ancestor-link'
$fileLink = Join-Path $library 'linked.nrnexus.json'
# Junctions exercise real NTFS reparse behavior without Developer Mode/admin.
New-Item -ItemType Junction -Path $ancestorLink -Target $outside | Out-Null
New-Item -ItemType Junction -Path $fileLink -Target $outside | Out-Null
try {
    Invoke-TestProcess $executable $OutputDirectory 'test' @(('"' + $data + '"'), ('"' + $module + '"'))
    $beforeRestart = Assert-UnicodeFixtures $data
    # The writer has exited. A new EXE process must recover solely from disk.
    Invoke-TestProcess $executable $OutputDirectory 'unicode-restart' @('--unicode-restart', ('"' + $data + '"'))
    $afterRestart = Assert-UnicodeFixtures $data
    foreach ($path in $beforeRestart.Keys) {
        if ($afterRestart[$path] -cne $beforeRestart[$path]) { throw "Unicode JSON changed across process restart: $path" }
    }
    Write-Host "Unicode script oracle: $($afterRestart.Count) files passed filename, metadata UTF-8 bytes/codepoints and restart hash checks."
    foreach ($inputHash in $hashes) {
        if ((Get-FileHash -LiteralPath $inputHash.path).Hash -ne $inputHash.sha256) {
            throw "Preset test input changed: $($inputHash.path)"
        }
    }
    @{ suite = 'preset-store'; build = 'passed'; run = 'passed'; reparse = 'passed'; cross_module = 'passed'
        unicode_storage = 'passed'; unicode_restart = 'passed'; unicode_bytes = 'passed'; invalid_utf8 = 'passed'; schema_version = 1 } |
        ConvertTo-Json | Set-Content -LiteralPath "$OutputDirectory/result.json" -Encoding utf8
} finally {
    # Remove only our junction entries; never recurse through their targets.
    foreach ($link in @($fileLink, $ancestorLink)) {
        if (Test-Path -LiteralPath $link) {
            $item = Get-Item -LiteralPath $link -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
                throw "Fixture is no longer a reparse point: $link"
            }
            $item.Delete()
        }
    }
}
Write-Host "Preset store outputs: $OutputDirectory"
Complete-TestOutput $OutputDirectory -Outcome Passed
