[CmdletBinding()]
param(
    [string]$ProjectRoot = "",
    [string]$ConfigurePreset = "windows-full-release",
    [string]$BuildPreset = "windows-full-release-apps",
    [string]$BuildDir = "",
    [switch]$SkipBuildCache,
    [switch]$RequireBuiltApps,
    [string]$ReportPath = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) { $ProjectRoot = Split-Path -Parent $PSScriptRoot }
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Join-Path $ProjectRoot "build_win_full" }
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
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
    Add-Check "binaryDir" ($presetDir -ieq $BuildDir) "preset=$presetDir expected=$BuildDir"
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
    Add-Check "cacheTlRender" ([string]$cache.CGPLAY_USE_TLRENDER -ieq "ON") ([string]$cache.CGPLAY_USE_TLRENDER)
    Add-Check "cacheGenerator" ([string]$cache.CMAKE_GENERATOR -eq "Visual Studio 17 2022") ([string]$cache.CMAKE_GENERATOR)
    Add-Check "cachePlatform" ([string]$cache.CMAKE_GENERATOR_PLATFORM -ieq "x64") ([string]$cache.CMAKE_GENERATOR_PLATFORM)
    Add-Check "cacheQtNotStubTree" ([string]$cache.Qt6_DIR -notmatch '(?i)build_qtclean') ([string]$cache.Qt6_DIR)
}

$releaseDir = Join-Path $BuildDir "bin\Release"
foreach ($name in @("CGPlay.exe", "CGPlayQuickLook.exe")) {
    $path = Join-Path $releaseDir $name
    Add-Check "built.$name" (-not $RequireBuiltApps -or (Test-Path -LiteralPath $path -PathType Leaf)) $path
}

$failed = @($checks | Where-Object { -not $_.passed })
$report = [PSCustomObject]@{
    schemaVersion = 1; checkedAtUtc = [DateTime]::UtcNow.ToString("o")
    projectRoot = $ProjectRoot; buildDir = $BuildDir
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
