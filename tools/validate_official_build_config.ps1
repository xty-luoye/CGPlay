[CmdletBinding()]
param(
    [string]$ProjectRoot = "",
    [string]$ConfigurePreset = "windows-full-release",
    [string]$BuildPreset = "windows-full-release-apps",
    [string]$BuildDir = "",
    [string]$ReleaseDir = "",
    [switch]$SkipBuildCache,
    [switch]$RequireBuiltApps,
    [string]$ReportPath = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) { $ProjectRoot = Split-Path -Parent $PSScriptRoot }
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
function Resolve-PhysicalDirectory([string]$Path, [int]$Depth = 0) {
    if ($Depth -gt 64) { throw "Directory link loop: $Path" }
    $full = [IO.Path]::GetFullPath($Path)
    if ($full.Length -gt ([IO.Path]::GetPathRoot($full)).Length) { $full = $full.TrimEnd('\','/') }
    if (Test-Path -LiteralPath $full) {
        $item = Get-Item -LiteralPath $full -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            $target = @($item.Target) | Select-Object -First 1
            if (-not $target) { throw "Cannot resolve directory link: $full" }
            if (-not [IO.Path]::IsPathRooted($target)) { $target = Join-Path (Split-Path -Parent $full) $target }
            return Resolve-PhysicalDirectory $target ($Depth + 1)
        }
    }
    $parent = Split-Path -Parent $full
    if (-not $parent) { return $full }
    return Join-Path (Resolve-PhysicalDirectory $parent ($Depth + 1)) (Split-Path -Leaf $full)
}
$ProjectRoot = Resolve-PhysicalDirectory $ProjectRoot
if ($ReleaseDir -and -not $BuildDir) { $BuildDir = Split-Path -Parent (Split-Path -Parent $ReleaseDir.TrimEnd('\','/')) }
if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Join-Path $ProjectRoot "build_win_full" }
$BuildDir = Resolve-PhysicalDirectory $BuildDir
if (-not $ReleaseDir) { $ReleaseDir = Join-Path $BuildDir "bin\Release" }
$ReleaseDir = [IO.Path]::GetFullPath($ReleaseDir)
$checks = [System.Collections.Generic.List[object]]::new()

function Add-Check([string]$Name, [bool]$Passed, [string]$Details) {
    $checks.Add([PSCustomObject]@{ name = $Name; passed = $Passed; details = $Details })
}

function Read-Cache([string]$Path) {
    $values = @{}
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        foreach ($line in Get-Content -LiteralPath $Path) {
            if ($line -match '^([^#/][^:=]*)(?::[^=]+)?=(.*)$') { $values[$matches[1]] = $matches[2] }
        }
    }
    return $values
}

$presetsPath = Join-Path $ProjectRoot "CMakePresets.json"
Add-Check "presetsFile" (Test-Path -LiteralPath $presetsPath -PathType Leaf) $presetsPath
if (-not (Test-Path -LiteralPath $presetsPath -PathType Leaf)) { throw "CMakePresets.json not found: $presetsPath" }
$presets = Get-Content -LiteralPath $presetsPath -Raw | ConvertFrom-Json
$configure = @($presets.configurePresets | Where-Object name -eq $ConfigurePreset) | Select-Object -First 1
$build = @($presets.buildPresets | Where-Object name -eq $BuildPreset) | Select-Object -First 1
Add-Check "configurePreset" ($null -ne $configure) $ConfigurePreset
Add-Check "buildPreset" ($null -ne $build) $BuildPreset

if ($configure) {
    $architecture = if ($configure.architecture -is [string]) { [string]$configure.architecture } else { [string]$configure.architecture.value }
    $presetDir = ([string]$configure.binaryDir).Replace('${sourceDir}', $ProjectRoot).Replace('/', '\')
    $presetDir = [System.IO.Path]::GetFullPath($presetDir)
    Add-Check "generator" ([string]$configure.generator -eq "Visual Studio 17 2022") ([string]$configure.generator)
    Add-Check "architecture" ($architecture -ieq "x64") $architecture
    # The preset's default layout is policy; an explicitly selected build must
    # prove its own cache identity instead of borrowing the default cache.
    $defaultBuildDir = Resolve-PhysicalDirectory (Join-Path $ProjectRoot "build_win_full")
    Add-Check "binaryDir" ((Resolve-PhysicalDirectory $presetDir) -ieq $defaultBuildDir) "presetDefault=$presetDir expectedDefault=$defaultBuildDir selected=$BuildDir"
    Add-Check "presetTlRender" ([string]$configure.cacheVariables.CGPLAY_USE_TLRENDER -ieq "ON") ([string]$configure.cacheVariables.CGPLAY_USE_TLRENDER)
    Add-Check "presetBuildType" ([string]$configure.cacheVariables.CMAKE_BUILD_TYPE -ieq "Release") ([string]$configure.cacheVariables.CMAKE_BUILD_TYPE)
    Add-Check "presetQt" (-not [string]::IsNullOrWhiteSpace([string]$configure.cacheVariables.Qt6_DIR)) ([string]$configure.cacheVariables.Qt6_DIR)
}

if ($build) {
    $targets = @($build.targets | ForEach-Object { [string]$_ })
    Add-Check "buildUsesConfigurePreset" ([string]$build.configurePreset -eq $ConfigurePreset) ([string]$build.configurePreset)
    Add-Check "releaseConfiguration" ([string]$build.configuration -ieq "Release") ([string]$build.configuration)
    Add-Check "targetCGPlay" ($targets -contains "CGPlay") ($targets -join ",")
    Add-Check "targetQuickLook" ($targets -contains "CGPlayQuickLook") ($targets -join ",")
}

$cachePath = Join-Path $BuildDir "CMakeCache.txt"
$cacheExists = Test-Path -LiteralPath $cachePath -PathType Leaf
Add-Check "buildCache" ($SkipBuildCache -or $cacheExists) $cachePath
$cache = Read-Cache $cachePath
if ($cacheExists) {
    $sourceMatches = $false
    $binaryMatches = $false
    $projectBinaryMatches = $false
    if ($cache.CMAKE_HOME_DIRECTORY) { $sourceMatches = (Resolve-PhysicalDirectory $cache.CMAKE_HOME_DIRECTORY) -ieq $ProjectRoot }
    if ($cache.CMAKE_CACHEFILE_DIR) { $binaryMatches = (Resolve-PhysicalDirectory $cache.CMAKE_CACHEFILE_DIR) -ieq $BuildDir }
    if ($cache.CGPlay_BINARY_DIR) { $projectBinaryMatches = (Resolve-PhysicalDirectory $cache.CGPlay_BINARY_DIR) -ieq $BuildDir }
    Add-Check "cacheSourceRoot" $sourceMatches "cacheSource=$($cache.CMAKE_HOME_DIRECTORY) selectedSource=$ProjectRoot"
    Add-Check "cacheBinaryDir" $binaryMatches "cacheBinary=$($cache.CMAKE_CACHEFILE_DIR) selectedBuild=$BuildDir"
    Add-Check "cacheProjectBinaryDir" $projectBinaryMatches "projectBinary=$($cache.CGPlay_BINARY_DIR) selectedBuild=$BuildDir"
    Add-Check "cacheProjectName" ([string]$cache.CMAKE_PROJECT_NAME -eq "CGPlay") ([string]$cache.CMAKE_PROJECT_NAME)
    Add-Check "cacheBuildType" ([string]$cache.CMAKE_BUILD_TYPE -ieq "Release") ([string]$cache.CMAKE_BUILD_TYPE)
    Add-Check "cacheTlRender" ([string]$cache.CGPLAY_USE_TLRENDER -ieq "ON") ([string]$cache.CGPLAY_USE_TLRENDER)
    Add-Check "cacheGenerator" ([string]$cache.CMAKE_GENERATOR -eq "Visual Studio 17 2022") ([string]$cache.CMAKE_GENERATOR)
    Add-Check "cachePlatform" ([string]$cache.CMAKE_GENERATOR_PLATFORM -ieq "x64") ([string]$cache.CMAKE_GENERATOR_PLATFORM)
    Add-Check "cacheQtNotStubTree" ([string]$cache.Qt6_DIR -notmatch '(?i)build_qtclean') ([string]$cache.Qt6_DIR)
    Add-Check "cacheQtConfig" (-not [string]::IsNullOrWhiteSpace([string]$cache.Qt6_DIR) -and (Test-Path -LiteralPath (Join-Path ([string]$cache.Qt6_DIR) "Qt6Config.cmake") -PathType Leaf)) ([string]$cache.Qt6_DIR)
}

$releaseLeaf = Split-Path -Leaf $ReleaseDir.TrimEnd('\','/')
$binLeaf = Split-Path -Leaf (Split-Path -Parent $ReleaseDir.TrimEnd('\','/'))
$selectedLayout = $releaseLeaf -ieq "Release" -and $binLeaf -ieq "bin" -and (Resolve-PhysicalDirectory $ReleaseDir) -ieq (Resolve-PhysicalDirectory (Join-Path $BuildDir "bin\Release"))
Add-Check "selectedReleaseLayout" $selectedLayout "selectedRelease=$ReleaseDir expected=$(Join-Path $BuildDir 'bin\Release')"
foreach ($name in @("CGPlay.exe", "CGPlayQuickLook.exe")) {
    $path = Join-Path $ReleaseDir $name
    Add-Check "built.$name" (-not $RequireBuiltApps -or ((Test-Path -LiteralPath $path -PathType Leaf) -and (Get-Item -LiteralPath $path).Length -gt 0)) $path
}

$failed = @($checks | Where-Object { -not $_.passed })
$report = [PSCustomObject]@{
    schemaVersion = 1; checkedAtUtc = [DateTime]::UtcNow.ToString("o")
    projectRoot = $ProjectRoot; buildDir = $BuildDir; releaseDir = $ReleaseDir
    configurePreset = $ConfigurePreset; buildPreset = $BuildPreset
    requireBuiltApps = [bool]$RequireBuiltApps; overallPass = $failed.Count -eq 0
    checkCount = $checks.Count; failedCount = $failed.Count; checks = $checks
}
if (-not [string]::IsNullOrWhiteSpace($ReportPath)) {
    $directory = Split-Path -Parent $ReportPath
    if ($directory) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
}
$report
if ($failed.Count -gt 0) { throw "Official build configuration validation failed: $(($failed.name) -join ', ')" }
