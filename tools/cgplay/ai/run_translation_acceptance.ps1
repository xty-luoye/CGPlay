param(
    [string]$Repo = "C:\Users\1\Desktop\RVLite",
    [string]$ArtifactsDir = "C:\Users\1\Desktop\RVLite\tests\artifacts\translation_all_sources_20260709",
    [string]$TranslationsDir = "C:\Users\1\Desktop\CGPlay_Translations",
    [string[]]$Media = @(),
    [string]$Backup = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$headless = Join-Path $Repo "tools\cgplay\ai\headless_translation_gate.py"
$reportWriter = Join-Path $Repo "tools\cgplay\ai\write_acceptance_report.py"
$summaryRaw = Join-Path $ArtifactsDir "headless_translation_gate.json"
$exe = Join-Path $Repo "build_win_full\bin\Release\CGPlay.exe"
$buildDeployJson = Join-Path $ArtifactsDir "build_deploy.json"

New-Item -ItemType Directory -Force -Path $ArtifactsDir | Out-Null

if ([string]::IsNullOrWhiteSpace($Backup)) {
    $latestBackup = Get-ChildItem -LiteralPath "E:\rjkf\cpglay" -Directory -Filter "RVLite_backup_*" -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "backup_manifest.json") } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -ne $latestBackup) {
        $Backup = $latestBackup.FullName
    }
}

function Get-SafeNameForAcceptance {
    param([string]$Value)
    $chars = New-Object System.Collections.Generic.List[char]
    foreach ($ch in $Value.ToCharArray()) {
        if ([char]::IsLetterOrDigit($ch) -or $ch -eq '-' -or $ch -eq '_' -or $ch -eq '.') {
            $chars.Add($ch)
        } elseif ([char]::IsWhiteSpace($ch)) {
            $chars.Add('_')
        }
    }
    $name = (-join $chars).Trim(' ', '.', '_')
    if ([string]::IsNullOrWhiteSpace($name)) { return "video" }
    if ($name.Length -gt 80) { return $name.Substring(0, 80) }
    return $name
}

function Get-MediaIdentityForCache {
    param([string]$MediaPath)
    $item = Get-Item -LiteralPath $MediaPath
    $absolute = $item.FullName
    $size = [int64]$item.Length
    $mtimeMs = [int64]([DateTimeOffset]::new($item.LastWriteTimeUtc).ToUnixTimeMilliseconds())
    $inputText = "$($absolute.ToLowerInvariant())|$size|$mtimeMs"
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($inputText)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $hash = -join ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString("x2") })
    return [ordered]@{ mediaPath = $absolute; fileSize = $size; mtimeMs = $mtimeMs; mediaFingerprint = $hash }
}

function Get-LegacySlashFingerprintForCache {
    param([string]$MediaPath)
    $item = Get-Item -LiteralPath $MediaPath
    $absolute = $item.FullName.Replace('\', '/').ToLowerInvariant()
    $size = [int64]$item.Length
    $mtimeMs = [int64]([DateTimeOffset]::new($item.LastWriteTimeUtc).ToUnixTimeMilliseconds())
    $inputText = "$absolute|$size|$mtimeMs"
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($inputText)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    return -join ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString("x2") })
}

function Get-VttCueCount {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return 0 }
    try {
        return (Select-String -LiteralPath $Path -Pattern "-->" -SimpleMatch -ErrorAction Stop).Count
    } catch {
        return 0
    }
}

function Write-CurrentMediaIdentitySidecar {
    param(
        [string]$CachePath,
        $Identity,
        [string]$Source
    )
    $sha = (Get-FileHash -LiteralPath $CachePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $contentSize = (Get-Item -LiteralPath $CachePath).Length
    $meta = [ordered]@{
        schema = "cgplay-media-identity-v1"
        mediaPath = $Identity.mediaPath
        fileSize = $Identity.fileSize
        mtimeMs = $Identity.mtimeMs
        durationMs = -1
        mediaFingerprint = $Identity.mediaFingerprint
        cachePath = $CachePath
        cacheContentSha256 = $sha
        cacheContentSize = $contentSize
        createdAt = (Get-Date).ToUniversalTime().ToString("o")
        source = $Source
    }
    $meta | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$CachePath.media.json" -Encoding UTF8
}

function Sync-AcceptanceGeneratedTracksForHeadless {
    param([string]$MediaPath)
    if (-not (Test-Path -LiteralPath $MediaPath)) { return }
    $item = Get-Item -LiteralPath $MediaPath
    $identity = Get-MediaIdentityForCache -MediaPath $MediaPath
    $safe = Get-SafeNameForAcceptance -Value $item.BaseName
    $tracksDir = Join-Path $ArtifactsDir "videos\$safe\generated_tracks"
    if (-not (Test-Path -LiteralPath $tracksDir)) { return }
    $prefix = Join-Path $TranslationsDir "$($item.BaseName).$($identity.mediaFingerprint.Substring(0,12))"
    New-Item -ItemType Directory -Force -Path $TranslationsDir | Out-Null
    $copiedSuffixes = @{}
    Get-ChildItem -LiteralPath $tracksDir -File -Filter "*.vtt" -ErrorAction SilentlyContinue |
        Sort-Object @{ Expression = { Get-VttCueCount -Path $_.FullName }; Descending = $true }, @{ Expression = { $_.LastWriteTimeUtc }; Descending = $true } |
        ForEach-Object {
            $match = [regex]::Match($_.Name, "^[\s\S]+?\.[0-9a-fA-F]{12}(\..+\.vtt)$")
            if (-not $match.Success) { return }
            $suffix = $match.Groups[1].Value
            if ($copiedSuffixes.ContainsKey($suffix)) { return }
            $metaPath = "$($_.FullName).media.json"
            $meta = $null
            if (Test-Path -LiteralPath $metaPath) {
                try { $meta = Get-Content -LiteralPath $metaPath -Encoding UTF8 | ConvertFrom-Json } catch { $meta = $null }
            }
            if (-not $meta -or -not $meta.mediaPath -or -not $meta.mediaFingerprint -or -not $meta.cacheContentSha256) {
                return
            }
            if (-not $meta.fileSize -or ([int64]$meta.fileSize -ne [int64]$identity.fileSize)) { return }
            if (-not $meta.mtimeMs -or ([int64]$meta.mtimeMs -ne [int64]$identity.mtimeMs)) { return }
            $metaPathMatchesCurrentMedia = $true
            $metaPathMatchesCurrentMedia =
                (([string]$meta.mediaPath).Replace('/','\').ToLowerInvariant() -eq $item.FullName.ToLowerInvariant())
            if (-not $metaPathMatchesCurrentMedia) { return }
            $trackHash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($trackHash -ne ([string]$meta.cacheContentSha256).ToLowerInvariant()) { return }
            $dst = "$prefix$suffix"
            Copy-Item -LiteralPath $_.FullName -Destination $dst -Force
            Write-CurrentMediaIdentitySidecar -CachePath $dst -Identity $identity -Source "headless-acceptance-media-scoped-cache-hit"
            $copiedSuffixes[$suffix] = $true
        }
}

function Sync-LegacySidecarsForHeadless {
    param([string]$MediaPath)
    # P0 cache-isolation rule: headless acceptance must not create or rewrite
    # identity for legacy unscoped subtitle caches. It may seal already-scoped,
    # matching caches with a content hash so later loads can detect stale reuse.
    if (-not (Test-Path -LiteralPath $MediaPath)) { return }
    New-Item -ItemType Directory -Force -Path $TranslationsDir | Out-Null
    $item = Get-Item -LiteralPath $MediaPath
    $identity = Get-MediaIdentityForCache -MediaPath $MediaPath
    $legacySlashFingerprint = Get-LegacySlashFingerprintForCache -MediaPath $MediaPath
    $prefix = Join-Path $TranslationsDir "$($item.BaseName).$($identity.mediaFingerprint.Substring(0,12))"
    $suffixes = @(
        ".source.vtt", ".source.srt", ".zh.vtt", ".zh.srt",
        ".ocr.source.vtt", ".ocr.translated.zh.vtt",
        ".workbench.vision.source.vtt", ".workbench.vision.translated.zh.vtt",
        ".online.source.vtt", ".online.translated.zh.vtt",
        ".enhanced.zh.vtt", ".refined.zh.vtt"
    )
    foreach ($suffix in $suffixes) {
        $scoped = "$prefix$suffix"
        if ($legacySlashFingerprint -and
            ([string]$legacySlashFingerprint -ne [string]$identity.mediaFingerprint)) {
            $legacyPrefix = Join-Path $TranslationsDir "$($item.BaseName).$($legacySlashFingerprint.Substring(0,12))"
            $legacyScoped = "$legacyPrefix$suffix"
            if (Test-Path -LiteralPath $legacyScoped) {
                $legacyCount = Get-VttCueCount -Path $legacyScoped
                $currentCount = Get-VttCueCount -Path $scoped
                if (($legacyCount -gt 0) -and (($currentCount -eq 0) -or ($legacyCount -gt $currentCount))) {
                    Copy-Item -LiteralPath $legacyScoped -Destination $scoped -Force
                    Write-CurrentMediaIdentitySidecar -CachePath $scoped -Identity $identity -Source "headless-acceptance-legacy-slash-fingerprint-resealed"
                    if ($suffix -eq ".ocr.source.vtt") {
                        $legacyIntake = "$legacyPrefix.ocr.intake.json"
                        $currentIntake = "$prefix.ocr.intake.json"
                        if (Test-Path -LiteralPath $legacyIntake) {
                            Copy-Item -LiteralPath $legacyIntake -Destination $currentIntake -Force
                        }
                    }
                }
            }
        }
        if (-not (Test-Path -LiteralPath $scoped)) { continue }
        $metaPath = "$scoped.media.json"
        $existing = $null
        if (Test-Path -LiteralPath $metaPath) {
            try { $existing = Get-Content -LiteralPath $metaPath -Encoding UTF8 | ConvertFrom-Json } catch { $existing = $null }
        }
        if ($existing -and $existing.mediaFingerprint -and ([string]$existing.mediaFingerprint -ne [string]$identity.mediaFingerprint)) {
            continue
        }
        if ($existing -and $existing.fileSize -and ([int64]$existing.fileSize -ne [int64]$identity.fileSize)) {
            continue
        }
        Write-CurrentMediaIdentitySidecar -CachePath $scoped -Identity $identity -Source "headless-acceptance-cache-sealed"
    }
}

$buildDeploy = [ordered]@{
    releaseBuild = [ordered]@{ passed = $false; command = "cmake --build $Repo\build_win_full --config Release --target CGPlay"; skipped = [bool]$SkipBuild }
    windeployqt = [ordered]@{ passed = $false; command = "C:\QtClean\6.5.3\msvc2019_64\bin\windeployqt.exe $exe"; skipped = [bool]$SkipBuild }
}

if (-not $SkipBuild) {
    & cmake --build (Join-Path $Repo "build_win_full") --config Release --target CGPlay
    $buildDeploy.releaseBuild.exitCode = $LASTEXITCODE
    $buildDeploy.releaseBuild.passed = ($LASTEXITCODE -eq 0)
    if (-not $buildDeploy.releaseBuild.passed) {
        $buildDeploy | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $buildDeployJson -Encoding UTF8
        throw "Release build failed"
    }
    & "C:\QtClean\6.5.3\msvc2019_64\bin\windeployqt.exe" $exe
    $buildDeploy.windeployqt.exitCode = $LASTEXITCODE
    $buildDeploy.windeployqt.passed = ($LASTEXITCODE -eq 0)
    $buildDeploy | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $buildDeployJson -Encoding UTF8
    if (-not $buildDeploy.windeployqt.passed) {
        throw "windeployqt failed"
    }
} else {
    $buildDeploy | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $buildDeployJson -Encoding UTF8
}

if ($Media.Count -eq 0) {
    $desktopAniOne = Get-ChildItem -LiteralPath "C:\Users\1\Desktop" -File -Filter "*.mp4" -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Name -match "Ani-One" -and
            $_.Name -notmatch "phase_test|ascii" -and
            $_.Length -gt 100MB
        } |
        Sort-Object Length -Descending |
        Select-Object -First 1
    $candidates = @(
        "C:\Users\1\Desktop\RVLite\tests\media\elephants_dream_dialogue_5min_burned_zh.mp4",
        "C:\Users\1\Desktop\RVLite\tests\media\elephants_dream_dialogue_5min_embedded_zh.mp4",
        "C:\Users\1\Desktop\RVLite\tests\media\no_subtitle_asr_sample.mp4",
        "C:\Users\1\Desktop\RVLite_Translation_TestVideos\elephants_dream_dialogue_5min_burned_zh.mp4",
        "C:\Users\1\Desktop\RVLite_Translation_TestVideos\elephants_dream_dialogue_5min_embedded_en.mp4",
        "C:\Users\1\Desktop\RVLite_Translation_TestVideos\elephants_dream_dialogue_45s_no_subtitle.mp4"
    )
    if ($null -ne $desktopAniOne) {
        $candidates = @($desktopAniOne.FullName) + $candidates
    } elseif (Test-Path -LiteralPath "C:\Users\1\Desktop\anione_phase_test.mp4") {
        $candidates = @("C:\Users\1\Desktop\anione_phase_test.mp4") + $candidates
    }
    $found = @($candidates | Where-Object { Test-Path -LiteralPath $_ })
    $preferredJinwoo = @(
        "E:\油罐下载\Jinwoo vs Barca ｜ Solo Leveling Season 2 -Arise from the Shadow-.mp4"
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    $jinwoo = $null
    if ($null -ne $preferredJinwoo) {
        $jinwoo = Get-Item -LiteralPath $preferredJinwoo
    } else {
        $jinwoo = Get-ChildItem -LiteralPath "E:\" -Directory -ErrorAction SilentlyContinue |
            ForEach-Object {
                Get-ChildItem -LiteralPath $_.FullName -Filter "*Jinwoo*.mp4" -ErrorAction SilentlyContinue
            } |
            Sort-Object `
                @{ Expression = { if ($_.Name -match "Barca|Arise|Solo Leveling") { 0 } else { 1 } } }, `
                @{ Expression = { $_.LastWriteTimeUtc }; Descending = $true } |
            Select-Object -First 1
    }
    if ($null -ne $jinwoo) {
        $found += $jinwoo.FullName
    }
    $knownResolved = @($found | ForEach-Object {
        try { (Get-Item -LiteralPath $_).FullName.ToLowerInvariant() } catch { $_.ToLowerInvariant() }
    })
    $holdoutRoots = @(
        "E:\油罐下载",
        "C:\Users\1\Desktop\RVLite_Translation_TestVideos",
        "C:\Users\1\Desktop\RVLite\tests\media"
    )
    $holdouts = @()
    foreach ($root in $holdoutRoots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $holdouts += Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object {
                $full = $_.FullName.ToLowerInvariant()
                $ext = $_.Extension.ToLowerInvariant()
                ($knownResolved -notcontains $full) -and
                (@(".mp4", ".mov", ".mkv") -contains $ext) -and
                ($_.Length -gt 1024KB) -and
                ($_.Name -notmatch 'anione|ani-one|jinwoo|solo leveling|elephants|no_subtitle')
            } |
            Sort-Object Length |
            Select-Object -First 2
    }
    if ($holdouts.Count -gt 0) {
        $found += ($holdouts | Select-Object -First 1 | ForEach-Object { $_.FullName })
    }
    $Media = @($found)
}

if ($Media.Count -eq 0) {
    throw "No media found for acceptance run. Pass -Media with one or more paths."
}

foreach ($m in $Media) {
    # Do not seed runtime translation caches from the previous acceptance
    # package unless the track has a current-media .media.json sidecar, matching
    # path/size/mtime/fingerprint, and a verified content hash. This preserves
    # the product cache-hit path without reviving stale generated_tracks.
    Sync-AcceptanceGeneratedTracksForHeadless -MediaPath $m
    Sync-LegacySidecarsForHeadless -MediaPath $m
}

$args = @($headless, "--artifacts-dir", $ArtifactsDir, "--translations-dir", $TranslationsDir, "--output", $summaryRaw, "--capture-png")
foreach ($m in $Media) {
    $args += @("--media", $m)
}

& python @args
$headlessExit = $LASTEXITCODE

& python $reportWriter `
    --repo $Repo `
    --artifacts-dir $ArtifactsDir `
    --headless-json $summaryRaw `
    --backup $Backup `
    --exe $exe `
    --date "2026-07-09" `
    --build-deploy-json $buildDeployJson

if ($headlessExit -ne 0) {
    Write-Host "Headless acceptance failed. See $ArtifactsDir\ACCEPTANCE_REPORT.md and acceptance_summary.json"
    exit $headlessExit
}

Write-Host "Headless acceptance passed. See $ArtifactsDir\ACCEPTANCE_REPORT.md"
exit 0
