[CmdletBinding()]
param(
    [string]$InstallDir = "",
    [ValidateSet("full", "lite")]
    [string]$PackageMode = "full",
    [switch]$RequireInstalled,
    [switch]$RequireQuickLookRunEntry,
    [switch]$SkipHashes,
    [string]$ReportPath = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $projectRoot "tests\artifacts\productization_top10_20260710\installed_smoke.json"
}

$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\CGPlay"
$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$uninstall = Get-ItemProperty -LiteralPath $uninstallKey -ErrorAction SilentlyContinue
if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    $InstallDir = if ([string]::IsNullOrWhiteSpace([string]$uninstall.InstallLocation)) {
        Join-Path $env:LOCALAPPDATA "Programs\CGPlay"
    } else {
        [string]$uninstall.InstallLocation
    }
}
$InstallDir = if (Test-Path -LiteralPath $InstallDir -PathType Container) {
    (Resolve-Path -LiteralPath $InstallDir).Path
} else {
    [System.IO.Path]::GetFullPath($InstallDir)
}

function Get-CommandTarget([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return "" }
    $trimmed = $Value.Trim()
    if ($trimmed -match '^"([^"]+)"') { return $matches[1] }
    return ($trimmed -split '\s+', 2)[0]
}

$checks = [System.Collections.Generic.List[object]]::new()
$records = [System.Collections.Generic.List[object]]::new()
function Add-PathCheck([string]$Name, [string]$RelativePath, [ValidateSet("Leaf", "Container")][string]$PathType = "Leaf") {
    $path = if ([string]::IsNullOrWhiteSpace($RelativePath)) { $InstallDir } else { Join-Path $InstallDir $RelativePath }
    $exists = Test-Path -LiteralPath $path -PathType $PathType
    $item = if ($exists) { Get-Item -LiteralPath $path } else { $null }
    $passed = $exists -and ($PathType -eq "Container" -or $item.Length -gt 0)
    $checks.Add([PSCustomObject]@{ name = $Name; path = $path; kind = $PathType.ToLowerInvariant(); passed = $passed })
    if ($PathType -eq "Leaf") {
        $records.Add([PSCustomObject]@{
            name = $Name
            path = $path
            exists = $exists
            length = if ($item) { $item.Length } else { $null }
            version = if ($item -and $item.Extension -in @(".exe", ".dll")) { $item.VersionInfo.FileVersion } else { $null }
            sha256 = if ($item -and -not $SkipHashes) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
            lastWriteTimeUtc = if ($item) { $item.LastWriteTimeUtc.ToString("o") } else { $null }
        })
    }
}

$installed = Test-Path -LiteralPath $InstallDir -PathType Container
if ($installed) {
    Add-PathCheck "installDirectory" "" "Container"
    foreach ($entry in @(
        @{ Name = "CGPlay"; Path = "CGPlay.exe" },
        @{ Name = "QuickLook"; Path = "CGPlayQuickLook.exe" },
        @{ Name = "QtCore"; Path = "Qt6Core.dll" },
        @{ Name = "QtGui"; Path = "Qt6Gui.dll" },
        @{ Name = "QtWidgets"; Path = "Qt6Widgets.dll" },
        @{ Name = "qwindows"; Path = "platforms\qwindows.dll" },
        @{ Name = "annotationPlugin"; Path = "plugins\CGPlayAnnotationPlugin.dll" },
        @{ Name = "ocioPlugin"; Path = "plugins\CGPlayOcioPlugin.dll" },
        @{ Name = "quickLookPlugin"; Path = "plugins\CGPlayQuickLookPlugin.dll" },
        @{ Name = "componentManifest"; Path = "resources\component_manifest.json" }
    )) { Add-PathCheck $entry.Name $entry.Path }
    Add-PathCheck "pluginsDirectory" "plugins" "Container"
    if ($PackageMode -eq "full") {
        Add-PathCheck "pythonRuntime" "runtime\python\python.exe"
        Add-PathCheck "ffmpeg" "ffmpeg.exe"
        Add-PathCheck "ffprobe" "ffprobe.exe"
    }
}

$runValue = (Get-ItemProperty -LiteralPath $runKey -Name CGPlayQuickLook -ErrorAction SilentlyContinue).CGPlayQuickLook
$expectedQuickLook = Join-Path $InstallDir "CGPlayQuickLook.exe"
$runTarget = Get-CommandTarget ([string]$runValue)
$runTargetsInstall = -not [string]::IsNullOrWhiteSpace($runTarget) -and $runTarget -ieq $expectedQuickLook
$checks.Add([PSCustomObject]@{
    name = "quickLookRunEntry"; path = "$runKey\CGPlayQuickLook"; kind = "registry"
    passed = -not $RequireQuickLookRunEntry -or $runTargetsInstall
    configured = -not [string]::IsNullOrWhiteSpace([string]$runValue)
    targetsInstall = $runTargetsInstall; target = $runTarget; value = $runValue
})

$uninstallRegistered = $null -ne $uninstall -and [string]$uninstall.InstallLocation -ieq $InstallDir
$checks.Add([PSCustomObject]@{
    name = "uninstallRegistration"; path = $uninstallKey; kind = "registry"
    passed = -not $installed -or $uninstallRegistered
    installLocation = $uninstall.InstallLocation; displayVersion = $uninstall.DisplayVersion
    uninstallString = $uninstall.UninstallString
})

$failed = @($checks | Where-Object { -not $_.passed })
$overallPass = if (-not $installed) { -not $RequireInstalled } else { $failed.Count -eq 0 }
$report = [PSCustomObject]@{
    schemaVersion = 1; checkedAtUtc = [DateTime]::UtcNow.ToString("o")
    installDir = $InstallDir; packageMode = $PackageMode; installed = $installed
    status = if ($installed) { "checked" } else { "not-installed" }
    requireInstalled = [bool]$RequireInstalled
    requireQuickLookRunEntry = [bool]$RequireQuickLookRunEntry
    overallPass = $overallPass; checkCount = $checks.Count; failedCount = $failed.Count
    installedVersion = $uninstall.DisplayVersion; checks = $checks; fileRecords = $records
}
$directory = Split-Path -Parent $ReportPath
if ($directory) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
$report | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
$report
if (-not $overallPass) { throw "Installed smoke failed. See $ReportPath" }
