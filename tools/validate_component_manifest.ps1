[CmdletBinding()]
param(
    [string]$ManifestPath = "",
    [string]$PayloadRoot = "",
    [switch]$RequireHashes,
    [switch]$RequirePayloads,
    [switch]$RequireSignature,
    [string]$SignaturePath = "",
    [string]$PublicKeyPath = "",
    [string]$ReportPath = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = Join-Path (Split-Path -Parent $PSScriptRoot) "resources\component_manifest.json"
}
if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw "Manifest not found: $ManifestPath"
}

$manifestFullPath = (Resolve-Path -LiteralPath $ManifestPath).Path
$manifestSha256 = (Get-FileHash -LiteralPath $manifestFullPath -Algorithm SHA256).Hash.ToLowerInvariant()
$manifest = Get-Content -LiteralPath $manifestFullPath -Raw | ConvertFrom-Json
$checks = [System.Collections.Generic.List[object]]::new()

function Add-ManifestCheck([string]$Name, [bool]$Passed, [string]$Details) {
    $checks.Add([PSCustomObject]@{
        name = $Name
        passed = $Passed
        details = $Details
    })
}

function Test-SafeRelativePath([string]$Path, [bool]$AllowDot = $false) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    if ($AllowDot -and $Path -eq ".") { return $true }
    if ($Path -match '[:*?"<>|]') { return $false }
    try {
        if ([System.IO.Path]::IsPathRooted($Path)) { return $false }
    } catch {
        return $false
    }

    $segments = @($Path.Replace('/', '\').Split('\') | Where-Object { $_ -ne "" })
    if ($segments.Count -eq 0 -or $segments -contains ".." -or $segments -contains ".") { return $false }

    foreach ($segment in $segments) {
        if ($segment.TrimEnd(' ', '.') -ne $segment) { return $false }
        $deviceName = [System.IO.Path]::GetFileNameWithoutExtension($segment)
        if ($deviceName -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$') { return $false }
    }
    return $true
}

function Test-HttpsUrl([string]$Url) {
    if ([string]::IsNullOrWhiteSpace($Url)) { return $true }
    $uri = $null
    return [Uri]::TryCreate($Url, [UriKind]::Absolute, [ref]$uri) -and $uri.Scheme -eq "https"
}

function Find-OpenSsl {
    $command = Get-Command openssl -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($candidate in @(
        "C:\Program Files\Git\usr\bin\openssl.exe",
        "C:\Program Files\OpenSSL-Win64\bin\openssl.exe",
        "C:\Program Files\OpenSSL\bin\openssl.exe"
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    return $null
}

$manifestVersion = 0
$manifestVersionValid = $null -ne $manifest.manifestVersion -and [int]::TryParse([string]$manifest.manifestVersion, [ref]$manifestVersion) -and $manifestVersion -ge 1
Add-ManifestCheck "manifestVersion" $manifestVersionValid "manifestVersion must be an integer >= 1"
Add-ManifestCheck "componentsObject" ($null -ne $manifest.components -and $manifest.components.PSObject.Properties.Count -gt 0) "components must be a non-empty object"
Add-ManifestCheck "manifestUrlHttps" (Test-HttpsUrl ([string]$manifest.manifestUrl)) "manifestUrl must be empty or use https"

$payloadRootResolved = ""
if (-not [string]::IsNullOrWhiteSpace($PayloadRoot)) {
    $payloadRootResolved = if (Test-Path -LiteralPath $PayloadRoot) {
        (Resolve-Path -LiteralPath $PayloadRoot).Path
    } else {
        [System.IO.Path]::GetFullPath($PayloadRoot)
    }
}

if ($null -ne $manifest.components) {
    foreach ($componentProperty in $manifest.components.PSObject.Properties) {
        $id = $componentProperty.Name
        $component = $componentProperty.Value
        $requiredFiles = @($component.requiredFiles)
        $hash = [string]$component.sha256
        $archiveName = [string]$component.archiveName
        $url = [string]$component.url
        $version = [string]$component.version
        $installSubdir = [string]$component.installSubdir

        Add-ManifestCheck "$id.idFormat" ($id -match '^[a-z0-9][a-z0-9-]*$') "component id must use lowercase letters, digits, and hyphens"
        Add-ManifestCheck "$id.version" (-not [string]::IsNullOrWhiteSpace($version)) "component version must not be empty"
        Add-ManifestCheck "$id.requiredFiles" ($requiredFiles.Count -gt 0) "component must name at least one required file"
        $safeRequiredFiles = @($requiredFiles | Where-Object { Test-SafeRelativePath ([string]$_) })
        $uniqueRequiredFiles = @($requiredFiles | ForEach-Object { ([string]$_).Replace('\', '/').ToLowerInvariant() } | Select-Object -Unique)
        Add-ManifestCheck "$id.requiredFilesSafe" ($safeRequiredFiles.Count -eq $requiredFiles.Count) "required files must be safe relative paths"
        Add-ManifestCheck "$id.requiredFilesUnique" ($uniqueRequiredFiles.Count -eq $requiredFiles.Count) "required files must not contain duplicates"
        Add-ManifestCheck "$id.sha256Format" ([string]::IsNullOrWhiteSpace($hash) -or $hash -match '^[0-9a-fA-F]{64}$') "sha256 must be empty or 64 hex characters"
        Add-ManifestCheck "$id.urlHttps" (Test-HttpsUrl $url) "component URL must be empty or use https"
        $archiveNameSafe = Test-SafeRelativePath $archiveName
        Add-ManifestCheck "$id.archiveName" $archiveNameSafe "archiveName must be a safe relative file name"
        Add-ManifestCheck "$id.archiveNameLeaf" ($archiveNameSafe -and [System.IO.Path]::GetFileName($archiveName) -eq $archiveName) "archiveName must not contain directories"
        Add-ManifestCheck "$id.installSubdir" (Test-SafeRelativePath $installSubdir $true) "installSubdir must be '.' or a safe relative path"

        $hashRequiredForComponent = $RequireHashes -and (-not [string]::IsNullOrWhiteSpace($url) -or -not [string]::IsNullOrWhiteSpace($archiveName))
        Add-ManifestCheck "$id.sha256Required" (-not $hashRequiredForComponent -or -not [string]::IsNullOrWhiteSpace($hash)) "release manifests must hash downloadable archives"

        if (-not [string]::IsNullOrWhiteSpace($payloadRootResolved) -and $archiveNameSafe) {
            $payloadPath = Join-Path $payloadRootResolved $archiveName
            $payloadExists = Test-Path -LiteralPath $payloadPath -PathType Leaf
            Add-ManifestCheck "$id.payloadExists" ($payloadExists -or -not $RequirePayloads) $payloadPath
            if ($payloadExists -and -not [string]::IsNullOrWhiteSpace($hash)) {
                $actualHash = (Get-FileHash -LiteralPath $payloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
                Add-ManifestCheck "$id.payloadHash" ($actualHash -eq $hash.ToLowerInvariant()) "expected=$hash actual=$actualHash"
            }
        }
    }
}

$signatureStatus = "not-configured"
$opensslPath = ""
$signatureConfigured = -not [string]::IsNullOrWhiteSpace($SignaturePath) -or -not [string]::IsNullOrWhiteSpace($PublicKeyPath)
if ($RequireSignature -and -not $signatureConfigured) {
    Add-ManifestCheck "detachedSignature" $false "RequireSignature needs SignaturePath and PublicKeyPath"
    $signatureStatus = "required-missing"
} elseif ($signatureConfigured) {
    if ([string]::IsNullOrWhiteSpace($SignaturePath) -or [string]::IsNullOrWhiteSpace($PublicKeyPath)) {
        Add-ManifestCheck "detachedSignature" $false "SignaturePath and PublicKeyPath must be supplied together"
        $signatureStatus = "invalid-configuration"
    } else {
        $opensslPath = Find-OpenSsl
        if ([string]::IsNullOrWhiteSpace($opensslPath)) {
            Add-ManifestCheck "detachedSignature" $false "openssl is required for detached RSA/SHA-256 verification"
            $signatureStatus = "openssl-unavailable"
        } elseif (-not (Test-Path -LiteralPath $SignaturePath -PathType Leaf) -or -not (Test-Path -LiteralPath $PublicKeyPath -PathType Leaf)) {
            Add-ManifestCheck "detachedSignature" $false "signature or public key file is missing"
            $signatureStatus = "inputs-missing"
        } else {
            & $opensslPath dgst -sha256 -verify $PublicKeyPath -signature $SignaturePath $manifestFullPath | Out-Null
            $signaturePassed = $LASTEXITCODE -eq 0
            Add-ManifestCheck "detachedSignature" $signaturePassed "openssl dgst -sha256 verification"
            $signatureStatus = if ($signaturePassed) { "verified" } else { "failed" }
        }
    }
}

$failed = @($checks | Where-Object { -not $_.passed })
$report = [PSCustomObject]@{
    schemaVersion = 1
    checkedAtUtc = [DateTime]::UtcNow.ToString("o")
    manifestPath = $manifestFullPath
    manifestSha256 = $manifestSha256
    payloadRoot = $payloadRootResolved
    requireHashes = [bool]$RequireHashes
    requirePayloads = [bool]$RequirePayloads
    requireSignature = [bool]$RequireSignature
    signatureStatus = $signatureStatus
    opensslPath = $opensslPath
    overallPass = $failed.Count -eq 0
    checkCount = $checks.Count
    failedCount = $failed.Count
    checks = $checks
}

if (-not [string]::IsNullOrWhiteSpace($ReportPath)) {
    $reportDirectory = Split-Path -Parent $ReportPath
    if (-not [string]::IsNullOrWhiteSpace($reportDirectory)) {
        New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
    }
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ReportPath -Encoding UTF8
}

$report
if ($failed.Count -gt 0) {
    $failedNames = ($failed | ForEach-Object { $_.name }) -join ", "
    throw "Component manifest validation failed: $failedNames"
}
