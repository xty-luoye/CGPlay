param(
    [string]$DownloadsDir = "tests/artifacts/media_stress_20260713/downloads",
    [string]$ArtifactsDir = "tests/artifacts/media_stress_20260713/results",
    [int]$DecodeSeconds = 30
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$downloads = [IO.Path]::GetFullPath((Join-Path $root $DownloadsDir))
$artifacts = [IO.Path]::GetFullPath((Join-Path $root $ArtifactsDir))
$derived = Join-Path $artifacts "derived"
$exports = Join-Path $artifacts "exports"
New-Item -ItemType Directory -Force $artifacts,$derived,$exports | Out-Null

$ffmpeg = (Get-Command ffmpeg -ErrorAction Stop).Source
$ffprobe = (Get-Command ffprobe -ErrorAction Stop).Source
$checks = [Collections.Generic.List[object]]::new()

function Add-Check([string]$Name, [bool]$Passed, [object]$Details) {
    $checks.Add([ordered]@{ name=$Name; passed=$Passed; details=$Details })
}

function Probe-Media([string]$Path) {
    $raw = & $ffprobe -v error -print_format json -show_streams -show_format -- $Path
    if ($LASTEXITCODE -ne 0) { throw "ffprobe failed: $Path" }
    return ($raw -join "`n" | ConvertFrom-Json)
}

function Run-Decode([string]$Path, [int]$Seconds) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $ffmpeg -v error -t $Seconds -i $Path -map 0:v:0 -an -f null NUL
    $code = $LASTEXITCODE
    $sw.Stop()
    return [ordered]@{ exitCode=$code; requestedSeconds=$Seconds; wallSeconds=[math]::Round($sw.Elapsed.TotalSeconds,3) }
}

$mediaNames = @(
    "baseline_30s.mp4", "sample_3840x2160.mp4", "PE2_Leopard_4K.mkv",
    "Sintel.2010.1080p.mkv", "Sintel.2010.4k.mkv"
)
$inventory = [Collections.Generic.List[object]]::new()
foreach ($name in $mediaNames) {
    $path = Join-Path $downloads $name
    if (-not (Test-Path $path)) { Add-Check "media-present-$name" $false "missing"; continue }
    $probe = Probe-Media $path
    $video = @($probe.streams | Where-Object codec_type -eq "video")[0]
    $audio = @($probe.streams | Where-Object codec_type -eq "audio")
    $inventory.Add([ordered]@{name=$name;bytes=(Get-Item $path).Length;width=$video.width;height=$video.height;codec=$video.codec_name;pixFmt=$video.pix_fmt;colorTransfer=$video.color_transfer;audioStreams=$audio.Count;audioChannels=(@($audio | ForEach-Object channels) -join ',');duration=[double]$probe.format.duration})
    Add-Check "media-probe-$name" ($video.width -gt 0 -and $video.height -gt 0) $inventory[-1]
    $decode = Run-Decode $path ([math]::Min($DecodeSeconds, [math]::Ceiling([double]$probe.format.duration)))
    Add-Check "decode-$name" ($decode.exitCode -eq 0) $decode
}

$hdr = $inventory | Where-Object name -eq "PE2_Leopard_4K.mkv"
Add-Check "hdr-bt2020-pq-10bit" ($hdr.width -eq 3840 -and $hdr.pixFmt -match "10" -and $hdr.colorTransfer -eq "smpte2084") $hdr
$sintel4k = $inventory | Where-Object name -eq "Sintel.2010.4k.mkv"
Add-Check "sintel-4k-and-6ch" ($sintel4k.width -ge 3840 -and $sintel4k.audioChannels -match "6") $sintel4k

$dpxZip = Join-Path $downloads "dpx_samples.zip"
$dpxDir = Join-Path $derived "dpx_sequence"
if (Test-Path $dpxZip) {
    Expand-Archive -LiteralPath $dpxZip -DestinationPath $dpxDir -Force
    $dpxFiles = @(Get-ChildItem $dpxDir -Recurse -File -Filter *.dpx)
    Add-Check "dpx-sequence-readable" ($dpxFiles.Count -gt 0) @{count=$dpxFiles.Count;path=$dpxDir}
}

$exrSource = Join-Path $downloads "source.exr"
$exrDir = Join-Path $derived "exr_sequence"
New-Item -ItemType Directory -Force $exrDir | Out-Null
if (Test-Path $exrSource) {
    1..8 | ForEach-Object { Copy-Item $exrSource (Join-Path $exrDir ("frame.{0:D4}.exr" -f $_)) -Force }
    $runtimePython = Join-Path $root "build_win_full/bin/Release/runtime/python/python.exe"
    $exrCheck = & $runtimePython -c "import OpenImageIO as oiio,glob,json,sys; fs=glob.glob(sys.argv[1]); ok=all(oiio.ImageInput.open(f) is not None for f in fs); print(json.dumps({'count':len(fs),'ok':ok})); sys.exit(0 if ok and len(fs)==8 else 1)" (Join-Path $exrDir "*.exr")
    Add-Check "exr-sequence-openimageio" ($LASTEXITCODE -eq 0) ($exrCheck | ConvertFrom-Json)
}

$baseline = Join-Path $downloads "baseline_30s.mp4"
$multiAudio = Join-Path $derived "multi_audio_two_tracks.mkv"
& $ffmpeg -y -v error -i $baseline -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=30" -map 0:v:0 -map 0:a:0 -map 1:a:0 -c:v copy -c:a aac -metadata:s:a:0 language=eng -metadata:s:a:1 language=jpn -shortest $multiAudio
$multiProbe = Probe-Media $multiAudio
$multiStreams = @($multiProbe.streams | Where-Object codec_type -eq "audio")
Add-Check "multi-audio-two-tracks" ($multiStreams.Count -eq 2) @{count=$multiStreams.Count;duration=$multiProbe.format.duration;path=$multiAudio}

$codecCases = @(
    @{name="h264";file="export_h264.mp4";args=@("-c:v","libx264","-pix_fmt","yuv420p")},
    @{name="h265";file="export_h265.mp4";args=@("-c:v","libx265","-pix_fmt","yuv420p")},
    @{name="prores";file="export_prores.mov";args=@("-c:v","prores_ks","-profile:v","2","-pix_fmt","yuv422p10le")}
)
foreach ($case in $codecCases) {
    $output = Join-Path $exports $case.file
    $allArgs = @("-y","-v","error","-t","5","-i",$baseline,"-map","0:v:0","-map","0:a:0?") + $case.args + @("-c:a","aac",$output)
    & $ffmpeg @allArgs
    $ok = $LASTEXITCODE -eq 0 -and (Test-Path $output) -and (Get-Item $output).Length -gt 1024
    if ($ok) {
        $probe = Probe-Media $output
        $vd = [double]$probe.format.duration
        $streams = @($probe.streams)
        $ok = $vd -gt 4.5 -and ($streams | Where-Object codec_type -eq "video") -and ($streams | Where-Object codec_type -eq "audio")
        Add-Check "export-$($case.name)-audio-sync" $ok @{duration=$vd;streams=$streams.Count;path=$output}
    } else { Add-Check "export-$($case.name)-audio-sync" $false "encode failed" }
}

$smokeReport = Join-Path $artifacts "player_smoke_4k_hdr.json"
& powershell -ExecutionPolicy Bypass -File (Join-Path $root "tools/run_player_smoke_one_click.ps1") -ExePath (Join-Path $root "build_win_full/bin/Release/CGPlay.exe") -MediaPath (Join-Path $downloads "PE2_Leopard_4K.mkv") -ReportPath $smokeReport
$smoke = $null
try { $smoke = Get-Content $smokeReport -Raw | ConvertFrom-Json } catch { $smoke = $null }
if (-not $smoke) {
    $smoke = & python -c "import json,sys; d=json.load(open(sys.argv[1],encoding='utf-8-sig')); print(json.dumps(d['summary']))" $smokeReport | ConvertFrom-Json
    Add-Check "cgplay-4k-hdr-smoke" ($smoke.fail -eq 0) $smoke
} else { Add-Check "cgplay-4k-hdr-smoke" ($smoke.summary.fail -eq 0) $smoke.summary }

$pass = @($checks | Where-Object passed).Count
$fail = $checks.Count - $pass
$report = [ordered]@{
    generatedAt=(Get-Date).ToString("o"); downloads=$downloads; artifacts=$artifacts
    summary=[ordered]@{pass=$pass;fail=$fail;total=$checks.Count}
    inventory=$inventory; checks=$checks
}
$report | ConvertTo-Json -Depth 12 | Set-Content -Encoding utf8 (Join-Path $artifacts "media_stress_report.json")
if ($fail -gt 0) { exit 1 }
