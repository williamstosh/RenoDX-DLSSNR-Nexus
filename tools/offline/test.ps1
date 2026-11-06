[CmdletBinding()]
param(
    [ValidateSet('all','backend','contracts','suites')][string]$TestSet = 'all',
    [string]$PythonPath,
    [string]$ReplayBackendExe,
    [ValidateSet('warp','nvidia')][string]$Adapter = 'warp',
    [switch]$ResolveOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$expectedVersion = '3.12.14'
$requestedPython = $PythonPath
if ([string]::IsNullOrWhiteSpace($requestedPython)) {
    $requestedPython = $env:NR_NEXUS_PYTHON
}

$pythonCandidates = @()
if (-not [string]::IsNullOrWhiteSpace($requestedPython)) {
    if ([IO.Path]::IsPathRooted($requestedPython)) {
        $pythonCandidates += [IO.Path]::GetFullPath($requestedPython)
    } else {
        $pythonCandidates += [IO.Path]::GetFullPath((Join-Path $root $requestedPython))
    }
} else {
    $pythonCandidates += Join-Path $root '.venv/Scripts/python.exe'
    if (-not [string]::IsNullOrWhiteSpace($env:APPDATA)) {
        $pythonCandidates += Join-Path $env:APPDATA "uv/python/cpython-$expectedVersion-windows-x86_64-none/python.exe"
    }
    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        $pythonCandidates += Join-Path $env:LOCALAPPDATA 'Programs/Python/Python312/python.exe'
    }

    $uvCandidates = @()
    if (-not [string]::IsNullOrWhiteSpace($env:NR_NEXUS_UV)) {
        $uvCandidates += $env:NR_NEXUS_UV
    }
    if (-not [string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        $uvCandidates += Join-Path $env:USERPROFILE '.local/bin/uv.exe'
    }
    foreach ($uvCandidate in $uvCandidates) {
        if (-not (Test-Path -LiteralPath $uvCandidate -PathType Leaf)) {
            continue
        }
        try {
            $foundPython = & $uvCandidate python find $expectedVersion 2>$null
            if ($LASTEXITCODE -eq 0 -and $foundPython) {
                $pythonCandidates += ([string]$foundPython[-1]).Trim()
            }
        } catch {
            continue
        }
    }
}

$probe = "import json,platform,struct,sys; print(json.dumps({'implementation':platform.python_implementation(),'version':platform.python_version(),'bits':struct.calcsize('P')*8,'executable':sys.executable}))"
$python = $null
$pythonInfo = $null
foreach ($candidate in ($pythonCandidates | Select-Object -Unique)) {
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        continue
    }
    try {
        $probeOutput = & $candidate -I -S -c $probe 2>$null
        if ($LASTEXITCODE -ne 0) {
            continue
        }
        $candidateInfo = ($probeOutput -join [Environment]::NewLine) | ConvertFrom-Json
    } catch {
        continue
    }
    if ($candidateInfo.implementation -eq 'CPython' -and $candidateInfo.version -eq $expectedVersion -and $candidateInfo.bits -eq 64) {
        $python = [IO.Path]::GetFullPath($candidateInfo.executable)
        $pythonInfo = $candidateInfo
        break
    }
    if (-not [string]::IsNullOrWhiteSpace($requestedPython)) {
        throw "PythonPath must identify 64-bit CPython $expectedVersion; found $($candidateInfo.implementation) $($candidateInfo.version), $($candidateInfo.bits)-bit."
    }
}
if ($null -eq $python) {
    throw "CPython $expectedVersion x64 was not found. Pass -PythonPath or set NR_NEXUS_PYTHON to its absolute path; PATH is not searched."
}
$pythonHash = (Get-FileHash -LiteralPath $python -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ResolveOnly) {
    Write-Output $python
    exit 0
}
Write-Output "Python: $python ($($pythonInfo.implementation) $($pythonInfo.version), $($pythonInfo.bits)-bit, SHA-256 $pythonHash)"

$backendRequired = $TestSet -in @('all','backend')
if ($backendRequired -and [string]::IsNullOrWhiteSpace($ReplayBackendExe)) {
    throw "-ReplayBackendExe is required for TestSet '$TestSet' so real backend cases cannot silently skip."
}
if (-not [string]::IsNullOrWhiteSpace($ReplayBackendExe)) {
    if ([IO.Path]::IsPathRooted($ReplayBackendExe)) {
        $ReplayBackendExe = [IO.Path]::GetFullPath($ReplayBackendExe)
    } else {
        $ReplayBackendExe = [IO.Path]::GetFullPath((Join-Path $root $ReplayBackendExe))
    }
    if (-not (Test-Path -LiteralPath $ReplayBackendExe -PathType Leaf)) {
        throw "Replay backend executable not found: $ReplayBackendExe"
    }
}

$patterns = switch ($TestSet) {
    'all' { @('test_offline_*.py') }
    'backend' { @('test_offline_replay_backend.py', 'test_offline_gpu_backend.py', 'test_offline_ngx_probe.py') }
    'contracts' { @('test_offline_contracts.py', 'test_offline_replay_contracts.py') }
    'suites' { @('test_offline_suites.py') }
}
$oldBackendExe = $env:NR_NEXUS_REPLAY_BACKEND_EXE
$oldAdapter = $env:NR_NEXUS_REPLAY_TEST_ADAPTER
$exitCode = 1
Push-Location $root
try {
    Remove-Item Env:NR_NEXUS_REPLAY_BACKEND_EXE -ErrorAction SilentlyContinue
    Remove-Item Env:NR_NEXUS_REPLAY_TEST_ADAPTER -ErrorAction SilentlyContinue
    if (-not [string]::IsNullOrWhiteSpace($ReplayBackendExe)) {
        $env:NR_NEXUS_REPLAY_BACKEND_EXE = $ReplayBackendExe
        $env:NR_NEXUS_REPLAY_TEST_ADAPTER = $Adapter
    }
    foreach ($pattern in $patterns) {
        $arguments = @('-I','-S','-m','unittest','discover','-s','tests','-p',$pattern,'-v')
        & $python @arguments
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne 0) {
            break
        }
    }
} finally {
    if ($null -eq $oldBackendExe) {
        Remove-Item Env:NR_NEXUS_REPLAY_BACKEND_EXE -ErrorAction SilentlyContinue
    } else {
        $env:NR_NEXUS_REPLAY_BACKEND_EXE = $oldBackendExe
    }
    if ($null -eq $oldAdapter) {
        Remove-Item Env:NR_NEXUS_REPLAY_TEST_ADAPTER -ErrorAction SilentlyContinue
    } else {
        $env:NR_NEXUS_REPLAY_TEST_ADAPTER = $oldAdapter
    }
    Pop-Location
}
exit $exitCode
