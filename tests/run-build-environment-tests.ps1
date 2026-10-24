[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-support.ps1')
Initialize-TestBuild
$taskNames = @('PATH','INCLUDE','LIB','LIBPATH','VCToolsInstallDir','WindowsSDKVersion')
$taskInitial = @{}
foreach ($taskName in $taskNames) {
    $taskInitial[$taskName] = [Environment]::GetEnvironmentVariable($taskName, 'Process')
}
foreach ($taskRun in 1..3) {
    Initialize-TestBuild
    foreach ($taskName in $taskNames) {
        if ([Environment]::GetEnvironmentVariable($taskName, 'Process') -cne $taskInitial[$taskName]) {
            throw "Repeated MSVC initialization changed $taskName on call $($taskRun + 1)."
        }
    }
}
Write-Host 'PASS repeated MSVC/SDK initialization preserves the qualified x64 environment.'
