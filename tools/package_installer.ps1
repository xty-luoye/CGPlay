[CmdletBinding()]
param(
    [string]$ProjectRoot = "",
    [string]$ReleaseDir = "",
    [string]$PythonRuntime = "",
    [string]$CodexRuntime = "",
    [string]$FfmpegBinDir = "",
    [ValidateSet("full","lite")]
    [string]$PackageMode = "full",
    [ValidateSet("auto","inno","nsis")]
    [string]$Installer = "auto",
    [string]$Version = "",
    [switch]$NoVersionBump
)

$ErrorActionPreference = "Stop"

function Write-Step([string]$Message) {
    Write-Host "==> $Message"
}

function Reset-Dir([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path | Out-Null
}

function Ensure-Dir([string]$Path) {
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Copy-IfExists([string]$Source, [string]$DestinationDir) {
    if (Test-Path -LiteralPath $Source) {
        Ensure-Dir $DestinationDir
        Copy-Item -LiteralPath $Source -Destination $DestinationDir -Force
        return $true
    }
    return $false
}

function Copy-Dir([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Missing required directory: $Source"
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
}

function Copy-DirContents([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Missing required directory: $Source"
    }
    Ensure-Dir $Destination
    $sourcePath = (Resolve-Path -LiteralPath $Source).Path
    $destPath = (Resolve-Path -LiteralPath $Destination).Path
    & robocopy $sourcePath $destPath /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -gt 7) {
        throw "robocopy failed copying $Source to $Destination (exit code $LASTEXITCODE)"
    }
}

function Test-PythonRuntimeCompleteness([string]$Root) {
    if ([string]::IsNullOrWhiteSpace($Root) -or -not (Test-Path -LiteralPath $Root)) {
        return $false
    }

    $requiredPaths = @(
        "python.exe",
        "Lib\email\__init__.py",
        "Lib\http\__init__.py",
        "Lib\importlib\metadata\__init__.py",
        "Lib\unittest\__init__.py",
        "Lib\site-packages\certifi\__init__.py",
        "Lib\site-packages\charset_normalizer\__init__.py",
        "Lib\site-packages\cv2\__init__.py",
        "Lib\site-packages\ctranslate2\__init__.py",
        "Lib\site-packages\faster_whisper\__init__.py",
        "Lib\site-packages\huggingface_hub\__init__.py",
        "Lib\site-packages\idna\__init__.py",
        "Lib\site-packages\requests\__init__.py",
        "Lib\site-packages\tokenizers\__init__.py",
        "Lib\site-packages\tqdm\__init__.py",
        "Lib\site-packages\urllib3\__init__.py",
        "Lib\site-packages\typing_extensions.py"
    )

    foreach ($relativePath in $requiredPaths) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $relativePath))) {
            return $false
        }
    }
    return $true
}

function Test-CodexRuntimeCompleteness([string]$Root) {
    if ([string]::IsNullOrWhiteSpace($Root) -or -not (Test-Path -LiteralPath $Root -PathType Container)) {
        return $false
    }
    foreach ($name in @("codex.exe", "codex-code-mode-host.exe", "LICENSE-OPENAI-CODEX.txt")) {
        $path = Join-Path $Root $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -le 0) {
            return $false
        }
    }
    return $true
}

function Remove-PathIfExists([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Get-ProjectVersion([string]$Root, [string]$Fallback = "1.0.7.02") {
    $cmakePath = Join-Path $Root "CMakeLists.txt"
    if (-not (Test-Path -LiteralPath $cmakePath)) {
        return $Fallback
    }

    $match = Select-String -Path $cmakePath -Pattern 'project\s*\(\s*CGPlay\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?)' -AllMatches |
        Select-Object -First 1
    if ($match -and $match.Matches.Count -gt 0) {
        return $match.Matches[0].Groups[1].Value
    }

    return $Fallback
}

function Get-NextProjectVersion([string]$Version) {
    $rawParts = @($Version.Split('.'))
    $parts = @($rawParts | ForEach-Object { [int]$_ })
    while ($parts.Count -lt 4) { $parts += 0 }
    $parts[3] += 1
    $revisionWidth = if ($rawParts.Count -ge 4) { [Math]::Max(2, $rawParts[3].Length) } else { 2 }
    return "{0}.{1}.{2}.{3}" -f $parts[0], $parts[1], $parts[2], $parts[3].ToString("D$revisionWidth")
}

function Set-ProjectVersion([string]$Root, [string]$Version) {
    $fileParts = @($Version.Split('.') | ForEach-Object { [int]$_ })
    while ($fileParts.Count -lt 4) { $fileParts += 0 }
    $fileVersion = $fileParts -join '.'
    $numericVersion = $fileParts -join ','
    $utf8 = [System.Text.UTF8Encoding]::new($false)

    function Update-VersionText([string]$Path, [string]$Pattern, [string]$Replacement) {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
            throw "Version source file is missing: $Path"
        }
        $text = [System.IO.File]::ReadAllText($Path)
        $updated = [System.Text.RegularExpressions.Regex]::Replace($text, $Pattern, $Replacement)
        if ($updated -eq $text) {
            throw "Version pattern did not match: $Path"
        }
        [System.IO.File]::WriteAllText($Path, $updated, $utf8)
    }

    Update-VersionText (Join-Path $Root "CMakeLists.txt") `
        '(?m)(project\s*\(\s*CGPlay\s+VERSION\s+)[0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?' `
        ('${1}' + $Version)
    Update-VersionText (Join-Path $Root "installer\CGPlay_Installer.nsi") `
        '(?m)^([ \t]*!define\s+APP_VERSION\s+")[^"]+(")' `
        ('${1}' + $Version + '${2}')
    Update-VersionText (Join-Path $Root "installer\CGPlay_Installer.nsi") `
        '(?m)^([ \t]*!define\s+APP_FILE_VERSION\s+")[^"]+(")' `
        ('${1}' + $fileVersion + '${2}')
    Update-VersionText (Join-Path $Root "resources\CGPlay.rc") `
        '(?m)^(FILEVERSION\s+)[0-9]+,[0-9]+,[0-9]+,[0-9]+' `
        ('${1}' + $numericVersion)
    Update-VersionText (Join-Path $Root "resources\CGPlay.rc") `
        '(?m)^(PRODUCTVERSION\s+)[0-9]+,[0-9]+,[0-9]+,[0-9]+' `
        ('${1}' + $numericVersion)
    Update-VersionText (Join-Path $Root "resources\CGPlay.rc") `
        '(?m)(VALUE\s+"FileVersion",\s+")[0-9.]+(\\0")' `
        ('${1}' + $Version + '${2}')
    Update-VersionText (Join-Path $Root "resources\CGPlay.rc") `
        '(?m)(VALUE\s+"ProductVersion",\s+")[0-9.]+(\\0")' `
        ('${1}' + $Version + '${2}')
    Update-VersionText (Join-Path $Root "src\ui\app\ApplicationRuntime.cpp") `
        '(setApplicationVersion\(")[0-9.]+(")\)' `
        ('${1}' + $Version + '${2})')
    Update-VersionText (Join-Path $Root "src\ui\app\ApplicationUiSupport.cpp") `
        '(QStringLiteral\(")[0-9.]+(")\)' `
        ('${1}' + $Version + '${2})')
}

function Save-ProjectVersionFiles([string]$Root) {
    $snapshot = @{}
    foreach ($relativePath in @(
        "CMakeLists.txt",
        "installer\CGPlay_Installer.nsi",
        "resources\CGPlay.rc",
        "src\ui\app\ApplicationRuntime.cpp",
        "src\ui\app\ApplicationUiSupport.cpp"
    )) {
        $path = Join-Path $Root $relativePath
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Version source file is missing: $path"
        }
        $snapshot[$path] = [System.IO.File]::ReadAllBytes($path)
    }
    return $snapshot
}

function Restore-ProjectVersionFiles($Snapshot) {
    foreach ($entry in $Snapshot.GetEnumerator()) {
        [System.IO.File]::WriteAllBytes([string]$entry.Key, [byte[]]$entry.Value)
    }
}

function Assert-ReleaseBinaryVersion([string]$Directory, [string]$ExpectedVersion) {
    foreach ($name in @("CGPlay.exe", "CGPlayQuickLook.exe", "CGPlayThumbnailProvider.dll")) {
        $path = Join-Path $Directory $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Release binary is missing: $path"
        }
        $actualVersion = (Get-Item -LiteralPath $path).VersionInfo.FileVersion
        if ($actualVersion -ne $ExpectedVersion) {
            throw "Release binary version mismatch: $name is $actualVersion, expected $ExpectedVersion"
        }
    }
}

function Find-InnoCompiler {
    foreach ($path in @(
        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        "C:\Program Files\Inno Setup 6\ISCC.exe"
    )) {
        if (Test-Path -LiteralPath $path) {
            return $path
        }
    }
    return $null
}

function Find-NsisCompiler {
    foreach ($path in @(
        "C:\Program Files (x86)\NSIS\makensis.exe",
        "C:\Program Files\NSIS\makensis.exe"
    )) {
        if (Test-Path -LiteralPath $path) {
            return $path
        }
    }
    return $null
}

function Copy-ReleaseFiles([string]$SourceDir, [string]$DestinationDir) {
    $excludedFiles = @(
        ".ffmpeg_ok",
        "1annotated_output.mp4",
        "1annotated_output.mp4.annotations.json",
        "1annotated_output_export.log"
    )
    $allowedExtensions = @(".exe", ".dll", ".qm")

    Get-ChildItem -LiteralPath $SourceDir -File | ForEach-Object {
        if ($excludedFiles -contains $_.Name) {
            return
        }
        if ($allowedExtensions -contains $_.Extension.ToLowerInvariant()) {
            Copy-Item -LiteralPath $_.FullName -Destination $DestinationDir -Force
        }
    }
}

function Copy-ReleaseDirectories([string]$SourceDir, [string]$DestinationDir) {
    $allowedDirs = @(
        "platforms",
        "styles",
        "imageformats",
        "iconengines",
        "networkinformation",
        "tls",
        "generic",
        "resources",
        "translations",
        "plugins"
    )

    foreach ($name in $allowedDirs) {
        $source = Join-Path $SourceDir $name
        if (Test-Path -LiteralPath $source) {
            Copy-Dir $source $DestinationDir
        }
    }

    $disabledPluginsDir = Join-Path $DestinationDir "plugins\disabled"
    if (Test-Path -LiteralPath $disabledPluginsDir) {
        Remove-PathIfExists $disabledPluginsDir
    }
}

function Copy-DistributedResources([string]$Root, [string]$Destination) {
    $resourceRoot = Join-Path $Root "resources"
    Ensure-Dir $Destination
    foreach ($file in @("CGPlay.ico", "CGPlay.png", "component_manifest.json")) {
        Copy-IfExists (Join-Path $resourceRoot $file) $Destination | Out-Null
    }

    $iconsSource = Join-Path $resourceRoot "icons"
    if (Test-Path -LiteralPath $iconsSource) {
        Copy-Dir $iconsSource $Destination
    }
}

function Validate-StagedPackage(
    [string]$AppRoot,
    [string]$PackageMode
) {
    $requiredPaths = @(
        (Join-Path $AppRoot "CGPlay.exe"),
        (Join-Path $AppRoot "CGPlayQuickLook.exe"),
        (Join-Path $AppRoot "CGPlayThumbnailProvider.dll"),
        (Join-Path $AppRoot "Qt6WebEngineCore.dll"),
        (Join-Path $AppRoot "QtWebEngineProcess.exe"),
        (Join-Path $AppRoot "plugins"),
        (Join-Path $AppRoot "plugins\CGPlayCodexPlugin.dll"),
        (Join-Path $AppRoot "resources\icudtl.dat"),
        (Join-Path $AppRoot "resources\qtwebengine_resources.pak"),
        (Join-Path $AppRoot "tools\cgplay\export\export_engine.py"),
        (Join-Path $AppRoot "LICENSE.txt"),
        (Join-Path $AppRoot "licenses\THIRD_PARTY_NOTICES.txt"),
        (Join-Path $AppRoot "licenses\FFMPEG_SOURCE_OFFER.txt"),
        (Join-Path $AppRoot "licenses\third_party"),
        (Join-Path $AppRoot "resources"),
        (Join-Path $AppRoot "presets"),
        (Join-Path $AppRoot "translations")
    )

    if ($PackageMode -eq "full") {
        $requiredPaths += @(
            (Join-Path $AppRoot "runtime\python\python.exe"),
            (Join-Path $AppRoot "runtime\codex\codex.exe"),
            (Join-Path $AppRoot "runtime\codex\codex-code-mode-host.exe"),
            (Join-Path $AppRoot "runtime\codex\LICENSE-OPENAI-CODEX.txt"),
            (Join-Path $AppRoot "ffmpeg.exe"),
            (Join-Path $AppRoot "ffprobe.exe")
        )
    }

    foreach ($path in $requiredPaths) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Required staged path is missing: $path"
        }
    }
}

$versionRollbackSnapshot = $null
$packagingComplete = $false
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}
if ([string]::IsNullOrWhiteSpace($ReleaseDir)) {
    $ReleaseDir = Join-Path $ProjectRoot "build_win_full\bin\Release"
}
if ([string]::IsNullOrWhiteSpace($CodexRuntime)) {
    $CodexRuntime = Join-Path $ProjectRoot "build_win_full\runtime\codex"
}
if ([string]::IsNullOrWhiteSpace($PythonRuntime)) {
    $PythonRuntime = $env:CGPLAY_PYTHON_RUNTIME
}
if ([string]::IsNullOrWhiteSpace($FfmpegBinDir)) {
    $FfmpegBinDir = $env:CGPLAY_FFMPEG_BIN_DIR
}
if ($PackageMode -eq "full") {
    if (-not (Test-PythonRuntimeCompleteness (Join-Path $ReleaseDir "runtime\python")) -and
        -not (Test-PythonRuntimeCompleteness $PythonRuntime)) {
        throw "A complete Python runtime is required. Pass -PythonRuntime or set CGPLAY_PYTHON_RUNTIME."
    }
    foreach ($ffmpegName in @("ffmpeg.exe", "ffprobe.exe")) {
        if (-not (Test-Path -LiteralPath (Join-Path $ReleaseDir $ffmpegName) -PathType Leaf)) {
            if ([string]::IsNullOrWhiteSpace($FfmpegBinDir) -or
                -not (Test-Path -LiteralPath (Join-Path $FfmpegBinDir $ffmpegName) -PathType Leaf)) {
                throw "$ffmpegName is required. Pass -FfmpegBinDir or set CGPLAY_FFMPEG_BIN_DIR."
            }
        }
    }
}
trap {
    if ($null -ne $versionRollbackSnapshot -and -not $packagingComplete) {
        Write-Warning "Packaging failed; restoring version source files."
        Restore-ProjectVersionFiles -Snapshot $versionRollbackSnapshot
    }
    throw $_
}

$previousVersion = Get-ProjectVersion -Root $ProjectRoot
$versionBumped = $false
$appVersion = if (-not [string]::IsNullOrWhiteSpace($Version)) {
    $Version.Trim()
} elseif ($NoVersionBump) {
    $previousVersion
} else {
    $versionBumped = $true
    Get-NextProjectVersion $previousVersion
}
if ($appVersion -notmatch '^\d+\.\d+\.\d+(?:\.\d+)?$') {
    throw "Invalid application version: $appVersion"
}

if ($versionBumped) {
    Write-Step "Bumping application version $previousVersion -> $appVersion"
    $versionRollbackSnapshot = Save-ProjectVersionFiles -Root $ProjectRoot
    Set-ProjectVersion -Root $ProjectRoot -Version $appVersion
    foreach ($process in @(Get-Process -Name CGPlay -ErrorAction SilentlyContinue)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    $buildDir = Join-Path $ProjectRoot "build_win_full"
    Write-Step "Rebuilding Release binaries for $appVersion"
    & cmake -S $ProjectRoot -B $buildDir
    if ($LASTEXITCODE -ne 0) { throw "CMake reconfiguration failed with exit code $LASTEXITCODE" }
    & cmake --build $buildDir --config Release --target CGPlay CGPlayQuickLook CGPlayThumbnailProvider
    if ($LASTEXITCODE -ne 0) { throw "Release rebuild failed with exit code $LASTEXITCODE" }
}
$fileVersionParts = @($appVersion.Split('.') | ForEach-Object { [int]$_ })
while ($fileVersionParts.Count -lt 4) {
    $fileVersionParts += 0
}
$appFileVersion = ($fileVersionParts -join '.')
Assert-ReleaseBinaryVersion -Directory $ReleaseDir -ExpectedVersion $appVersion

$stagingRoot = Join-Path $ProjectRoot ("build_win_full\package\" + $PackageMode)
$appRoot = Join-Path $stagingRoot "CGPlay"
$runtimeRoot = Join-Path $appRoot "runtime"
$pythonTarget = Join-Path $runtimeRoot "python"
$codexTarget = Join-Path $runtimeRoot "codex"
$toolsTarget = Join-Path $appRoot "tools"
$licensesTarget = Join-Path $appRoot "licenses"
$resourcesTarget = Join-Path $appRoot "resources"
$presetsTarget = Join-Path $appRoot "presets"
$translationsTarget = Join-Path $appRoot "translations"
$componentsTarget = Join-Path $appRoot "components"
$installerOutRoot = Join-Path $ProjectRoot "build_win_full\installer"
$installerOut = Join-Path $installerOutRoot $PackageMode
$innoScript = Join-Path $ProjectRoot "scripts\installer\CGPlay.iss"
$nsisScript = Join-Path $ProjectRoot "installer\CGPlay_Installer.nsi"
$validationArtifactRoot = Join-Path $ProjectRoot ("tests\artifacts\package_" + ($appVersion -replace '\.','_'))

if (-not (Test-Path -LiteralPath $ReleaseDir)) {
    throw "Release directory not found: $ReleaseDir"
}

$officialConfigValidator = Join-Path $ProjectRoot "tools\validate_official_build_config.ps1"
if (-not (Test-Path -LiteralPath $officialConfigValidator -PathType Leaf)) {
    throw "Official build configuration validator is missing: $officialConfigValidator"
}
Ensure-Dir $validationArtifactRoot
Write-Step "Validating official Release/tlRender build configuration"
& $officialConfigValidator `
    -ProjectRoot $ProjectRoot `
    -BuildDir (Join-Path $ProjectRoot "build_win_full") `
    -RequireBuiltApps `
    -ReportPath (Join-Path $validationArtifactRoot "package_official_build_config_validation.json") | Out-Host

Write-Step "Preparing clean staging directory"
Reset-Dir $stagingRoot
Ensure-Dir $appRoot
Ensure-Dir $runtimeRoot
Ensure-Dir $toolsTarget
Ensure-Dir $licensesTarget
Ensure-Dir $resourcesTarget
Ensure-Dir $presetsTarget
Ensure-Dir $translationsTarget
Ensure-Dir $componentsTarget
Ensure-Dir $installerOut

Write-Step "Using application version $appVersion"

Write-Step "Copying runtime-aligned build output"
Copy-ReleaseFiles -SourceDir $ReleaseDir -DestinationDir $appRoot
Copy-ReleaseDirectories -SourceDir $ReleaseDir -DestinationDir $appRoot

$releaseToolsDir = Join-Path $ReleaseDir "tools"
if (Test-Path -LiteralPath $releaseToolsDir) {
    Write-Step "Copying tools from release output"
    Copy-DirContents $releaseToolsDir $toolsTarget
} else {
    Write-Step "Release output has no tools directory; falling back to repository tools"
    Copy-Dir (Join-Path $ProjectRoot "tools\cgplay") $toolsTarget
}

if ($PackageMode -eq "full") {
    $releasePythonDir = Join-Path $ReleaseDir "runtime\python"
    if (Test-PythonRuntimeCompleteness $releasePythonDir) {
        Write-Step "Copying bundled Python runtime from release output"
        Copy-DirContents $releasePythonDir $pythonTarget
    } elseif (Test-PythonRuntimeCompleteness $PythonRuntime) {
        Write-Step "Release output runtime is incomplete; falling back to local runtime source"
        Copy-DirContents $PythonRuntime $pythonTarget
    } elseif (Test-Path -LiteralPath (Join-Path $releasePythonDir "python.exe")) {
        throw "Release output Python runtime exists but is incomplete, and fallback runtime source is also incomplete: $releasePythonDir"
    } else {
        throw "No complete Python runtime source is available. Checked: $releasePythonDir and $PythonRuntime"
    }

    $releaseCodexDir = Join-Path $ReleaseDir "runtime\codex"
    if (Test-CodexRuntimeCompleteness $releaseCodexDir) {
        Write-Step "Copying bundled Codex CLI runtime from release output"
        Copy-DirContents $releaseCodexDir $codexTarget
    } elseif (Test-CodexRuntimeCompleteness $CodexRuntime) {
        Write-Step "Copying bundled Codex CLI runtime from packaging source"
        Copy-DirContents $CodexRuntime $codexTarget
    } else {
        throw "No complete Codex CLI runtime is available. Checked: $releaseCodexDir and $CodexRuntime"
    }
}

Write-Step "Staging distributed resources"
Copy-DistributedResources -Root $ProjectRoot -Destination $resourcesTarget

$repoPresets = Join-Path $ProjectRoot "presets"
if (Test-Path -LiteralPath $repoPresets) {
    Write-Step "Copying presets"
    Copy-DirContents $repoPresets $presetsTarget
} else {
    Write-Step "No presets directory found; keeping empty presets folder"
}

if ($PackageMode -eq "full") {
    Write-Step "Ensuring ffmpeg binaries are present"
    $ffmpegTarget = Join-Path $appRoot "ffmpeg.exe"
    $ffprobeTarget = Join-Path $appRoot "ffprobe.exe"
    if (-not (Test-Path -LiteralPath $ffmpegTarget)) {
        Copy-IfExists (Join-Path $FfmpegBinDir "ffmpeg.exe") $appRoot | Out-Null
    }
    if (-not (Test-Path -LiteralPath $ffprobeTarget)) {
        Copy-IfExists (Join-Path $FfmpegBinDir "ffprobe.exe") $appRoot | Out-Null
    }
}

Write-Step "Bundling CGPlay GPLv3 license"
if (-not (Copy-IfExists (Join-Path $ProjectRoot "LICENSE.txt") $appRoot)) {
    throw "Missing required CGPlay license: $(Join-Path $ProjectRoot 'LICENSE.txt')"
}

Write-Step "Bundling third-party notices and source information"
foreach ($legalName in @(
    "THIRD_PARTY_NOTICES.txt",
    "FFMPEG_SOURCE_OFFER.txt"
)) {
    $legalSource = Join-Path (Join-Path $ProjectRoot "docs") $legalName
    if (-not (Copy-IfExists $legalSource $licensesTarget)) {
        throw "Missing required legal document: $legalSource"
    }
}
$licenseSourceDir = Join-Path (Join-Path $ProjectRoot "docs") "third_party_licenses"
if (-not (Test-Path -LiteralPath $licenseSourceDir -PathType Container)) {
    throw "Missing third-party license directory: $licenseSourceDir"
}
Copy-DirContents $licenseSourceDir (Join-Path $licensesTarget "third_party")

Write-Step "Bundling user manuals"
$manualTarget = Join-Path $appRoot "docs"
Ensure-Dir $manualTarget
foreach ($manualName in @("CGPlay_User_Guide_1.0.7.md", "CGPlay_User_Guide_1.0.7.html")) {
    $manualSource = Join-Path (Join-Path $ProjectRoot "docs") $manualName
    if (-not (Copy-IfExists $manualSource $manualTarget)) {
        throw "Missing required user manual: $manualSource"
    }
}

if ($PackageMode -eq "full") {
    Write-Step "Pruning unneeded Python packages"
    $sitePackagesTarget = Join-Path $pythonTarget "Lib\site-packages"
    @(
        "PyQt6",
        "PyQt6-6.11.0.dist-info",
        "PyQt6_Qt6-6.11.1.dist-info",
        "PyQt6_sip-13.10.2.dist-info",
        "PyQt6.uic",
        "aqt",
        "aqtinstall-3.3.0.dist-info",
        "OpenGL",
        "OpenGL_accelerate",
        "pyopengl-3.1.10.dist-info",
        "Cryptodome",
        "setuptools",
        "pip",
        "pkg_resources",
        "beautifulsoup4-4.14.3.dist-info",
        "bs4",
        "bs4-0.0.2.dist-info",
        "defusedxml",
        "defusedxml-0.7.1.dist-info",
        "humanize",
        "humanize-4.15.0.dist-info",
        "imageio",
        "imageio-2.37.3.dist-info",
        "backports",
        "backports_zstd-1.5.0.dist-info",
        "bcj",
        "brotli-1.2.0.dist-info",
        "inflate64",
        "inflate64-1.0.4.dist-info",
        "multivolumefile",
        "multivolumefile-0.2.3.dist-info",
        "openexr-3.4.12.dist-info",
        "py7zr",
        "py7zr-1.0.0.dist-info",
        "pybcj-1.0.6.dist-info",
        "pycryptodomex-3.23.0.dist-info",
        "pyppmd",
        "pyppmd-1.2.0.dist-info",
        "pyzstd",
        "pyzstd-0.18.0.dist-info",
        "soupsieve",
        "soupsieve-2.8.dist-info",
        "texttable-1.7.0.dist-info",
        "OpenColorIO",
        "openimageio-3.1.14.0.dist-info",
        "psutil",
        "psutil-7.1.3.dist-info",
        "__pycache__"
    ) | ForEach-Object {
        Remove-PathIfExists (Join-Path $sitePackagesTarget $_)
    }

    Get-ChildItem -LiteralPath $pythonTarget -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -ieq ".pdb" } |
        Remove-Item -Force

    Get-ChildItem -LiteralPath $pythonTarget -Recurse -Directory -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -in @("__pycache__", "test", "tests", "idlelib", "tkinter", "turtledemo", "lib2to3", "curses", "sqlite3", "pydoc_data", "distutils", "ensurepip", "xml", "xmlrpc", "wsgiref", "zoneinfo", "dbm") } |
        ForEach-Object { Remove-PathIfExists $_.FullName }
}

Validate-StagedPackage -AppRoot $appRoot -PackageMode $PackageMode

$payloadValidator = Join-Path $ProjectRoot "tools\validate_package_payload.ps1"
if (-not (Test-Path -LiteralPath $payloadValidator -PathType Leaf)) {
    throw "Package payload validator is missing: $payloadValidator"
}
Write-Step "Running strict package payload validation"
& $payloadValidator `
    -AppRoot $appRoot `
    -PackageMode $PackageMode `
    -ReportPath (Join-Path $validationArtifactRoot "package_staged_payload_validation.json") | Out-Host

$manifestValidator = Join-Path $ProjectRoot "tools\validate_component_manifest.ps1"
if (-not (Test-Path -LiteralPath $manifestValidator -PathType Leaf)) {
    throw "Component manifest validator is missing: $manifestValidator"
}
Write-Step "Validating staged component manifest"
& $manifestValidator `
    -ManifestPath (Join-Path $appRoot "resources\component_manifest.json") `
    -ReportPath (Join-Path $validationArtifactRoot "package_staged_manifest_validation.json") | Out-Host

if ($PackageMode -eq "full") {
    Write-Step "Running quick import validation for bundled Python runtime"
    $testScript = @'
import sys
import importlib.util
import importlib.metadata

import OpenImageIO
import av
import certifi
import charset_normalizer
import cv2
import faster_whisper
import huggingface_hub
from huggingface_hub import snapshot_download
import idna
import numpy
import PIL
import rapid_videocr
import requests
import torch
import tokenizers
import tqdm
import typing_extensions
import urllib3
import email
import http
import unittest

def ensure_spec(name: str) -> None:
    if importlib.util.find_spec(name) is None:
        raise RuntimeError(f"Missing module: {name}")

print(sys.executable)
print(OpenImageIO.__version__)
print(av.__version__)
print(huggingface_hub.__version__)
print(snapshot_download.__name__)
print(idna.__version__)
print(numpy.__version__)
print(PIL.__version__)
print(cv2.__version__)
print(faster_whisper.__file__)
print(rapid_videocr.__file__)
print(requests.__version__)
print(torch.__version__)
print(tokenizers.__version__)
print(tqdm.__version__)
print(typing_extensions.__version__ if hasattr(typing_extensions, "__version__") else "typing-extensions")
print(urllib3.__version__)
print(unittest.TestCase.__name__)

for module_name in [
    "ctranslate2",
    "faster_whisper",
]:
    ensure_spec(module_name)
    try:
        print(importlib.metadata.version(module_name.replace("_", "-")))
    except importlib.metadata.PackageNotFoundError:
        print(module_name)
'@
    $testScriptPath = Join-Path $stagingRoot "validate_runtime.py"
    Set-Content -LiteralPath $testScriptPath -Value $testScript -Encoding ASCII
    & (Join-Path $pythonTarget "python.exe") $testScriptPath | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Bundled Python runtime validation failed with exit code $LASTEXITCODE"
    }
    Remove-Item -LiteralPath $testScriptPath -Force
}

$selectedInstaller = $Installer
if ($selectedInstaller -eq "auto") {
    $selectedInstaller = if (Find-InnoCompiler) { "inno" } elseif (Find-NsisCompiler) { "nsis" } else { "" }
}

if ([string]::IsNullOrWhiteSpace($selectedInstaller)) {
    throw "No supported installer compiler was found. Install Inno Setup 6 or NSIS."
}

Write-Step "Compiling installer with $selectedInstaller"

switch ($selectedInstaller) {
    "inno" {
        $iscc = Find-InnoCompiler
        if (-not $iscc) {
            throw "Inno Setup compiler not found."
        }
        if (-not (Test-Path -LiteralPath $innoScript)) {
            throw "Inno Setup script not found: $innoScript"
        }
        & $iscc `
            "/DAppVersion=$appVersion" `
            "/DPackageMode=$PackageMode" `
            "/DPackageSource=$appRoot" `
            "/DInstallerOutputDir=$installerOut" `
            $innoScript
        if ($LASTEXITCODE -ne 0) {
            throw "Inno Setup compilation failed with exit code $LASTEXITCODE"
        }
    }
    "nsis" {
        $makensis = Find-NsisCompiler
        if (-not $makensis) {
            throw "NSIS compiler not found."
        }
        if (-not (Test-Path -LiteralPath $nsisScript)) {
            throw "NSIS script not found: $nsisScript"
        }
        $nsisPackageSource = ($appRoot -replace '\\','/')
        $nsisOutputDir = ($installerOut -replace '\\','/')
        & $makensis `
            "/DAPP_VERSION=$appVersion" `
            "/DAPP_FILE_VERSION=$appFileVersion" `
            "/DPACKAGE_MODE=$PackageMode" `
            "/DPACKAGE_SOURCE_DIR=$nsisPackageSource" `
            "/DINSTALLER_OUTPUT_DIR=$nsisOutputDir" `
            $nsisScript
        if ($LASTEXITCODE -ne 0) {
            throw "NSIS compilation failed with exit code $LASTEXITCODE"
        }
    }
    default {
        throw "Unsupported installer type: $selectedInstaller"
    }
}

$packagingComplete = $true
Write-Step "Packaging complete"
[PSCustomObject]@{
    Version = $appVersion
    PreviousVersion = $previousVersion
    VersionBumped = $versionBumped
    Installer = $selectedInstaller
    PackageMode = $PackageMode
    StagingRoot = $appRoot
    OutputDir = $installerOut
} | Format-List

Get-ChildItem -LiteralPath $installerOut | Select-Object Name,Length,LastWriteTime
