[CmdletBinding()]
param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$PackageRoot = "",
    [string]$OutputDir = "",
    [string]$GithubBase = "",
    [string]$MirrorBase = "",
    [string]$Version = ""
)
$ErrorActionPreference = "Stop"
function Assert-NoReparseAncestors([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing recursive component cleanup through a reparse point: $cursor"
            }
        }
        $cursor = Split-Path -Parent $cursor
    }
}
if (-not $PackageRoot) { $PackageRoot = Join-Path $ProjectRoot "build_win_full\package\full\CGPlay" }
if (-not $OutputDir) { $OutputDir = Join-Path $ProjectRoot "build_win_full\releases" }
if (-not $Version) {
    $found = [regex]::Match([IO.File]::ReadAllText((Join-Path $ProjectRoot "CMakeLists.txt")), 'project\s*\(\s*CGPlay\s+VERSION\s+([0-9.]+)')
    if (-not $found.Success) { throw "Cannot find project version." }
    $Version = $found.Groups[1].Value
}
if (-not $GithubBase) { $GithubBase = "https://github.com/xty-luoye/CGPlay/releases/download/v$Version" }
if (-not (Test-Path -LiteralPath $PackageRoot -PathType Container)) { throw "Package root not found: $PackageRoot" }
function Copy-Contents([string]$From, [string]$To) {
    if (-not (Test-Path -LiteralPath $From -PathType Container)) { throw "Missing component directory: $From" }
    New-Item -ItemType Directory -Path $To -Force | Out-Null
    Get-ChildItem -LiteralPath $From -Force | Copy-Item -Destination $To -Recurse -Force
}
function Urls([string]$Name) {
    $result = @($GithubBase.TrimEnd("/") + "/" + $Name)
    if ($MirrorBase) { $result += ($MirrorBase.TrimEnd("/") + "/" + $Name) }
    return @($result)
}
function Hash([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
$licenseRoot = Join-Path $PackageRoot "licenses"
foreach ($required in @("LICENSE.txt", "licenses\THIRD_PARTY_NOTICES.txt",
    "licenses\FFMPEG_SOURCE_OFFER.txt", "licenses\DEPENDENCY_SOURCES_$Version.md",
    "licenses\DEPENDENCY_PROVENANCE_$Version.json")) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageRoot $required) -PathType Leaf)) { throw "Missing release material: $required" }
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
Assert-NoReparseAncestors $OutputDir
# Keep source bundles and installer assets intact. Only a private fresh work
# directory and the five named component ZIPs belong to this invocation.
$temporary = Join-Path $OutputDir ("_components_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $temporary | Out-Null
$definitions = [ordered]@{
    core = @{ file = "cgplay-core-win64.zip"; install = "."; version = $Version; required = @("CGPlay.exe", "CGPlayQuickLook.exe") }
    ffmpeg = @{ file = "cgplay-ffmpeg-win64.zip"; install = "."; version = $Version; required = @("ffmpeg.exe", "ffprobe.exe") }
    "python-runtime" = @{ file = "cgplay-python-runtime-win64.zip"; install = "runtime/python"; version = $Version; required = @("python.exe") }
    tools = @{ file = "cgplay-tools-win64.zip"; install = "tools"; version = $Version; required = @("cgplay/export/export_engine.py", "cgplay/export/compare_engine.py", "cgplay/export/recording_finalize.py") }
    "recording-runtime" = @{ file = "cgplay-recording-runtime-win64.zip"; install = "runtime/recorder"; version = "33.0.0-cgplay$Version"; required = @("CGPlayRecorder.exe", "runtime-manifest.json") }
}
$manifest = [ordered]@{
    manifestVersion = 1; appVersion = $Version
    manifestUrl = "https://github.com/xty-luoye/CGPlay/releases/latest/download/manifest.json"
    manifestUrls = @("https://github.com/xty-luoye/CGPlay/releases/latest/download/manifest.json")
    components = [ordered]@{}
}
try {
    foreach ($id in $definitions.Keys) {
        $definition = $definitions[$id]
        $destination = Join-Path $temporary $id
        New-Item -ItemType Directory -Path $destination | Out-Null
        switch ($id) {
            "core" {
                Get-ChildItem -LiteralPath $PackageRoot -File | Where-Object {
                    $_.Extension -in @(".exe", ".dll", ".qm") -and $_.Name -notin @("ffmpeg.exe", "ffprobe.exe")
                } | Copy-Item -Destination $destination -Force
                foreach ($name in @("platforms","styles","imageformats","iconengines","networkinformation","tls","generic","resources","translations","plugins","presets","docs","licenses")) {
                    $source = Join-Path $PackageRoot $name
                    if (Test-Path -LiteralPath $source -PathType Container) { Copy-Item -LiteralPath $source -Destination $destination -Recurse -Force }
                }
                Copy-Item -LiteralPath (Join-Path $PackageRoot "LICENSE.txt") -Destination $destination
                $codex = Join-Path $PackageRoot "runtime\codex"
                if (Test-Path -LiteralPath $codex -PathType Container) {
                    Copy-Contents $codex (Join-Path $destination "runtime\codex")
                }
            }
            "ffmpeg" {
                foreach ($name in @("ffmpeg.exe","ffprobe.exe")) { Copy-Item -LiteralPath (Join-Path $PackageRoot $name) -Destination $destination }
            }
            "python-runtime" { Copy-Contents (Join-Path $PackageRoot "runtime\python") $destination }
            "tools" { Copy-Contents (Join-Path $PackageRoot "tools") $destination }
            "recording-runtime" { Copy-Contents (Join-Path $PackageRoot "runtime\recorder") $destination }
        }
        if ($id -ne "core") {
            # Each downloadable component retains its own redistributable
            # notice/source information even when extracted independently.
            Copy-Contents $licenseRoot (Join-Path $destination "distribution-licenses")
            Copy-Item -LiteralPath (Join-Path $PackageRoot "LICENSE.txt") -Destination (Join-Path $destination "distribution-licenses\CGPlay-LICENSE.txt")
        }
        foreach ($name in $definition.required) {
            if (-not (Test-Path -LiteralPath (Join-Path $destination $name) -PathType Leaf)) { throw "Missing $id component file: $name" }
        }
        $archive = Join-Path $OutputDir $definition.file
        $partial = Join-Path $OutputDir ($definition.file + ".partial.zip")
        Compress-Archive -Path (Join-Path $destination "*") -DestinationPath $partial -CompressionLevel Optimal -Force
        Move-Item -LiteralPath $partial -Destination $archive -Force
        $locations = @(Urls $definition.file)
        $component = [ordered]@{
            version = $definition.version; url = $locations[0]; urls = $locations
            sha256 = Hash $archive; archiveName = $definition.file
            installSubdir = $definition.install; requiredFiles = $definition.required
        }
        if ($id -eq "recording-runtime") { $component.optional = $true }
        $manifest.components[$id] = $component
    }
    $manifestPath = Join-Path $OutputDir "manifest.json"
    $manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    Hash $manifestPath | Set-Content -LiteralPath (Join-Path $OutputDir "manifest.sha256") -Encoding ASCII
    [ordered]@{ passed = $true; version = $Version; components = @($definitions.Keys); manifest = $manifestPath; sourceAssetsPreserved = $true } | ConvertTo-Json -Depth 4
} finally {
    # The exact GUID directory was created above. Refuse reparse points and
    # verify its absolute parent before removing that one owned work directory.
    $expectedParent = [IO.Path]::GetFullPath($OutputDir).TrimEnd('\','/')
    $candidate = Get-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    if ($candidate -and $candidate.Parent.FullName.TrimEnd('\','/') -eq $expectedParent -and
        -not ($candidate.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        Assert-NoReparseAncestors $candidate.FullName
        Remove-Item -LiteralPath $candidate.FullName -Recurse -Force
    }
}
