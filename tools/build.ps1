[CmdletBinding()]
param(
    [string]$RenodxRoot = (Join-Path $PSScriptRoot '../.local/renodx'),
    [Parameter(Mandatory)][string]$ToolRoot,
    [string]$BuildDirectory,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [switch]$ImeDiagnostics
)

$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'check-version.ps1') | Format-List
& (Join-Path $PSScriptRoot 'check-branding.ps1')
$taskSource = (Resolve-Path -LiteralPath $RenodxRoot).Path
$taskTools = (Resolve-Path -LiteralPath $ToolRoot).Path
$taskBuild = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory) } else { Join-Path $taskRoot 'build' }
$taskAddon = Join-Path $taskSource 'src/addons/dlss5'
$taskLock = Get-Content -LiteralPath (Join-Path $taskRoot 'host/lock.json') -Raw | ConvertFrom-Json
if ((& git --no-optional-locks -C $taskSource rev-parse HEAD) -cne $taskLock.host.commit) {
    throw 'The build requires the RenoDX commit pinned in host/lock.json.'
}
foreach ($taskHeader in $taskLock.compatibility.headers.PSObject.Properties) {
    if ((Get-FileHash -LiteralPath (Join-Path $taskSource $taskHeader.Name)).Hash -cne $taskHeader.Value) {
        throw "Host compatibility input differs from host/lock.json: $($taskHeader.Name)"
    }
}
$taskHostChanges = @(& git --no-optional-locks -C $taskSource diff --name-only --ignore-submodules=all HEAD -- . ':(exclude)external' ':(exclude)src/addons/dlss5')
if ($LASTEXITCODE -ne 0 -or (Compare-Object ($taskHostChanges | Sort-Object) @($taskLock.compatibility.headers.PSObject.Properties.Name | Sort-Object))) {
    throw 'Host modifications must be limited to the pinned compatibility headers.'
}
foreach ($taskDependency in $taskLock.dependencies) {
    $taskDependencyRoot = Join-Path $taskSource $taskDependency.path
    if ((& git --no-optional-locks -C $taskDependencyRoot rev-parse HEAD) -cne $taskDependency.commit) {
        throw "Dependency commit differs from host/lock.json: $($taskDependency.path)"
    }
    & git --no-optional-locks -C $taskDependencyRoot diff --quiet --ignore-submodules=all HEAD
    if ($LASTEXITCODE -ne 0) { throw "Dependency inputs are modified: $($taskDependency.path)" }
}
$taskSourceFiles = @(Get-ChildItem -LiteralPath $taskRoot -File | Where-Object Extension -In @('.cpp','.hpp','.h') | ForEach-Object Name)
$taskSourceFiles += @(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'shaders'),(Join-Path $taskRoot 'ui') -File -Recurse | ForEach-Object { [IO.Path]::GetRelativePath($taskRoot, $_.FullName) })
foreach ($taskFile in $taskSourceFiles + @('version.h', 'cmake/nexus-version.cmake')) {
    if ((Get-FileHash -LiteralPath (Join-Path $taskAddon $taskFile)).Hash -cne
        (Get-FileHash -LiteralPath (Join-Path $taskRoot $taskFile)).Hash) {
        throw "The host's dlss5 source differs from this checkout: $taskFile"
    }
}
$taskVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$taskVsRoot = & $taskVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $taskVsRoot) { throw 'Visual Studio 2022 C++ BuildTools are required.' }
$taskCmake = Join-Path $taskVsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$taskVcVars = Join-Path $taskVsRoot 'VC/Auxiliary/Build/vcvarsall.bat'
# The host CMake target only tracks detours.cpp; rebuild the small static library
# from all pinned inputs so an old generated include/library cannot be reused.
Push-Location -LiteralPath (Join-Path $taskSource 'external/Detours/src')
try {
    & $env:ComSpec /d /c "call `"$taskVcVars`" amd64 10.0.26100.0 -vcvars_ver=14.44 >nul && nmake /nologo /A ARCH=X64"
    if ($LASTEXITCODE -ne 0) { throw "Pinned Detours rebuild failed: $LASTEXITCODE" }
} finally {
    Pop-Location
}
$taskArguments = @(
    '-S', $taskSource, '-B', $taskBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    ('-DCMAKE_PROJECT_renodx_INCLUDE=' + (Join-Path $taskRoot 'cmake/nexus-version.cmake')),
    ('-DFXC_BIN=' + (Join-Path $taskTools 'fxc.exe')),
    ('-DDXC_BIN=' + (Join-Path $taskTools 'dxc.exe')),
    ('-DSLANGC_BIN=' + (Join-Path $taskTools 'slangc.exe')),
    '-DRENODX_BUILD_TESTS=OFF',
    ('-DNR_NEXUS_IME_DIAGNOSTICS=' + $(if ($ImeDiagnostics) { 'ON' } else { 'OFF' })),
    '-DCMAKE_SYSTEM_VERSION=10.0.26100.0',
    '-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /W3 /GR /EHsc /utf-8',
    '-DCMAKE_MODULE_LINKER_FLAGS=/DELAYLOAD:winhttp.dll'
)
& $taskCmake @taskArguments
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
& $taskCmake --build $taskBuild --config $Configuration --target dlss5 --parallel 2
if ($LASTEXITCODE -ne 0) { throw "dlss5 build failed: $LASTEXITCODE" }
