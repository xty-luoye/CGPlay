[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$TlRenderSourceDir,
    [Parameter(Mandatory)][string]$FtkSourceDir,
    [Parameter(Mandatory)][string]$NativePrefix,
    [Parameter(Mandatory)][string]$OpenColorIOPrefix,
    [Parameter(Mandatory)][string]$ImagePrefix,
    [Parameter(Mandatory)][string]$SubprocessIncludeDir,
    [Parameter(Mandatory)][string]$BuildRoot,
    [ValidateRange(1, 64)][int]$Parallel = 4
)

$ErrorActionPreference = 'Stop'
foreach ($engineCommand in @('cmake', 'git')) { Get-Command $engineCommand -ErrorAction Stop | Out-Null }
function Resolve-EngineInput([string]$Value) { return (Resolve-Path -LiteralPath $Value -ErrorAction Stop).Path.Replace('\', '/') }
$tlSource = Resolve-EngineInput $TlRenderSourceDir
$ftkSource = Resolve-EngineInput $FtkSourceDir
$native = Resolve-EngineInput $NativePrefix
$ocio = Resolve-EngineInput $OpenColorIOPrefix
$images = Resolve-EngineInput $ImagePrefix
$subprocess = Resolve-EngineInput $SubprocessIncludeDir
$hook = Resolve-EngineInput (Join-Path $PSScriptRoot 'qualified_native_headers.cmake')
$patch = Resolve-EngineInput (Join-Path $PSScriptRoot 'tlrender_0_27_cgplay_compatibility.patch')
foreach ($engineFile in @("$tlSource/lib/tlRender/Core/Version.h", "$ftkSource/lib/ftk/Core/Version.h",
    "$ocio/include/OpenColorIO/OpenColorIO.h", "$images/include/OpenImageIO/imageio.h", "$subprocess/subprocess.h")) {
    if (-not (Test-Path -LiteralPath $engineFile -PathType Leaf)) { throw "Required engine input missing: $engineFile" }
}
$tlVersion = Get-Content -LiteralPath "$tlSource/lib/tlRender/Core/Version.h" -Raw
$ftkVersion = Get-Content -LiteralPath "$ftkSource/lib/ftk/Core/Version.h" -Raw
if ($tlVersion -notmatch '#define TLRENDER_VERSION_MAJOR 0\b' -or
    $tlVersion -notmatch '#define TLRENDER_VERSION_MINOR 27\b' -or $tlVersion -notmatch '#define TLRENDER_VERSION_PATCH 1\b' -or
    $ftkVersion -notmatch '#define FTK_VERSION_MAJOR 0\b' -or
    $ftkVersion -notmatch '#define FTK_VERSION_MINOR 19\b' -or $ftkVersion -notmatch '#define FTK_VERSION_PATCH 0\b') {
    throw 'Use the clean pinned tlRender 0.27.1 and feather-tk 0.19.0 sources described in README.md.'
}
if (-not [IO.Path]::IsPathRooted($BuildRoot)) { throw 'BuildRoot must be an explicit absolute path.' }
$outputRoot = [IO.Path]::GetFullPath($BuildRoot).Replace('\', '/').TrimEnd('/')
if (Test-Path -LiteralPath $outputRoot) { throw "Choose a new BuildRoot; an existing output is never overwritten: $outputRoot" }
$null = New-Item -ItemType Directory -Path $outputRoot
$stage = "$outputRoot/source/tlRender"
$null = New-Item -ItemType Directory -Path $stage
# Never patch the caller's checkout or copy its Git object database.
Get-ChildItem -LiteralPath $tlSource -Force | Where-Object { $_.Name -ne '.git' } |
    Copy-Item -Destination $stage -Recurse -Force
& git -C $stage apply --check $patch
if ($LASTEXITCODE -ne 0) { throw 'The tlRender source does not match the pinned unmodified patch baseline.' }
& git -C $stage apply $patch
if ($LASTEXITCODE -ne 0) { throw 'Cannot apply the CGPlay engine compatibility patch.' }

$ftkPrefix = "$outputRoot/install/ftk"
$tlPrefix = "$outputRoot/install/tlRender"
$headerRoots = "$ocio;$images"
$prefixes = "$images;$ocio;$ftkPrefix;$native"
$common = @('-G', 'Visual Studio 17 2022', '-A', 'x64', '-DCMAKE_CXX_STANDARD=20', '-DBUILD_SHARED_LIBS=OFF',
    "-DCMAKE_PROJECT_INCLUDE=$hook", "-DCGPLAY_NATIVE_HEADER_PREFIXES=$headerRoots", "-DCMAKE_PREFIX_PATH=$prefixes")
$commands = [Collections.Generic.List[object]]::new()
function Invoke-EngineCMake([string[]]$Arguments) {
    $commands.Add([PSCustomObject]@{ program = 'cmake'; arguments = $Arguments })
    & cmake @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Engine CMake failed with exit code $LASTEXITCODE" }
}
Invoke-EngineCMake (@('-S', $ftkSource, '-B', "$outputRoot/build-ftk", "-DCMAKE_INSTALL_PREFIX=$ftkPrefix") + $common + @(
    '-Dftk_API=GL_4_1', '-Dftk_SDL2=ON', '-Dftk_SDL3=OFF', '-Dftk_TESTS=OFF', '-Dftk_EXAMPLES=OFF',
    '-Dftk_PYTHON=OFF', '-Dftk_nfd=OFF', '-Dftk_CJK=ON'))
Invoke-EngineCMake @('--build', "$outputRoot/build-ftk", '--config', 'Release', '--target', 'install', '--parallel', [string]$Parallel)
Invoke-EngineCMake (@('-S', $stage, '-B', "$outputRoot/build-tlRender", "-DCMAKE_INSTALL_PREFIX=$tlPrefix") + $common + @(
    '-DTLRENDER_FTK_PACKAGE=ON', "-Dftk_DIR=$ftkPrefix/lib/cmake/ftk", '-Dftk_API=GL_4_1',
    "-DTLRENDER_FFMPEG_PREFIX=$native", "-Dsubprocess_INCLUDE_DIR=$subprocess",
    "-DOpenColorIO_DIR=$ocio/lib/cmake/OpenColorIO", "-DImath_DIR=$images/lib/cmake/Imath",
    "-DOpenEXR_DIR=$images/lib/cmake/OpenEXR", "-DOpenImageIO_DIR=$images/lib/cmake/OpenImageIO",
    '-DTLRENDER_OCIO=ON', '-DTLRENDER_EXR=ON', '-DTLRENDER_OIIO=ON', '-DTLRENDER_FFMPEG=ON',
    '-DTLRENDER_FFMPEG_PLUGIN=ON', '-DTLRENDER_SVG=ON', '-DTLRENDER_USD=OFF', '-DTLRENDER_PYTHON=OFF',
    '-DTLRENDER_PROGRAMS=OFF', '-DTLRENDER_EXAMPLES=OFF', '-DTLRENDER_TESTS=OFF'))
Invoke-EngineCMake @('--build', "$outputRoot/build-tlRender", '--config', 'Release', '--target', 'install', '--parallel', [string]$Parallel)

$receipt = [ordered]@{
    schemaVersion = 1; built = $true; runtimeAcceptancePerformed = $false
    tlRenderVersion = '0.27.1'; ftkVersion = '0.19.0'; languageStandard = 20
    expectedTlRenderUpstreamCommit = '0a413e542c54bd5ce89f0b782b00e00982f20e1d'
    expectedFtkUpstreamCommit = '51d9c3d06b608a4a9bd0a0eec54bbf6814699a71'
    sourceValidation = 'Exact version headers and patch applicability; verify original archive hashes against the release dependency manifest before extraction.'
    patchSHA256 = (Get-FileHash -LiteralPath $patch -Algorithm SHA256).Hash.ToLowerInvariant()
    headerHookSHA256 = (Get-FileHash -LiteralPath $hook -Algorithm SHA256).Hash.ToLowerInvariant()
    ftkInstallDir = $ftkPrefix; tlRenderInstallDir = $tlPrefix; patchedTlRenderSourceDir = $stage
    nativePrefix = $native; ocioPrefix = $ocio; imagePrefix = $images; commands = $commands.ToArray()
}
$receipt | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath "$outputRoot/build-stack-receipt.json" -Encoding UTF8
Write-Output "Built SDKs. Player configuration: FEATHER_TK_INSTALL_DIR=$ftkPrefix TLRENDER_INSTALL_DIR=$tlPrefix CGPLAY_FFMPEG_INSTALL_DIR=$native"
