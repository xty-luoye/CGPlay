param(
    [string]$ProjectRoot = "C:\Users\1\Desktop\RVLite",
    [string]$OutputDir = "C:\Users\1\Desktop\RVLite\build_win_full\release_upload"
)

$ErrorActionPreference = "Stop"

function Reset-Dir([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path | Out-Null
}

function Copy-FileSafe([string]$Source, [string]$DestinationRoot) {
    if (-not (Test-Path -LiteralPath $Source)) {
        return
    }
    if ($Source -match '\\tests\\reports\\') { return }
    if ($Source -match '\\tests\\media\\') { return }
    if ($Source -match '\\backups\\') { return }
    if ($Source -match '\\build_win_full\\release_upload\\') { return }
    if ($Source -match '\\build_win_full\\releases\\_tmp\\') { return }
    if ($Source -match '\\_backup_src_compare\\') { return }
    if ($Source -match '\\ui_v1_backup\\') { return }
    if ($Source -match '\\resources\\demo_') { return }
    if ($Source -match '\.(pdb|pyc|log|tmp)$') { return }
    if ($Source -match '\.(png|jpg|jpeg|bmp|webp)$' -and
        $Source -notmatch '\\resources\\CGPlay\.png$' -and
        $Source -notmatch '\\resources\\UI\.png$' -and
        $Source -notmatch '\\resources\\ui\.png$' -and
        $Source -notmatch '\\resources\\icons\\') { return }

    $resolvedSource = (Resolve-Path -LiteralPath $Source).Path
    $projectRootResolved = (Resolve-Path -LiteralPath $ProjectRoot).Path
    $relative = $resolvedSource.Substring($projectRootResolved.Length).TrimStart('\','/')
    $destination = Join-Path $DestinationRoot $relative
    $destDir = Split-Path -Parent $destination
    New-Item -ItemType Directory -Path $destDir -Force | Out-Null
    Copy-Item -LiteralPath $resolvedSource -Destination $destination -Force
}

Reset-Dir $OutputDir

$allowList = @(
    "README.md",
    "LICENSE.txt",
    "CMakeLists.txt",
    "src",
    "cmake",
    "installer",
    "tools",
    "resources\\CGPlay.ico",
    "resources\\CGPlay.png",
    "resources\\CGPlay.rc",
    "resources\\resources.qrc",
    "resources\\icons",
    "resources\\component_manifest.json",
    "build_win_full\\installer\\full",
    "build_win_full\\installer\\lite",
    "build_win_full\\releases"
)

foreach ($entry in $allowList) {
    $source = Join-Path $ProjectRoot $entry
    if (-not (Test-Path -LiteralPath $source)) {
        continue
    }

    if ((Get-Item -LiteralPath $source).PSIsContainer) {
        $resolved = (Resolve-Path -LiteralPath $source).Path
        Get-ChildItem -LiteralPath $resolved -Recurse -File | ForEach-Object {
            Copy-FileSafe $_.FullName $OutputDir
        }
    } else {
        Copy-FileSafe $source $OutputDir
    }
}

Get-ChildItem -LiteralPath $OutputDir -Recurse -File |
    Select-Object FullName, Length |
    Format-Table -AutoSize
