[CmdletBinding()]
param(
    [string]$AppRoot = "",
    [switch]$RequireRunning,
    [switch]$RequireRunEntry,
    [switch]$RequireSingleInstance,
    [switch]$StartIfStopped,
    [string]$ReportPath = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($AppRoot)) {
    $AppRoot = Join-Path $projectRoot "build_win_full\bin\Release"
}
$AppRoot = if (Test-Path -LiteralPath $AppRoot -PathType Container) {
    (Resolve-Path -LiteralPath $AppRoot).Path
} else {
    [System.IO.Path]::GetFullPath($AppRoot)
}
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $projectRoot "tests\artifacts\productization_top10_20260710\quicklook_health.json"
}

$quickLookPath = Join-Path $AppRoot "CGPlayQuickLook.exe"
$exeExists = Test-Path -LiteralPath $quickLookPath -PathType Leaf
$exeItem = if ($exeExists) { Get-Item -LiteralPath $quickLookPath } else { $null }
$exeSha256 = if ($exeExists) { (Get-FileHash -LiteralPath $quickLookPath -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
$processes = @(Get-CimInstance Win32_Process -Filter "Name='CGPlayQuickLook.exe'" -ErrorAction SilentlyContinue)
$matchingProcesses = @($processes | Where-Object { $_.ExecutablePath -and $_.ExecutablePath -ieq $quickLookPath })
$startedByProbe = $false

if ($StartIfStopped -and $exeExists -and $matchingProcesses.Count -eq 0) {
    Start-Process -FilePath $quickLookPath -WorkingDirectory $AppRoot -WindowStyle Hidden
    Start-Sleep -Seconds 2
    $processes = @(Get-CimInstance Win32_Process -Filter "Name='CGPlayQuickLook.exe'" -ErrorAction SilentlyContinue)
    $matchingProcesses = @($processes | Where-Object { $_.ExecutablePath -and $_.ExecutablePath -ieq $quickLookPath })
    $startedByProbe = $matchingProcesses.Count -gt 0
}

$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$runValue = (Get-ItemProperty -LiteralPath $runKey -Name CGPlayQuickLook -ErrorAction SilentlyContinue).CGPlayQuickLook
$runConfigured = -not [string]::IsNullOrWhiteSpace($runValue)
$runTarget = if (-not $runConfigured) {
    ""
} elseif ($runValue.Trim() -match '^"([^"]+)"') {
    $matches[1]
} else {
    ($runValue.Trim() -split '\s+', 2)[0]
}
$runTargetExists = -not [string]::IsNullOrWhiteSpace($runTarget) -and (Test-Path -LiteralPath $runTarget -PathType Leaf)
$runTargetsAppRoot = $runTargetExists -and $runTarget -ieq $quickLookPath

$checks = @(
    [PSCustomObject]@{ name = "quickLookExecutable"; passed = $exeExists; path = $quickLookPath; version = if ($exeItem) { $exeItem.VersionInfo.FileVersion } else { $null }; sha256 = $exeSha256 },
    [PSCustomObject]@{ name = "quickLookProcess"; passed = (-not $RequireRunning -or $matchingProcesses.Count -gt 0); required = [bool]$RequireRunning; count = $matchingProcesses.Count },
    [PSCustomObject]@{ name = "quickLookSingleInstance"; passed = (-not $RequireSingleInstance -or $matchingProcesses.Count -le 1); required = [bool]$RequireSingleInstance; count = $matchingProcesses.Count },
    [PSCustomObject]@{ name = "quickLookRunEntry"; passed = (-not $RequireRunEntry -or $runTargetsAppRoot); required = [bool]$RequireRunEntry; configured = $runConfigured; value = $runValue; target = $runTarget; targetExists = $runTargetExists; targetsAppRoot = $runTargetsAppRoot }
)
$failed = @($checks | Where-Object { -not $_.passed })
$recommendedActions = [System.Collections.Generic.List[string]]::new()
if (-not $exeExists) { $recommendedActions.Add("Repair or reinstall CGPlay so CGPlayQuickLook.exe is restored.") }
if ($exeExists -and $matchingProcesses.Count -eq 0) { $recommendedActions.Add("Start CGPlayQuickLook.exe manually, or rerun with -StartIfStopped for a controlled probe.") }
if ($matchingProcesses.Count -gt 1) { $recommendedActions.Add("Close duplicate CGPlayQuickLook.exe processes, then start one instance from the installed directory.") }
if (-not $runTargetsAppRoot) { $recommendedActions.Add("Repair the HKCU Run entry so it targets this AppRoot's CGPlayQuickLook.exe.") }
$report = [PSCustomObject]@{
    schemaVersion = 1
    checkedAtUtc = [DateTime]::UtcNow.ToString("o")
    appRoot = $AppRoot
    overallPass = $failed.Count -eq 0
    startedByProbe = $startedByProbe
    processIds = @($matchingProcesses.ProcessId)
    recommendedActions = $recommendedActions
    checks = $checks
}

$reportDirectory = Split-Path -Parent $ReportPath
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
$report

if ($failed.Count -gt 0) {
    throw "QuickLook health check failed. See $ReportPath"
}
