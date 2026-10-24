#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('query', 'set')][string]$Command = 'query',
    [string]$AdapterLUID,
    [Nullable[uint32]]$TargetId,
    [ValidateSet('true', 'false')][string]$Enabled,
    [string]$VcVars64
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$outputDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../tmp/hdr-state'))
$environmentNames = @('PATH', 'INCLUDE', 'LIB', 'LIBPATH', 'WindowsSdkDir', 'WindowsSDKVersion', 'VCToolsInstallDir')
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$exitCode = 1
try {
    if ($Command -eq 'set') {
        if ($AdapterLUID -cnotmatch '^[0-9a-fA-F]{16}$' -or $null -eq $TargetId -or -not $Enabled) {
            throw 'set requires -AdapterLUID (16 hex digits), -TargetId and -Enabled true|false from query.'
        }
    } elseif ($PSBoundParameters.ContainsKey('AdapterLUID') -or
              $PSBoundParameters.ContainsKey('TargetId') -or $PSBoundParameters.ContainsKey('Enabled')) {
        throw 'Target arguments are accepted only with -Command set.'
    }
    if (-not $VcVars64) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'Visual Studio C++ build tools were not found.' }
        $VcVars64 = Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat'
    }
    if (-not (Test-Path -LiteralPath $VcVars64 -PathType Leaf)) { throw 'Missing vcvars64.bat.' }

    # Import the build environment only into this process.
    # Select the SDK that declares native INFO_2 and SET_HDR_STATE structures.
    $requestedEnvironment = $environmentNames -join ' '
    $buildEnvironment = & $env:ComSpec /d /v:on /c "call `"$VcVars64`" 10.0.26100.0 -vcvars_ver=14.44 >nul && for %V in ($requestedEnvironment) do @echo %V=!%V!"
    if ($LASTEXITCODE -ne 0) { throw 'vcvars64 failed.' }
    foreach ($entry in $buildEnvironment) {
        if ($entry -match '^([^=]+)=(.*)$' -and $matches[1] -in $environmentNames) {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $executable = Join-Path $outputDirectory 'hdr-state.exe'
    $arguments = @(
        '/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT',
        '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=NTDDI_WIN11_GA',
        (Join-Path $PSScriptRoot 'hdr-state.cpp'),
        "/Fo$(Join-Path $outputDirectory 'hdr-state.obj')", "/Fe$executable", '/link', 'user32.lib'
    )
    & cl.exe @arguments *> (Join-Path $outputDirectory 'compile.log')
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) { throw 'HDR tool compilation failed; see tmp/hdr-state/compile.log.' }
    $toolArguments = @($Command)
    if ($Command -eq 'set') {
        $toolArguments += @('--adapter-luid', $AdapterLUID, '--target-id', $TargetId.ToString(), '--enabled', $Enabled.ToLowerInvariant())
    }
    $stdout = Join-Path $outputDirectory "$Command.stdout.json"
    $stderr = Join-Path $outputDirectory "$Command.stderr.log"
    & $executable @toolArguments 1> $stdout 2> $stderr
    $exitCode = $LASTEXITCODE
    Get-Content -LiteralPath $stdout -Raw | Write-Output
    $errorText = Get-Content -LiteralPath $stderr -Raw
    if ($errorText) { [Console]::Error.Write($errorText) }
} catch {
    if ($exitCode -eq 0) { $exitCode = 1 }
    @{ ok = $false; error = $_.Exception.Message; exitCode = $exitCode } | ConvertTo-Json -Compress | Write-Output
} finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
}
exit $exitCode
