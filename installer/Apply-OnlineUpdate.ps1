$ErrorActionPreference = 'Stop'
$env:PSModulePath = Join-Path $PSHOME 'Modules'
$root = [IO.Path]::GetFullPath($env:CGPLAY_ONLINE_INSTALL_DIR).TrimEnd('\')
$source = $env:CGPLAY_ONLINE_SOURCE_DIR
$release = 'https://github.com/xty-luoye/CGPlay/releases/download'
$zip = 'CGPlay_Update_1.0.8.01_to_1.0.8.02_win64.zip'
$base = 'CGPlay_Setup_1.0.8.01_full.exe'
$baseHash = '5037c1a3a957906e7f4e7a77090f4560eae529d438e95d29588e6db5a1cee765'
$zipHash = 'a919a5cc7f2af9f0a3fc9a6e9f32f3c371a844926da6382d46e26b76d0267d25'
$files = [ordered]@{
    'CGPlayQuickLook.exe' = @('3a11fd16772df2aa19a3c638229305b5ad09b210d322b7e27b3f4c94a02a97a8','9a8296e06f02b7a3b0a25801f4756b5da8551e5e422e5d1a4110e79c32468889')
    'CGPlayThumbnailProvider.dll' = @('f6af5cbe6f1d934d532c1a21ba5031d2699596c08515cdcb6ec00577ef56da32','b1b8a2ce3dd9c1f1b41334b0f2668d923c2b1cde7490e34fc6df20f9c64c8c2f')
    'CGPlay.exe' = @('aad451375edfcdb78cbce824aebc54a03c5329ab21d5fe9d73a12356e81a025e','301e8afb49cbbfdc8561284cac6cca9acba1a97cc891b42513fd783a123b219d')
}
function Get-VerifiedAsset($tag, $name, $sha) {
    $path = Join-Path $source $name
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        $path = Join-Path $temp $name
        Write-Output "Downloading $name" | Out-Host
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $wc = New-Object Net.WebClient
        $wc.Headers['User-Agent'] = 'CGPlay-Setup/1.0.8.02'
        try {
            try { $wc.DownloadFile("$release/$tag/$name", $path) }
            catch {
                $id = '732672825'
                $wc.Headers['Accept'] = 'application/octet-stream'
                $wc.DownloadFile("https://api.github.com/repos/xty-luoye/CGPlay/releases/assets/$id", $path)
            }
        } finally { $wc.Dispose() }
    }
    if ((Get-FileHash -LiteralPath $path).Hash -ne $sha) { throw "SHA-256 mismatch: $name" }
    return $path
}
function Test-Base {
    foreach ($name in $files.Keys) {
        $path = Join-Path $root $name
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path).Hash -notin $files[$name]) { return $false }
    }
    foreach ($name in 'Qt6Core.dll','platforms\qwindows.dll','ffmpeg.exe','runtime\python\python.exe') {
        if (!(Test-Path -LiteralPath (Join-Path $root $name) -PathType Leaf)) { return $false }
    }
    return $true
}
$mutex = New-Object Threading.Mutex($false,'Local\CGPlayOnlineSetup')
$temp = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString('N'))
$held = $false
$restartPreview = $false
$stage = $null
$keepBackup = $false
try {
    try { $held = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $held = $true }
    if (!$held) { throw 'Another CGPlay update is running.' }
    if (!$root -or $root -eq [IO.Path]::GetPathRoot($root).TrimEnd('\')) { throw 'Select an application directory.' }
    $player = Join-Path $root 'CGPlay.exe'
    $preview = Join-Path $root 'CGPlayQuickLook.exe'
    if (Test-Path -LiteralPath $player) {
        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($player)
        if ($info.ProductName -notlike '*CGPlay*' -or [version]$info.ProductVersion -gt [version]'1.0.8.2') { throw 'Unexpected application or newer CGPlay version in the selected folder.' }
    } elseif ((Test-Path -LiteralPath $root) -and @(Get-ChildItem -LiteralPath $root -Force | Select-Object -First 1).Count) { throw 'Select an empty directory or an existing CGPlay installation.' }
    [void][IO.Directory]::CreateDirectory($temp)
    $compatible = Test-Base
    if (!$compatible -and @(Get-Process -Name CGPlay -ErrorAction SilentlyContinue).Count) { throw 'Close CGPlay before installing the base runtime.' }
    if (@(Get-Process -Name CGPlay -ErrorAction SilentlyContinue | Where-Object Path -eq $player).Count) { throw 'Close CGPlay before updating.' }
    # Verify the upgrade before running an older base installer or changing any existing files.
    $archive = Join-Path $PSScriptRoot $zip
    if ((Get-FileHash -LiteralPath $archive).Hash -ne $zipHash) { throw 'Corrupt embedded update archive.' }
    Expand-Archive -LiteralPath $archive -DestinationPath "$temp\payload"
    foreach ($name in $files.Keys) {
        if ((Get-FileHash -LiteralPath "$temp\payload\$name").Hash -ne $files[$name][1]) { throw "Invalid payload: $name" }
    }
    if (!$compatible) {
        $installer = Get-VerifiedAsset 'v1.0.8.01' $base $baseHash
        $proc = Start-Process -FilePath $installer -ArgumentList "/S /D=$root" -WindowStyle Hidden -PassThru
        $proc.WaitForExit()
        if ($proc.ExitCode -ne 0 -or !(Test-Base)) { throw 'Base installation failed.' }
    }
    $changes = @($files.Keys | Where-Object { (Get-FileHash -LiteralPath (Join-Path $root $_)).Hash -ne $files[$_][1] })
    if ($changes.Count) {
        foreach ($proc in (Get-Process -Name CGPlayQuickLook -ErrorAction SilentlyContinue | Where-Object Path -eq $preview)) {
            $proc.Kill()
            if (!$proc.WaitForExit(10000)) { throw 'Close QuickLook and retry.' }
            $restartPreview = $true
        }
        $stage = Join-Path $root ('.cgplay-update-' + [guid]::NewGuid().ToString('N'))
        [void][IO.Directory]::CreateDirectory($stage)
        $done = New-Object 'Collections.Generic.List[string]'
        try {
            foreach ($name in $changes) {
                $new = Join-Path $stage $name
                [IO.File]::Copy("$temp\payload\$name",$new)
                [IO.File]::Replace($new,(Join-Path $root $name),"$new.bak")
                $done.Add($name)
            }
        } catch {
            foreach ($name in $done) {
                try { [IO.File]::Replace("$stage\$name.bak",(Join-Path $root $name),"$stage\$name.failed") }
                catch { $keepBackup = $true }
            }
            throw
        }
    }
    $reg = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\CGPlay'
    if ((Get-ItemProperty -LiteralPath $reg -ErrorAction SilentlyContinue).InstallLocation -eq $root) {
        Set-ItemProperty -LiteralPath $reg -Name DisplayVersion -Value '1.0.8.02'
    }
    Write-Host 'CGPlay 1.0.8.02 installed.'
} finally {
    if ($restartPreview -and (Test-Path -LiteralPath $preview)) { Start-Process -FilePath $preview -WindowStyle Hidden }
    if ($stage -and !$keepBackup -and [IO.Directory]::GetParent($stage).FullName -eq $root -and [IO.Path]::GetFileName($stage) -match '^\.cgplay-update-[0-9a-f]{32}$') { [IO.Directory]::Delete($stage,$true) }
    if ((Test-Path -LiteralPath $temp) -and [IO.Path]::GetFileName($temp) -match '^[0-9a-f]{32}$' -and [IO.Directory]::GetParent($temp).FullName.TrimEnd('\') -eq [IO.Path]::GetTempPath().TrimEnd('\')) { [IO.Directory]::Delete($temp,$true) }
    if ($held) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
