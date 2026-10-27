[CmdletBinding()]
param(
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$HostRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/renodx'),
    [string]$ShaderIncludeDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.include'),
    [string]$BuildProject = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.vcxproj'),
    [string]$OutputDirectory,
    [string]$VcVars64,
    [string]$PythonExecutable,
    [switch]$BuildOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'test-support.ps1')
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$HostRoot = [IO.Path]::GetFullPath($HostRoot)
$ShaderIncludeDirectory = [IO.Path]::GetFullPath($ShaderIncludeDirectory)
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $SourceRoot ('tmp/tests/report-' + [guid]::NewGuid().ToString('N')) }
$OutputDirectory = Initialize-TestOutput $OutputDirectory
Initialize-TestBuild $VcVars64
$projectInputs = Get-TestProjectInputs $BuildProject $SourceRoot $HostRoot $ShaderIncludeDirectory
$sources = @('report_issue_tests', 'report_privacy_tests', 'report_flow_tests')
# 上传夹具由独立 worker 维护。
if (Test-Path (Join-Path $PSScriptRoot 'report_upload_tests.cpp')) { $sources += 'report_upload_tests' }
$snapshot = @{}
foreach ($path in @('dlssnr.hpp','report_flow.hpp','report_bundle.hpp','report_upload.hpp','report_issue.hpp')) {
    $full = Join-Path $SourceRoot $path
    $snapshot[$full] = (Get-FileHash -LiteralPath $full).Hash
}
foreach ($name in $sources) {
    $directory = New-Item -ItemType Directory -Path (Join-Path $OutputDirectory $name)
    $exe = Join-Path $directory.FullName "$name.exe"
    $arguments = @('/nologo','/std:c++20','/EHsc','/W3','/MT','/O2','/Gy','/Gw','/bigobj','/utf-8',
        '/DNOMINMAX','/DNDEBUG','/DRESHADE_ADDON=2')
    $includes = @($SourceRoot, "$HostRoot/external/json/include")
    $libraries = @()
    if ($name -eq 'report_flow_tests') {
        $includes = $projectInputs.Includes
        $libraries = @($projectInputs.Libraries) + @('d3d11.lib','d3d12.lib','dxgi.lib','dxguid.lib','psapi.lib','d3dcompiler.lib')
    }
    $arguments += @($includes | ForEach-Object { "/I$_" })
    $arguments += @((Join-Path $PSScriptRoot "$name.cpp"), "/Fo$directory/$name.obj", "/Fe$exe",
        '/sourceDependencies', "$directory/dependencies.json", '/link','/OPT:REF','/OPT:ICF') + $libraries
    $arguments | ConvertTo-Json | Set-Content -LiteralPath "$directory/arguments.json" -Encoding utf8
    Register-TestCompilerOutput -Directory $OutputDirectory -ObjectPath (Join-Path $directory.FullName "$name.obj") -ExecutablePath $exe
    & cl.exe @arguments *> "$directory/compile.log"
    if ($LASTEXITCODE -ne 0) {
        Get-Content "$directory/compile.log" -Tail 45
        throw "Report test build failed: $name; see $directory/compile.log"
    }
    $dependencies = (Get-Content "$directory/dependencies.json" -Raw | ConvertFrom-Json).Data.Includes
    @(@((Join-Path $PSScriptRoot "$name.cpp"), $PSCommandPath) + $dependencies | Sort-Object -Unique | ForEach-Object {
        @{ path = $_; sha256 = (Get-FileHash -LiteralPath $_).Hash }
    }) | ConvertTo-Json | Set-Content "$directory/inputs.json" -Encoding utf8
}
foreach ($path in $snapshot.Keys) {
    if ((Get-FileHash -LiteralPath $path).Hash -ne $snapshot[$path]) {
        throw "Input changed during compilation: $path; rerun against the updated source."
    }
}
if ($BuildOnly) { Write-Host "Report test executables built: $OutputDirectory"; return }
if (-not $PythonExecutable) {
    $pythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
    if (-not $pythonCommand -or $pythonCommand.Source -match '[\\/]WindowsApps[\\/]') {
        throw 'A working Python interpreter is required. Supply -PythonExecutable with its executable path (WindowsApps aliases are not supported).'
    }
    $PythonExecutable = $pythonCommand.Source
}
Invoke-TestProcess "$OutputDirectory/report_issue_tests/report_issue_tests.exe" $OutputDirectory 'issue'
$privacyData = New-Item -ItemType Directory -Path "$OutputDirectory/privacy-data"
Invoke-TestProcess "$OutputDirectory/report_privacy_tests/report_privacy_tests.exe" $OutputDirectory 'privacy' @($privacyData.FullName)
$cases = @('no-capture','no-nr-frame','timeout','written','selection','retention','json-privacy',
    'cancel-describe','cancel-review','cancel-capture','repeat','collect-failure','pack-failure','upload-consent',
    'issue-default','issue-consent','issue-feature','issue-fallback','staged-edit','test-environment','test-empty-url',
    'boundary-browser-failure','boundary-clipboard-failure','boundary-both-failure','boundary-success','boundary-manual-fallback')
foreach ($case in $cases) {
    $directory = New-Item -ItemType Directory -Path (Join-Path $OutputDirectory $case)
    # 日志放在夹具 cwd 外，保持空目录前置条件。
    $process = Start-Process -FilePath "$OutputDirectory/report_flow_tests/report_flow_tests.exe" -ArgumentList $case `
        -WorkingDirectory $directory.FullName -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$OutputDirectory/$case.stdout.log" -RedirectStandardError "$OutputDirectory/$case.stderr.log"
    $null = $process.Handle
    if (-not $process.WaitForExit(45000)) { $process.Kill(); $process.WaitForExit(); throw "Flow timeout: $case" }
    $process.WaitForExit()
    Get-Content "$OutputDirectory/$case.stdout.log" -Tail 2
    Get-Content "$OutputDirectory/$case.stderr.log"
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) { throw "Flow case failed: $case; outputs: $OutputDirectory" }
}
& $PythonExecutable (Join-Path $PSScriptRoot 'report_loopback.py') verify $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw 'Independent report ZIP verification failed.' }
if ($sources -contains 'report_upload_tests') {
    $uploadExe = "$OutputDirectory/report_upload_tests/report_upload_tests.exe"
    Invoke-TestProcess $uploadExe $OutputDirectory 'upload-unit' @('--unit')
    $ready = Join-Path $OutputDirectory 'loopback-ready.json'
    $requestLog = Join-Path $OutputDirectory 'loopback-requests.jsonl'
    $archive = (Get-Content "$OutputDirectory/no-capture/archive-manifest.json" -Raw | ConvertFrom-Json).archive
    $archiveHash = (Get-FileHash -LiteralPath $archive).Hash
    $server = Start-Process -FilePath $PythonExecutable -ArgumentList @(
        '-u', ('"' + (Join-Path $PSScriptRoot 'report_loopback.py') + '"'), 'serve',
        '--ready-file', ('"' + $ready + '"'), '--log-file', ('"' + $requestLog + '"'),
        '--expected-file', ('"' + $archive + '"')) `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput "$OutputDirectory/loopback.stdout.log" `
        -RedirectStandardError "$OutputDirectory/loopback.stderr.log"
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        while (-not (Test-Path -LiteralPath $ready)) {
            if ($server.HasExited -or [DateTime]::UtcNow -ge $deadline) { throw 'Loopback server failed to start.' }
            Start-Sleep -Milliseconds 100
        }
        $baseUrl = (Get-Content $ready -Raw | ConvertFrom-Json).base_url
        if ($baseUrl -notmatch '^http://127\.0\.0\.1:[0-9]+$') { throw 'Unsafe loopback server URL.' }
        foreach ($case in @(
            @{name='success'; endpoint='/success'; expected='success'; status='200'},
            @{name='failure'; endpoint='/fail'; expected='failure'; status='503'},
            @{name='invalid'; endpoint='/invalid'; expected='failure'; status='200'},
            @{name='proxy'; endpoint='/proxy'; expected='failure'; status='407'},
            @{name='reset'; endpoint='/reset'; expected='failure'; status='0'},
            @{name='redirect'; endpoint='/redirect'; expected='failure'; status='307'},
            @{name='oversize-response'; endpoint='/oversize-response'; expected='failure'; status='200'},
            @{name='cancel'; endpoint='/success'; expected='cancelled'; status='0'}
        )) {
            Invoke-TestProcess $uploadExe $OutputDirectory ("upload-" + $case.name) @(
                '--upload', ($baseUrl + $case.endpoint), ('"' + $archive + '"'), $case.expected, $case.status)
            if ((Get-FileHash -LiteralPath $archive).Hash -ne $archiveHash) { throw "Upload changed saved ZIP: $($case.name)" }
        }
        $requests = @(Get-Content $requestLog | ForEach-Object { $_ | ConvertFrom-Json })
        if (@($requests | Where-Object { -not $_.multipart_valid }).Count -ne 0) { throw 'Loopback rejected multipart structure.' }
        foreach ($path in @('/success','/fail','/invalid','/proxy','/reset','/redirect','/oversize-response')) {
            if (@($requests | Where-Object { $_.path -eq $path }).Count -eq 0) { throw "Missing loopback request: $path" }
        }
        if (@($requests | Where-Object { $_.path -eq '/redirect-target' }).Count -ne 0) { throw 'Upload followed redirect.' }
    } finally {
        if (-not $server.HasExited) { $server.Kill(); $server.WaitForExit() }
        $server.Dispose()
    }
    # 复用已停止服务的端口，验证真实 WinHTTP 本机离线/无法连接错误。
    Invoke-TestProcess $uploadExe $OutputDirectory 'upload-offline' @(
        '--upload', ($baseUrl + '/offline'), ('"' + $archive + '"'), 'failure', '0')
} else {
    throw 'Upload test source missing; complete report suite cannot be verified.'
}
Write-Host "Report issue, privacy, flow and loopback upload suite passed. Outputs: $OutputDirectory"
Complete-TestOutput $OutputDirectory -Outcome Passed
