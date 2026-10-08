[CmdletBinding()]
param(
    [string]$AppRoot = "",
    [ValidateSet("full", "lite")]
    [string]$PackageMode = "full",
    [string]$OutputDirectory = "",
    [int]$MaxLogFiles = 30,
    [switch]$NoArchive
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
if ([string]::IsNullOrWhiteSpace($AppRoot)) {
    $installedRoot = Join-Path $env:LOCALAPPDATA "Programs\CGPlay"
    $AppRoot = if (Test-Path -LiteralPath $installedRoot) { $installedRoot } else { Join-Path $projectRoot "build_win_full\bin\Release" }
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot "tests\artifacts\productization_top10_20260710\diagnostics_$timestamp"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

function Invoke-DiagnosticCheck([string]$Name, [string]$ScriptPath, [hashtable]$Arguments) {
    $reportPath = Join-Path $OutputDirectory "$Name.json"
    $Arguments.ReportPath = $reportPath
    $passed = $false
    $errorText = ""
    try {
        & $ScriptPath @Arguments | Out-Null
        $passed = $true
    } catch {
        $errorText = $_.Exception.Message
    }
    return [PSCustomObject]@{
        name = $Name; passed = $passed; reportPath = $reportPath
        reportExists = Test-Path -LiteralPath $reportPath -PathType Leaf; error = $errorText
    }
}

$logsOutput = Join-Path $OutputDirectory "logs"
New-Item -ItemType Directory -Path $logsOutput -Force | Out-Null
$copiedLogs = [System.Collections.Generic.List[string]]::new()
$logRootIndex = 0
foreach ($logRoot in @((Join-Path $env:LOCALAPPDATA "CGPlay\logs"), (Join-Path $env:APPDATA "CGPlay\logs")) | Select-Object -Unique) {
    $logRootIndex++
    if (-not (Test-Path -LiteralPath $logRoot -PathType Container)) { continue }
    Get-ChildItem -LiteralPath $logRoot -File -Filter "*.log" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First $MaxLogFiles | ForEach-Object {
            $target = Join-Path $logsOutput ("source{0}_{1}" -f $logRootIndex, $_.Name)
            Copy-Item -LiteralPath $_.FullName -Destination $target -Force
            $copiedLogs.Add($target)
        }
}

$inventory = foreach ($relativePath in @(
    "CGPlay.exe", "CGPlayQuickLook.exe", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll",
    "platforms\qwindows.dll", "plugins\CGPlayAnnotationPlugin.dll", "plugins\CGPlayOcioPlugin.dll",
    "plugins\CGPlayQuickLookPlugin.dll", "resources\component_manifest.json",
    "runtime\python\python.exe", "ffmpeg.exe", "ffprobe.exe"
)) {
    $path = Join-Path $AppRoot $relativePath
    $item = Get-Item -LiteralPath $path -ErrorAction SilentlyContinue
    [PSCustomObject]@{
        relativePath = $relativePath; path = $path; exists = $null -ne $item
        length = if ($item) { $item.Length } else { $null }
        version = if ($item -and $item.Extension -in @(".exe", ".dll")) { $item.VersionInfo.FileVersion } else { $null }
        lastWriteTimeUtc = if ($item) { $item.LastWriteTimeUtc.ToString("o") } else { $null }
    }
}

$validationResults = @(
    Invoke-DiagnosticCheck "official_build_config" (Join-Path $PSScriptRoot "validate_official_build_config.ps1") @{ ProjectRoot = $projectRoot; RequireBuiltApps = $true }
    Invoke-DiagnosticCheck "component_manifest" (Join-Path $PSScriptRoot "validate_component_manifest.ps1") @{ ManifestPath = (Join-Path $projectRoot "resources\component_manifest.json") }
    Invoke-DiagnosticCheck "package_payload" (Join-Path $PSScriptRoot "validate_package_payload.ps1") @{ AppRoot = $AppRoot; PackageMode = $PackageMode }
    Invoke-DiagnosticCheck "quicklook_health" (Join-Path $PSScriptRoot "test_quicklook_health.ps1") @{ AppRoot = $AppRoot }
)

$runValue = (Get-ItemProperty -LiteralPath "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name CGPlayQuickLook -ErrorAction SilentlyContinue).CGPlayQuickLook
$summary = [PSCustomObject]@{
    schemaVersion = 1; collectedAtUtc = [DateTime]::UtcNow.ToString("o")
    appRoot = $AppRoot; packageMode = $PackageMode; projectRoot = $projectRoot
    computerName = $env:COMPUTERNAME; osVersion = [Environment]::OSVersion.VersionString
    powershellVersion = $PSVersionTable.PSVersion.ToString(); quickLookRunEntry = $runValue
    copiedLogCount = $copiedLogs.Count; inventory = $inventory; validationResults = $validationResults
}
$summaryPath = Join-Path $OutputDirectory "diagnostics_summary.json"
$summary | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $summaryPath -Encoding UTF8

$archivePath = ""
if (-not $NoArchive) {
    $archivePath = "$OutputDirectory.zip"
    if (Test-Path -LiteralPath $archivePath) { Remove-Item -LiteralPath $archivePath -Force }
    Compress-Archive -LiteralPath $OutputDirectory -DestinationPath $archivePath -CompressionLevel Optimal
}

[PSCustomObject]@{
    outputDirectory = $OutputDirectory; summaryPath = $summaryPath; archivePath = $archivePath
    copiedLogCount = $copiedLogs.Count; validationPass = @($validationResults | Where-Object { -not $_.passed }).Count -eq 0
}
