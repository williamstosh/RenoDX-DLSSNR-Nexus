[CmdletBinding()]
param(
    [string]$HostRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/renodx'),
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/tests/report-privacy-' + [guid]::NewGuid().ToString('N'))),
    [string]$VcVars64
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'test-support.ps1')
Initialize-TestBuild $VcVars64
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$exe = Join-Path $OutputDirectory 'report_privacy_tests.exe'
$arguments = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/O2', '/MT', '/utf-8',
    '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', "/I$HostRoot/external/json/include",
    (Join-Path $PSScriptRoot 'report_privacy_tests.cpp'), "/Fo$OutputDirectory/report_privacy_tests.obj", "/Fe$exe")
& cl.exe @arguments *> (Join-Path $OutputDirectory 'compile.log')
if ($LASTEXITCODE -ne 0) { throw "Privacy test build failed; see $OutputDirectory/compile.log" }
$data = New-Item -ItemType Directory -Path (Join-Path $OutputDirectory 'data')
Invoke-TestProcess $exe $OutputDirectory 'test' @($data.FullName)
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $data.FullName 'report.zip'))
try {
    if ($archive.Entries.Count -ne 2) { throw 'Unexpected ZIP entries.' }
    foreach ($entry in $archive.Entries) {
        $reader = [IO.StreamReader]::new($entry.Open())
        try {
            $content = $reader.ReadToEnd()
            if ($entry.FullName -eq 'report.json') { $null = $content | ConvertFrom-Json }
            if ($content -match 'private|SYNTHETIC123|query-value') { throw 'Synthetic secret survived ZIP export.' }
        } finally { $reader.Dispose() }
    }
} finally { $archive.Dispose() }
Write-Host "ZIP independently read; outputs: $OutputDirectory"
