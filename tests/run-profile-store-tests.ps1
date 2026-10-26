[CmdletBinding()]
param(
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$HostRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/renodx'),
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/profile-store-' + [guid]::NewGuid().ToString('N'))),
    [string]$VcVars64
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'test-support.ps1')

Initialize-TestBuild $VcVars64
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$HostRoot = [IO.Path]::GetFullPath($HostRoot)
$source = Join-Path $SourceRoot 'tests/profile_store_tests.cpp'
$executable = Join-Path $OutputDirectory 'profile_store_tests.exe'
$arguments = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/utf-8',
    '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', "/I$SourceRoot", "/I$HostRoot/external/json/include",
    $source, "/Fo$OutputDirectory/tests.obj", "/Fe$executable", '/sourceDependencies', "$OutputDirectory/dependencies.json")
$arguments | ConvertTo-Json | Set-Content -LiteralPath "$OutputDirectory/arguments.json" -Encoding utf8
& cl.exe @arguments *> "$OutputDirectory/compile.log"
if ($LASTEXITCODE -ne 0) { throw "Profile store build failed; see $OutputDirectory/compile.log" }
$dependencies = (Get-Content -LiteralPath "$OutputDirectory/dependencies.json" -Raw | ConvertFrom-Json).Data.Includes
@(@($source, $PSCommandPath, (Join-Path $PSScriptRoot 'test-support.ps1')) + $dependencies |
    Sort-Object -Unique | ForEach-Object { @{ path = $_; sha256 = (Get-FileHash -LiteralPath $_).Hash } }) |
    ConvertTo-Json | Set-Content -LiteralPath "$OutputDirectory/inputs.json" -Encoding utf8
$data = Join-Path $OutputDirectory 'data'
$outside = Join-Path $data 'outside'
New-Item -ItemType Directory -Path $outside | Out-Null
[IO.File]::WriteAllText((Join-Path $outside 'sentinel'), 'outside sentinel')
$junctions = @('ancestor-link', 'destination-link', 'lock-reparse.json.lock')
try {
    foreach ($name in $junctions) {
        New-Item -ItemType Junction -Path (Join-Path $data $name) -Target $outside | Out-Null
    }
    Invoke-TestProcess $executable $OutputDirectory 'test' @(('"' + $data + '"'), 'test')
    Invoke-TestProcess $executable $OutputDirectory 'restore' @(('"' + $data + '"'), 'restore')
} finally {
    # Remove only the three junction objects created above; never recurse into targets.
    foreach ($name in $junctions) {
        $junction = Join-Path $data $name
        if (Test-Path -LiteralPath $junction) {
            $item = Get-Item -LiteralPath $junction -Force
            if ($item.LinkType -eq 'Junction' -and $item.Target -eq $outside) {
                $item.Delete()
            }
        }
    }
}
Write-Host "Profile store evidence retained: $OutputDirectory"
