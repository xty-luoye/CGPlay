[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$AppRoot,
    [ValidateSet("full", "lite")]
    [string]$PackageMode = "full",
    [string]$ReportPath = ""
)

$ErrorActionPreference = "Stop"

function Add-Check {
    param(
        [System.Collections.Generic.List[object]]$Checks,
        [string]$Name,
        [string]$Path,
        [ValidateSet("File", "Directory")]
        [string]$Kind = "File"
    )

    $pathType = if ($Kind -eq "File") { "Leaf" } else { "Container" }
    $exists = Test-Path -LiteralPath $Path -PathType $pathType
    $length = $null
    if ($exists -and $Kind -eq "File") {
        $length = (Get-Item -LiteralPath $Path).Length
        $exists = $length -gt 0
    }
    $Checks.Add([PSCustomObject]@{
        name = $Name
        kind = $Kind.ToLowerInvariant()
        path = $Path
        passed = $exists
        length = $length
    })
}

$resolvedRoot = if (Test-Path -LiteralPath $AppRoot) {
    (Resolve-Path -LiteralPath $AppRoot).Path
} else {
    [System.IO.Path]::GetFullPath($AppRoot)
}

$checks = [System.Collections.Generic.List[object]]::new()
$requiredFiles = @(
    "CGPlay.exe",
    "CGPlayQuickLook.exe",
    "CGPlayThumbnailProvider.dll",
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Widgets.dll",
    "Qt6Network.dll",
    "Qt6Concurrent.dll",
    "Qt6OpenGL.dll",
    "Qt6OpenGLWidgets.dll",
    "Qt6WebChannel.dll",
    "Qt6WebEngineCore.dll",
    "Qt6WebEngineWidgets.dll",
    "QtWebEngineProcess.exe",
    "platforms\qwindows.dll",
    "plugins\CGPlayAnnotationPlugin.dll",
    "plugins\CGPlayCodexPlugin.dll",
    "plugins\CGPlayOcioPlugin.dll",
    "plugins\CGPlayQuickLookPlugin.dll",
    "resources\component_manifest.json",
    "resources\icudtl.dat",
    "resources\qtwebengine_resources.pak",
    "resources\qtwebengine_resources_100p.pak",
    "resources\qtwebengine_resources_200p.pak",
    "docs\CGPlay_User_Guide_1.0.7.md",
    "docs\CGPlay_User_Guide_1.0.7.html",
    "tools\cgplay\export\export_engine.py"
)
$requiredDirectories = @(
    "plugins",
    "resources",
    "docs",
    "presets",
    "translations",
    "components"
)

if ($PackageMode -eq "full") {
    $requiredFiles += @(
        "runtime\python\python.exe",
        "runtime\codex\codex.exe",
        "runtime\codex\codex-code-mode-host.exe",
        "runtime\codex\LICENSE-OPENAI-CODEX.txt",
        "ffmpeg.exe",
        "ffprobe.exe"
    )
    $requiredDirectories += @("runtime\python", "runtime\codex")
}

foreach ($relativePath in $requiredFiles) {
    Add-Check -Checks $checks -Name $relativePath -Path (Join-Path $resolvedRoot $relativePath) -Kind File
}
foreach ($relativePath in $requiredDirectories) {
    Add-Check -Checks $checks -Name $relativePath -Path (Join-Path $resolvedRoot $relativePath) -Kind Directory
}

$failed = @($checks | Where-Object { -not $_.passed })
$report = [PSCustomObject]@{
    schemaVersion = 1
    checkedAtUtc = [DateTime]::UtcNow.ToString("o")
    appRoot = $resolvedRoot
    packageMode = $PackageMode
    overallPass = $failed.Count -eq 0
    checkCount = $checks.Count
    failedCount = $failed.Count
    checks = $checks
}

if (-not [string]::IsNullOrWhiteSpace($ReportPath)) {
    $reportDirectory = Split-Path -Parent $ReportPath
    if (-not [string]::IsNullOrWhiteSpace($reportDirectory)) {
        New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
    }
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
}

$report
if ($failed.Count -gt 0) {
    $missing = ($failed | ForEach-Object { $_.path }) -join [Environment]::NewLine
    throw "Package payload validation failed. Missing or empty required paths:$([Environment]::NewLine)$missing"
}
