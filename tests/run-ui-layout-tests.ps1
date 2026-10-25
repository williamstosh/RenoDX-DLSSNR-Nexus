[CmdletBinding()]
param(
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$HostRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/renodx'),
    [string]$ShaderIncludeDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.include'),
    [string]$BuildProject = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.vcxproj'),
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/tests/ui-' + [guid]::NewGuid().ToString('N'))),
    [string]$VcVars64
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-support.ps1')
$OutputDirectory = Initialize-TestOutput $OutputDirectory
Initialize-TestBuild $VcVars64
$imguiRoot = Join-Path $HostRoot 'external/reshade/deps/imgui'
$executable = Join-Path $OutputDirectory 'ui-layout-tests.exe'
$sources = @((Join-Path $SourceRoot 'tests/ui_layout_tests.cpp'))
$sources += @('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp') | ForEach-Object { Join-Path $imguiRoot $_ }
$arguments = @('/nologo','/std:c++20','/EHsc','/O2','/MT','/utf-8','/DNOMINMAX',"/I$SourceRoot","/I$imguiRoot")
$arguments += $sources
$arguments += @("/Fo$OutputDirectory/", "/Fe$executable")
& cl.exe @arguments *> (Join-Path $OutputDirectory 'compile.log')
if ($LASTEXITCODE -ne 0) { throw "UI test build failed: $OutputDirectory/compile.log" }
Invoke-TestProcess $executable $OutputDirectory 'layout'
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
& cl.exe @arguments *> (Join-Path $OutputDirectory 'menu-compile.log')
if ($LASTEXITCODE -ne 0) { throw "Production menu test build failed: $OutputDirectory/menu-compile.log" }
Invoke-TestProcess $menuExecutable $menuDirectory 'menu'
@{build='passed';run='passed';scope='real ImGui layout, synthetic live status; no game or GPU inference'} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
