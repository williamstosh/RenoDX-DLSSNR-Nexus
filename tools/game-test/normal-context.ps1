# Keep installation, protection and restoration in the same ordinary user context.
function Invoke-NormalGameTestContext {
    param(
        [Parameter(Mandatory)][string]$EntryScript,
        [Parameter(Mandatory)][System.Collections.IDictionary]$Parameters,
        [Parameter(Mandatory)][string]$Python
    )
    $taskLauncher = Join-Path $PSScriptRoot 'normal_process.py'
    $taskProbeOutput = & $Python -B $taskLauncher --probe
    $taskProbeCode = $LASTEXITCODE
    if ($taskProbeCode -eq 0) { return $null }
    if ($taskProbeCode -ne 2) { throw "Cannot verify ordinary AppData context: $taskProbeOutput" }
    if ($env:NR_NEXUS_NORMAL_USER_CONTEXT -eq '1') {
        throw 'AppData is still redirected after relaunch; installation was not changed.'
    }
    $taskRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $taskRelayDirectory = Join-Path $taskRoot ('tmp/normal-process/parameters-' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($taskRelayDirectory) | Out-Null
    $taskRelayPath = Join-Path $taskRelayDirectory 'parameters.json'
    $taskValues = [ordered]@{}
    foreach ($taskKey in $Parameters.Keys) {
        $taskValue = $Parameters[$taskKey]
        $taskValues[$taskKey] = if ($taskValue -is [System.Management.Automation.SwitchParameter]) {
            [bool]$taskValue
        } else { $taskValue }
    }
    [IO.File]::WriteAllText($taskRelayPath, ($taskValues | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    Write-Host 'AppData redirection detected; relaunching the complete test entry in the ordinary user context.'
    & $Python -B $taskLauncher --entry ([IO.Path]::GetFullPath($EntryScript)) --parameters $taskRelayPath | Out-Host
    return [int]$LASTEXITCODE
}
