[CmdletBinding()]
param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ReleaseDir = "",
    [string]$InstallerRoot = "",
    [string]$Version = "",
    [string]$Tag = "",
    [string]$Repo = "xty-luoye/CGPlay",
    [string]$NotesFile = "",
    [switch]$Draft,
    [switch]$CreateIfMissing,
    [switch]$DryRun
)
$ErrorActionPreference = "Stop"
if (-not $ReleaseDir) { $ReleaseDir = Join-Path $ProjectRoot "build_win_full\releases" }
if (-not $InstallerRoot) { $InstallerRoot = Join-Path $ProjectRoot "build_win_full\installer" }
if (-not $Version) {
    $match = [regex]::Match([IO.File]::ReadAllText((Join-Path $ProjectRoot "CMakeLists.txt")), 'project\s*\(\s*CGPlay\s+VERSION\s+([0-9.]+)')
    if (-not $match.Success) { throw "Pass -Version or provide the project CMake version." }
    $Version = $match.Groups[1].Value
}
if (-not $Tag) { $Tag = "v$Version" }
if (-not $NotesFile) { $NotesFile = Join-Path $ProjectRoot "docs\RELEASE_NOTES_$Version.md" }
function Assert-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -le 0) {
        throw "Missing or empty release asset: $Path"
    }
    if ((Get-Item -LiteralPath $Path).Length -ge 2GB) {
        throw "Release asset exceeds GitHub's 2GiB per-file limit; split it explicitly: $Path"
    }
}
function Find-Installer([string]$Mode) {
    $name = "CGPlay_Setup_{0}_{1}.exe" -f $Version, $Mode
    $nested = Join-Path (Join-Path $InstallerRoot $Mode) $name
    if (Test-Path -LiteralPath $nested -PathType Leaf) { return $nested }
    return Join-Path $InstallerRoot $name
}
$fullInstaller = Find-Installer "full"
Assert-File $fullInstaller
Assert-File $NotesFile
$assets = @($fullInstaller)
$liteInstaller = Find-Installer "lite"
if (Test-Path -LiteralPath $liteInstaller -PathType Leaf) { Assert-File $liteInstaller; $assets += $liteInstaller }
# The full download always contains the actual full package.
# Never copy a lite installer over *_full.exe.
foreach ($name in @(
    "cgplay-core-win64.zip", "cgplay-ffmpeg-win64.zip", "cgplay-python-runtime-win64.zip",
    "cgplay-tools-win64.zip", "cgplay-recording-runtime-win64.zip", "manifest.json", "manifest.sha256",
    "cgplay-corresponding-sources-$Version.zip", "cgplay-corresponding-sources-$Version.zip.sha256"
)) { $path = Join-Path $ReleaseDir $name; Assert-File $path; $assets += $path }
$sourceIndex = Join-Path $ReleaseDir "cgplay-corresponding-sources-$Version.assets.json"
if (Test-Path -LiteralPath $sourceIndex -PathType Leaf) {
    Assert-File $sourceIndex
    $index = Get-Content -LiteralPath $sourceIndex -Raw | ConvertFrom-Json
    if ($index.version -ne $Version) { throw "Source asset index has the wrong version." }
    $assets += $sourceIndex
    foreach ($item in $index.assets) {
        if ([IO.Path]::GetFileName($item.name) -ne $item.name -or $item.name -notlike '*.zip') {
            throw "Unsafe source asset index entry: $($item.name)"
        }
        $part = Join-Path $ReleaseDir $item.name
        $sidecar = $part + '.sha256'
        Assert-File $part; Assert-File $sidecar
        if ($assets -notcontains $part) { $assets += $part; $assets += $sidecar }
    }
}
$manifest = Get-Content -LiteralPath (Join-Path $ReleaseDir "manifest.json") -Raw | ConvertFrom-Json
if ($manifest.appVersion -ne $Version) { throw "Release version and manifest appVersion differ." }
if (-not $manifest.components.'recording-runtime') { throw "Recording runtime is missing from the release manifest." }
$plan = [ordered]@{ repo = $Repo; tag = $Tag; version = $Version; draft = [bool]$Draft; notesFile = $NotesFile; assets = $assets; fullInstaller = $fullInstaller; dryRun = [bool]$DryRun }
if ($DryRun) { $plan | ConvertTo-Json -Depth 5; return }
$manifestPath = Join-Path $ReleaseDir "manifest.json"
$declaredManifestHash = ((Get-Content -LiteralPath (Join-Path $ReleaseDir "manifest.sha256") -Raw).Trim() -split '\s+')[0]
$actualManifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
if ($declaredManifestHash -notmatch '^[0-9a-fA-F]{64}$' -or $declaredManifestHash -ne $actualManifestHash) {
    throw "manifest.sha256 does not match the actual release manifest."
}
& (Join-Path $PSScriptRoot "validate_component_manifest.ps1") -ManifestPath $manifestPath -PayloadRoot $ReleaseDir -RequireHashes -RequirePayloads | Out-Host
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($id in @("core", "ffmpeg", "python-runtime", "tools", "recording-runtime")) {
    $component = $manifest.components.$id
    $expectedArchive = "cgplay-$id-win64.zip"
    if (-not $component -or $component.archiveName -ne $expectedArchive) { throw "Missing or mismatched release component: $id" }
    $zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $ReleaseDir $expectedArchive))
    try {
        foreach ($required in $component.requiredFiles) {
            $entry = $zip.GetEntry(([string]$required).Replace('\','/'))
            if (-not $entry -or $entry.Length -le 0) { throw "Component $id ZIP lacks nonempty required file: $required" }
        }
    } finally { $zip.Dispose() }
}
& python (Join-Path $PSScriptRoot "validate_release_sources.py") --provenance (Join-Path $ProjectRoot "docs\DEPENDENCY_PROVENANCE_$Version.json") --bundle (Join-Path $ReleaseDir "cgplay-corresponding-sources-$Version.zip")
if ($LASTEXITCODE -ne 0) { throw "Corresponding-source bundle validation failed before any GitHub changes." }
$gh = (Get-Command gh -ErrorAction Stop).Source
& $gh auth status | Out-Null
if ($LASTEXITCODE -ne 0) { throw "GitHub CLI is not authenticated." }
& $gh release view $Tag --repo $Repo | Out-Null
if ($LASTEXITCODE -ne 0) {
    if (-not $CreateIfMissing) { throw "Release $Tag does not exist in $Repo." }
    $arguments = @("release", "create", $Tag, "--repo", $Repo, "--title", "CGPlay $Version", "--notes-file", $NotesFile)
    if ($Draft) { $arguments += "--draft" }
    & $gh @arguments
    if ($LASTEXITCODE -ne 0) { throw "Release creation failed." }
} else {
    & $gh release edit $Tag --repo $Repo --notes-file $NotesFile
    if ($LASTEXITCODE -ne 0) { throw "Release notes update failed." }
}
$arguments = @("release", "upload", $Tag) + $assets + @("--repo", $Repo, "--clobber")
& $gh @arguments
if ($LASTEXITCODE -ne 0) { throw "Release upload failed." }
