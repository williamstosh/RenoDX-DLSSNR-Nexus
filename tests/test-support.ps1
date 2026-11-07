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

function Get-TestOutputBudget {
    if ([string]::IsNullOrWhiteSpace($env:NR_NEXUS_TEST_BUDGET_BYTES)) { return [long](4GB) }
    [long]$budget = 0
    if (-not [long]::TryParse($env:NR_NEXUS_TEST_BUDGET_BYTES, [Globalization.NumberStyles]::Integer,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$budget) -or $budget -lt 0) {
        throw 'NR_NEXUS_TEST_BUDGET_BYTES must be a non-negative integer.'
    }
    return $budget
}

function Test-TestOutputOwnerActive {
    param([int]$OwnerPid)
    if ($OwnerPid -le 0) { return $false }
    if ($OwnerPid -eq $PID) { return $true }
    $owner = $null
    try { $owner = [Diagnostics.Process]::GetProcessById($OwnerPid) }
    catch [ArgumentException] { return $false }
    try { return -not $owner.HasExited } finally { $owner.Dispose() }
}

function Remove-TestRegisteredDisposables {
    param([string]$Directory, $State)
    $directoryPath = [IO.Path]::GetFullPath($Directory)
    $records = if (($State.PSObject.Properties.Name -contains 'disposables') -or
        ($State -is [Collections.IDictionary] -and $State.Contains('disposables'))) {
        @($State.disposables)
    } elseif (($State.PSObject.Properties.Name -contains 'intermediates') -or
        ($State -is [Collections.IDictionary] -and $State.Contains('intermediates'))) {
        @($State.intermediates | ForEach-Object { @{ path=[IO.Path]::GetRelativePath($directoryPath, $_); kind='compiler-intermediate' } })
    } else { @() }
    foreach ($record in $records) {
        $path = [IO.Path]::GetFullPath((Join-Path $directoryPath ([string]$record.path)))
        try {
            Assert-TestArtifactPath $path
            if (-not $path.StartsWith($directoryPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Disposable path escaped its owned output directory.'
            }
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
            $item = Get-Item -LiteralPath $path -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse disposable refused.' }
            if ($item.Attributes -band [IO.FileAttributes]::ReadOnly) { throw 'Read-only disposable retained.' }
            $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
            $stream.Dispose()
            Remove-Item -LiteralPath $path -ErrorAction Stop
            $State.cleanup += @{ path=[IO.Path]::GetRelativePath($directoryPath,$path); bytes=$item.Length; kind=$record.kind }
        } catch {
            $State.cleanupErrors += @{ path=$path; error=$_.Exception.Message }
            Write-Warning "Intermediate cleanup stopped: $path : $($_.Exception.Message)"
        }
    }
    $remaining = @($records | Where-Object {
        $candidate = Join-Path $directoryPath ([string]$_.path)
        Test-Path -LiteralPath $candidate -PathType Leaf
    })
    if ($State -is [Collections.IDictionary]) { $State['disposables'] = $remaining }
    else { $State.disposables = $remaining }
}

function Invoke-TestOutputMaintenance {
    param([string]$Root, [long]$CapacityBudgetBytes = $(Get-TestOutputBudget))
    $rootPath = [IO.Path]::GetFullPath($Root)
    if (-not (Test-Path -LiteralPath $rootPath -PathType Container)) { return }
    $markers = @(Get-ChildItem -LiteralPath $rootPath -Filter 'artifact-lifecycle.json' -File -Recurse -Force -ErrorAction SilentlyContinue)
    foreach ($markerFile in $markers) {
        try {
            $state = Get-Content -LiteralPath $markerFile.FullName -Raw | ConvertFrom-Json -AsHashtable
            if ($state.schema -ne 'nr-nexus-test-output/1') { continue }
            $ownerActive = Test-TestOutputOwnerActive ([int]$state.ownerPid)
            if ($state.status -eq 'Running') {
                if ($ownerActive) { continue }
                $state.status = 'Failed'
                $state.orphanedUtc = [DateTime]::UtcNow.ToString('o')
                $state.failureReason = 'owner process no longer exists'
            } elseif ($ownerActive) { continue }
            if ($state.mode -eq 'Standard') {
                Remove-TestRegisteredDisposables $markerFile.DirectoryName $state
            }
            $state.lastMaintenanceUtc = [DateTime]::UtcNow.ToString('o')
            $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $markerFile.FullName -Encoding utf8
        } catch {
            Write-Warning "Test output maintenance skipped $($markerFile.FullName): $($_.Exception.Message)"
        }
    }
    if ($CapacityBudgetBytes -le 0) { return }
    [long]$bytes = 0
    foreach ($item in Get-ChildItem -LiteralPath $rootPath -File -Recurse -Force -ErrorAction SilentlyContinue) {
        if (-not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { $bytes += $item.Length }
    }
    if ($bytes -le $CapacityBudgetBytes) { return }
    $states = foreach ($markerFile in @(Get-ChildItem -LiteralPath $rootPath -Filter 'artifact-lifecycle.json' -File -Recurse -Force -ErrorAction SilentlyContinue)) {
        try {
            $state = Get-Content -LiteralPath $markerFile.FullName -Raw | ConvertFrom-Json -AsHashtable
            if ($state.mode -eq 'Standard' -and $state.status -in @('Passed','Failed') -and
                -not (Test-TestOutputOwnerActive ([int]$state.ownerPid))) {
                [pscustomobject]@{ Marker=$markerFile; State=$state; SortKey=[string]$state.finishedUtc }
            }
        } catch { }
    }
    foreach ($entry in @($states | Sort-Object SortKey)) {
        if ($bytes -le $CapacityBudgetBytes) { break }
        $before = $bytes
        Remove-TestRegisteredDisposables $entry.Marker.DirectoryName $entry.State
        $entry.State.budgetCleanupUtc = [DateTime]::UtcNow.ToString('o')
        $entry.State | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $entry.Marker.FullName -Encoding utf8
        $bytes = 0
        foreach ($item in Get-ChildItem -LiteralPath $rootPath -File -Recurse -Force -ErrorAction SilentlyContinue) {
            if (-not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { $bytes += $item.Length }
        }
        if ($before -eq $bytes) { continue }
    }
}

function Initialize-TestOutput {
    param([string]$Path, [ValidateSet('Standard','Full','Acceptance')][string]$RetentionMode = $(if ($env:NR_NEXUS_TEST_RETENTION) { $env:NR_NEXUS_TEST_RETENTION } else { 'Standard' }))
    $RetentionMode = @('Standard','Full','Acceptance') | Where-Object { $_ -ieq $RetentionMode } | Select-Object -First 1
    $root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $resolved = [IO.Path]::GetFullPath($Path)
    $allowed = @('tmp', 'artifacts') | Where-Object {
        $resolved.StartsWith((Join-Path $root $_) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
    }
    if (-not $allowed) { throw 'Test output must be below this repository tmp/ or artifacts/.' }
    Invoke-TestOutputMaintenance -Root ([IO.Path]::GetDirectoryName($resolved))
    Assert-TestArtifactPath $resolved
    if ((Test-Path -LiteralPath $resolved) -and (Get-ChildItem -LiteralPath $resolved -Force | Select-Object -First 1)) {
        throw "Refusing nonempty output: $resolved"
    }
    [void][IO.Directory]::CreateDirectory($resolved)
    if (-not (Get-Variable -Name NexusTestOutputs -Scope Global -ErrorAction SilentlyContinue)) {
        $global:NexusTestOutputs = @{}
    }
    $state = [ordered]@{ schema='nr-nexus-test-output/1'; runId=[guid]::NewGuid().ToString('N')
        ownerPid=$PID; mode=$RetentionMode; status='Running'; startedUtc=[DateTime]::UtcNow.ToString('o')
        entry=$MyInvocation.ScriptName; sourceCommit=(& git --no-optional-locks -C $root rev-parse HEAD)
        budgetBytes=(Get-TestOutputBudget); disposables=@(); cleanup=@(); cleanupErrors=@() }
    $global:NexusTestOutputs[$resolved] = $state
    $state | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $resolved 'artifact-lifecycle.json') -Encoding utf8
    return $resolved
}

function Assert-TestArtifactPath {
    param([string]$Path)
    $root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $current = [IO.Path]::GetFullPath($Path)
    if ($current -ne $root -and -not $current.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Artifact path escapes repository.'
    }
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Artifact path crosses reparse point: $current"
            }
        }
        if ($current -eq $root) { break }
        $current = [IO.Path]::GetDirectoryName($current)
    }
}

function Register-TestCompilerOutput {
    param([string]$Directory, [string[]]$ObjectPath, [string]$ExecutablePath)
    $directoryPath = [IO.Path]::GetFullPath($Directory)
    $state = $global:NexusTestOutputs[$directoryPath]
    if (-not $state -or $state.status -ne 'Running') { throw 'Compiler output requires an active owned test directory.' }
    $paths = @($ObjectPath)
    if ($ExecutablePath) { $paths += @([IO.Path]::ChangeExtension($ExecutablePath, '.lib'), [IO.Path]::ChangeExtension($ExecutablePath, '.exp')) }
    foreach ($path in $paths) {
        $absolute = [IO.Path]::GetFullPath($path)
        Assert-TestArtifactPath $absolute
        if (-not $absolute.StartsWith($directoryPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetExtension($absolute) -notin @('.obj','.lib','.exp') -or (Test-Path -LiteralPath $absolute)) {
            throw "Compiler intermediate is not a new owned path: $absolute"
        }
        Register-TestDisposable -Directory $directoryPath -Path $absolute -Kind 'compiler-intermediate'
    }
}

function Register-TestDisposable {
    param([string]$Directory, [string]$Path, [string]$Kind = 'intermediate')
    $directoryPath = [IO.Path]::GetFullPath($Directory)
    $state = $global:NexusTestOutputs[$directoryPath]
    if (-not $state -or $state.status -ne 'Running') { throw 'Disposable requires an active owned test directory.' }
    $absolute = [IO.Path]::GetFullPath($Path)
    Assert-TestArtifactPath $absolute
    if (-not $absolute.StartsWith($directoryPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        $absolute -eq (Join-Path $directoryPath 'artifact-lifecycle.json')) {
        throw "Disposable path escaped owner: $absolute"
    }
    $relative = [IO.Path]::GetRelativePath($directoryPath, $absolute)
    if (-not (@($state.disposables | Where-Object { $_.path -eq $relative }).Count)) {
        $state.disposables += @{ path=$relative; kind=$Kind }
    }
    $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $directoryPath 'artifact-lifecycle.json') -Encoding utf8
}

function Get-TestOutputSize {
    param([string]$Directory)
    $pending = [Collections.Generic.Queue[string]]::new()
    $pending.Enqueue($Directory)
    $count = 0
    [long]$bytes = 0
    while ($pending.Count) {
        foreach ($item in Get-ChildItem -LiteralPath $pending.Dequeue() -Force) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
            if ($item.PSIsContainer) { $pending.Enqueue($item.FullName) }
            elseif ($item.Name -ne 'artifact-lifecycle.json') { $count++; $bytes += $item.Length }
        }
    }
    return @{files=$count;bytes=$bytes;excludes='artifact-lifecycle.json and reparse targets'}
}

function Complete-TestOutput {
    param([string]$Directory, [ValidateSet('Passed','Failed')][string]$Outcome = 'Passed')
    $directoryPath = [IO.Path]::GetFullPath($Directory)
    if (-not (Get-Variable -Name NexusTestOutputs -Scope Global -ErrorAction SilentlyContinue) -or
        -not $global:NexusTestOutputs.ContainsKey($directoryPath)) { return }
    $state = $global:NexusTestOutputs[$directoryPath]
    if ($state.status -ne 'Running') { return }
    Assert-TestArtifactPath $directoryPath
    $marker = Get-Content -LiteralPath (Join-Path $directoryPath 'artifact-lifecycle.json') -Raw | ConvertFrom-Json
    if ($marker.runId -ne $state.runId -or $marker.ownerPid -ne $PID -or $marker.mode -ne $state.mode) {
        throw 'Test output ownership changed; cleanup refused.'
    }
    $state['generated'] = Get-TestOutputSize $directoryPath
    $state.status = $Outcome
    if ($state.mode -eq 'Standard') { Remove-TestRegisteredDisposables $directoryPath $state }
    $state['retained'] = Get-TestOutputSize $directoryPath
    $state['finishedUtc'] = [DateTime]::UtcNow.ToString('o')
    $state | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $directoryPath 'artifact-lifecycle.json') -Encoding utf8
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
    param([string]$Executable, [string]$Directory, [string]$Name, [string[]]$Arguments = @(),
        [ValidateRange(1,180000)][int]$TimeoutMilliseconds = 45000)
    $start = @{ FilePath=$Executable; WorkingDirectory=$Directory; WindowStyle='Hidden'; PassThru=$true
        RedirectStandardOutput=(Join-Path $Directory "$Name.stdout.log"); RedirectStandardError=(Join-Path $Directory "$Name.stderr.log") }
    $owner = $null
    if ($Arguments.Count) { $start.ArgumentList = $Arguments }
    if (Get-Variable -Name NexusTestOutputs -Scope Global -ErrorAction SilentlyContinue) {
        $owner = $global:NexusTestOutputs.Keys | Where-Object {
            $Directory -eq $_ -or $Directory.StartsWith($_ + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
        } | Sort-Object Length -Descending | Select-Object -First 1
        if ($owner) { $start.Environment = @{ NR_NEXUS_TEST_RETENTION=$global:NexusTestOutputs[$owner].mode } }
    }
    $process = Start-Process @start
    $null = $process.Handle
    if (-not $process.WaitForExit($TimeoutMilliseconds)) {
        $process.Kill(); $process.WaitForExit()
        if ($owner) { Complete-TestOutput $owner -Outcome Failed }
        throw "Test timeout: $Name"
    }
    $process.WaitForExit()
    Get-Content -LiteralPath $start.RedirectStandardOutput
    Get-Content -LiteralPath $start.RedirectStandardError
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) {
        if ($owner) { Complete-TestOutput $owner -Outcome Failed }
        throw "Test failed: $Name exit=$($process.ExitCode)"
    }
}
