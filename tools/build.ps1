[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RenodxRoot,
    [Parameter(Mandatory)][string]$ToolRoot,
    [string]$BuildDirectory,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'check-version.ps1') | Format-List
$taskSource = (Resolve-Path -LiteralPath $RenodxRoot).Path
$taskTools = (Resolve-Path -LiteralPath $ToolRoot).Path
$taskBuild = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory) } else { Join-Path $taskRoot 'build' }
$taskAddon = Join-Path $taskSource 'src/addons/dlss5'
foreach ($taskFile in @('version.h', 'dlssnr.hpp', 'cmake/nexus-version.cmake')) {
    if ((Get-FileHash -LiteralPath (Join-Path $taskAddon $taskFile)).Hash -cne
        (Get-FileHash -LiteralPath (Join-Path $taskRoot $taskFile)).Hash) {
        throw "The host's dlss5 source differs from this checkout: $taskFile"
    }
}
$taskVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVsRoot = & $taskVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $taskVsRoot) { throw 'Visual Studio 2022 C++ BuildTools are required.' }
$taskCmake = Join-Path $taskVsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$taskArguments = @(
    '-S', $taskSource, '-B', $taskBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    ('-DCMAKE_PROJECT_renodx_INCLUDE=' + (Join-Path $taskRoot 'cmake/nexus-version.cmake')),
    ('-DFXC_BIN=' + (Join-Path $taskTools 'fxc.exe')),
    ('-DDXC_BIN=' + (Join-Path $taskTools 'dxc.exe')),
    ('-DSLANGC_BIN=' + (Join-Path $taskTools 'slangc.exe')),
    '-DRENODX_BUILD_TESTS=OFF',
    '-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /W3 /GR /EHsc /utf-8',
    '-DCMAKE_MODULE_LINKER_FLAGS=/DELAYLOAD:winhttp.dll'
)
& $taskCmake @taskArguments
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
& $taskCmake --build $taskBuild --config $Configuration --target dlss5 --parallel 2
if ($LASTEXITCODE -ne 0) { throw "dlss5 build failed: $LASTEXITCODE" }
