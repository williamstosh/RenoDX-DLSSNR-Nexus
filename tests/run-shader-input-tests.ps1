[CmdletBinding()]
param(
    [string]$HostRoot = (Join-Path $PSScriptRoot '../.local/renodx'),
    [string]$ToolRoot = (Join-Path $PSScriptRoot '../.local/shader-tools'),
    [string]$HostToolDirectory = (Join-Path $PSScriptRoot '../build/Release'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot ('../tmp/tests/shader-inputs/' + [guid]::NewGuid().ToString('N')))
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'test-support.ps1')
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$HostRoot = (Resolve-Path -LiteralPath $HostRoot).Path
$ToolRoot = (Resolve-Path -LiteralPath $ToolRoot).Path
$HostToolDirectory = (Resolve-Path -LiteralPath $HostToolDirectory).Path
$OutputDirectory = Initialize-TestOutput $OutputDirectory -RetentionMode Standard
$outcome = 'Failed'
$hostCommit = $null
$checks = [Collections.Generic.List[string]]::new()
$identity = [ordered]@{}
$snapshots = [Collections.Generic.List[object]]::new()

function Assert-ShaderInput([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $checks.Add($Message)
}

function Write-Input([string]$Path, [string]$Content) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path))
    [IO.File]::WriteAllText($Path, $Content)
}

function Invoke-ShaderTool([string]$Executable, [string[]]$Arguments, [string]$Name, [switch]$ExpectFailure) {
    $log = Join-Path $OutputDirectory "$Name.log"
    & $Executable @Arguments *> $log
    $code = $LASTEXITCODE
    if ($ExpectFailure) {
        Assert-ShaderInput ($code -ne 0) "$Name rejects invalid configuration"
    } elseif ($code -ne 0) {
        throw "$Name exited $code; see $log"
    }
}

function Configure-ShaderCase([string]$Build, [string]$Root, [string]$Addon = 'dlss5', [string]$Override = '', [string]$Name) {
    $arguments = @('-S', $project, '-B', $Build, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja",
        "-DTEST_ADDON=$Addon", ('-DTEST_ROOT=' + $Root.Replace('\','/')),
        ('-DTEST_OVERRIDE=' + $Override.Replace('\','/')))
    Invoke-ShaderTool $cmake $arguments $Name
}

function Build-ShaderCase([string]$Build, [string]$Addon, [string]$Name) {
    Invoke-ShaderTool $cmake @('--build', $Build, '--config', 'Release', '--target', "$Addon-shaders", '--parallel', '2') $Name
}

function Assert-EmbeddedBinary([string]$Build, [string]$Addon, [string]$Stem, [string]$Extension, [string]$Reference) {
    $embed = Join-Path $Build "$Addon.include/embed"
    $binary = Join-Path $embed "$Stem.$Extension"
    Assert-ShaderInput ((Get-FileHash -LiteralPath $binary).Hash -eq (Get-FileHash -LiteralPath $Reference).Hash) "$Addon/$Stem.$Extension matches real compiler input"
    $header = Get-Content -LiteralPath (Join-Path $embed "$Stem.h") -Raw
    $array = [regex]::Match($header, '(?s)_base\[\] = \{(.*?)\};')
    Assert-ShaderInput $array.Success "$Addon/$Stem has an embedded byte array"
    [byte[]]$bytes = @([regex]::Matches($array.Groups[1].Value, '\d+') | ForEach-Object { [byte]$_.Value })
    Assert-ShaderInput ([Convert]::ToBase64String($bytes) -ceq [Convert]::ToBase64String([IO.File]::ReadAllBytes($binary))) "$Addon/$Stem embedded bytes match generated binary"
}

function Save-ShaderSnapshot([string]$Build, [string]$Addon, [string[]]$Stems, [string]$Name, [string[]]$Forbidden = @()) {
    $embed = Join-Path $Build "$Addon.include/embed"
    $header = Get-Content -LiteralPath (Join-Path $embed 'shaders.h') -Raw
    $actual = @([regex]::Matches($header, '#include "\./([^"]+)\.h"') | ForEach-Object { $_.Groups[1].Value } | Sort-Object)
    Assert-ShaderInput (-not (Compare-Object @($Stems | Sort-Object) $actual)) "$Name embeds exactly the authorized shader set"
    $graph = (Get-Content -LiteralPath (Join-Path $Build 'build.ninja') -Raw) +
        (Get-Content -LiteralPath (Join-Path $Build 'CMakeFiles/VerifyGlobs.cmake') -Raw)
    foreach ($path in $Forbidden) {
        Assert-ShaderInput (-not $graph.Replace('\', '/').Contains($path.Replace('\', '/'))) "$Name excludes $([IO.Path]::GetRelativePath($OutputDirectory, $path)) from graph/globs"
    }
    $files = @(Get-ChildItem -LiteralPath $embed -File | ForEach-Object {
        [ordered]@{ name=$_.Name; sha256=(Get-FileHash -LiteralPath $_.FullName).Hash }
    })
    $snapshots.Add([ordered]@{ stage=$Name; addon=$Addon; files=$files; shadersHeader=$header })
}

try {
    $lock = Get-Content -LiteralPath (Join-Path $repo 'host/lock.json') -Raw | ConvertFrom-Json
    $hostCommit = & git --no-optional-locks -C $HostRoot rev-parse HEAD
    Assert-ShaderInput ($LASTEXITCODE -eq 0 -and $hostCommit -ceq $lock.host.commit) 'selected host is the pinned commit'
    $hostCmake = Join-Path $HostRoot 'CMakeLists.txt'
    $source = Get-Content -LiteralPath $hostCmake -Raw
    # Execute the selected host function verbatim, including its real glob/copy/custom commands.
    $functions = [regex]::Matches($source, '(?ms)^function\(build_shader_target ADDON ADDON_PATH\).*?^endfunction\(\)')
    Assert-ShaderInput ($functions.Count -eq 1) 'selected host has one extractable build_shader_target'
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'Visual Studio CMake/Ninja discovery failed.' }
    $cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    $ninja = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
    $fxc = Join-Path $ToolRoot 'fxc.exe'
    $slang = Join-Path $ToolRoot 'slangc.exe'
    $embedTool = Join-Path $HostToolDirectory 'embed_file.exe'
    $analyzeTool = Join-Path $HostToolDirectory 'analyze_shader_deps.exe'
    foreach ($path in @($hostCmake, $cmake, $ninja, $fxc, $slang, $embedTool, $analyzeTool,
            (Join-Path $HostRoot 'src/embed_file.cpp'), (Join-Path $HostRoot 'src/analyze_shader_deps.cpp'), $PSCommandPath)) {
        $identity[$path] = (Get-FileHash -LiteralPath $path).Hash
    }
    $project = Join-Path $OutputDirectory 'project'
    Write-Input (Join-Path $project 'host-shaders.cmake') $functions[0].Value
    $cmakeTemplate = @'
cmake_minimum_required(VERSION 3.27)
project(shader_input_regression LANGUAGES NONE)
add_executable(embed_file IMPORTED GLOBAL)
set_target_properties(embed_file PROPERTIES IMPORTED_LOCATION "@EMBED@")
add_executable(analyze_shader_deps IMPORTED GLOBAL)
set_target_properties(analyze_shader_deps PROPERTIES IMPORTED_LOCATION "@ANALYZE@")
set(FXC_BIN "@FXC@")
set(SLANGC_BIN "@SLANG@")
if(NOT TEST_OVERRIDE STREQUAL "")
  set(RENODX_SHADER_SOURCE_ROOT_dlss5 "${TEST_OVERRIDE}")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/host-shaders.cmake")
build_shader_target(${TEST_ADDON} "${TEST_ROOT}")
'@
    $cmakeTemplate = $cmakeTemplate.Replace('@EMBED@', $embedTool.Replace('\','/')).Replace('@ANALYZE@', $analyzeTool.Replace('\','/')).
        Replace('@FXC@', $fxc.Replace('\','/')).Replace('@SLANG@', $slang.Replace('\','/'))
    Write-Input (Join-Path $project 'CMakeLists.txt') $cmakeTemplate
    $inputs = Join-Path $OutputDirectory 'synthetic-repo'
    $shaders = Join-Path $inputs 'shaders'
    $hlsl = Join-Path $shaders 'collision.ps_5_0.hlsl'
    $include = Join-Path $shaders 'value.hlsli'
    Write-Input $hlsl "#include `"value.hlsli`"`nfloat4 main() : SV_Target { return VALUE; }`n"
    Write-Input $include '#define VALUE float4(0.25, 0.5, 0.75, 1.0)'
    $slangSource = Join-Path $shaders 'compiled.comp.slang'
    Write-Input $slangSource '[shader("compute")][numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) {}'
    $reference = Join-Path $OutputDirectory 'reference'
    [void][IO.Directory]::CreateDirectory($reference)
    $gold = Join-Path $reference 'current.bytes'
    $fxcFlags = @('-T','ps_5_0','/D','__SHADER_TARGET_MAJOR=5','/D','__SHADER_TARGET_MINOR=0',
        '/Gec','/nologo','/O3','/Qstrip_reflect','-E','main')
    Invoke-ShaderTool $fxc (@($hlsl) + $fxcFlags + @('-Fo',$gold)) 'reference-fxc'
    $oldSource = Join-Path $reference 'old.ps_5_0.hlsl'
    Write-Input $oldSource 'float4 main() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }'
    $stale = Join-Path $reference 'stale.bytes'
    Invoke-ShaderTool $fxc (@($oldSource) + $fxcFlags + @('-Fo',$stale)) 'stale-fxc'
    Assert-ShaderInput ((Get-FileHash $gold).Hash -ne (Get-FileHash $stale).Hash) 'stale and current CSO differ'
    Copy-Item -LiteralPath $gold -Destination (Join-Path $shaders 'allowed_prebuilt.cso')
    $goldSpv = Join-Path $reference 'spirv.bytes'
    Invoke-ShaderTool $slang @($slangSource,'-stage','compute','-O3','-g0','-entry','main','-Wno-30056','-Wno-15205','-target','spirv','-o',$goldSpv) 'reference-slang'
    Copy-Item -LiteralPath $goldSpv -Destination (Join-Path $shaders 'copied.comp.spv')
    $pollution = @()
    foreach ($directory in @('tmp','build','artifacts')) {
        $folder = Join-Path $inputs $directory
        [void][IO.Directory]::CreateDirectory($folder)
        foreach ($name in @('collision.cso','illegal-name.cso',"${directory}_only.cso")) {
            $path = Join-Path $folder $name
            Copy-Item -LiteralPath $stale -Destination $path
            $pollution += $path
        }
        foreach ($name in @('illegal-name.ps_5_0.hlsl','collision.ps_5_0.hlsl')) {
            $path = Join-Path $folder $name
            Write-Input $path '#error outside authorized shader root'
            $pollution += $path
        }
        foreach ($name in @('illegal-name.comp.spv','copied.comp.spv')) {
            $path = Join-Path $folder $name
            Write-Input $path 'not SPIR-V'
            $pollution += $path
        }
    }
    $pollutionHashes = @{}
    foreach ($path in $pollution) { $pollutionHashes[$path] = (Get-FileHash $path).Hash }
    $build = Join-Path $OutputDirectory 'override-build'
    $stems = @('collision','allowed_prebuilt','compiled','copied')
    Configure-ShaderCase $build $inputs -Override $shaders -Name 'override-configure'
    Assert-ShaderInput (-not (Test-Path (Join-Path $build 'dlss5.include/embed/collision.cso'))) 'configure did not copy outside stale CSO'
    Build-ShaderCase $build dlss5 'override-build'
    Assert-EmbeddedBinary $build dlss5 collision cso $gold
    Assert-EmbeddedBinary $build dlss5 allowed_prebuilt cso $gold
    Assert-EmbeddedBinary $build dlss5 compiled spv $goldSpv
    Assert-EmbeddedBinary $build dlss5 copied spv $goldSpv
    Save-ShaderSnapshot $build dlss5 $stems initial $pollution
    Configure-ShaderCase $build $inputs -Override $shaders -Name 'override-reconfigure'
    Assert-EmbeddedBinary $build dlss5 collision cso $gold
    Build-ShaderCase $build dlss5 'override-noop-build'
    Save-ShaderSnapshot $build dlss5 $stems reconfigured $pollution
    # Include-only edit must be observed through the real analyze_shader_deps depfile.
    Start-Sleep -Milliseconds 1100
    Write-Input $include '#define VALUE float4(0.875, 0.625, 0.375, 1.0)'
    $updated = Join-Path $reference 'updated.bytes'
    Invoke-ShaderTool $fxc (@($hlsl) + $fxcFlags + @('-Fo',$updated)) 'updated-reference-fxc'
    Build-ShaderCase $build dlss5 'include-incremental-build'
    Assert-ShaderInput ((Get-FileHash $gold).Hash -ne (Get-FileHash $updated).Hash) 'include edit changes bytecode'
    Assert-EmbeddedBinary $build dlss5 collision cso $updated
    Start-Sleep -Milliseconds 1100
    Write-Input $hlsl "#include `"value.hlsli`"`nfloat4 main() : SV_Target { return VALUE * 0.5; }`n"
    $edited = Join-Path $reference 'edited.bytes'
    Invoke-ShaderTool $fxc (@($hlsl) + $fxcFlags + @('-Fo',$edited)) 'edited-reference-fxc'
    Build-ShaderCase $build dlss5 'hlsl-incremental-build'
    Assert-ShaderInput ((Get-FileHash $updated).Hash -ne (Get-FileHash $edited).Hash) 'HLSL edit changes bytecode'
    Assert-EmbeddedBinary $build dlss5 collision cso $edited
    $added = Join-Path $shaders 'nested/added.ps_5_0.hlsl'
    Write-Input $added 'float4 main() : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }'
    $addedGold = Join-Path $reference 'added.bytes'
    Invoke-ShaderTool $fxc (@($added) + $fxcFlags + @('-Fo',$addedGold)) 'added-reference-fxc'
    # No explicit configure: CONFIGURE_DEPENDS must detect the new shader.
    Build-ShaderCase $build dlss5 'new-shader-incremental-build'
    Assert-EmbeddedBinary $build dlss5 added cso $addedGold
    Save-ShaderSnapshot $build dlss5 ($stems + 'added') incremental $pollution
    foreach ($path in $pollution) {
        Assert-ShaderInput ((Get-FileHash $path).Hash -eq $pollutionHashes[$path]) "pollution input unchanged: $([IO.Path]::GetRelativePath($inputs, $path))"
    }
    $legacy = Join-Path $OutputDirectory 'legacy-root'
    Write-Input (Join-Path $legacy 'nested/original.ps_5_0.hlsl') (Get-Content -LiteralPath $added -Raw)
    [void][IO.Directory]::CreateDirectory((Join-Path $legacy 'tmp'))
    Copy-Item -LiteralPath $gold -Destination (Join-Path $legacy 'tmp/original_prebuilt.cso')
    foreach ($addon in @('dlss5','otheraddon')) {
        $caseBuild = Join-Path $OutputDirectory "$addon-default-build"
        $override = if ($addon -eq 'otheraddon') { $shaders } else { '' }
        Configure-ShaderCase $caseBuild $legacy $addon $override "$addon-default-configure"
        Build-ShaderCase $caseBuild $addon "$addon-default-build"
        Assert-EmbeddedBinary $caseBuild $addon original cso $addedGold
        Assert-EmbeddedBinary $caseBuild $addon original_prebuilt cso $gold
        Save-ShaderSnapshot $caseBuild $addon @('original','original_prebuilt') "$addon-default-root"
    }
    $invalidBuild = Join-Path $OutputDirectory 'invalid-root-build'
    Invoke-ShaderTool $cmake @('-S',$project,'-B',$invalidBuild,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$ninja",
        '-DTEST_ADDON=dlss5',"-DTEST_ROOT=$legacy", "-DTEST_OVERRIDE=$OutputDirectory/missing") 'invalid-root-configure' -ExpectFailure
    Assert-ShaderInput ((Get-Content (Join-Path $OutputDirectory 'invalid-root-configure.log') -Raw) -match 'Shader source root for dlss5 does not exist:') 'invalid-root error identifies the addon and missing directory'

    # Reuse one build directory and identical HLSL path: a root change must not leave a
    # COPYONLY CSO newer than its unchanged shader source hidden behind Ninja's log.
    $migrationRoot = Join-Path $OutputDirectory 'migration-repo'
    $migrationShaders = Join-Path $migrationRoot 'shaders'
    Write-Input (Join-Path $migrationShaders 'collision.ps_5_0.hlsl') (Get-Content -LiteralPath $added -Raw)
    [void][IO.Directory]::CreateDirectory((Join-Path $migrationRoot 'tmp'))
    Copy-Item -LiteralPath $stale -Destination (Join-Path $migrationRoot 'tmp/collision.cso')
    $migrationBuild = Join-Path $OutputDirectory 'migration-build'
    Configure-ShaderCase $migrationBuild $migrationRoot -Name 'migration-old-configure'
    Build-ShaderCase $migrationBuild dlss5 'migration-old-build'
    Start-Sleep -Milliseconds 1100
    Configure-ShaderCase $migrationBuild $migrationRoot -Name 'migration-old-reconfigure'
    $migrationBinary = Join-Path $migrationBuild 'dlss5.include/embed/collision.cso'
    Assert-ShaderInput ((Get-FileHash $migrationBinary).Hash -eq (Get-FileHash $stale).Hash) 'old-root reconfigure reproduces stale COPYONLY overwrite'
    Copy-Item -LiteralPath (Join-Path $migrationBuild 'build.ninja') -Destination (Join-Path $OutputDirectory 'migration-old.ninja')
    Configure-ShaderCase $migrationBuild $migrationRoot -Override $migrationShaders -Name 'migration-new-configure'
    Copy-Item -LiteralPath (Join-Path $migrationBuild 'build.ninja') -Destination (Join-Path $OutputDirectory 'migration-new.ninja')
    Invoke-ShaderTool $cmake @('--build',$migrationBuild,'--target','dlss5-shaders','--parallel','2','--','-d','explain') 'migration-new-build'
    Save-ShaderSnapshot $migrationBuild dlss5 @('collision') migrated @(Join-Path $migrationRoot 'tmp/collision.cso')
    Assert-EmbeddedBinary $migrationBuild dlss5 collision cso $addedGold

    # The production wrapper uses Visual Studio, whose timestamp tracking differs
    # from Ninja's depfile database. Keep this extra generator check to migration.
    $vsBuild = Join-Path $OutputDirectory 'migration-vs-build'
    $vsArguments = @('-S',$project,'-B',$vsBuild,'-G','Visual Studio 17 2022','-A','x64',
        '-DTEST_ADDON=dlss5',('-DTEST_ROOT=' + $migrationRoot.Replace('\','/')))
    Invoke-ShaderTool $cmake ($vsArguments + '-DTEST_OVERRIDE=') 'migration-vs-old-configure'
    Build-ShaderCase $vsBuild dlss5 'migration-vs-old-build'
    Start-Sleep -Milliseconds 1100
    Invoke-ShaderTool $cmake ($vsArguments + '-DTEST_OVERRIDE=') 'migration-vs-old-reconfigure'
    $vsBinary = Join-Path $vsBuild 'dlss5.include/embed/collision.cso'
    Assert-ShaderInput ((Get-FileHash $vsBinary).Hash -eq (Get-FileHash $stale).Hash) 'Visual Studio old-root reconfigure reproduces stale COPYONLY overwrite'
    $oldVsProject = Join-Path $OutputDirectory 'migration-vs-old.vcxproj'
    Copy-Item -LiteralPath (Join-Path $vsBuild 'dlss5-shaders.vcxproj') -Destination $oldVsProject
    Invoke-ShaderTool $cmake ($vsArguments + ('-DTEST_OVERRIDE=' + $migrationShaders.Replace('\','/'))) 'migration-vs-new-configure'
    $newVsProject = Join-Path $OutputDirectory 'migration-vs-new.vcxproj'
    Copy-Item -LiteralPath (Join-Path $vsBuild 'dlss5-shaders.vcxproj') -Destination $newVsProject
    $oldCommands = @(Select-String 'fxc.exe' $oldVsProject | ForEach-Object Line)
    $newCommands = @(Select-String 'fxc.exe' $newVsProject | ForEach-Object Line)
    Assert-ShaderInput (-not (Compare-Object $oldCommands $newCommands)) 'Visual Studio migration preserves the identical compiler command and source path'
    Build-ShaderCase $vsBuild dlss5 'migration-vs-new-build'
    $snapshots.Add([ordered]@{ stage='Visual Studio migration'; expected=(Get-FileHash $addedGold).Hash;
        actual=(Get-FileHash $vsBinary).Hash; stale=(Get-FileHash $stale).Hash })
    foreach ($path in $identity.Keys) {
        Assert-ShaderInput ((Get-FileHash -LiteralPath $path).Hash -eq $identity[$path]) "test input/tool unchanged: $([IO.Path]::GetFileName($path))"
    }
    if ((Get-FileHash $vsBinary).Hash -ne (Get-FileHash $addedGold).Hash) {
        $failureSample = Join-Path $OutputDirectory 'failure-sample'
        [void][IO.Directory]::CreateDirectory($failureSample)
        Copy-Item -LiteralPath $vsBinary -Destination (Join-Path $failureSample 'actual.cso')
        Copy-Item -LiteralPath $addedGold -Destination (Join-Path $failureSample 'expected.bytes')
        Copy-Item -LiteralPath (Join-Path $vsBuild 'dlss5.include/embed/collision.h') -Destination $failureSample
    }
    Assert-ShaderInput ((Get-FileHash $vsBinary).Hash -eq (Get-FileHash $addedGold).Hash) 'Visual Studio same-path root migration recompiles stale CSO'
    Assert-EmbeddedBinary $vsBuild dlss5 collision cso $addedGold
    $vsBinaryTime = (Get-Item -LiteralPath $vsBinary).LastWriteTimeUtc.Ticks
    Invoke-ShaderTool $cmake ($vsArguments + ('-DTEST_OVERRIDE=' + $migrationShaders.Replace('\','/'))) 'migration-vs-same-root-configure'
    Assert-ShaderInput ((Get-Item -LiteralPath $vsBinary).LastWriteTimeUtc.Ticks -eq $vsBinaryTime) 'Visual Studio same-root configure preserves generated CSO timestamp'
    Assert-EmbeddedBinary $vsBuild dlss5 collision cso $addedGold
    Build-ShaderCase $vsBuild dlss5 'migration-vs-same-root-build'
    Assert-ShaderInput (-not ((Get-Content (Join-Path $OutputDirectory 'migration-vs-same-root-build.log') -Raw) -match 'Generating dlss5.include/embed/collision')) 'Visual Studio same-root build preserves incremental outputs'
    Invoke-ShaderTool $cmake ($vsArguments + '-DTEST_OVERRIDE=') 'migration-vs-disable-configure'
    $cache = Get-Content -LiteralPath (Join-Path $vsBuild 'CMakeCache.txt') -Raw
    Assert-ShaderInput ($cache.Contains('RENODX_SHADER_SOURCE_ROOT_dlss5_PREVIOUS:INTERNAL=' + $migrationRoot.Replace('\','/'))) 'disabling override records the fallback effective root'
    Assert-ShaderInput ((Get-FileHash $vsBinary).Hash -eq (Get-FileHash $stale).Hash) 'disabled override restores legacy COPYONLY input behavior'
    Build-ShaderCase $vsBuild dlss5 'migration-vs-disabled-build'
    Invoke-ShaderTool $cmake ($vsArguments + ('-DTEST_OVERRIDE=' + $migrationShaders.Replace('\','/'))) 'migration-vs-reenable-configure'
    Build-ShaderCase $vsBuild dlss5 'migration-vs-reenable-build'
    Assert-EmbeddedBinary $vsBuild dlss5 collision cso $addedGold
    $snapshots.Add([ordered]@{ stage='Visual Studio override reenabled'; expected=(Get-FileHash $addedGold).Hash;
        actual=(Get-FileHash $vsBinary).Hash })
    $outcome = 'Passed'
    Write-Host "Shader input regression passed: $OutputDirectory"
} catch {
    $_ | Out-String | Set-Content -LiteralPath (Join-Path $OutputDirectory 'failure.log') -Encoding utf8
    throw
} finally {
    [ordered]@{ status=$outcome; evidence='STATIC; real CMake/Ninja, FXC, Slang, analyze and embed; no GPU/game';
        hostCommit=$hostCommit; inputs=$identity; passedChecks=@($checks.ToArray()); snapshots=@($snapshots.ToArray()) } |
        ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
    # Register only files created below this unique run; retain source, graph, hashes and logs.
    foreach ($item in Get-ChildItem -LiteralPath $OutputDirectory -File -Recurse) {
        if ([IO.Path]::GetRelativePath($OutputDirectory, $item.FullName).Replace('\','/').StartsWith('failure-sample/')) { continue }
        if ($item.Extension -in @('.cso','.spv','.bytes','.d','.bin') -or $item.Name -like '.ninja_*') {
            Register-TestDisposable $OutputDirectory $item.FullName -Kind 'synthetic-shader-intermediate'
        }
    }
    Complete-TestOutput $OutputDirectory -Outcome $outcome
}
