param(
    [string]$TlRenderSourceDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($TlRenderSourceDir)) {
    if (-not [string]::IsNullOrWhiteSpace($env:TLRENDER_SOURCE_DIR)) {
        $TlRenderSourceDir = $env:TLRENDER_SOURCE_DIR
    } else {
        throw "Set -TlRenderSourceDir or the TLRENDER_SOURCE_DIR environment variable to your compatible tlRender source tree."
    }
}
$source = [IO.Path]::GetFullPath($TlRenderSourceDir)
if (-not (Test-Path -LiteralPath (Join-Path $source "CMakeLists.txt"))) {
    throw "tlRender source was not found: $source"
}

$patches = @(
    (Join-Path $root "cmake\patches\tlrender_vp_slice_threading.patch"),
    (Join-Path $root "cmake\patches\tlrender_ffmpeg_common_extensions.patch")
)

function Invoke-GitApply([string[]]$Arguments) {
    # Native argument splatting preserves source/patch paths containing spaces.
    & git.exe @Arguments
    return [int]$LASTEXITCODE
}

Push-Location $source
try {
    foreach ($patch in $patches) {
        $checkCode = Invoke-GitApply @("apply", "--check", "--unsafe-paths", $patch)
        if ($checkCode -eq 0) {
            $applyCode = Invoke-GitApply @("apply", "--unsafe-paths", $patch)
            if ($applyCode -ne 0) { throw "Could not apply patch: $patch" }
            Write-Host "Applied tlRender patch: $([IO.Path]::GetFileName($patch))"
            continue
        }

        $reverseCode = Invoke-GitApply @("apply", "--reverse", "--check", "--unsafe-paths", $patch)
        if ($reverseCode -ne 0) {
            throw "tlRender source does not match patch baseline: $patch"
        }
        Write-Host "tlRender patch already applied: $([IO.Path]::GetFileName($patch))"
    }
} finally {
    Pop-Location
}
