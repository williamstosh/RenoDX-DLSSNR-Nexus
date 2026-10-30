[CmdletBinding()]
param(
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$HostRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/renodx'),
    [string]$ShaderIncludeDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.include'),
    [string]$BuildProject = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.vcxproj'),
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/tests/ui-' + [guid]::NewGuid().ToString('N'))),
    [string]$VcVars64,
    [ValidateSet('Quick','Full')][string]$SnapshotMode,
    [ValidateSet('Standard','Full','Acceptance')][string]$RetentionMode = $(if ($env:NR_NEXUS_TEST_RETENTION) { $env:NR_NEXUS_TEST_RETENTION } else { 'Standard' })
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-support.ps1')
$RetentionMode = @('Standard','Full','Acceptance') | Where-Object { $_ -ieq $RetentionMode } | Select-Object -First 1
$OutputDirectory = Initialize-TestOutput $OutputDirectory -RetentionMode $RetentionMode
$timer = [Diagnostics.Stopwatch]::StartNew()
Initialize-TestBuild $VcVars64
$imguiRoot = Join-Path $HostRoot 'external/reshade/deps/imgui'
$executable = Join-Path $OutputDirectory 'ui-layout-tests.exe'
$sources = @((Join-Path $SourceRoot 'tests/ui_layout_tests.cpp'))
$sources += @('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp') | ForEach-Object { Join-Path $imguiRoot $_ }
if (-not $SnapshotMode) {
$arguments = @('/nologo','/std:c++20','/EHsc','/O2','/MT','/utf-8','/DNOMINMAX',"/I$SourceRoot","/I$imguiRoot")
$arguments += $sources
$arguments += @("/Fo$OutputDirectory/", "/Fe$executable")
Register-TestCompilerOutput $OutputDirectory -ObjectPath @($sources | ForEach-Object { Join-Path $OutputDirectory ([IO.Path]::GetFileNameWithoutExtension($_) + '.obj') }) -ExecutablePath $executable
$arguments | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'layout-arguments.json') -Encoding utf8
& cl.exe @arguments *> (Join-Path $OutputDirectory 'compile.log')
if ($LASTEXITCODE -ne 0) { throw "UI test build failed: $OutputDirectory/compile.log" }
Invoke-TestProcess $executable $OutputDirectory 'layout'
}
$projectInputs = Get-TestProjectInputs $BuildProject $SourceRoot $HostRoot $ShaderIncludeDirectory
$menuExecutable = Join-Path $OutputDirectory 'ui-menu-tests.exe'
$menuDirectory = Join-Path $OutputDirectory 'menu'
New-Item -ItemType Directory -Path $menuDirectory | Out-Null
$sources[0] = Join-Path $SourceRoot 'tests/ui_menu_tests.cpp'
$arguments = @('/nologo','/std:c++20','/EHsc','/W3','/O2','/MT','/utf-8','/DNOMINMAX','/DRESHADE_ADDON=2',
    '/DIMGUI_ENABLE_TEST_ENGINE','/bigobj','/Gy','/Gw')
$arguments += @($projectInputs.Includes | ForEach-Object { "/I$_" })
$arguments += $sources
$arguments += @("/Fo$menuDirectory/", "/Fe$menuExecutable", '/link', '/OPT:REF', '/OPT:ICF') + $projectInputs.Libraries
Register-TestCompilerOutput $OutputDirectory -ObjectPath @($sources | ForEach-Object { Join-Path $menuDirectory ([IO.Path]::GetFileNameWithoutExtension($_) + '.obj') }) -ExecutablePath $menuExecutable
$arguments | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'menu-arguments.json') -Encoding utf8
@{ sourceCommit=(& git --no-optional-locks -C $SourceRoot rev-parse HEAD)
    sourceChanges=@(& git --no-optional-locks -C $SourceRoot status --porcelain=v1 --untracked-files=all)
    hostCommit=(& git --no-optional-locks -C $HostRoot rev-parse HEAD)
    retention=$RetentionMode; entry=$PSCommandPath
    inputs=@(@($sources) + @($PSCommandPath,(Join-Path $PSScriptRoot 'test-support.ps1'),$BuildProject) |
        ForEach-Object { @{path=$_;sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash} })
    fonts=@('msyh.ttc','segoeui.ttf','malgun.ttf','leelawui.ttf' | ForEach-Object {
        $path = Join-Path $env:WINDIR "Fonts/$_"
        if (Test-Path -LiteralPath $path) { @{name=$_;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash} }
    }) } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'inputs.json') -Encoding utf8
& cl.exe @arguments *> (Join-Path $OutputDirectory 'menu-compile.log')
if ($LASTEXITCODE -ne 0) { throw "Production menu test build failed: $OutputDirectory/menu-compile.log" }
$previousSnapshotMode = $env:NR_NEXUS_UI_SNAPSHOTS
try {
    if ($SnapshotMode) { $env:NR_NEXUS_UI_SNAPSHOTS = $SnapshotMode }
    # The complete production matrix renders over 13,000 frames across themes,
    # languages and DPI sizes. Keep its bound separate from focused 45s cases
    # so slower hosted runners can finish the same assertions without retries.
    Invoke-TestProcess $menuExecutable $menuDirectory 'menu' -TimeoutMilliseconds 180000
} finally { $env:NR_NEXUS_UI_SNAPSHOTS = $previousSnapshotMode }
$timer.Stop()
@{build='passed';run='passed';retention=$RetentionMode;seconds=$timer.Elapsed.TotalSeconds
    snapshotMode=$SnapshotMode; scope='real ImGui layout, synthetic live status; no game or GPU inference'} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
Complete-TestOutput $OutputDirectory
