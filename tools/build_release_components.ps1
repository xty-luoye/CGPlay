[CmdletBinding()]
param(
    [string]$ProjectRoot = "C:\Users\1\Desktop\RVLite",
    [string]$PackageRoot = "C:\Users\1\Desktop\RVLite\build_win_full\package\full\CGPlay",
    [string]$OutputDir = "C:\Users\1\Desktop\RVLite\build_win_full\releases",
    [string]$GithubBase = "https://github.com/xty-luoye/CGPlay/releases/latest/download",
    [string]$MirrorBase = "https://cgplay-app.netlify.app/downloads",
    [string]$Version = "1.0.7.02"
)

$ErrorActionPreference = "Stop"

function Reset-Dir([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path | Out-Null
}

function New-ZipFromFolder([string]$SourceDir, [string]$ZipPath) {
    if (Test-Path -LiteralPath $ZipPath) {
        Remove-Item -LiteralPath $ZipPath -Force
    }
    Compress-Archive -Path (Join-Path $SourceDir '*') -DestinationPath $ZipPath -Force
}

function Write-Sha256([string]$Path) {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLower()
}

function New-UrlList([string]$FileName) {
    $urls = @()
    if (-not [string]::IsNullOrWhiteSpace($MirrorBase)) {
        $urls += ($MirrorBase.TrimEnd("/") + "/" + $FileName)
    }
    if (-not [string]::IsNullOrWhiteSpace($GithubBase)) {
        $urls += ($GithubBase.TrimEnd("/") + "/" + $FileName)
    }
    return @($urls)
}

function First-Url([string]$FileName) {
    $urls = @(New-UrlList $FileName)
    if ($urls.Count -gt 0) { return $urls[0] }
    return ""
}

function Copy-DirContents([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Missing required directory: $Source"
    }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $sourcePath = (Resolve-Path -LiteralPath $Source).Path
    $destPath = (Resolve-Path -LiteralPath $Destination).Path
    & robocopy $sourcePath $destPath /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -gt 7) {
        if (-not (Test-Path -LiteralPath (Join-Path $Destination "python.exe")) -or
            -not (Test-Path -LiteralPath (Join-Path $Destination "Lib"))) {
            throw "robocopy failed copying $Source to $Destination (exit code $LASTEXITCODE)"
        }
        Write-Warning "robocopy returned exit code $LASTEXITCODE for $Source, but the destination looks valid."
    }
}

if (-not (Test-Path -LiteralPath $PackageRoot)) {
    throw "Package root not found: $PackageRoot"
}

Reset-Dir $OutputDir
Remove-Item -LiteralPath (Join-Path $OutputDir "_tmp") -Recurse -Force -ErrorAction SilentlyContinue

$tmpRoot = Join-Path $OutputDir "_tmp"
Reset-Dir $tmpRoot

$coreDir = Join-Path $tmpRoot "cgplay-core-win64"
$ffmpegDir = Join-Path $tmpRoot "cgplay-ffmpeg-win64"
$pythonDir = Join-Path $tmpRoot "cgplay-python-runtime-win64"
$toolsDir = Join-Path $tmpRoot "cgplay-tools-win64"

New-Item -ItemType Directory -Path $coreDir,$ffmpegDir,$pythonDir,$toolsDir -Force | Out-Null

Get-ChildItem -LiteralPath $PackageRoot -File | ForEach-Object {
    if ($_.Name -in @("ffmpeg.exe","ffprobe.exe")) {
        Copy-Item -LiteralPath $_.FullName -Destination $ffmpegDir -Force
    } elseif ($_.Extension -in @(".exe",".dll",".qm")) {
        Copy-Item -LiteralPath $_.FullName -Destination $coreDir -Force
    }
}

foreach ($folder in @("platforms","styles","imageformats","iconengines","networkinformation","tls","generic","resources","translations","plugins")) {
    $src = Join-Path $PackageRoot $folder
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination $coreDir -Recurse -Force
    }
}

$runtimePython = Join-Path $PackageRoot "runtime\python"
if (Test-Path -LiteralPath $runtimePython) {
    Copy-DirContents $runtimePython $pythonDir
}

$toolsSource = Join-Path $PackageRoot "tools"
if (Test-Path -LiteralPath $toolsSource) {
    Copy-Item -LiteralPath $toolsSource -Destination $toolsDir -Recurse -Force
}

$uiDesignSrc = "C:\Users\1\Desktop\UI.png"
$uiDesignDst = Join-Path $OutputDir "UI.png"
if (Test-Path -LiteralPath $uiDesignSrc) {
    Copy-Item -LiteralPath $uiDesignSrc -Destination $uiDesignDst -Force
}

$coreZip = Join-Path $OutputDir "cgplay-core-win64.zip"
$ffmpegZip = Join-Path $OutputDir "cgplay-ffmpeg-win64.zip"
$pythonZip = Join-Path $OutputDir "cgplay-python-runtime-win64.zip"
$toolsZip = Join-Path $OutputDir "cgplay-tools-win64.zip"

New-ZipFromFolder $coreDir $coreZip
New-ZipFromFolder $ffmpegDir $ffmpegZip
New-ZipFromFolder $pythonDir $pythonZip
New-ZipFromFolder $toolsDir $toolsZip

$manifest = [ordered]@{
    manifestVersion = 1
    appVersion = $Version
    manifestUrl = First-Url "manifest.json"
    manifestUrls = New-UrlList "manifest.json"
    components = [ordered]@{
        core = [ordered]@{
            version = $Version
            url = First-Url "cgplay-core-win64.zip"
            urls = New-UrlList "cgplay-core-win64.zip"
            sha256 = (Write-Sha256 $coreZip)
            archiveName = "cgplay-core-win64.zip"
            installSubdir = "."
            requiredFiles = @("CGPlay.exe","CGPlayQuickLook.exe")
        }
        ffmpeg = [ordered]@{
            version = "8.1.1"
            url = First-Url "cgplay-ffmpeg-win64.zip"
            urls = New-UrlList "cgplay-ffmpeg-win64.zip"
            sha256 = (Write-Sha256 $ffmpegZip)
            archiveName = "cgplay-ffmpeg-win64.zip"
            installSubdir = "."
            requiredFiles = @("ffmpeg.exe","ffprobe.exe")
        }
        "python-runtime" = [ordered]@{
            version = "3.11.9-cgplay1"
            url = First-Url "cgplay-python-runtime-win64.zip"
            urls = New-UrlList "cgplay-python-runtime-win64.zip"
            sha256 = (Write-Sha256 $pythonZip)
            archiveName = "cgplay-python-runtime-win64.zip"
            installSubdir = "runtime/python"
            requiredFiles = @("python.exe")
        }
        tools = [ordered]@{
            version = $Version
            url = First-Url "cgplay-tools-win64.zip"
            urls = New-UrlList "cgplay-tools-win64.zip"
            sha256 = (Write-Sha256 $toolsZip)
            archiveName = "cgplay-tools-win64.zip"
            installSubdir = "tools"
            requiredFiles = @("cgplay/export/export_engine.py")
        }
    }
}

$manifestPath = Join-Path $OutputDir "manifest.json"
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
(Write-Sha256 $manifestPath) | Set-Content -LiteralPath (Join-Path $OutputDir "manifest.sha256") -Encoding ASCII

Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
Get-ChildItem -LiteralPath $OutputDir | Select-Object Name, Length, LastWriteTime
