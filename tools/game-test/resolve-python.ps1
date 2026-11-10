# Resolve a real Python interpreter for local test entry points.
function Test-NrNexusPython {
    param(
        [Parameter(Mandatory)][string]$Path,
        [string[]]$RequiredImports = @('PIL')
    )
    try {
        $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf) -or
            $resolved.StartsWith((Join-Path $env:LOCALAPPDATA 'Microsoft/WindowsApps'), [StringComparison]::OrdinalIgnoreCase)) {
            return $false
        }
        $imports = ($RequiredImports | ForEach-Object { "import $_" }) -join '; '
        $probe = if ($imports) { "$imports; " } else { '' }
        & $resolved -c ($probe + 'import sys; print(sys.executable)') *> $null
        return $LASTEXITCODE -eq 0
    } catch {
        return $false
    }
}

function Resolve-NrNexusPython {
    param(
        [string]$RequestedPath,
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [string[]]$RequiredImports = @('PIL')
    )
    if ($RequestedPath) {
        $candidate = [IO.Path]::GetFullPath($RequestedPath)
        if (-not (Test-NrNexusPython $candidate $RequiredImports)) {
            throw "Python interpreter is missing, not runnable, or lacks required modules: $candidate. Pass -Python with a real interpreter path."
        }
        return $candidate
    }

    $candidates = @(
        (Join-Path $RepositoryRoot '.local/game-compare-venv/Scripts/python.exe'),
        (Join-Path $RepositoryRoot '.venv/Scripts/python.exe')
    )
    if ($env:NR_NEXUS_PYTHON) { $candidates += $env:NR_NEXUS_PYTHON }
    foreach ($name in @('python.exe', 'py.exe')) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command -and $command.Source) { $candidates += $command.Source }
    }
    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (Test-NrNexusPython $candidate $RequiredImports) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "No working Python interpreter with the required modules was found. Install or restore .local/game-compare-venv, or pass -Python with a real interpreter path; WindowsApps aliases are ignored."
}
