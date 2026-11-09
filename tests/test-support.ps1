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
    if ([string]::IsNullOrWhiteSpace($env:NR_NEXUS_TEST_BUDGET_BYTES)) { return [long](10GB) }
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
        if (@($State['retainedPaths'] | Where-Object { $_ -and ($record.path -eq $_ -or
            $record.path.Replace('\','/').StartsWith(([string]$_).Replace('\','/').TrimEnd('/') + '/', [StringComparison]::OrdinalIgnoreCase)) }).Count) { continue }
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

function Compress-TestOutput {
    param([string]$Directory, [Collections.IDictionary]$State)
    # Compact an owned run, keeping evidence and reusable inputs at their original
    # paths. This is deliberately independent of the disposable-file inventory.
    Assert-TestArtifactPath $Directory
    if ($State['keepRawArtifacts'] -or (Test-Path -LiteralPath (Join-Path $Directory 'build-manifest.json'))) { return }
    $items = @(Get-ChildItem -LiteralPath $Directory -Recurse -Force -ErrorAction Stop)
    foreach ($item in $items) { Assert-TestArtifactPath $item.FullName }
    if (-not $State.Contains('disposables')) { $State['disposables'] = @() }
    $captureRoots = @($items | Where-Object { $_.Name -eq 'captures.json' } | ForEach-Object { $_.DirectoryName })
    $keep = @($State['retainedPaths'])
    foreach ($marker in @($items | Where-Object { $_.Name -eq 'artifact-lifecycle.json' })) {
        $nested = Get-Content -LiteralPath $marker.FullName -Raw | ConvertFrom-Json -AsHashtable
        if ($nested.schema -ne 'nr-nexus-test-output/1' -or $nested['keepRawArtifacts'] -or
            (Test-TestOutputOwnerActive ([int]$nested.ownerPid))) { return }
        foreach ($path in @($nested['retainedPaths'])) {
            if ($path) { $keep += [IO.Path]::GetRelativePath($Directory, (Join-Path $marker.DirectoryName $path)) }
        }
        if (Test-Path -LiteralPath (Join-Path $marker.DirectoryName 'build-manifest.json')) {
            $keep += [IO.Path]::GetRelativePath($Directory, $marker.DirectoryName)
        }
    }
    foreach ($failure in @($items | Where-Object { $_.Name -eq 'failure.json' })) {
        $keep += [IO.Path]::GetRelativePath($Directory, $failure.FullName)
        $capture = Get-Content -LiteralPath $failure.FullName -Raw | ConvertFrom-Json -AsHashtable
        if ($capture['atlas_file']) { $keep += [IO.Path]::GetRelativePath($Directory, (Join-Path $failure.DirectoryName $capture.atlas_file)) }
        $keep += [IO.Path]::GetRelativePath($Directory, [IO.Path]::ChangeExtension($failure.FullName, '.rgba'))
    }
    foreach ($item in @($items | Where-Object { -not $_.PSIsContainer })) {
        $relative = [IO.Path]::GetRelativePath($Directory, $item.FullName).Replace('\','/')
        $explicit = @($keep | Where-Object {
            $_ -and ($relative -eq ([string]$_).Replace('\','/').TrimEnd('/') -or
                $relative.StartsWith(([string]$_).Replace('\','/').TrimEnd('/') + '/', [StringComparison]::OrdinalIgnoreCase))
        }).Count -gt 0
        if ($explicit -or $relative -match '(^|/)(inputs|runner|source|failure-sample|backup|protected)/') { continue }
        $mesh = $item.Extension -in @('.json','.rgba') -and $item.DirectoryName -in $captureRoots
        if (-not $mesh -and $item.Extension -in @('.json','.jsonl','.log','.txt','.md','.csv','.ini','.xml','.settings','.py','.ps1','.cpp','.h','.hpp')) { continue }
        if ($item.Name -eq 'artifact-lifecycle.json') { continue }
        $State['disposables'] = @($State['disposables']) + @{ path=$relative; kind='budget-intermediate' }
    }
    Remove-TestRegisteredDisposables $Directory $State
}

function Invoke-TestOutputMaintenance {
    param([string]$Root, [long]$CapacityBudgetBytes = $(Get-TestOutputBudget))
    $rootPath = [IO.Path]::GetFullPath($Root)
    if (-not (Test-Path -LiteralPath $rootPath -PathType Container)) { return }
    Assert-TestArtifactPath $rootPath
    $markers = @(Get-ChildItem -LiteralPath $rootPath -Filter 'artifact-lifecycle.json' -File -Recurse -Force -ErrorAction SilentlyContinue)
    $ancestorMarkers = @()
    $ancestor = [IO.Path]::GetDirectoryName($rootPath)
    $repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    while ($ancestor -and $ancestor.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        $ancestorMarker = Join-Path $ancestor 'artifact-lifecycle.json'
        if (Test-Path -LiteralPath $ancestorMarker) { $ancestorMarkers += Get-Item -LiteralPath $ancestorMarker }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    $protected = @(@($markers) + @($ancestorMarkers) | ForEach-Object {
        $candidate = $_
        try {
            Assert-TestArtifactPath $_.FullName
            $metadata = Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json -AsHashtable
            if ($metadata.schema -ne 'nr-nexus-test-output/1' -or $metadata['keepRawArtifacts'] -or
                (Test-TestOutputOwnerActive ([int]$metadata.ownerPid))) { $_.DirectoryName }
        } catch { $candidate.DirectoryName }
    })
    $states = @()
    foreach ($markerFile in $markers) {
        try {
            Assert-TestArtifactPath $markerFile.FullName
            $state = Get-Content -LiteralPath $markerFile.FullName -Raw | ConvertFrom-Json -AsHashtable
            if ($state.schema -ne 'nr-nexus-test-output/1') { continue }
            $directory = $markerFile.DirectoryName
            if (@($protected | Where-Object { $_ -and ($directory -eq $_ -or
                $directory.StartsWith($_ + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
                $_.StartsWith($directory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) }).Count) { continue }
            $ownerActive = Test-TestOutputOwnerActive ([int]$state.ownerPid)
            if ($state.status -eq 'Running') {
                if ($ownerActive) { continue }
                $state.status = 'Failed'
                $state.orphanedUtc = [DateTime]::UtcNow.ToString('o')
                $state.failureReason = 'owner process no longer exists'
            } elseif ($ownerActive) { continue }
            if (-not $state['keepRawArtifacts']) {
                Remove-TestRegisteredDisposables $markerFile.DirectoryName $state
            }
            $state.lastMaintenanceUtc = [DateTime]::UtcNow.ToString('o')
            $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $markerFile.FullName -Encoding utf8
            if (-not $state['keepRawArtifacts'] -and $state.status -in @('Passed','Failed')) {
                $states += [pscustomobject]@{ Marker=$markerFile; State=$state; SortKey=[string]$state.startedUtc }
            }
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
    foreach ($entry in @($states | Sort-Object SortKey)) {
        if ($bytes -le $CapacityBudgetBytes) { break }
        try {
            $directory = $entry.Marker.DirectoryName
            if (@($protected | Where-Object { $_ -and $directory.StartsWith($_ + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) }).Count) { continue }
            $state = Get-Content -LiteralPath $entry.Marker.FullName -Raw | ConvertFrom-Json -AsHashtable
            if ($state['keepRawArtifacts'] -or (Test-TestOutputOwnerActive ([int]$state.ownerPid))) { continue }
            $before = (Get-TestOutputSize $entry.Marker.DirectoryName).bytes
            Compress-TestOutput $entry.Marker.DirectoryName $state
            $state.budgetCleanupUtc = [DateTime]::UtcNow.ToString('o')
            $state.retained = Get-TestOutputSize $entry.Marker.DirectoryName
            $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $entry.Marker.FullName -Encoding utf8
            $bytes -= $before - $state.retained.bytes
        } catch {
            Write-Warning "Budget cleanup skipped $($entry.Marker.DirectoryName): $($_.Exception.Message)"
        }
    }
}

function Invoke-TestWorkspaceMaintenance {
    param([string]$RepositoryRoot = (Join-Path $PSScriptRoot '..'), [long]$CapacityBudgetBytes = $(Get-TestOutputBudget))
    $roots = @('tmp','artifacts') | ForEach-Object { Join-Path $RepositoryRoot $_ }
    $sizes = @($roots | ForEach-Object { if (Test-Path -LiteralPath $_) { (Get-TestOutputSize $_).bytes } else { 0L } })
    for ($index = 0; $index -lt $roots.Count; $index++) {
        [long]$total = ($sizes | Measure-Object -Sum).Sum
        $capacity = if ($CapacityBudgetBytes -gt 0 -and $total -gt $CapacityBudgetBytes) {
            [Math]::Max(1L, $CapacityBudgetBytes - ($total - $sizes[$index]))
        } else { 0L }
        Invoke-TestOutputMaintenance -Root $roots[$index] -CapacityBudgetBytes $capacity
        $sizes[$index] = if (Test-Path -LiteralPath $roots[$index]) { (Get-TestOutputSize $roots[$index]).bytes } else { 0L }
    }
    [long]$remaining = ($sizes | Measure-Object -Sum).Sum
    if ($CapacityBudgetBytes -gt 0 -and $remaining -gt $CapacityBudgetBytes) {
        Write-Warning "Test outputs remain above the shared budget: $remaining / $CapacityBudgetBytes bytes; active runs, recovery backups, retained evidence and unmanaged directories are protected."
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
    $keepRawArtifacts = $env:NR_NEXUS_TEST_KEEP_RAW -eq '1'
    $state = [ordered]@{ schema='nr-nexus-test-output/1'; runId=[guid]::NewGuid().ToString('N')
        ownerPid=$PID; mode=$RetentionMode; status='Running'; startedUtc=[DateTime]::UtcNow.ToString('o')
        keepRawArtifacts=$keepRawArtifacts; entry=$MyInvocation.ScriptName; sourceCommit=(& git --no-optional-locks -C $root rev-parse HEAD)
        budgetBytes=(Get-TestOutputBudget); retainedPaths=@(); disposables=@(); cleanup=@(); cleanupErrors=@() }
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
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($Directory)
    $count = 0
    [long]$bytes = 0
    while ($pending.Count -gt 0) {
        $directoryPath = $pending.Pop()
        try { $items = [IO.DirectoryInfo]::new($directoryPath).EnumerateFileSystemInfos() }
        catch [IO.DirectoryNotFoundException] { continue }
        catch [IO.FileNotFoundException] { continue }
        try {
            foreach ($item in $items) {
                try {
                    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
                    if ($item -is [IO.DirectoryInfo]) { $pending.Push($item.FullName) }
                    elseif ($item.Name -ne 'artifact-lifecycle.json') { $bytes += $item.Length; $count++ }
                } catch [IO.DirectoryNotFoundException] { continue }
                  catch [IO.FileNotFoundException] { continue }
            }
        } catch [IO.DirectoryNotFoundException] { continue }
          catch [IO.FileNotFoundException] { continue }
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
    if (-not $state.keepRawArtifacts) { Remove-TestRegisteredDisposables $directoryPath $state }
    $state['retained'] = Get-TestOutputSize $directoryPath
    $state['finishedUtc'] = [DateTime]::UtcNow.ToString('o')
    $state | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $directoryPath 'artifact-lifecycle.json') -Encoding utf8
    Invoke-TestOutputMaintenance -Root ([IO.Path]::GetDirectoryName($directoryPath))
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
