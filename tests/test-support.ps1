# 仅供本地回归入口使用；不安装插件、不启动游戏。
function Initialize-TestBuild {
    param([string]$VcVars64)
    # Child suites share this process. Re-running vcvars without its private
    # markers appends the same paths and can exceed cmd.exe's 8191-char limit.
    if (-not $VcVars64 -and $env:VCToolsInstallDir -match '[\\/]14\.44\.' -and
        ([string]$env:WindowsSDKVersion).TrimEnd('\') -eq '10.0.26100.0' -and $env:INCLUDE -and $env:LIB) {
        $taskCompiler = Get-Command cl.exe -CommandType Application -ErrorAction SilentlyContinue
        $taskExpectedCompiler = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/cl.exe'
        if ($taskCompiler -and $taskCompiler.Path.Equals($taskExpectedCompiler, [StringComparison]::OrdinalIgnoreCase)) {
            return
        }
    }
    if (-not $VcVars64) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'MSVC discovery failed.' }
        $VcVars64 = Join-Path ($installation | Select-Object -First 1) 'VC/Auxiliary/Build/vcvars64.bat'
    }
    $names = @('PATH', 'INCLUDE', 'LIB', 'LIBPATH', 'WindowsSdkDir', 'WindowsSDKVersion', 'VCToolsInstallDir')
    $environment = & $env:ComSpec /d /v:on /c "call `"$VcVars64`" 10.0.26100.0 -vcvars_ver=14.44 >nul && for %V in ($($names -join ' ')) do @echo %V=!%V!"
    if ($LASTEXITCODE -ne 0) { throw 'MSVC 14.44 / Windows SDK 10.0.26100.0 required.' }
    foreach ($entry in $environment) {
        if ($entry -match '^([^=]+)=(.*)$' -and $matches[1] -in $names) {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
    if ($env:VCToolsInstallDir -notmatch '[\\/]14\.44\.' -or $env:WindowsSDKVersion.TrimEnd('\') -ne '10.0.26100.0') {
        throw 'Unexpected compiler/SDK selected; require MSVC 14.44 and SDK 26100.'
    }
}

function Initialize-TestOutput {
    param([string]$Path)
    $root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $resolved = [IO.Path]::GetFullPath($Path)
    $allowed = @('tmp', 'artifacts') | Where-Object {
        $resolved.StartsWith((Join-Path $root $_) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
    }
    if (-not $allowed) { throw 'Test output must be below this repository tmp/ or artifacts/.' }
    if ((Test-Path -LiteralPath $resolved) -and (Get-ChildItem -LiteralPath $resolved -Force | Select-Object -First 1)) {
        throw "Refusing nonempty output: $resolved"
    }
    New-Item -ItemType Directory -Force -Path $resolved | Out-Null
    return $resolved
}

function Get-TestProjectInputs {
    param([string]$BuildProject, [string]$SourceRoot, [string]$HostRoot, [string]$ShaderIncludeDirectory)
    [xml]$project = Get-Content -LiteralPath $BuildProject -Raw
    $ns = [Xml.XmlNamespaceManager]::new($project.NameTable)
    $ns.AddNamespace('msb', $project.DocumentElement.NamespaceURI)
    $release = $project.SelectNodes('//msb:ItemDefinitionGroup', $ns) |
        Where-Object { $_.Condition -eq "'`$(Configuration)|`$(Platform)'=='Release|x64'" } | Select-Object -First 1
    if (-not $release) { throw 'Release|x64 build inputs missing.' }
    # 插件根和本次生成 shader 优先；宿主根只提供共享工具与依赖。
    $includes = @($SourceRoot, $ShaderIncludeDirectory, "$HostRoot/src", "$HostRoot/src/addons/dlss5")
    $includes += @($release.ClCompile.AdditionalIncludeDirectories -split ';' | Where-Object { $_ -and $_ -notmatch '^%\(' })
    foreach ($match in [regex]::Matches($release.ClCompile.AdditionalOptions, '/external:I\s+"([^"]+)"')) {
        $includes += $match.Groups[1].Value
    }
    foreach ($include in $includes) {
        if ($include.Contains('$(')) { throw "Unresolved build macro: $include" }
        if (-not (Test-Path -LiteralPath $include -PathType Container)) { throw "Missing include: $include" }
    }
    if (-not (Test-Path -LiteralPath "$ShaderIncludeDirectory/embed/shaders.h")) { throw 'New generated shaders missing.' }
    $libraries = @($release.Link.AdditionalDependencies -split ';' | Where-Object { $_ -and $_ -notmatch '^%\(' })
    return @{ Includes = $includes; Libraries = $libraries }
}

function Invoke-TestProcess {
    param([string]$Executable, [string]$Directory, [string]$Name, [string[]]$Arguments = @())
    $start = @{ FilePath=$Executable; WorkingDirectory=$Directory; WindowStyle='Hidden'; PassThru=$true
        RedirectStandardOutput=(Join-Path $Directory "$Name.stdout.log"); RedirectStandardError=(Join-Path $Directory "$Name.stderr.log") }
    if ($Arguments.Count) { $start.ArgumentList = $Arguments }
    $process = Start-Process @start
    $null = $process.Handle
    if (-not $process.WaitForExit(45000)) { $process.Kill(); $process.WaitForExit(); throw "Test timeout: $Name" }
    $process.WaitForExit()
    Get-Content -LiteralPath $start.RedirectStandardOutput
    Get-Content -LiteralPath $start.RedirectStandardError
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) { throw "Test failed: $Name exit=$($process.ExitCode)" }
}
