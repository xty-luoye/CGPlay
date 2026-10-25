[CmdletBinding()]
param(
    [string]$AppRoot = "",
    [string]$ReportPath = "",
    [string]$Python = "python"
)
$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $AppRoot) { $AppRoot = Join-Path $projectRoot "build_win_full\package\full\CGPlay" }
$manifests = @(Get-ChildItem -LiteralPath (Join-Path $AppRoot "licenses") -Filter "DEPENDENCY_PROVENANCE_*.json" -File)
if ($manifests.Count -ne 1) { throw "Exactly one current dependency provenance manifest is required." }
$arguments = @((Join-Path $PSScriptRoot "validate_release_sources.py"), "--provenance", $manifests[0].FullName, "--app-root", $AppRoot)
if ($ReportPath) { $arguments += @("--report", $ReportPath) }
# Do not load bundled executables/DLLs merely to check notices. Binary identities
# and configuration are recorded by the isolated runtime qualification.
& $Python @arguments
if ($LASTEXITCODE -ne 0) { throw "Release license/source material validation failed." }
