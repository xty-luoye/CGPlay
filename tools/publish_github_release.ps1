[CmdletBinding()]
param(
    [string]$ProjectRoot = "C:\Users\1\Desktop\RVLite",
    [string]$ReleaseDir = "C:\Users\1\Desktop\RVLite\build_win_full\releases",
    [string]$InstallerRoot = "C:\Users\1\Desktop\RVLite\build_win_full\installer",
    [string]$Version = "1.0.7.02",
    [string]$Tag = "v1.0.7.02",
    [string]$Repo = "xty-luoye/CGPlay",
    [switch]$Draft,
    [switch]$CreateIfMissing
)

$ErrorActionPreference = "Stop"

function Assert-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing required file: $Path"
    }
}

function Get-GhPath {
    $cmd = Get-Command gh -ErrorAction SilentlyContinue
    if ($null -eq $cmd) {
        $fallback = "C:\Program Files\GitHub CLI\gh.exe"
        if (Test-Path -LiteralPath $fallback) {
            return $fallback
        }
        throw "GitHub CLI gh not found. Install gh and authenticate with 'gh auth login'."
    }
    return $cmd.Source
}

function Get-VersionedInstallerPath([string]$Mode) {
    $fileName = "CGPlay_Setup_{0}_{1}.exe" -f $Version, $Mode
    $preferred = Join-Path (Join-Path $InstallerRoot $Mode) $fileName
    if (Test-Path -LiteralPath $preferred) {
        return $preferred
    }
    return Join-Path $InstallerRoot $fileName
}

Assert-File (Join-Path $ReleaseDir "manifest.json")
Assert-File (Join-Path $ReleaseDir "manifest.sha256")
$liteInstaller = Get-VersionedInstallerPath "lite"
$fullInstaller = Get-VersionedInstallerPath "full"
Assert-File $liteInstaller
Assert-File $fullInstaller

# Older CGPlay builds try the "_full.exe" update URL before "_lite.exe".
# Keep that URL small and reliable by serving the lite installer there, while
# still publishing the real offline full installer under an explicit name.
$compatFullInstaller = Join-Path $InstallerRoot ("CGPlay_Setup_{0}_full.exe" -f $Version)
$offlineFullInstaller = Join-Path $InstallerRoot ("CGPlay_Setup_{0}_offline_full.exe" -f $Version)
Copy-Item -LiteralPath $liteInstaller -Destination $compatFullInstaller -Force
Copy-Item -LiteralPath $fullInstaller -Destination $offlineFullInstaller -Force

$gh = Get-GhPath
$assets = @(
    $liteInstaller,
    $compatFullInstaller,
    $offlineFullInstaller,
    (Join-Path $ReleaseDir "cgplay-core-win64.zip"),
    (Join-Path $ReleaseDir "cgplay-ffmpeg-win64.zip"),
    (Join-Path $ReleaseDir "cgplay-python-runtime-win64.zip"),
    (Join-Path $ReleaseDir "cgplay-tools-win64.zip"),
    (Join-Path $ReleaseDir "manifest.json"),
    (Join-Path $ReleaseDir "manifest.sha256")
)

& $gh auth status | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "GitHub CLI is not authenticated. Run: & `"$gh`" auth login"
}

& $gh release view $Tag --repo $Repo | Out-Null
if ($LASTEXITCODE -ne 0) {
    if (-not $CreateIfMissing) {
        throw "Release $Tag does not exist in $Repo. Re-run with -CreateIfMissing to create it first."
    }

    $createArgs = @(
        "release", "create", $Tag,
        "--repo", $Repo,
        "--title", ("CGPlay " + $Version),
        "--notes", ("CGPlay " + $Version + " release assets")
    )
    if ($Draft) {
        $createArgs += "--draft"
    }

    & $gh @createArgs
    if ($LASTEXITCODE -ne 0) {
        throw "gh release create failed with exit code $LASTEXITCODE"
    }
}

$uploadArgs = @(
    "release", "upload", $Tag
) + $assets + @("--repo", $Repo, "--clobber")

if ($Draft) {
    $uploadArgs += "--draft"
}

& $gh @uploadArgs
if ($LASTEXITCODE -ne 0) {
    throw "gh release upload failed with exit code $LASTEXITCODE"
}
