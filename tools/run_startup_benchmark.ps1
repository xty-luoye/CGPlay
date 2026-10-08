param(
    [string]$ExePath = "",
    [string]$ArtifactsDir = "tests/artifacts/player_format_speed_20260805/startup_results",
    [int]$Runs = 5,
    [int]$TimeoutMs = 30000,
    [int]$MaxP50Ms = 10000,
    [int]$MaxP95Ms = 12000,
    [switch]$RequireLazySettings
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $root "build_win_full/bin/Release/CGPlay.exe"
}
$exe = (Resolve-Path -LiteralPath $ExePath).Path
$artifacts = [IO.Path]::GetFullPath((Join-Path $root $ArtifactsDir))
New-Item -ItemType Directory -Force $artifacts | Out-Null

$rows = @()
for ($run = 1; $run -le $Runs; ++$run) {
    $stdout = Join-Path $artifacts ("run_{0}.stdout.txt" -f $run)
    $stderr = Join-Path $artifacts ("run_{0}.stderr.txt" -f $run)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $exe `
        -ArgumentList @(
            "--automation-background",
            "--automation-settings-namespace", ("startup_benchmark_{0}_{1}" -f $PID, $run),
            "--overlay-debug"
        ) `
        -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $ready = $false
    try {
        while (-not $process.HasExited -and $timer.ElapsedMilliseconds -lt $TimeoutMs) {
            Start-Sleep -Milliseconds 20
            if ((Test-Path -LiteralPath $stderr) -and
                (Select-String -LiteralPath $stderr -SimpleMatch '"event":"main.ready"' -Quiet)) {
                $ready = $true
                break
            }
        }
    } finally {
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force
        }
        $process.WaitForExit()
    }

    $details = Get-Content -LiteralPath $stderr -Raw -Encoding UTF8
    $background = $details -match '"background":true'
    $zeroVisibleWindows = $details -match '"visiblePlatformWindows":0'
    $phases = @()
    $readyDetails = $null
    foreach ($line in ($details -split "`r?`n")) {
        if ($line -match '\[StartupTiming\] (\{.+\})$') {
            $phases += ($Matches[1] | ConvertFrom-Json)
        } elseif ($line -match '\[OverlayDebug\] (\{.+"event":"main.ready".+\})$') {
            $readyDetails = $Matches[1] | ConvertFrom-Json
        }
    }
    $settingsConstructed = if ($null -ne $readyDetails -and
        $null -ne $readyDetails.PSObject.Properties['settingsConstructedAtStartup']) {
        [bool]$readyDetails.settingsConstructedAtStartup
    } else { $null }
    $rows += [ordered]@{
        run = $run
        mainReady = $ready
        mainReadyMs = $timer.ElapsedMilliseconds
        background = $background
        visiblePlatformWindows = if ($zeroVisibleWindows) { 0 } else { -1 }
        settingsConstructedAtStartup = $settingsConstructed
        phases = $phases
    }
}

$values = @($rows | Where-Object mainReady | ForEach-Object mainReadyMs | Sort-Object)
function Percentile([object[]]$Items, [double]$Ratio) {
    if ($Items.Count -eq 0) { return $null }
    return $Items[[math]::Floor(($Items.Count - 1) * $Ratio)]
}
$summary = [ordered]@{
    generatedAt = (Get-Date).ToString("o")
    executable = $exe
    runs = $rows
    p50Ms = Percentile $values 0.50
    p95Ms = Percentile $values 0.95
    maxP50Ms = $MaxP50Ms
    maxP95Ms = $MaxP95Ms
    allReady = @($rows | Where-Object { -not $_.mainReady }).Count -eq 0
    allBackground = @($rows | Where-Object { -not $_.background }).Count -eq 0
    allZeroVisibleWindows = @($rows | Where-Object { $_.visiblePlatformWindows -ne 0 }).Count -eq 0
    requireLazySettings = [bool]$RequireLazySettings
    allLazySettings = @($rows | Where-Object {
        $null -eq $_.settingsConstructedAtStartup -or $_.settingsConstructedAtStartup
    }).Count -eq 0
}
$summary.performancePass =
    $null -ne $summary.p50Ms -and $summary.p50Ms -le $MaxP50Ms -and
    $null -ne $summary.p95Ms -and $summary.p95Ms -le $MaxP95Ms
$summaryPath = Join-Path $artifacts "startup_summary.json"
$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
$summary | ConvertTo-Json -Depth 6
if (-not $summary.allReady -or -not $summary.allBackground -or
    -not $summary.allZeroVisibleWindows -or -not $summary.performancePass -or
    ($RequireLazySettings -and -not $summary.allLazySettings)) {
    exit 1
}
