param(
    [string]$ExePath = "",
    [string]$MediaPath = "",
    [string]$ReportPath = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent $ScriptDir
$PackageExe = Join-Path $Root "build_win_full\package\CGPlay\CGPlay.exe"
$BuildExe = Join-Path $Root "build_win_full\bin\Release\CGPlay.exe"
$DefaultMedia = Join-Path $Root "tests\media\1080p_h264.mp4"

function Resolve-Executable {
    param([string]$Preferred)

    if ($Preferred -and (Test-Path -LiteralPath $Preferred)) {
        return (Resolve-Path -LiteralPath $Preferred).Path
    }
    if (Test-Path -LiteralPath $PackageExe) {
        return (Resolve-Path -LiteralPath $PackageExe).Path
    }
    if (Test-Path -LiteralPath $BuildExe) {
        return (Resolve-Path -LiteralPath $BuildExe).Path
    }
    throw "CGPlay.exe was not found. Build or package CGPlay first."
}

function Resolve-ExistingFile {
    param(
        [string]$PathValue,
        [string]$Label
    )

    if (-not $PathValue) {
        throw "$Label is empty."
    }
    if (-not (Test-Path -LiteralPath $PathValue)) {
        throw "$Label was not found: $PathValue"
    }
    return (Resolve-Path -LiteralPath $PathValue).Path
}

function Quote-ProcessArg {
    param([string]$Value)

    return '"' + ($Value -replace '"', '\"') + '"'
}

function New-SettingsNamespace {
    param([string]$ReportPath)

    $stem = [IO.Path]::GetFileNameWithoutExtension($ReportPath)
    if ([string]::IsNullOrWhiteSpace($stem)) {
        $stem = "report"
    }
    $safeStem = $stem -replace '[^A-Za-z0-9_.-]', '_'
    $fullReportPath = [IO.Path]::GetFullPath($ReportPath).ToLowerInvariant()
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($fullReportPath)))).Replace('-', '').Substring(0, 8).ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
    # $PID isolates concurrent invocations; the path digest and report stem make the store
    # deterministic and recognizable without touching normal CGPlay settings.
    $namespace = "player_smoke_{0}_{1}_{2}" -f $PID, $digest, $safeStem
    if ($namespace.Length -gt 64) {
        $namespace = $namespace.Substring(0, 64)
    }
    return $namespace
}

try {
    $ResolvedExe = Resolve-Executable -Preferred $ExePath

    if (-not $MediaPath) {
        $MediaPath = $DefaultMedia
    }
    $ResolvedMedia = Resolve-ExistingFile -PathValue $MediaPath -Label "Test media"

    if (-not $ReportPath) {
        $Stamp = Get-Date -Format "yyyyMMdd_HHmmss"
        $ReportDir = Join-Path $Root "tests\reports\player_smoke_$Stamp"
        $ReportPath = Join-Path $ReportDir "player_smoke_report.json"
    } else {
        $ReportDir = Split-Path -Parent $ReportPath
    }
    if ([string]::IsNullOrWhiteSpace($ReportDir)) {
        $ReportDir = Split-Path -Parent $ReportPath
    }
    New-Item -ItemType Directory -Path $ReportDir -Force | Out-Null
    $SettingsNamespace = New-SettingsNamespace -ReportPath $ReportPath

    Write-Host "CGPlay player function and shortcut smoke test" -ForegroundColor Cyan
    Write-Host ("EXE   : {0}" -f $ResolvedExe)
    Write-Host ("Media : {0}" -f $ResolvedMedia)
    Write-Host ("Report: {0}" -f $ReportPath)
    Write-Host ("Settings namespace: {0}" -f $SettingsNamespace)
    Write-Host ""

    $ProcessArgs = @(
        "--automation-background",
        "--automation-settings-namespace",
        $SettingsNamespace,
        "--smoke-player",
        (Quote-ProcessArg $ResolvedMedia),
        "--smoke-output",
        (Quote-ProcessArg $ReportPath)
    )
    $Process = Start-Process -FilePath $ResolvedExe -ArgumentList $ProcessArgs -Wait -PassThru
    $ExeExit = [int]$Process.ExitCode
    if ($ExeExit -ne 0) {
        Write-Host ""
        Write-Host ("CGPlay returned non-zero exit code: {0}" -f $ExeExit) -ForegroundColor Red
    }

    if (-not (Test-Path -LiteralPath $ReportPath)) {
        throw "Report was not generated: $ReportPath"
    }

    $Report = Get-Content -LiteralPath $ReportPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $Summary = $Report.summary
    $PassCount = [int]$Summary.pass
    $FailCount = [int]$Summary.fail
    $SkipCount = [int]$Summary.skip
    $ManualCount = [int]$Summary.manual
    $BackgroundMode = [bool]$Report.automation.background
    $VisiblePlatformWindows = [int]$Report.automation.visiblePlatformWindows
    $CoverageComplete = [bool]$Report.coverage.complete

    Write-Host ""
    Write-Host ("PASS={0}  FAIL={1}  SKIP={2}  MANUAL={3}" -f $PassCount, $FailCount, $SkipCount, $ManualCount) -ForegroundColor Green
    Write-Host ("Report: {0}" -f $ReportPath)
    Write-Host ("BACKGROUND={0}  VISIBLE_WINDOWS={1}" -f $BackgroundMode, $VisiblePlatformWindows)
    Write-Host ("COVERAGE_COMPLETE={0}" -f $CoverageComplete)

    $FailedItems = @($Report.results | Where-Object { $_.status -eq "FAIL" })
    if ($FailedItems.Count -gt 0) {
        Write-Host ""
        Write-Host "Failed items:" -ForegroundColor Red
        foreach ($Item in $FailedItems) {
            Write-Host ("- {0}: {1}" -f $Item.name, $Item.message)
        }
    }

    if ($ManualCount -gt 0) {
        Write-Host ""
        Write-Host "Manual-only checks:" -ForegroundColor Yellow
        foreach ($Item in $Report.manual_checks) {
            Write-Host ("- {0}" -f $Item)
        }
    }

    if ($ExeExit -ne 0 -or $FailCount -gt 0 -or $ManualCount -ne 0 -or -not $CoverageComplete -or -not $BackgroundMode -or $VisiblePlatformWindows -ne 0) {
        exit 1
    }

    exit 0
}
catch {
    Write-Host ""
    Write-Host ("Smoke test failed: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 2
}
