[CmdletBinding()]
param(
    [string]$AppRoot = "",
    [string]$ReportPath = ""
)

$ErrorActionPreference = "Stop"
# Historical filename retained for callers. This checks basic release documents,
# not source completeness, copyright ownership, or permission to publish binaries.
if ([string]::IsNullOrWhiteSpace($AppRoot)) {
    $AppRoot = Join-Path (Split-Path -Parent $PSScriptRoot) "build_win_full\package\full\CGPlay"
}

function Test-File([string]$Path) {
    return (Test-Path -LiteralPath $Path -PathType Leaf) -and ((Get-Item -LiteralPath $Path).Length -gt 0)
}

$required = @(
    "LICENSE.txt",
    "licenses\THIRD_PARTY_NOTICES.txt",
    "licenses\FFMPEG_SOURCE_OFFER.txt",
    "licenses\third_party"
)

$checks = @()
foreach ($relative in $required) {
    $path = Join-Path $AppRoot $relative
    $checks += [PSCustomObject]@{
        name = "present:$relative"
        passed = (Test-Path -LiteralPath $path)
        detail = $path
    }
}

$ffmpegPath = Join-Path $AppRoot "ffmpeg.exe"
if (Test-File $ffmpegPath) {
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $licenseText = (& $ffmpegPath -L 2>&1 | Out-String)
    $ErrorActionPreference = $savedErrorActionPreference
    $checks += [PSCustomObject]@{
        name = "ffmpeg-reports-license"
        passed = ($licenseText -match "GNU (Lesser )?General Public License" -or $licenseText -match "GNU L?GPL")
        detail = (($licenseText.Split([Environment]::NewLine) | Where-Object { $_ -match "GNU ((Lesser )?General Public License|L?GPL)" } | Select-Object -First 1))
    }
} else {
    $checks += [PSCustomObject]@{
        name = "ffmpeg-reports-license"
        passed = $false
        detail = "ffmpeg.exe is missing"
    }
}

foreach ($relative in @(
    "LICENSE.txt",
    "licenses\FFMPEG_SOURCE_OFFER.txt"
)) {
    $path = Join-Path $AppRoot $relative
    if (Test-File $path) {
        $text = Get-Content -LiteralPath $path -Raw
        $checks += [PSCustomObject]@{
            name = "no-release-placeholder:$relative"
            passed = ($text -notmatch "REPLACE BEFORE RELEASE")
            detail = "Basic document check only; matching source and publication rights require separate review"
        }
    }
}

$passed = @($checks | Where-Object { -not $_.passed }).Count -eq 0
$report = [PSCustomObject]@{
    scope = "basic-license-materials"
    binaryPublicationApproved = $false
    appRoot = (Resolve-Path -LiteralPath $AppRoot -ErrorAction SilentlyContinue).Path
    passed = $passed
    checks = $checks
}

if (-not [string]::IsNullOrWhiteSpace($ReportPath)) {
    $parent = Split-Path -Parent $ReportPath
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
}

$report | ConvertTo-Json -Depth 6
if (-not $passed) {
    exit 1
}
