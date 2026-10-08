<#
.SYNOPSIS
Runs the CGPlay video format matrix entirely in background automation mode.

.DESCRIPTION
Generates deterministic short videos with the bundled FFmpeg, downloads three
public FFmpeg WMV3/VC-1 boundary samples, validates metadata with ffprobe, and
runs CGPlay's player smoke for each sample. Playable rows require media open,
first-frame state, one-frame advance, metadata, and zero visible windows. The
known malformed WMV3 AVI must be rejected cleanly without a visible window.

All generated media, logs, smoke reports, and aggregate JSON files are written
below tests/artifacts/<TaskName>/.

.PARAMETER TaskName
Artifact directory name. Defaults to player_format_matrix_<timestamp>.

.PARAMETER Root
RVLite repository root. Normally inferred from this script's location.

.PARAMETER ExePath
CGPlay executable. Defaults to build_win_full/bin/Release/CGPlay.exe.

.PARAMETER TimeoutSeconds
Per-process timeout for FFmpeg, ffprobe, and CGPlay.

.PARAMETER SampleNames
Optional exact sample names for a focused rerun. The default runs all samples.

.PARAMETER Help
Prints usage without checking dependencies or running the matrix.

.EXAMPLE
pwsh -File tools/run_player_format_matrix.ps1

.EXAMPLE
pwsh -File tools/run_player_format_matrix.ps1 -TaskName player_formats_release
#>
[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9_.-]*$')]
    [string]$TaskName = ("player_format_matrix_{0}" -f (Get-Date -Format "yyyyMMdd_HHmmss")),
    [string]$Root = "",
    [string]$ExePath = "",
    [ValidateRange(15, 600)]
    [int]$TimeoutSeconds = 120,
    [string]$SampleNames = "",
    [switch]$Help
)

$ErrorActionPreference = "Stop"
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

if ($Help) {
    @"
CGPlay background player format matrix

Usage:
  powershell -File tools/run_player_format_matrix.ps1 [-TaskName <name>] [-Root <path>] [-ExePath <path>] [-TimeoutSeconds <15-600>]

Evidence:
  metadata, media open, first-frame state, one-frame advance, background mode,
  and visiblePlatformWindows=0 for every generated format.

Output:
  tests/artifacts/<TaskName>/
"@
    exit 0
}

if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
}
$Root = (Resolve-Path -LiteralPath $Root).Path
$ArtifactBase = [System.IO.Path]::GetFullPath((Join-Path $Root "tests\artifacts"))
$ArtifactRoot = [System.IO.Path]::GetFullPath((Join-Path $ArtifactBase $TaskName))
if (-not $ArtifactRoot.StartsWith($ArtifactBase + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "TaskName resolves outside tests/artifacts: $TaskName"
}

$MediaDir = Join-Path $ArtifactRoot "media"
$ResultDir = Join-Path $ArtifactRoot "results"
$Ffmpeg = Join-Path $Root "build_win_full\runtime\ffmpeg\ffmpeg.exe"
$Ffprobe = Join-Path $Root "build_win_full\runtime\ffmpeg\ffprobe.exe"
if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $Root "build_win_full\bin\Release\CGPlay.exe"
}

foreach ($Required in @($Ffmpeg, $Ffprobe, $ExePath)) {
    if (-not (Test-Path -LiteralPath $Required -PathType Leaf)) {
        throw "Required executable not found: $Required"
    }
}

New-Item -ItemType Directory -Path $MediaDir -Force | Out-Null
New-Item -ItemType Directory -Path $ResultDir -Force | Out-Null

# Each entry is intentionally generated locally so the matrix is repeatable and offline.
$Samples = @(
    [pscustomobject]@{ Name="h264_mp4"; File="h264.mp4"; Codec=@("h264"); Format=@("mov","mp4"); Size="320x180"; Args=@("-c:v","libx264","-preset","ultrafast","-crf","28","-pix_fmt","yuv420p","-movflags","+faststart") },
    [pscustomobject]@{ Name="hevc_mp4"; File="hevc.mp4"; Codec=@("hevc"); Format=@("mov","mp4"); Size="320x180"; Args=@("-c:v","libx265","-preset","ultrafast","-crf","32","-pix_fmt","yuv420p","-tag:v","hvc1","-x265-params","log-level=error") },
    [pscustomobject]@{ Name="av1_mkv"; File="av1.mkv"; Codec=@("av1"); Format=@("matroska","webm"); Size="320x180"; Args=@("-c:v","libaom-av1","-cpu-used","8","-crf","40","-b:v","0","-row-mt","1","-pix_fmt","yuv420p") },
    [pscustomobject]@{ Name="av1_mp4"; File="av1_2560x1080.mp4"; Codec=@("av1"); Format=@("mov","mp4"); Size="2560x1080"; Args=@("-c:v","libaom-av1","-cpu-used","8","-crf","35","-b:v","0","-row-mt","1","-pix_fmt","yuv420p","-movflags","+faststart") },
    [pscustomobject]@{ Name="av1_mp4_10bit"; File="av1_2560x1080_10bit.mp4"; Codec=@("av1"); Format=@("mov","mp4"); Size="2560x1080"; Args=@("-c:v","libaom-av1","-cpu-used","8","-crf","35","-b:v","0","-row-mt","1","-pix_fmt","yuv420p10le","-movflags","+faststart") },
    [pscustomobject]@{ Name="av1_mkv_10bit"; File="av1_2560x1080_10bit.mkv"; Codec=@("av1"); Format=@("matroska","webm"); Size="2560x1080"; Args=@("-c:v","libaom-av1","-cpu-used","8","-crf","35","-b:v","0","-row-mt","1","-pix_fmt","yuv420p10le") },
    [pscustomobject]@{ Name="vp8_webm"; File="vp8.webm"; Codec=@("vp8"); Format=@("matroska","webm"); Size="320x180"; Args=@("-c:v","libvpx","-deadline","realtime","-cpu-used","8","-b:v","700k","-pix_fmt","yuv420p") },
    [pscustomobject]@{ Name="vp9_webm"; File="vp9.webm"; Codec=@("vp9"); Format=@("matroska","webm"); Size="320x180"; Args=@("-c:v","libvpx-vp9","-deadline","realtime","-cpu-used","8","-crf","36","-b:v","0","-row-mt","1","-pix_fmt","yuv420p") },
    [pscustomobject]@{ Name="mpeg2_mpg"; File="mpeg2.mpg"; Codec=@("mpeg2video"); Format=@("mpeg"); Size="320x180"; Args=@("-c:v","mpeg2video","-q:v","5","-pix_fmt","yuv420p","-f","mpeg") },
    [pscustomobject]@{ Name="mpeg2_ts"; File="mpeg2.ts"; Codec=@("mpeg2video"); Format=@("mpegts"); Size="320x180"; Args=@("-c:v","mpeg2video","-q:v","5","-pix_fmt","yuv420p","-f","mpegts") },
    [pscustomobject]@{ Name="mpeg2_m2ts"; File="mpeg2.m2ts"; Codec=@("mpeg2video"); Format=@("mpegts"); Size="320x180"; Args=@("-c:v","mpeg2video","-q:v","5","-pix_fmt","yuv420p","-mpegts_m2ts_mode","1","-f","mpegts") },
    [pscustomobject]@{ Name="mpeg4_avi"; File="mpeg4.avi"; Codec=@("mpeg4"); Format=@("avi"); Size="320x180"; Args=@("-c:v","mpeg4","-q:v","4","-pix_fmt","yuv420p") },
    [pscustomobject]@{ Name="mjpeg_avi"; File="mjpeg.avi"; Codec=@("mjpeg"); Format=@("avi"); Size="320x180"; Args=@("-c:v","mjpeg","-q:v","5","-pix_fmt","yuvj420p") },
    [pscustomobject]@{ Name="prores_mov"; File="prores.mov"; Codec=@("prores"); Format=@("mov","mp4"); Size="320x180"; Args=@("-c:v","prores_ks","-profile:v","0","-pix_fmt","yuv422p10le") },
    [pscustomobject]@{ Name="png_mov"; File="png_codec.mov"; Codec=@("png"); Format=@("mov","mp4"); Size="320x180"; Args=@("-c:v","png","-pix_fmt","rgb24","-f","mov") },
    [pscustomobject]@{ Name="dnxhr_mov"; File="dnxhr.mov"; Codec=@("dnxhd"); Format=@("mov","mp4"); Size="640x360"; Args=@("-c:v","dnxhd","-profile:v","dnxhr_lb","-pix_fmt","yuv422p") },
    [pscustomobject]@{ Name="wmv2_wmv"; File="wmv2.wmv"; Codec=@("wmv2"); Format=@("asf"); Size="320x180"; Args=@("-c:v","wmv2","-b:v","800k","-pix_fmt","yuv420p") },
    [pscustomobject]@{ Name="wmv2_avi"; File="wmv2.avi"; Codec=@("wmv2"); Format=@("avi"); Size="320x180"; Args=@("-c:v","wmv2","-b:v","800k","-pix_fmt","yuv420p") },
    [pscustomobject]@{ Name="wmv3_wmv"; File="wmv3_sample.wmv"; Codec=@("wmv3"); Format=@("asf"); Size="320x240"; SourceUrl="https://samples.ffmpeg.org/V-codecs/WMV9/buggyDMOdecoding.wmv"; ExpectedSha256="E7868F27B8EB1C7E38E030AA05167B921C8E0402FB3B2B5A798147192185A1F2"; ExpectPlayable=$true },
    [pscustomobject]@{ Name="wmv3_avi_invalid"; File="wmv3_sample.avi"; Codec=@("wmv3"); Format=@("avi"); Size="640x480"; SourceUrl="https://samples.ffmpeg.org/V-codecs/WMV9/wmv3.avi"; ExpectedSha256="78E179CDBE96943A921488D7B59A40D917804167F84468F4356CEC0B5A9976B6"; ExpectPlayable=$false },
    [pscustomobject]@{ Name="vc1_avi"; File="vc1_sample.avi"; Codec=@("vc1"); Format=@("avi"); Size="1920x1080"; SourceUrl="https://samples.ffmpeg.org/V-codecs/WVC1/glitch-ffvc1.avi"; ExpectedSha256="9DE31FFD7D674BF5856FEDEDC9DD0C0E511FF21BF860C42FEF7813DC05D5B37E"; ExpectPlayable=$true },
    [pscustomobject]@{ Name="msmpeg4v3_avi"; File="msmpeg4v3.avi"; Codec=@("msmpeg4v3"); Format=@("avi"); Size="320x180"; Args=@("-c:v","msmpeg4","-q:v","4","-pix_fmt","yuv420p") },
    [pscustomobject]@{ Name="h264_flv"; File="h264.flv"; Codec=@("h264"); Format=@("flv"); Size="320x180"; Args=@("-c:v","libx264","-preset","ultrafast","-crf","28","-pix_fmt","yuv420p","-f","flv") },
    [pscustomobject]@{ Name="h264_mov"; File="h264.mov"; Codec=@("h264"); Format=@("mov","mp4"); Size="320x180"; Args=@("-c:v","libx264","-preset","ultrafast","-crf","28","-pix_fmt","yuv420p") }
)
if (-not [string]::IsNullOrWhiteSpace($SampleNames)) {
    $RequestedNames = @($SampleNames.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $UnknownNames = @($RequestedNames | Where-Object { $_ -notin $Samples.Name })
    if ($UnknownNames.Count -gt 0) {
        throw "Unknown sample name(s): $($UnknownNames -join ', ')"
    }
    $Samples = @($Samples | Where-Object { $_.Name -in $RequestedNames })
}

function ConvertTo-ProcessArgument {
    param([AllowEmptyString()][string]$Value)
    if ($Value -notmatch '[\s"]') { return $Value }
    return '"' + ($Value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

function Invoke-BackgroundProcess {
    param(
        [Parameter(Mandatory=$true)][string]$FilePath,
        [Parameter(Mandatory=$true)][string[]]$Arguments,
        [Parameter(Mandatory=$true)][string]$StdoutPath,
        [Parameter(Mandatory=$true)][string]$StderrPath,
        [int]$Timeout = 120
    )

    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $FilePath
    $StartInfo.Arguments = (($Arguments | ForEach-Object { ConvertTo-ProcessArgument $_ }) -join ' ')
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    $Timer = [System.Diagnostics.Stopwatch]::StartNew()
    $null = $Process.Start()
    $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
    $StderrTask = $Process.StandardError.ReadToEndAsync()
    $Finished = $Process.WaitForExit($Timeout * 1000)
    if (-not $Finished) {
        try { $Process.Kill($true) } catch { try { $Process.Kill() } catch {} }
        $Process.WaitForExit()
    }
    $Timer.Stop()
    [System.IO.File]::WriteAllText($StdoutPath, $StdoutTask.Result, $Utf8NoBom)
    [System.IO.File]::WriteAllText($StderrPath, $StderrTask.Result, $Utf8NoBom)
    return [pscustomobject]@{
        ExitCode = $(if ($Finished) { $Process.ExitCode } else { -999 })
        TimedOut = -not $Finished
        DurationMs = $Timer.ElapsedMilliseconds
    }
}

function Find-SmokeResult {
    param([object]$Report, [string]$Name)
    return @($Report.results | Where-Object { $_.name -eq $Name }) | Select-Object -First 1
}

$Matrix = foreach ($Sample in $Samples) {
    $MediaPath = Join-Path $MediaDir $Sample.File
    $Prefix = Join-Path $ResultDir $Sample.Name
    $HasExpectedPlayable = $Sample.PSObject.Properties.Name -contains "ExpectPlayable"
    $ExpectedPlayable = if ($HasExpectedPlayable) { [bool]$Sample.ExpectPlayable } else { $true }
    if (-not [string]::IsNullOrWhiteSpace([string]$Sample.SourceUrl)) {
        try {
            $null = Invoke-WebRequest -UseBasicParsing -Uri $Sample.SourceUrl -OutFile $MediaPath -TimeoutSec $TimeoutSeconds
            $Generation = [pscustomobject]@{ ExitCode=0; TimedOut=$false; DurationMs=$null }
            [System.IO.File]::WriteAllText(($Prefix + "_download.stderr.txt"), "", $Utf8NoBom)
        } catch {
            [System.IO.File]::WriteAllText(($Prefix + "_download.stderr.txt"), $_.Exception.Message, $Utf8NoBom)
            $Generation = [pscustomobject]@{ ExitCode=1; TimedOut=$false; DurationMs=$null }
        }
    } else {
        $Generation = Invoke-BackgroundProcess -FilePath $Ffmpeg `
            -Arguments (@("-y","-hide_banner","-loglevel","error","-f","lavfi","-i",("testsrc2=size={0}:rate=24:duration=2" -f $Sample.Size),"-an") + $Sample.Args + @($MediaPath)) `
            -StdoutPath ($Prefix + "_generate.stdout.txt") -StderrPath ($Prefix + "_generate.stderr.txt") -Timeout $TimeoutSeconds
    }
    $MediaHash = if (Test-Path -LiteralPath $MediaPath -PathType Leaf) {
        (Get-FileHash -LiteralPath $MediaPath -Algorithm SHA256).Hash
    } else { $null }
    $MediaHashPass = [string]::IsNullOrWhiteSpace([string]$Sample.ExpectedSha256) -or
        $MediaHash -eq [string]$Sample.ExpectedSha256

    $ProbePath = $Prefix + "_probe.json"
    $ProbeRun = if ($Generation.ExitCode -eq 0) {
        Invoke-BackgroundProcess -FilePath $Ffprobe `
            -Arguments @("-v","error","-select_streams","v:0","-show_entries","stream=codec_name,codec_long_name,profile,pix_fmt,width,height,avg_frame_rate,r_frame_rate,duration","-show_entries","format=format_name,format_long_name,duration","-of","json",$MediaPath) `
            -StdoutPath $ProbePath -StderrPath ($Prefix + "_probe.stderr.txt") -Timeout $TimeoutSeconds
    } else { $null }

    $Probe = $null
    if ($null -ne $ProbeRun -and $ProbeRun.ExitCode -eq 0) {
        try { $Probe = Get-Content -LiteralPath $ProbePath -Raw -Encoding UTF8 | ConvertFrom-Json } catch {}
    }
    $Stream = if ($null -ne $Probe) { @($Probe.streams) | Select-Object -First 1 } else { $null }
    $FormatNames = if ($null -ne $Probe) { @([string]$Probe.format.format_name -split ',') } else { @() }
    $ExpectedDimensions = @($Sample.Size -split 'x')
    $FrameRate = if ($null -ne $Stream) { [string]$Stream.avg_frame_rate } else { "" }
    if ($FrameRate -in @("", "0/0", "N/A") -and $null -ne $Stream) {
        $FrameRate = [string]$Stream.r_frame_rate
    }
    $MetadataPass = $null -ne $Stream -and
        $Sample.Codec -contains [string]$Stream.codec_name -and
        @($Sample.Format | Where-Object { $FormatNames -contains $_ }).Count -gt 0 -and
        [int]$Stream.width -eq [int]$ExpectedDimensions[0] -and
        [int]$Stream.height -eq [int]$ExpectedDimensions[1] -and
        -not [string]::IsNullOrWhiteSpace([string]$Stream.pix_fmt) -and
        $FrameRate -notin @("", "0/0", "N/A") -and
        [double]$Probe.format.duration -gt 0

    $SmokePath = $Prefix + "_smoke.json"
    $SmokeRun = if ($Generation.ExitCode -eq 0 -and ($MetadataPass -or -not $ExpectedPlayable)) {
        Invoke-BackgroundProcess -FilePath $ExePath `
            -Arguments @("--automation-background","--automation-settings-namespace",("format_matrix_" + $TaskName + "_" + $Sample.Name),"--smoke-player",$MediaPath,"--smoke-output",$SmokePath) `
            -StdoutPath ($Prefix + "_smoke.stdout.txt") -StderrPath ($Prefix + "_smoke.stderr.txt") -Timeout $TimeoutSeconds
    } else { $null }

    $Report = $null
    if ($null -ne $SmokeRun -and (Test-Path -LiteralPath $SmokePath -PathType Leaf)) {
        try { $Report = Get-Content -LiteralPath $SmokePath -Raw -Encoding UTF8 | ConvertFrom-Json } catch {}
    }
    $Open = if ($null -ne $Report) { Find-SmokeResult -Report $Report -Name "open media" } else { $null }
    $Right = if ($null -ne $Report) { Find-SmokeResult -Report $Report -Name "Right" } else { $null }
    $BackgroundPass = $null -ne $Report -and $Report.automation.background -eq $true -and [int]$Report.automation.visiblePlatformWindows -eq 0
    $OpenPass = $null -ne $Open -and $Open.status -eq "PASS" -and $Report.baseline.valid -eq $true
    $FirstFramePass = $OpenPass -and [int]$Report.baseline.frame -ge 0 -and [int]$Report.baseline.total -gt 0 -and [double]$Report.baseline.fps -gt 0
    $FrameAdvancePass = $false
    if ($null -ne $Right -and $Right.status -eq "PASS") {
        $BeforeFrame = [int]$Right.details.before.frame
        $AfterFrame = [int]$Right.details.after.frame
        $TotalFrames = [Math]::Max(1, [int]$Right.details.before.total)
        $FrameAdvancePass = $AfterFrame -eq (($BeforeFrame + 1) % $TotalFrames)
    }
    $SafeRejectPass = -not $ExpectedPlayable -and $null -ne $SmokeRun -and
        -not $SmokeRun.TimedOut -and $BackgroundPass -and $null -ne $Open -and
        $Open.status -eq "FAIL" -and $Open.message -eq "player invalid"
    $Pass = if ($ExpectedPlayable) {
        $Generation.ExitCode -eq 0 -and $MediaHashPass -and $MetadataPass -and $null -ne $SmokeRun -and
            $SmokeRun.ExitCode -eq 0 -and $BackgroundPass -and $OpenPass -and
            $FirstFramePass -and $FrameAdvancePass
    } else {
        $Generation.ExitCode -eq 0 -and $MediaHashPass -and $SafeRejectPass
    }

    [pscustomobject][ordered]@{
        name = $Sample.Name
        media = $MediaPath
        requestedCodec = $Sample.Codec -join '|'
        requestedFormat = $Sample.Format -join '|'
        expectedPlayable = $ExpectedPlayable
        expectedSha256 = if ($null -ne $Sample.ExpectedSha256) { [string]$Sample.ExpectedSha256 } else { $null }
        mediaSha256 = $MediaHash
        mediaHashPass = [bool]$MediaHashPass
        codec = if ($null -ne $Stream) { $Stream.codec_name } else { $null }
        format = if ($null -ne $Probe) { $Probe.format.format_name } else { $null }
        width = if ($null -ne $Stream) { $Stream.width } else { $null }
        height = if ($null -ne $Stream) { $Stream.height } else { $null }
        pixelFormat = if ($null -ne $Stream) { $Stream.pix_fmt } else { $null }
        frameRate = if ($null -ne $Stream) { $FrameRate } else { $null }
        durationSeconds = if ($null -ne $Probe) { $Probe.format.duration } else { $null }
        metadataPass = [bool]$MetadataPass
        openPass = [bool]$OpenPass
        openDispatchMs = if ($null -ne $Open) { $Open.details.openDispatchMs } else { $null }
        firstFramePass = [bool]$FirstFramePass
        frameAdvancePass = [bool]$FrameAdvancePass
        safeRejectPass = [bool]$SafeRejectPass
        background = if ($null -ne $Report) { $Report.automation.background } else { $null }
        visiblePlatformWindows = if ($null -ne $Report) { $Report.automation.visiblePlatformWindows } else { $null }
        generationExitCode = $Generation.ExitCode
        probeExitCode = if ($null -ne $ProbeRun) { $ProbeRun.ExitCode } else { $null }
        playerExitCode = if ($null -ne $SmokeRun) { $SmokeRun.ExitCode } else { $null }
        pass = [bool]$Pass
    }
}

$MatrixPath = Join-Path $ArtifactRoot "matrix.json"
$SummaryPath = Join-Path $ArtifactRoot "summary.json"
[System.IO.File]::WriteAllText($MatrixPath, ($Matrix | ConvertTo-Json -Depth 10), $Utf8NoBom)
$Failed = @($Matrix | Where-Object { -not $_.pass })
$Summary = [ordered]@{
    generatedAt = (Get-Date).ToString("o")
    backgroundRequired = $true
    visiblePlatformWindowsRequired = 0
    executable = (Resolve-Path -LiteralPath $ExePath).Path
    total = @($Matrix).Count
    pass = @($Matrix).Count - $Failed.Count
    fail = $Failed.Count
    allBackground = @($Matrix | Where-Object { $_.background -ne $true }).Count -eq 0
    allZeroVisiblePlatformWindows = @($Matrix | Where-Object { $_.visiblePlatformWindows -ne 0 }).Count -eq 0
    matrix = $MatrixPath
    failedFormats = @($Failed | ForEach-Object { [string]$_.name })
}
[System.IO.File]::WriteAllText($SummaryPath, ($Summary | ConvertTo-Json -Depth 6), $Utf8NoBom)

$Matrix | Select-Object name,codec,format,metadataPass,openPass,firstFramePass,frameAdvancePass,background,visiblePlatformWindows,openDispatchMs,pass | Format-Table -AutoSize
Write-Output ("RESULT total={0} pass={1} fail={2} summary={3}" -f $Summary.total,$Summary.pass,$Summary.fail,$SummaryPath)
if ($Failed.Count -gt 0 -or -not $Summary.allBackground -or -not $Summary.allZeroVisiblePlatformWindows) { exit 1 }
exit 0
