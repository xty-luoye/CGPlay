[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SourceDirectory,
    [Parameter(Mandatory=$true)][string]$IncludePath
)
$ErrorActionPreference = 'Stop'
$sourceRoot = (Get-Item -LiteralPath $SourceDirectory).FullName.TrimEnd('\','/')
if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) { throw 'Package payload directory is missing.' }
$lines = [Collections.Generic.List[string]]::new()
$lines.Add('; Generated from the actual package payload. Never enumerate the install directory at uninstall time.')
$lines.Add('!macro CGPlayUninstallOwnedFiles')
$directories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$pending = [Collections.Generic.Queue[string]]::new()
$pending.Enqueue($sourceRoot)
while ($pending.Count -gt 0) {
    $current = $pending.Dequeue()
    foreach ($entry in Get-ChildItem -LiteralPath $current -Force) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Package payload contains a reparse point: $($entry.FullName)"
        }
        $relative = $entry.FullName.Substring($sourceRoot.Length + 1).Replace('/','\')
        if ($relative -match '["\r\n]' -or $relative -match '(^|\\)\.\.(\\|$)') { throw "Unsafe package path: $relative" }
        if ($entry.PSIsContainer) {
            [void]$directories.Add($relative)
            $pending.Enqueue($entry.FullName)
        } else {
            $escaped = $relative.Replace('$','$$')
            $lines.Add('  Push "$INSTDIR\' + $escaped + '"')
            $lines.Add('  Call un.DeleteOwnedFile')
        }
    }
}
# Only empty owned directories may be removed, deepest first.
foreach ($relative in ($directories | Sort-Object { $_.Split('\').Length } -Descending)) {
    $lines.Add('  Push "$INSTDIR\' + $relative.Replace('$','$$') + '"')
    $lines.Add('  Call un.RemoveOwnedDirectory')
}
$lines.Add('!macroend')
[IO.File]::WriteAllLines([IO.Path]::GetFullPath($IncludePath), $lines, [Text.UTF8Encoding]::new($true))
