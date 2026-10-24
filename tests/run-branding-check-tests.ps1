param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$taskCheck = Join-Path $PSScriptRoot '../tools/check-branding.ps1'
$taskFixture = Join-Path $OutputDirectory 'branding-fixture.hpp'
$taskCases = @(
    @{Text='const char* title = "RenoDX DLSS5 Generic";'; Pass=$false},
    @{Text='const char* title = "renodx_dlss-5";'; Pass=$false},
    @{Text='const char* title = "RenoDXDLSS5";'; Pass=$false},
    @{Text='const char* title = "DLSS 5 Neural Rendering";'; Pass=$false},
    @{Text='const char* version = "v8.5.0-rc10";'; Pass=$false},
    @{Text='const char* version = "8.5.0-rc10";'; Pass=$false},
    @{Text='const char* version = "v0.1.0-dev.2";'; Pass=$false},
    @{Text='const char* title = "NR Nexus";'; Pass=$false},
    @{Text='// RenoDX DLSS5 Generic v8.5.0-rc10'; Pass=$true},
    @{Text='const char* path = "renodx-dlss5.addon64";'; Pass=$true},
    @{Text='const char* path = "RenoDX-DLSS5-crash.log";'; Pass=$true},
    @{Text='const char* schema = "renodx-dlss5-report/1";'; Pass=$true},
    @{Text='const char* key = "RenoDX.DLSS5";'; Pass=$true},
    @{Text='const char* technical = "DLSS DLSS5 DLSSNR RenoDX NGX";'; Pass=$true}
)
foreach ($taskCase in $taskCases) {
    [IO.File]::WriteAllText($taskFixture, $taskCase.Text)
    & pwsh.exe -NoProfile -File $taskCheck -Path $taskFixture *> (Join-Path $OutputDirectory 'checker-case.log')
    if (($LASTEXITCODE -eq 0) -ne $taskCase.Pass) { throw "Brand checker case failed: $($taskCase.Text)" }
}
Write-Host "Brand checker: $($taskCases.Count) positive/negative cases passed."
