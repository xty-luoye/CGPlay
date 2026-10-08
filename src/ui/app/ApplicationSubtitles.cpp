#include "ApplicationInternal.h"

namespace cgplay {

void MainWindow::_loadGeneratedSubtitleTrack()
{
    _setupGeneratedSubtitleOverlay();
    _p->generatedSubtitleCues.clear();
    _p->generatedSubtitlePath.clear();
    _p->generatedSubtitleLastText.clear();
    _p->generatedSubtitleLastCueStartSeconds = -1.0;
    _p->generatedSubtitleLastCueEndSeconds = -1.0;
    _p->generatedSubtitleLastShownAtSeconds = -1.0;
    _p->generatedSubtitleQuickFallbackCueCount = 0;
    _p->generatedSubtitleRefinedMatchedCueCount = 0;
    _p->generatedSubtitleEnhancedMatchedCueCount = 0;
    _p->generatedSubtitleEnhancedLoaded = false;
    _p->generatedSubtitleEnhancedRejectedReason.clear();
    _p->generatedSubtitleCurrentCueFallbackSafe = true;
    _p->generatedSubtitleRefinedMerged = false;
    _p->generatedSubtitleEnhancedMerged = false;
    if (_p->generatedSubtitleLabel) {
        _p->generatedSubtitleLabel->hide();
        _p->generatedSubtitleLabel->clear();
    }

    if (_p->currentPath.trimmed().isEmpty()) {
        return;
    }

    const QString refinedVttPath = SubtitleGenerationService::defaultRefinedTranslatedVttPath(
        _p->currentPath,
        QStringLiteral("zh-Hans"));
    const QString refinedSrtPath = SubtitleGenerationService::defaultRefinedTranslatedSrtPath(
        _p->currentPath,
        QStringLiteral("zh-Hans"));
    const QString translatedVttPath = SubtitleGenerationService::defaultTranslatedVttPath(
        _p->currentPath,
        QStringLiteral("zh-Hans"));
    const QString translatedSrtPath = SubtitleGenerationService::defaultTranslatedSrtPath(
        _p->currentPath,
        QStringLiteral("zh-Hans"));
    const QString refinedZhVttPath =
        SubtitleGenerationService::defaultRefinedTranslatedVttPath(_p->currentPath, QStringLiteral("zh"));
    const QString refinedZhSrtPath =
        SubtitleGenerationService::defaultRefinedTranslatedSrtPath(_p->currentPath, QStringLiteral("zh"));
    const QString translatedZhVttPath =
        SubtitleGenerationService::defaultTranslatedVttPath(_p->currentPath, QStringLiteral("zh"));
    const QString translatedZhSrtPath =
        SubtitleGenerationService::defaultTranslatedSrtPath(_p->currentPath, QStringLiteral("zh"));
    const QStringList quickCandidates{
        translatedVttPath,
        translatedSrtPath,
        translatedZhVttPath,
        translatedZhSrtPath
    };
    const QStringList refinedCandidates{
        refinedVttPath,
        refinedSrtPath,
        refinedZhVttPath,
        refinedZhSrtPath
    };
    const QStringList enhancementCandidates{
        SubtitleGenerationService::defaultEnhancedTranslatedVttPath(_p->currentPath, QStringLiteral("zh-Hans")),
        SubtitleGenerationService::defaultEnhancedTranslatedVttPath(_p->currentPath, QStringLiteral("zh")),
        SubtitleGenerationService::defaultLowConfidenceRepairVttPath(_p->currentPath, QStringLiteral("zh-Hans")),
        SubtitleGenerationService::defaultLowConfidenceRepairVttPath(_p->currentPath, QStringLiteral("zh"))
    };
    const QString ocrSourceVttPath = SubtitleGenerationService::defaultOcrSubtitleCachePath(_p->currentPath);
    QString ocrTranslatedVttPath = ocrSourceVttPath;
    if (ocrTranslatedVttPath.endsWith(QStringLiteral(".ocr.source.vtt"))) {
        ocrTranslatedVttPath.chop(QStringLiteral(".ocr.source.vtt").size());
        ocrTranslatedVttPath += QStringLiteral(".ocr.translated.zh.vtt");
    } else if (!ocrTranslatedVttPath.trimmed().isEmpty()) {
        ocrTranslatedVttPath += QStringLiteral(".translated.zh.vtt");
    }
    QString onlineTranslatedVttPath = SubtitleGenerationService::defaultOnlineSubtitleCachePath(_p->currentPath);
    if (onlineTranslatedVttPath.endsWith(QStringLiteral(".online.source.vtt"))) {
        onlineTranslatedVttPath.chop(QStringLiteral(".online.source.vtt").size());
        onlineTranslatedVttPath += QStringLiteral(".online.translated.zh.vtt");
    } else if (!onlineTranslatedVttPath.trimmed().isEmpty()) {
        onlineTranslatedVttPath += QStringLiteral(".translated.zh.vtt");
    }
    const double fps = _p->playbackCtrl && _p->playbackCtrl->fps() > 0.0 ? _p->playbackCtrl->fps() : 24.0;
    const int currentFrame = _p->playbackCtrl ? _p->playbackCtrl->currentFrame() : 0;
    const int totalFrames = _p->playbackCtrl ? _p->playbackCtrl->totalFrames() : 0;
    const double currentSeconds = std::max(0, currentFrame) / fps;
    const double mediaDurationSeconds = totalFrames > 0 ? totalFrames / fps : 0.0;
    const QString currentMediaPath = _p->currentPath;
    const QString currentMediaFingerprint =
        SubtitleGenerationService::mediaFingerprint(currentMediaPath, mediaDurationSeconds);
    const QString companionSourceText = companionSubtitleSourceTextForMedia(currentMediaPath);
    const auto cacheValidForCurrentMedia = [currentMediaPath,
                                            currentMediaFingerprint,
                                            mediaDurationSeconds](const QString& path,
                                                                  const QString& role,
                                                                  QString* reasonOut = nullptr) {
        QString mismatchReason;
        QJsonObject recordedIdentity;
        const bool ok = generatedSubtitleCacheMatchesCurrentMedia(
            path,
            currentMediaPath,
            mediaDurationSeconds,
            &mismatchReason,
            &recordedIdentity);
        if (!ok) {
            if (reasonOut) {
                *reasonOut = mismatchReason;
            }
            qWarning() << "[SubtitleGeneration] Rejecting subtitle cache for media identity mismatch"
                       << "role=" << role
                       << "cache=" << QFileInfo(path).absoluteFilePath()
                       << "reason=" << mismatchReason
                       << "currentFingerprint=" << currentMediaFingerprint
                       << "recordedFingerprint="
                       << recordedIdentity.value(QStringLiteral("mediaFingerprint")).toString();
            return false;
        }
        return true;
    };
    const bool enhancedDisplayAllowed =
        _p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId();
    const bool allowCurrentSmokeTrack =
        qApp &&
        (qApp->property("cgplay.subtitleGenerationMockOnlineWorker").toBool() ||
         qApp->property("cgplay.subtitleGenerationMockOcrWorker").toBool() ||
         qApp->property("cgplay.subtitleGenerationMockRepairWorker").toBool());
    const auto hasCurrentOrUsableSourceCache =
        [currentSeconds, mediaDurationSeconds, allowCurrentSmokeTrack, cacheValidForCurrentMedia](const QString& path) {
            if (!QFileInfo::exists(path)) {
                return false;
            }
            if (!cacheValidForCurrentMedia(path, QStringLiteral("source-provenance"))) {
                return false;
            }
            const QVector<GeneratedSubtitleCue> cues = SubtitleGenerationService::readSubtitleFile(path);
            if (cues.isEmpty()) {
                return false;
            }
            if (generatedSubtitleHasCueNearSeconds(cues, currentSeconds, 5.0)) {
                return true;
            }
            QString unusedReason;
            return generatedSubtitleTrackUsable(
                cues,
                currentSeconds,
                mediaDurationSeconds,
                allowCurrentSmokeTrack,
                &unusedReason);
        };

    QString loadedQuickPath;
    for (const QString& candidate : quickCandidates) {
        if (!QFileInfo::exists(candidate)) {
            continue;
        }
        QString cacheMismatchReason;
        if (!cacheValidForCurrentMedia(candidate, QStringLiteral("quick"), &cacheMismatchReason)) {
            _p->generatedSubtitleEnhancedRejectedReason =
                QStringLiteral("quick-cache-media-identity-rejected:%1").arg(cacheMismatchReason);
            continue;
        }
        QVector<GeneratedSubtitleCue> cues = SubtitleGenerationService::readSubtitleFile(candidate);
        if (cues.isEmpty()) {
            continue;
        }
        QString leakReason;
        const int scrubbedLeaks =
            scrubGeneratedSubtitleCrossMediaLeaks(&cues, companionSourceText, QStringLiteral("quick"), &leakReason);
        if (scrubbedLeaks > 0) {
            _p->generatedSubtitleEnhancedRejectedReason =
                QStringLiteral("quick-cache-cross-media-leak-rejected:%1").arg(leakReason);
            qWarning() << "[SubtitleGeneration] Removed cross-media leaked quick subtitle cues"
                       << QFileInfo(candidate).absoluteFilePath()
                       << "removed=" << scrubbedLeaks
                       << "reason=" << leakReason;
        }
        QString unusableReason;
        if (!generatedSubtitleTrackUsable(cues, currentSeconds, mediaDurationSeconds, allowCurrentSmokeTrack, &unusableReason)) {
            qInfo() << "[SubtitleGeneration] Ignoring stale/incomplete subtitle track"
                    << QFileInfo(candidate).absoluteFilePath()
                    << unusableReason
                    << "cueCount=" << cues.size()
                    << "translatedEnd=" << generatedSubtitleTranslatedMaxEndSeconds(cues)
                    << "sourceEnd=" << generatedSubtitleMaxEndSeconds(cues)
                    << "current=" << currentSeconds
                    << "duration=" << mediaDurationSeconds;
            continue;
        }
        _p->generatedSubtitleCues = cues;
        for (auto& cue : _p->generatedSubtitleCues) {
            repairGeneratedSubtitleCueText(&cue);
        }
        loadedQuickPath = QFileInfo(candidate).absoluteFilePath();
        _p->generatedSubtitlePath = loadedQuickPath;
        _p->generatedSubtitleMediaFingerprint = currentMediaFingerprint;
        _p->generatedSubtitleCoverageEndSeconds =
            generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues);
        _p->generatedSubtitleProcessedEndSeconds = _p->generatedSubtitleCoverageEndSeconds;
        _p->generatedSubtitleQuickFallbackCueCount = _p->generatedSubtitleCues.size();
        const QFileInfo quickInfo(loadedQuickPath);
        for (const QString& refinedCandidate : refinedCandidates) {
            const QFileInfo refinedInfo(refinedCandidate);
            if (!refinedInfo.exists()) {
                continue;
            }
            QString refinedMismatchReason;
            if (!cacheValidForCurrentMedia(refinedCandidate, QStringLiteral("refined"), &refinedMismatchReason)) {
                _p->generatedSubtitleEnhancedRejectedReason =
                    QStringLiteral("refined-cache-media-identity-rejected:%1").arg(refinedMismatchReason);
                continue;
            }
            if (quickInfo.exists() &&
                refinedInfo.lastModified() < quickInfo.lastModified()) {
                qInfo() << "[SubtitleGeneration] Ignoring older refined subtitle cache; quick track stays baseline"
                        << refinedInfo.absoluteFilePath()
                        << "quick=" << loadedQuickPath;
                continue;
            }
            const QVector<GeneratedSubtitleCue> refinedCues =
                SubtitleGenerationService::readSubtitleFile(refinedCandidate);
            QVector<GeneratedSubtitleCue> scrubbedRefinedCues = refinedCues;
            QString refinedLeakReason;
            const int refinedLeakCount = scrubGeneratedSubtitleCrossMediaLeaks(
                &scrubbedRefinedCues,
                companionSourceText,
                QStringLiteral("refined"),
                &refinedLeakReason);
            if (refinedLeakCount > 0) {
                qWarning() << "[SubtitleGeneration] Removed cross-media leaked refined subtitle cues"
                           << refinedInfo.absoluteFilePath()
                           << "removed=" << refinedLeakCount
                           << "reason=" << refinedLeakReason;
            }
            if (scrubbedRefinedCues.isEmpty()) {
                continue;
            }
            const double quickCoverage =
                generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues);
            const double refinedCoverage =
                generatedSubtitleTranslatedMaxEndSeconds(scrubbedRefinedCues);
            int matchedCueCount = 0;
            const int changedCount =
                mergeGeneratedSubtitleRefinedEnhancements(
                    &_p->generatedSubtitleCues,
                    scrubbedRefinedCues,
                    &matchedCueCount);
            qInfo() << "[SubtitleGeneration] Merged refined subtitle cache as cue-level enhancement"
                    << refinedInfo.absoluteFilePath()
                    << "matched=" << matchedCueCount
                    << "changed=" << changedCount
                    << "quickCueCount=" << _p->generatedSubtitleCues.size()
                    << "refinedCueCount=" << scrubbedRefinedCues.size()
                    << "quickCoverage=" << quickCoverage
                    << "refinedCoverage=" << refinedCoverage;
            _p->generatedSubtitleRefinedMatchedCueCount = matchedCueCount;
            _p->generatedSubtitleRefinedMerged = changedCount > 0;
            _p->generatedSubtitleQuickFallbackCueCount =
                std::max(0, static_cast<int>(_p->generatedSubtitleCues.size()) - matchedCueCount);
            _p->generatedSubtitleCoverageEndSeconds =
                generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues);
            break;
        }
        if (!enhancedDisplayAllowed) {
            _p->generatedSubtitleEnhancedRejectedReason =
                QStringLiteral("disabled-in-quick-playback");
            qInfo() << "[SubtitleGeneration] Enhanced subtitle display disabled in quick-playback; quick track stays baseline"
                    << loadedQuickPath;
        } else {
            bool sawEnhancementCandidate = false;
            for (const QString& enhancementCandidate : enhancementCandidates) {
                const QFileInfo enhancementInfo(enhancementCandidate);
                if (!enhancementInfo.exists()) {
                    continue;
                }
                QString enhancementMismatchReason;
                if (!cacheValidForCurrentMedia(enhancementCandidate, QStringLiteral("enhanced"), &enhancementMismatchReason)) {
                    _p->generatedSubtitleEnhancedRejectedReason =
                        QStringLiteral("enhanced-cache-media-identity-rejected:%1").arg(enhancementMismatchReason);
                    continue;
                }
                sawEnhancementCandidate = true;
                const QVector<GeneratedSubtitleCue> enhancementCues =
                    SubtitleGenerationService::readSubtitleFile(enhancementCandidate);
                QVector<GeneratedSubtitleCue> scrubbedEnhancementCues = enhancementCues;
                QString enhancementLeakReason;
                const int enhancementLeakCount = scrubGeneratedSubtitleCrossMediaLeaks(
                    &scrubbedEnhancementCues,
                    companionSourceText,
                    QStringLiteral("enhanced"),
                    &enhancementLeakReason);
                if (enhancementLeakCount > 0) {
                    qWarning() << "[SubtitleGeneration] Removed cross-media leaked enhanced subtitle cues"
                               << enhancementInfo.absoluteFilePath()
                               << "removed=" << enhancementLeakCount
                               << "reason=" << enhancementLeakReason;
                }
                if (scrubbedEnhancementCues.isEmpty()) {
                    _p->generatedSubtitleEnhancedRejectedReason =
                        QStringLiteral("empty-enhanced-cache");
                    continue;
                }
                const bool olderThanQuick =
                    quickInfo.exists() &&
                    enhancementInfo.lastModified().msecsTo(quickInfo.lastModified()) > 2000;
                bool acceptedOlderVisualCue = false;
                if (olderThanQuick) {
                    const bool enhancementHasCurrentCue =
                        enhancementCandidate.contains(QStringLiteral(".enhanced.")) &&
                        generatedSubtitleHasCueNearSeconds(scrubbedEnhancementCues, currentSeconds, 5.0);
                    if (enhancementHasCurrentCue) {
                        const QVector<GeneratedSubtitleCue> ocrTranslatedCues =
                            SubtitleGenerationService::readSubtitleFile(ocrTranslatedVttPath);
                        acceptedOlderVisualCue =
                            generatedSubtitleHasCueNearSeconds(ocrTranslatedCues, currentSeconds, 5.0);
                    }
                }
                if (olderThanQuick && !acceptedOlderVisualCue) {
                    _p->generatedSubtitleEnhancedRejectedReason =
                        QStringLiteral("older-than-quick-baseline");
                    qInfo() << "[SubtitleGeneration] Ignoring older enhanced subtitle cache; quick track stays baseline"
                            << enhancementInfo.absoluteFilePath()
                            << "quick=" << loadedQuickPath;
                    continue;
                }
                const bool isFusionEnhancedCache =
                    enhancementCandidate.contains(QStringLiteral(".enhanced."), Qt::CaseInsensitive);
                if (isFusionEnhancedCache) {
                    const bool hasNonAsrHqEvidence =
                        hasCurrentOrUsableSourceCache(ocrTranslatedVttPath) ||
                        hasCurrentOrUsableSourceCache(onlineTranslatedVttPath) ||
                        !localReferenceSubtitlePathForMedia(_p->currentPath).trimmed().isEmpty();
                    if (!hasNonAsrHqEvidence) {
                        _p->generatedSubtitleEnhancedRejectedReason =
                            QStringLiteral("enhanced-source-provenance-missing");
                        qInfo() << "[SubtitleGeneration] Ignoring enhanced subtitle cache without non-ASR source evidence; quick track stays baseline"
                                << enhancementInfo.absoluteFilePath()
                                << "quick=" << loadedQuickPath;
                        continue;
                    }
                }
                int matchedCueCount = 0;
                const int changedCount =
                    mergeGeneratedSubtitleRefinedEnhancements(
                        &_p->generatedSubtitleCues,
                        scrubbedEnhancementCues,
                        &matchedCueCount);
                if (matchedCueCount <= 0) {
                    _p->generatedSubtitleEnhancedRejectedReason =
                        QStringLiteral("no-matching-enhanced-cues");
                    qInfo() << "[SubtitleGeneration] Ignoring unaligned enhanced subtitle cache; quick track stays baseline"
                            << enhancementInfo.absoluteFilePath()
                            << "quickCueCount=" << _p->generatedSubtitleCues.size()
                            << "enhancementCueCount=" << scrubbedEnhancementCues.size();
                    continue;
                }
                qInfo() << "[SubtitleGeneration] Merged enhanced subtitle cache as cue-level enhancement"
                        << enhancementInfo.absoluteFilePath()
                        << "matched=" << matchedCueCount
                        << "changed=" << changedCount
                        << "quickCueCount=" << _p->generatedSubtitleCues.size()
                        << "enhancementCueCount=" << scrubbedEnhancementCues.size();
                _p->generatedSubtitleEnhancedMatchedCueCount = matchedCueCount;
                _p->generatedSubtitleEnhancedLoaded = true;
                _p->generatedSubtitleEnhancedMerged = changedCount > 0;
                if (acceptedOlderVisualCue) {
                    _p->generatedSubtitleEnhancedRejectedReason =
                        QStringLiteral("older-enhanced-current-visual-cue-accepted-per-cue");
                } else {
                    _p->generatedSubtitleEnhancedRejectedReason =
                        matchedCueCount == _p->generatedSubtitleCues.size()
                        ? QStringLiteral("accepted-per-cue")
                        : QStringLiteral("partial-enhanced-cache-cue-level-fallback");
                }
                _p->generatedSubtitleQuickFallbackCueCount =
                    std::max(0, static_cast<int>(_p->generatedSubtitleCues.size()) - matchedCueCount);
                _p->generatedSubtitleCoverageEndSeconds =
                    generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues);
                break;
            }
            if (!sawEnhancementCandidate &&
                _p->generatedSubtitleEnhancedRejectedReason.trimmed().isEmpty()) {
                _p->generatedSubtitleEnhancedRejectedReason =
                    QStringLiteral("no-enhanced-cache");
            }
        }
        _p->generatedSubtitleCurrentCueFallbackSafe =
            !_p->generatedSubtitlePath.contains(QStringLiteral(".enhanced.")) &&
            !_p->generatedSubtitlePath.contains(QStringLiteral(".repair.")) &&
            _p->generatedSubtitleQuickFallbackCueCount >= 0;
        _publishTranslationPlaybackStrategyDiagnostics(
            _p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId());
        _updateGeneratedSubtitleForFrame(_p->playbackCtrl ? _p->playbackCtrl->currentFrame() : 0);
        if (statusBar()) {
            statusBar()->showMessage(QStringLiteral("已加载字幕：%1").arg(QFileInfo(loadedQuickPath).fileName()), 3000);
        }
        return;
    }
    if (enhancedDisplayAllowed) {
        bool visualTrackAvailable = false;
        QString visualTrackPath;
        QVector<GeneratedSubtitleCue> visualTrackCues;
        const QStringList visualFirstCandidates{
            ocrTranslatedVttPath,
            onlineTranslatedVttPath,
            ocrSourceVttPath
        };
        for (const QString& candidate : visualFirstCandidates) {
            if (!QFileInfo::exists(candidate)) {
                continue;
            }
            QString visualMismatchReason;
            if (!cacheValidForCurrentMedia(candidate, QStringLiteral("visual-first"), &visualMismatchReason)) {
                _p->generatedSubtitleEnhancedRejectedReason =
                    QStringLiteral("visual-cache-media-identity-rejected:%1").arg(visualMismatchReason);
                continue;
            }
            const QVector<GeneratedSubtitleCue> candidateCues =
                SubtitleGenerationService::readSubtitleFile(candidate);
            QVector<GeneratedSubtitleCue> scrubbedCandidateCues = candidateCues;
            QString visualLeakReason;
            const int visualLeakCount = scrubGeneratedSubtitleCrossMediaLeaks(
                &scrubbedCandidateCues,
                companionSourceText,
                QStringLiteral("visual-first"),
                &visualLeakReason);
            if (visualLeakCount > 0) {
                qWarning() << "[SubtitleGeneration] Removed cross-media leaked visual subtitle cues"
                           << QFileInfo(candidate).absoluteFilePath()
                           << "removed=" << visualLeakCount
                           << "reason=" << visualLeakReason;
            }
            const bool candidateIsOcrSource =
                QFileInfo(candidate).absoluteFilePath() == QFileInfo(ocrSourceVttPath).absoluteFilePath();
            if (candidateIsOcrSource) {
                QVector<GeneratedSubtitleCue> literalCjkCues;
                literalCjkCues.reserve(scrubbedCandidateCues.size());
                for (GeneratedSubtitleCue cue : scrubbedCandidateCues) {
                    const QString sourceText = !cue.sourceText.trimmed().isEmpty()
                        ? cue.sourceText.trimmed()
                        : cue.translatedText.trimmed();
                    if (!generatedSubtitleContainsHan(sourceText)) {
                        continue;
                    }
                    cue.sourceText = sourceText;
                    cue.translatedText = generatedSubtitleNormalizeChineseLiteralForZhHans(sourceText);
                    literalCjkCues.push_back(cue);
                }
                scrubbedCandidateCues = literalCjkCues;
            }
            if (!scrubbedCandidateCues.isEmpty()) {
                visualTrackAvailable = true;
                visualTrackPath = QFileInfo(candidate).absoluteFilePath();
                visualTrackCues = scrubbedCandidateCues;
                break;
            }
        }
        for (const QString& candidate : enhancementCandidates) {
            if (visualTrackAvailable) {
                break;
            }
            if (candidate.contains(QStringLiteral(".enhanced."), Qt::CaseInsensitive) &&
                !hasCurrentOrUsableSourceCache(ocrTranslatedVttPath) &&
                !hasCurrentOrUsableSourceCache(onlineTranslatedVttPath) &&
                localReferenceSubtitlePathForMedia(_p->currentPath).trimmed().isEmpty()) {
                continue;
            }
            if (!QFileInfo::exists(candidate)) {
                continue;
            }
            QString enhancementMismatchReason;
            if (!cacheValidForCurrentMedia(candidate, QStringLiteral("visual-enhanced"), &enhancementMismatchReason)) {
                _p->generatedSubtitleEnhancedRejectedReason =
                    QStringLiteral("visual-enhanced-cache-media-identity-rejected:%1").arg(enhancementMismatchReason);
                continue;
            }
            const QVector<GeneratedSubtitleCue> candidateCues =
                SubtitleGenerationService::readSubtitleFile(candidate);
            QVector<GeneratedSubtitleCue> scrubbedCandidateCues = candidateCues;
            QString enhancedVisualLeakReason;
            const int enhancedVisualLeakCount = scrubGeneratedSubtitleCrossMediaLeaks(
                &scrubbedCandidateCues,
                companionSourceText,
                QStringLiteral("visual-enhanced"),
                &enhancedVisualLeakReason);
            if (enhancedVisualLeakCount > 0) {
                qWarning() << "[SubtitleGeneration] Removed cross-media leaked visual enhanced cues"
                           << QFileInfo(candidate).absoluteFilePath()
                           << "removed=" << enhancedVisualLeakCount
                           << "reason=" << enhancedVisualLeakReason;
            }
            if (!scrubbedCandidateCues.isEmpty()) {
                visualTrackAvailable = true;
                visualTrackPath = QFileInfo(candidate).absoluteFilePath();
                visualTrackCues = scrubbedCandidateCues;
                break;
            }
        }
        if (!visualTrackAvailable && QFileInfo::exists(ocrTranslatedVttPath) &&
            cacheValidForCurrentMedia(ocrTranslatedVttPath, QStringLiteral("ocr-translated"))) {
            const QVector<GeneratedSubtitleCue> ocrTranslatedCues =
                SubtitleGenerationService::readSubtitleFile(ocrTranslatedVttPath);
            QVector<GeneratedSubtitleCue> scrubbedOcrTranslatedCues = ocrTranslatedCues;
            QString ocrLeakReason;
            const int ocrLeakCount = scrubGeneratedSubtitleCrossMediaLeaks(
                &scrubbedOcrTranslatedCues,
                companionSourceText,
                QStringLiteral("ocr-translated"),
                &ocrLeakReason);
            if (ocrLeakCount > 0) {
                qWarning() << "[SubtitleGeneration] Removed cross-media leaked OCR translated cues"
                           << QFileInfo(ocrTranslatedVttPath).absoluteFilePath()
                           << "removed=" << ocrLeakCount
                           << "reason=" << ocrLeakReason;
            }
            if (!scrubbedOcrTranslatedCues.isEmpty()) {
                visualTrackAvailable = true;
                visualTrackPath = QFileInfo(ocrTranslatedVttPath).absoluteFilePath();
                visualTrackCues = scrubbedOcrTranslatedCues;
            }
        }
        if (visualTrackAvailable) {
            if (_p->generatedSubtitleCues.isEmpty() && !visualTrackCues.isEmpty()) {
                _p->generatedSubtitleCues = visualTrackCues;
                for (auto& cue : _p->generatedSubtitleCues) {
                    repairGeneratedSubtitleCueText(&cue);
                }
                _p->generatedSubtitlePath = visualTrackPath;
                _p->generatedSubtitleMediaFingerprint = currentMediaFingerprint;
                _p->generatedSubtitleCoverageEndSeconds =
                    generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues);
                _p->generatedSubtitleProcessedEndSeconds = _p->generatedSubtitleCoverageEndSeconds;
                _p->generatedSubtitleQuickFallbackCueCount = 0;
            }
            _p->generatedSubtitleEnhancedLoaded = true;
            _p->generatedSubtitleEnhancedRejectedReason =
                QStringLiteral("visual-track-loaded-without-quick-baseline");
            _p->generatedSubtitleCurrentCueFallbackSafe = true;
            _publishTranslationPlaybackStrategyDiagnostics(true);
            _updateGeneratedSubtitleForFrame(_p->playbackCtrl ? _p->playbackCtrl->currentFrame() : 0);
        }
    }
}

void MainWindow::_updateGeneratedSubtitleForFrame(int frame)
{
    if (!_p->generatedSubtitleLabel || !_p->playbackCtrl) {
        return;
    }

    const auto clearGeneratedSubtitleOverlay = [this] {
        _p->generatedSubtitleLastText.clear();
        _p->generatedSubtitleLastCueStartSeconds = -1.0;
        _p->generatedSubtitleLastCueEndSeconds = -1.0;
        _p->generatedSubtitleLastShownAtSeconds = -1.0;
        _p->generatedSubtitleLabel->clear();
        _p->generatedSubtitleLabel->hide();
    };

    if (!_p->generatedSubtitleMediaFingerprint.trimmed().isEmpty() &&
        !_p->currentPath.trimmed().isEmpty()) {
        const double guardFps = _p->playbackCtrl->fps() > 0.0 ? _p->playbackCtrl->fps() : 24.0;
        const double guardDurationSeconds =
            _p->playbackCtrl->totalFrames() > 0 ? _p->playbackCtrl->totalFrames() / guardFps : 0.0;
        const QString currentFingerprint =
            SubtitleGenerationService::mediaFingerprint(_p->currentPath, guardDurationSeconds);
        if (_p->generatedSubtitleMediaFingerprint != currentFingerprint) {
            qWarning() << "[SubtitleGeneration] Clearing generated subtitle track with stale media fingerprint"
                       << "trackFingerprint=" << _p->generatedSubtitleMediaFingerprint
                       << "currentFingerprint=" << currentFingerprint
                       << "path=" << _p->generatedSubtitlePath;
            _p->generatedSubtitleCues.clear();
            _p->generatedSubtitlePath.clear();
            _p->generatedSubtitleMediaFingerprint.clear();
            clearGeneratedSubtitleOverlay();
            return;
        }
    }

    if (_p->playbackBar && !_p->playbackBar->translationVisible()) {
        clearGeneratedSubtitleOverlay();
        return;
    }

    if (_p->translationStripVisible) {
        clearGeneratedSubtitleOverlay();
        return;
    }

    const double fps = _p->playbackCtrl->fps() > 0.0 ? _p->playbackCtrl->fps() : 24.0;
    const double seconds = std::max(0, frame) / fps;
    _scheduleHighQualityVisualContinuation();
    const QJsonObject playbackStrategyDiagnostics =
        _translationPlaybackStrategyDiagnostics(
            _p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId());
    const QJsonObject currentMediaHighQuality =
        playbackStrategyDiagnostics.value(QStringLiteral("currentMediaHighQuality")).toObject();
    const bool visualNoTextSuppressQuickAsr =
        currentMediaHighQuality.value(QStringLiteral("visualNoTextSuppressQuickAsr")).toBool(false);
    const bool visualTrackAuthoritative =
        currentMediaHighQuality.value(QStringLiteral("visualTrackAuthoritative")).toBool(false);
    const bool visualTrackAuthoritativeAtTime =
        currentMediaHighQuality.value(QStringLiteral("visualTrackAuthoritativeAtTime")).toBool(
            visualTrackAuthoritative &&
            currentMediaHighQuality.value(QStringLiteral("currentTimeWithinVisualCoverage")).toBool(false));
    const QJsonObject visualPrecheck =
        currentMediaHighQuality.value(QStringLiteral("visualPrecheck")).toObject();
    const QString visualTextDetectedString =
        currentMediaHighQuality.value(QStringLiteral("visualTextDetected")).toString();
    const bool visualTextDetected =
        currentMediaHighQuality.value(QStringLiteral("visualCueAtTime")).toBool(false) ||
        currentMediaHighQuality.value(QStringLiteral("visualSourceCueAtTime")).toBool(false) ||
        visualPrecheck.value(QStringLiteral("visualTextDetectedBool")).toBool(false) ||
        visualTextDetectedString.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
    const bool highQualityVisualCueAtTime =
        _p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId() &&
        currentMediaHighQuality.value(QStringLiteral("visualCueAtTime")).toBool(false);
    const QString highQualityFinalDisplayedText =
        currentMediaHighQuality.value(QStringLiteral("finalDisplayedText")).toString().trimmed();
    const QString highQualityQuickDisplayedText =
        currentMediaHighQuality.value(QStringLiteral("quickDisplayedText")).toString().trimmed();
    if (_p->playbackBar &&
        _p->playbackBar->translationVisible() &&
        _p->currentPath.trimmed().isEmpty() == false) {
        const QString partialPath = SubtitleGenerationService::defaultTranslatedVttPath(
            _p->currentPath,
            QStringLiteral("zh-Hans"));
        if (QFileInfo::exists(partialPath)) {
            QString partialMismatchReason;
            if (!generatedSubtitleCacheMatchesCurrentMedia(
                    partialPath,
                    _p->currentPath,
                    0.0,
                    &partialMismatchReason)) {
                qWarning() << "[SubtitleGeneration] Ignoring frame-refresh subtitle cache from non-current media"
                           << QFileInfo(partialPath).absoluteFilePath()
                           << partialMismatchReason;
            } else {
                const QVector<GeneratedSubtitleCue> partialCues =
                    SubtitleGenerationService::readSubtitleFile(partialPath);
                QVector<GeneratedSubtitleCue> scrubbedPartialCues = partialCues;
                QString partialLeakReason;
                const int partialLeakCount = scrubGeneratedSubtitleCrossMediaLeaks(
                    &scrubbedPartialCues,
                    companionSubtitleSourceTextForMedia(_p->currentPath),
                    QStringLiteral("frame-refresh"),
                    &partialLeakReason);
                if (partialLeakCount > 0) {
                    qWarning() << "[SubtitleGeneration] Removed cross-media leaked frame-refresh cues"
                               << QFileInfo(partialPath).absoluteFilePath()
                               << "removed=" << partialLeakCount
                               << "reason=" << partialLeakReason;
                }
                if (!scrubbedPartialCues.isEmpty()) {
                    const bool preserveHighQualityCueText =
                        _p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId() &&
                        _p->generatedSubtitleEnhancedLoaded;
                    mergeGeneratedSubtitleCues(
                        &_p->generatedSubtitleCues,
                        scrubbedPartialCues,
                        preserveHighQualityCueText);
                    if (!preserveHighQualityCueText) {
                        _p->generatedSubtitlePath = QFileInfo(partialPath).absoluteFilePath();
                        _p->generatedSubtitleMediaFingerprint =
                            SubtitleGenerationService::mediaFingerprint(_p->currentPath);
                    }
                }
            }
        }
    }
    if (_p->generatedSubtitleCues.isEmpty() &&
        !(highQualityVisualCueAtTime && !highQualityFinalDisplayedText.isEmpty())) {
        if (_p->playbackBar &&
            _p->playbackBar->translationVisible() &&
            !visualTrackAuthoritativeAtTime &&
            !_p->subtitleGenerationBusy) {
            _scheduleGeneratedSubtitleContinuation(
                std::max(0.0, seconds - kSubtitleContinuationOverlapSeconds),
                0);
        }
        clearGeneratedSubtitleOverlay();
        return;
    }
    _p->generatedSubtitleCoverageEndSeconds =
        generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues);
    const double coverageEndSeconds = _p->generatedSubtitleCoverageEndSeconds;
    _publishTranslationPlaybackStrategyDiagnostics(
        _p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId());
    const bool suppressQuickAsrForVisualTrack =
        visualNoTextSuppressQuickAsr ||
        (visualTrackAuthoritativeAtTime && !highQualityVisualCueAtTime) ||
        (visualTrackAuthoritativeAtTime &&
         highQualityVisualCueAtTime &&
         highQualityFinalDisplayedText.isEmpty() &&
         highQualityQuickDisplayedText.isEmpty());
    if (suppressQuickAsrForVisualTrack) {
        clearGeneratedSubtitleOverlay();
        return;
    }
    if (_p->playbackBar &&
        _p->playbackBar->translationVisible() &&
        coverageEndSeconds > 0.0 &&
        seconds >= coverageEndSeconds - kSubtitlePrefetchLeadSeconds) {
        _scheduleGeneratedSubtitleContinuation(std::max(0.0, coverageEndSeconds - kSubtitleContinuationOverlapSeconds), 0);
    }
    QString text;
    double activeCueStartSeconds = -1.0;
    double activeCueEndSeconds = -1.0;
    double nextCueStartSeconds = -1.0;
    if (highQualityVisualCueAtTime && !highQualityFinalDisplayedText.isEmpty()) {
        text = repairGeneratedSubtitleMojibake(highQualityFinalDisplayedText);
        activeCueStartSeconds = std::max(0.0, seconds - 0.02);
        activeCueEndSeconds = seconds + std::max(kGeneratedSubtitleHoldSeconds, 1.0);
    } else {
        for (const auto& cue : _p->generatedSubtitleCues) {
            if (generatedSubtitleCueHasDisplayableTranslation(cue) &&
                seconds + 0.02 >= cue.startSeconds && seconds <= cue.endSeconds + 0.02) {
                text = cue.translatedText.trimmed();
                text = repairGeneratedSubtitleMojibake(text);
                activeCueStartSeconds = cue.startSeconds;
                activeCueEndSeconds = cue.endSeconds;
                break;
            }
            if (generatedSubtitleCueHasDisplayableTranslation(cue) &&
                cue.startSeconds > seconds &&
                (nextCueStartSeconds < 0.0 || cue.startSeconds < nextCueStartSeconds)) {
                nextCueStartSeconds = cue.startSeconds;
            }
        }
        if (text.isEmpty() &&
            _p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId() &&
            !visualTrackAuthoritativeAtTime &&
            !highQualityQuickDisplayedText.isEmpty()) {
            text = repairGeneratedSubtitleMojibake(highQualityQuickDisplayedText);
            activeCueStartSeconds =
                currentMediaHighQuality.value(QStringLiteral("quickCueStart")).toDouble(std::max(0.0, seconds - 0.02));
            activeCueEndSeconds =
                currentMediaHighQuality.value(QStringLiteral("quickCueEnd")).toDouble(seconds + std::max(kGeneratedSubtitleHoldSeconds, 1.0));
        }
    }

    if (text.isEmpty()) {
        if (_p->playbackBar &&
            _p->playbackBar->translationVisible() &&
            !_p->subtitleGenerationBusy) {
            _scheduleGeneratedSubtitleContinuation(
                std::max(0.0, seconds - kSubtitleContinuationOverlapSeconds),
                0);
        }
        const bool subtitleWorkPending =
            _p->subtitleGenerationBusy ||
            _p->subtitleContinuationScheduled ||
            _p->subtitleContinuationQueuedWhileBusy ||
            seconds >= coverageEndSeconds - 1.0;
        if (subtitleWorkPending && coverageEndSeconds > 0.0) {
            const QString sourceSrtPath = SubtitleGenerationService::defaultSourceSrtPath(_p->currentPath);
            const QString sourceVttPath = QFileInfo(sourceSrtPath).absolutePath() + QLatin1Char('/') +
                QFileInfo(sourceSrtPath).completeBaseName() + QStringLiteral(".vtt");
            const QVector<GeneratedSubtitleCue> sourceCues =
                QFileInfo::exists(sourceVttPath)
                    ? SubtitleGenerationService::readSubtitleFile(sourceVttPath)
                    : QVector<GeneratedSubtitleCue>();
            const bool sourceDialogueNearby =
                !sourceCues.isEmpty() &&
                generatedSubtitleHasCueNearSeconds(sourceCues, seconds, 0.35);
            if (sourceDialogueNearby && statusBar()) {
                statusBar()->showMessage(QString::fromUtf8(u8"后续字幕生成中..."), 1200);
            }
        }
        clearGeneratedSubtitleOverlay();
        return;
    }
    QWidget* host = _p->viewer ? _p->viewer->overlayParentWidget() : nullptr;
    if (!host) {
        host = _p->viewer;
    }
    const int visibleMaxWidth = host ? std::max(80, host->width() - 44 - 16) : 720;
    const QString visibleText = generatedSubtitleVisibleDisplayText(
        text,
        QFontMetrics(_p->generatedSubtitleLabel->font()),
        visibleMaxWidth);
    if (visibleText.isEmpty()) {
        clearGeneratedSubtitleOverlay();
        return;
    }
    QString finalLeakReason;
    const QString companionSourceText = companionSubtitleSourceTextForMedia(_p->currentPath);
    if (generatedSubtitleTextLooksCrossMediaLeaked(text, companionSourceText, &finalLeakReason) ||
        generatedSubtitleTextLooksLikeRawEnglishFinal(text)) {
        qWarning() << "[SubtitleGeneration] Suppressing invalid final generated subtitle text"
                   << "reason=" << (finalLeakReason.isEmpty() ? QStringLiteral("raw-english-final") : finalLeakReason)
                   << "media=" << _p->currentPath
                   << "text=" << text.left(120);
        clearGeneratedSubtitleOverlay();
        return;
    }
    _p->generatedSubtitleLastText = text;
    _p->generatedSubtitleLastCueStartSeconds = activeCueStartSeconds;
    _p->generatedSubtitleLastCueEndSeconds = activeCueEndSeconds;
    _p->generatedSubtitleLastShownAtSeconds = seconds;
    _p->generatedSubtitleLabel->setText(visibleText);
    _layoutGeneratedSubtitleOverlay();
    _p->generatedSubtitleLabel->show();
    _p->generatedSubtitleLabel->raise();
}

void MainWindow::_scheduleGeneratedSubtitleContinuation(double preferredStartSeconds, int delayMs)
{
    if (!_p->playbackBar || !_p->playbackBar->translationVisible()) {
        return;
    }
    if (_p->currentPath.trimmed().isEmpty()) {
        return;
    }
    const double fps = _p->playbackCtrl && _p->playbackCtrl->fps() > 0.0 ? _p->playbackCtrl->fps() : 24.0;
    const double currentSeconds = _p->playbackCtrl
        ? std::max(0, _p->playbackCtrl->currentFrame()) / fps
        : 0.0;
    _p->generatedSubtitleCoverageEndSeconds =
        generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues);
    const double coverageEndSeconds = _p->generatedSubtitleCoverageEndSeconds;
    const double mediaDurationSeconds = _p->playbackCtrl && _p->playbackCtrl->totalFrames() > 0
        ? _p->playbackCtrl->totalFrames() / fps
        : 0.0;
    if (mediaDurationSeconds > 0.0 && coverageEndSeconds >= mediaDurationSeconds - 2.0) {
        return;
    }

    double startSeconds = preferredStartSeconds >= 0.0 ? preferredStartSeconds : coverageEndSeconds;
    if (mediaDurationSeconds > 0.0 && startSeconds >= mediaDurationSeconds - 1.0) {
        return;
    }

    if (_p->subtitleGenerationBusy) {
        if (!_p->subtitleContinuationQueuedWhileBusy ||
            _p->subtitleContinuationQueuedStartSeconds < 0.0 ||
            startSeconds < _p->subtitleContinuationQueuedStartSeconds) {
            _p->subtitleContinuationQueuedWhileBusy = true;
            _p->subtitleContinuationQueuedStartSeconds = startSeconds;
        }
        return;
    }

    if (_p->subtitleContinuationScheduled) {
        if (_p->subtitleContinuationStartSeconds < 0.0 ||
            (startSeconds > currentSeconds - 6.0 && startSeconds < _p->subtitleContinuationStartSeconds)) {
            _p->subtitleContinuationStartSeconds = startSeconds;
        }
        return;
    }

    _p->subtitleContinuationScheduled = true;
    _p->subtitleContinuationStartSeconds = startSeconds;
    QTimer::singleShot(std::max(0, delayMs), this, [this]() {
        const double startSeconds = _p->subtitleContinuationStartSeconds;
        _p->subtitleContinuationScheduled = false;
        _p->subtitleContinuationStartSeconds = -1.0;
        _p->subtitleContinuationRetryCount = 0;
        if (_p->playbackBar &&
            _p->playbackBar->translationVisible() &&
            !_p->subtitleGenerationBusy &&
            startSeconds >= 0.0) {
            _generateSubtitlesForCurrentMedia(startSeconds, true);
        }
    });
}

void MainWindow::_scheduleGeneratedSubtitleRefinement(
    const QString& mediaPath,
    const QVector<GeneratedSubtitleCue>& cues,
    bool mockWithoutApi)
{
    if (mediaPath.trimmed().isEmpty() || cues.isEmpty() || _p->subtitleRefinementBusy) {
        if (qApp) {
            qApp->setProperty("cgplay.subtitleRefinementDecision", QStringLiteral("skip-empty-or-busy"));
        }
        return;
    }
    if (QFileInfo(_p->currentPath).absoluteFilePath() != QFileInfo(mediaPath).absoluteFilePath()) {
        if (qApp) {
            qApp->setProperty("cgplay.subtitleRefinementDecision", QStringLiteral("skip-media-changed"));
        }
        return;
    }
    const bool refinementEnabled = mockWithoutApi ||
        (_p->userSettings &&
         _p->userSettings->value(QStringLiteral("ai/subtitles/refine/enabled"), false).toBool());
    if (!refinementEnabled) {
        if (qApp) {
            qApp->setProperty("cgplay.subtitleRefinementDecision", QStringLiteral("skip-disabled"));
        }
        return;
    }
    const bool dedicatedWorkspaceRefinement =
        hasDedicatedSubtitleRefinementWorkspace(_p->userSettings);
    const double cueCoverageSeconds = generatedSubtitleTranslatedMaxEndSeconds(cues);
    const double cueStartSeconds = cues.isEmpty() ? 0.0 : cues.first().startSeconds;
    const double minimumRefinementCoverageSeconds =
        dedicatedWorkspaceRefinement ? 18.0 : 60.0;
    if (!mockWithoutApi && cueCoverageSeconds - cueStartSeconds < minimumRefinementCoverageSeconds) {
        if (qApp) {
            qApp->setProperty("cgplay.subtitleRefinementDecision", QStringLiteral("skip-coverage-too-short"));
        }
        return;
    }
    if (!mockWithoutApi &&
        cueCoverageSeconds <= _p->subtitleRefinementLastCoverageEndSeconds + 45.0) {
        if (qApp) {
            qApp->setProperty("cgplay.subtitleRefinementDecision", QStringLiteral("skip-coverage-not-advanced"));
        }
        return;
    }
    const double currentSeconds = generatedSubtitleCurrentSeconds(_p->playbackCtrl.get());
    const bool translationActive = _p->playbackBar && _p->playbackBar->translationVisible();
    const bool fastSubtitlePathBusy =
        _p->subtitleGenerationBusy ||
        _p->subtitleContinuationScheduled ||
        _p->subtitleContinuationQueuedWhileBusy;
    const bool cacheIsThin =
        translationActive &&
        cueCoverageSeconds - currentSeconds < kSubtitlePrefetchLeadSeconds;
    if (!mockWithoutApi &&
        !dedicatedWorkspaceRefinement &&
        (fastSubtitlePathBusy || cacheIsThin)) {
        qInfo() << "[SubtitleRefinement] Delaying background refinement"
                << "fastBusy=" << fastSubtitlePathBusy
                << "coverageAhead=" << (cueCoverageSeconds - currentSeconds)
                << "cueCount=" << cues.size();
        if (qApp) {
            qApp->setProperty("cgplay.subtitleRefinementDecision", QStringLiteral("delay-fast-path"));
        }
        QTimer::singleShot(kSubtitleRefinementIdleRetryMs, this, [this, mediaPath]() {
            if (!_p->subtitleRefinementBusy &&
                QFileInfo(_p->currentPath).absoluteFilePath() == QFileInfo(mediaPath).absoluteFilePath() &&
                !_p->generatedSubtitleCues.isEmpty()) {
                _scheduleGeneratedSubtitleRefinement(mediaPath, _p->generatedSubtitleCues, false);
            }
        });
        return;
    }

    auto* providerManager = ServiceLocator::getService<IAIProviderManager>();
    if (!providerManager) {
        if (qApp) {
            qApp->setProperty("cgplay.subtitleRefinementDecision", QStringLiteral("skip-no-provider-manager"));
        }
        return;
    }

    SubtitleRefinementRequest request;
    request.mediaPath = mediaPath;
    request.targetLanguage = QStringLiteral("zh-Hans");
    request.sourceLanguageHint = QStringLiteral("ja");
    if (_p->userSettings) {
        request.sourceLanguageHint = _p->userSettings
            ->value(QStringLiteral("ai/subtitles/sourceLanguage"), request.sourceLanguageHint)
            .toString()
            .trimmed();
        if (request.sourceLanguageHint.isEmpty() ||
            request.sourceLanguageHint.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
            request.sourceLanguageHint = QStringLiteral("ja");
        }
    }
    request.quickCues = cues;
    request.sourceCues = cues;
    request.mockWithoutApi = mockWithoutApi;
    request.cancelRequested = std::make_shared<std::atomic_bool>(false);

    _p->subtitleRefinementBusy = true;
    _p->subtitleRefinementLastError.clear();
    _p->subtitleRefinementLastValidation = {};
    _p->subtitleRefinementCancelRequested = request.cancelRequested;

    qInfo() << "[SubtitleRefinement] Starting background refinement"
            << QFileInfo(mediaPath).fileName()
            << "cueCount=" << cues.size()
            << "coverageEnd=" << cueCoverageSeconds;
    if (qApp) {
        qApp->setProperty("cgplay.subtitleRefinementDecision", QStringLiteral("started"));
        qApp->setProperty("cgplay.subtitleRefinementStarted", true);
        qApp->setProperty("cgplay.subtitleRefinementDedicatedWorkspace", dedicatedWorkspaceRefinement);
    }

    auto future = QtConcurrent::run([providerManager, settings = _p->userSettings, request]() {
        SubtitleGenerationService service(providerManager, settings.get());
        return service.refine(request);
    });
    auto* watcher = new QFutureWatcher<SubtitleRefinementResult>(this);
    _p->subtitleRefinementWatcher = watcher;
    connect(watcher, &QFutureWatcher<SubtitleRefinementResult>::finished, this, [this,
                                                                                 watcherPtr = QPointer<QFutureWatcher<SubtitleRefinementResult>>(watcher),
                                                                                 mediaPath]() {
        if (!watcherPtr) {
            return;
        }
        const auto scheduleContinuationAfterRefinement = [this]() {
            if (!_p->subtitleContinuationAfterRefinementPending) {
                return;
            }
            const double startSeconds = _p->subtitleContinuationAfterRefinementStartSeconds;
            _p->subtitleContinuationAfterRefinementPending = false;
            _p->subtitleContinuationAfterRefinementStartSeconds = -1.0;
            if (_p->playbackBar &&
                _p->playbackBar->translationVisible() &&
                !_p->subtitleGenerationBusy &&
                startSeconds >= 0.0) {
                _scheduleGeneratedSubtitleContinuation(startSeconds, 0);
            }
        };
        const SubtitleRefinementResult result = watcherPtr->result();
        watcherPtr->deleteLater();
        _p->subtitleRefinementBusy = false;
        _p->subtitleRefinementCancelRequested.reset();
        _p->subtitleRefinementWatcher.clear();
        _p->subtitleRefinementLastValidation = result.validation;
        if (qApp) {
            qApp->setProperty("cgplay.subtitleRefinementDone", true);
            qApp->setProperty("cgplay.subtitleRefinementSuccess", result.success);
            qApp->setProperty("cgplay.subtitleRefinementVttPath", result.refinedVttPath);
        }

        if (!result.success) {
            _p->subtitleRefinementLastError = result.errorMessage;
            qWarning() << "[SubtitleRefinement] Background refinement failed"
                       << result.errorMessage
                       << result.validation;
            scheduleContinuationAfterRefinement();
            return;
        }

        qInfo() << "[SubtitleRefinement] Background refinement passed"
                << result.refinedVttPath
                << "changed=" << result.changedCueCount
                << "durationMs=" << result.durationMs;

        if (QFileInfo(_p->currentPath).absoluteFilePath() != QFileInfo(mediaPath).absoluteFilePath()) {
            scheduleContinuationAfterRefinement();
            return;
        }

        const double currentCoverage = generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues);
        const double refinedCoverage = generatedSubtitleTranslatedMaxEndSeconds(result.cues);
        int matchedCueCount = 0;
        const int changedCount = mergeGeneratedSubtitleRefinedEnhancements(
            &_p->generatedSubtitleCues,
            result.cues,
            &matchedCueCount);
        if (matchedCueCount > 0) {
            const double mergedCoverage = generatedSubtitleTranslatedMaxEndSeconds(_p->generatedSubtitleCues);
            _p->generatedSubtitleCoverageEndSeconds = mergedCoverage;
            _p->subtitleRefinementLastCoverageEndSeconds = std::max(
                _p->subtitleRefinementLastCoverageEndSeconds,
                mergedCoverage);
            _p->generatedSubtitleProcessedEndSeconds = std::max(
                _p->generatedSubtitleProcessedEndSeconds,
                mergedCoverage);
            qInfo() << "[SubtitleRefinement] Applied cue-level refined enhancements"
                    << "matched=" << matchedCueCount
                    << "changed=" << changedCount
                    << "quickCoverage=" << currentCoverage
                    << "refinedCoverage=" << refinedCoverage
                    << "mergedCoverage=" << mergedCoverage;
            if (_p->playbackBar && _p->playbackBar->translationVisible()) {
                _updateGeneratedSubtitleForFrame(_p->playbackCtrl ? _p->playbackCtrl->currentFrame() : 0);
            }
        } else if (!_p->generatedSubtitleCues.isEmpty()) {
            qInfo() << "[SubtitleRefinement] Refined result did not match quick cues; quick subtitles stay baseline"
                    << "quickCoverage=" << currentCoverage
                    << "refinedCoverage=" << refinedCoverage
                    << "refinedCueCount=" << result.cues.size();
            QTimer::singleShot(2000, this, [this, mediaPath]() {
                if (!_p->subtitleRefinementBusy &&
                    QFileInfo(_p->currentPath).absoluteFilePath() == QFileInfo(mediaPath).absoluteFilePath()) {
                    _scheduleGeneratedSubtitleRefinement(mediaPath, _p->generatedSubtitleCues, false);
                }
            });
        }
        scheduleContinuationAfterRefinement();
    });
    watcher->setFuture(future);
}

void MainWindow::_setTranslationPlaybackMode(const QString& modeId)
{
    _p->translationPlaybackMode = TranslationPlaybackStrategy::normalizeModeId(modeId);
    if (_p->translationPlaybackMode != TranslationPlaybackStrategy::highQualityModeId()) {
        _finishHighQualityPrePlaybackWait(
            QStringLiteral("mode-changed-to-quick"),
            false,
            true,
            true);
    }
    _p->translationPlaybackModeWaited = false;
    _p->translationPlaybackModeWaitMs = 0;
    if (_p->playbackBar) {
        _p->playbackBar->setTranslationMode(_p->translationPlaybackMode);
    }
    if (_p->windowSettings) {
        _p->windowSettings->setValue(QStringLiteral("ai/subtitles/playbackMode"), _p->translationPlaybackMode);
    }
    _publishTranslationPlaybackStrategyDiagnostics(
        _p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId());
    if (_p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId() &&
        _p->playbackBar &&
        _p->playbackBar->translationVisible()) {
        _startCurrentMediaHighQualityEnhancement(QStringLiteral("mode-selected-current-media"));
    }
    if (statusBar()) {
        statusBar()->showMessage(
            _p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId()
                ? QString::fromUtf8(u8"高质量翻译模式已选择：将对当前媒体后台生成/加载 enhanced，失败回退 quick。")
                : QString::fromUtf8(u8"快速播放模式已选择：优先显示 quick 字幕。"),
            2500);
    }
}

bool MainWindow::_shouldStartHighQualityPrePlaybackWait() const
{
    if (_p->translationPlaybackMode != TranslationPlaybackStrategy::highQualityModeId()) {
        return false;
    }
    if (!_p->playbackCtrl) {
        return false;
    }
    return _p->playbackCtrl->playbackState() == 0;
}

void MainWindow::_startHighQualityPrePlaybackWait()
{
    if (!_shouldStartHighQualityPrePlaybackWait()) {
        _p->translationPrePlaybackWaitPlaybackAlreadyRunning =
            _p->playbackCtrl && _p->playbackCtrl->playbackState() != 0;
        _p->translationPrePlaybackWaitResult =
            _p->translationPrePlaybackWaitPlaybackAlreadyRunning
                ? QStringLiteral("playback-running-fallback-quick")
                : QStringLiteral("not-started");
        _p->translationPrePlaybackWaitDegradedToQuick =
            _p->translationPrePlaybackWaitPlaybackAlreadyRunning;
        return;
    }

    _p->translationPlaybackModeWaited = true;
    _p->translationPlaybackModeWaitMs = 0;
    _p->translationPrePlaybackWaitActive = true;
    _p->translationPrePlaybackWaitCanceled = false;
    _p->translationPrePlaybackWaitTimedOut = false;
    _p->translationPrePlaybackWaitDegradedToQuick = false;
    _p->translationPrePlaybackWaitPlaybackAlreadyRunning = false;
    _p->translationPrePlaybackWaitResult = QStringLiteral("waiting");
    _p->translationPrePlaybackWaitTargetSeconds = kHighQualityPrePlaybackTargetSeconds;
    _p->translationPrePlaybackWaitMaxProcessedSeconds = 0.0;
    _p->translationPrePlaybackWaitEnhancementStarted =
        _p->highQualityEnhancementBusy &&
        QFileInfo(_p->highQualityEnhancementMediaPath).absoluteFilePath() ==
            QFileInfo(_p->currentPath).absoluteFilePath();
    if (_p->playbackCtrl && _p->playbackCtrl->fps() > 0.0 && _p->playbackCtrl->totalFrames() > 0) {
        const double totalSeconds =
            static_cast<double>(_p->playbackCtrl->totalFrames()) / _p->playbackCtrl->fps();
        const double remainingSeconds =
            std::max(1.0, totalSeconds - generatedSubtitleCurrentSeconds(_p->playbackCtrl.get()));
        _p->translationPrePlaybackWaitTargetSeconds =
            std::min(kHighQualityPrePlaybackTargetSeconds, remainingSeconds);
    }
    _p->translationPrePlaybackWaitCoverageAheadSeconds = 0.0;
    _p->translationPrePlaybackWaitStartMs = QDateTime::currentMSecsSinceEpoch();

    qint64 timeoutMs = kHighQualityPrePlaybackDefaultTimeoutMs;
    if (_p->userSettings) {
        timeoutMs = _p->userSettings
            ->value(QStringLiteral("ai/subtitles/highQuality/prePlaybackWaitTimeoutMs"), timeoutMs)
            .toLongLong();
    }
    if (qApp) {
        const qint64 overrideTimeoutMs =
            qApp->property("cgplay.subtitleHighQualityPrePlaybackWaitTimeoutMs").toLongLong();
        if (overrideTimeoutMs > 0) {
            timeoutMs = overrideTimeoutMs;
        }
        const double overrideTargetSeconds =
            qApp->property("cgplay.subtitleHighQualityPrePlaybackTargetSeconds").toDouble();
        if (overrideTargetSeconds > 0.0) {
            _p->translationPrePlaybackWaitTargetSeconds = overrideTargetSeconds;
        }
    }
    _p->translationPrePlaybackWaitTimeoutMs = std::clamp<qint64>(timeoutMs, 1000, 600000);
    _p->translationPrePlaybackWaitTargetSeconds = std::clamp(
        _p->translationPrePlaybackWaitTargetSeconds,
        1.0,
        kHighQualityPrePlaybackTargetMaxSeconds);

    if (!_p->translationPrePlaybackWaitTimer) {
        _p->translationPrePlaybackWaitTimer = new QTimer(this);
        _p->translationPrePlaybackWaitTimer->setInterval(250);
        connect(_p->translationPrePlaybackWaitTimer, &QTimer::timeout, this, [this]() {
            _updateHighQualityPrePlaybackWait(QStringLiteral("timer"));
        });
    }
    _p->translationPrePlaybackWaitTimer->start();

    const int progressMaximum = std::max(
        10,
        qRound(_p->translationPrePlaybackWaitTargetSeconds * 10.0));
    auto* progress = new QProgressDialog(
        QString::fromUtf8(u8"高质量翻译预缓冲：正在生成前 600 秒字幕；可随时使用 quick 播放。"),
        QString::fromUtf8(u8"使用 quick 播放"),
        0,
        progressMaximum,
        this);
    progress->setWindowTitle(QString::fromUtf8(u8"高质量翻译"));
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setWindowModality(Qt::NonModal);
    progress->setValue(0);
    _p->translationPrePlaybackWaitProgress = progress;
    connect(progress, &QProgressDialog::canceled, this, [this]() {
        _finishHighQualityPrePlaybackWait(
            QStringLiteral("canceled-fallback-quick"),
            false,
            true,
            true);
    });
    progress->show();
    _startCurrentMediaHighQualityEnhancement(QStringLiteral("high-quality-preplay-wait"));
    _publishTranslationPlaybackStrategyDiagnostics(true);
}

void MainWindow::_updateHighQualityPrePlaybackWait(const QString& reason)
{
    Q_UNUSED(reason);
    if (!_p->translationPrePlaybackWaitActive) {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsedMs = std::max<qint64>(0, nowMs - _p->translationPrePlaybackWaitStartMs);
    _p->translationPlaybackModeWaitMs = elapsedMs;

    const QJsonObject strategyDiagnostics = _translationPlaybackStrategyDiagnostics(true);
    const QJsonObject currentMediaHighQuality =
        strategyDiagnostics.value(QStringLiteral("currentMediaHighQuality")).toObject();
    const QString activeHqSource =
        currentMediaHighQuality.value(QStringLiteral("activeHqSource")).toString(
            strategyDiagnostics.value(QStringLiteral("activeSource")).toString(QStringLiteral("quick-fallback")));
    const double hqCoverageSeconds =
        currentMediaHighQuality.value(QStringLiteral("hqCoverageSeconds")).toDouble(0.0);
    const double progressOverreportedSeconds =
        currentMediaHighQuality.value(QStringLiteral("progressOverreportedSeconds")).toDouble(0.0);
    const int hqTranslatedCueCount =
        currentMediaHighQuality.value(QStringLiteral("hqTranslatedCueCount")).toInt(0);
    const QString fallbackReason =
        currentMediaHighQuality.value(QStringLiteral("fallbackReason")).toString(
            strategyDiagnostics.value(QStringLiteral("fallbackReason")).toString(QStringLiteral("high-quality-not-ready")));
    _p->translationPrePlaybackWaitCoverageAheadSeconds =
        std::max(0.0, hqCoverageSeconds);

    const TranslationEnhancementScheduleRequest progressRequest =
        _buildTranslationEnhancementRequest(true, false, true);
    const QJsonObject ocrProgress = readGeneratedSubtitleProgress(
        generatedSubtitleOcrProgressPath(progressRequest.ocrSubtitleCachePath));
    const QString progressMediaPath = QFileInfo(
        ocrProgress.value(QStringLiteral("mediaPath")).toString()).absoluteFilePath();
    const bool progressMatchesCurrentMedia =
        !progressMediaPath.isEmpty() &&
        progressMediaPath == QFileInfo(_p->currentPath).absoluteFilePath();
    const double workerProcessedCoverageSeconds = progressMatchesCurrentMedia
        ? std::max(0.0, ocrProgress.value(QStringLiteral("processedCoverageSeconds")).toDouble())
        : 0.0;
    _p->translationPrePlaybackWaitMaxProcessedSeconds = std::max(
        _p->translationPrePlaybackWaitMaxProcessedSeconds,
        workerProcessedCoverageSeconds);
    const QString workerProgressStatus = progressMatchesCurrentMedia
        ? ocrProgress.value(QStringLiteral("status")).toString()
        : QString();
    const QString workerProgressSource = workerProgressStatus == QStringLiteral("ai-vision")
        ? QString::fromUtf8(u8"AI 视觉")
        : (workerProgressStatus == QStringLiteral("finalizing-ai-vision")
              ? QString::fromUtf8(u8"AI 视觉整理 / OCR 兜底")
              : QStringLiteral("ocr-scanning"));

    if (_p->playbackCtrl && _p->playbackCtrl->playbackState() != 0) {
        _finishHighQualityPrePlaybackWait(
            QStringLiteral("playback-started-fallback-quick"),
            false,
            false,
            true);
        return;
    }
    const bool hqSourceReady =
        activeHqSource != QStringLiteral("quick-fallback") &&
        hqTranslatedCueCount > 0;
    const bool workerTargetProcessed =
        progressMatchesCurrentMedia &&
        workerProgressStatus == QStringLiteral("completed") &&
        _p->translationPrePlaybackWaitMaxProcessedSeconds + 0.5 >=
            _p->translationPrePlaybackWaitTargetSeconds;
    if (workerTargetProcessed) {
        if (_p->translationPrePlaybackWaitProgress) {
            _p->translationPrePlaybackWaitProgress->setValue(
                _p->translationPrePlaybackWaitProgress->maximum());
        }
        _finishHighQualityPrePlaybackWait(QStringLiteral("hq-worker-target-processed"));
        return;
    }
    if (hqSourceReady &&
        _p->translationPrePlaybackWaitCoverageAheadSeconds >=
            _p->translationPrePlaybackWaitTargetSeconds) {
        _finishHighQualityPrePlaybackWait(QStringLiteral("hq-target-coverage-ready"));
        return;
    }
    if (elapsedMs >= _p->translationPrePlaybackWaitTimeoutMs) {
        _finishHighQualityPrePlaybackWait(
            QStringLiteral("high-quality-not-ready-fallback-quick:%1").arg(fallbackReason),
            true,
            false,
            true);
        return;
    }

    if (_p->translationPrePlaybackWaitProgress) {
        const double displayedProgressSeconds = std::min(
            _p->translationPrePlaybackWaitTargetSeconds,
            std::max(_p->translationPrePlaybackWaitCoverageAheadSeconds,
                     _p->translationPrePlaybackWaitMaxProcessedSeconds));
        int progressValue = qRound(displayedProgressSeconds * 10.0);
        if (!hqSourceReady) {
            progressValue = std::min(
                progressValue,
                _p->translationPrePlaybackWaitProgress->maximum() - 1);
        }
        _p->translationPrePlaybackWaitProgress->setValue(progressValue);
        _p->translationPrePlaybackWaitProgress->setLabelText(
            QString::fromUtf8(u8"高质量翻译预缓冲：画面已检查 %1/%2 秒，最终中文字幕已生成至前方 %3 秒，来源 %4。")
                .arg(displayedProgressSeconds, 0, 'f', 1)
                .arg(_p->translationPrePlaybackWaitTargetSeconds, 0, 'f', 0)
                .arg(_p->translationPrePlaybackWaitCoverageAheadSeconds, 0, 'f', 1)
                .arg(progressOverreportedSeconds <= 0.25
                         ? (workerProcessedCoverageSeconds > hqCoverageSeconds
                                ? workerProgressSource
                                : activeHqSource)
                         : QStringLiteral("%1/progress-capped").arg(activeHqSource)));
    }
}

void MainWindow::_finishHighQualityPrePlaybackWait(
    const QString& result,
    bool timedOut,
    bool canceled,
    bool degradedToQuick)
{
    if (!_p || (!_p->translationPrePlaybackWaitActive &&
                _p->translationPrePlaybackWaitResult == result)) {
        return;
    }
    _p->translationPrePlaybackWaitActive = false;
    _p->translationPrePlaybackWaitResult = result;
    _p->translationPrePlaybackWaitTimedOut = timedOut;
    _p->translationPrePlaybackWaitCanceled = canceled;
    _p->translationPrePlaybackWaitDegradedToQuick = degradedToQuick;
    if ((degradedToQuick || timedOut || canceled) && _p->highQualityEnhancementCancelRequested) {
        _p->highQualityEnhancementCancelRequested->store(true);
    }
    if (_p->translationPrePlaybackWaitTimer) {
        _p->translationPrePlaybackWaitTimer->stop();
    }
    if (_p->translationPrePlaybackWaitProgress) {
        QSignalBlocker blocker(_p->translationPrePlaybackWaitProgress.data());
        _p->translationPrePlaybackWaitProgress->close();
        _p->translationPrePlaybackWaitProgress->deleteLater();
        _p->translationPrePlaybackWaitProgress.clear();
    }
    if (statusBar() && _p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId()) {
        statusBar()->showMessage(
            degradedToQuick
                ? QString::fromUtf8(u8"高质量预缓冲未完成，已回退 quick 字幕继续播放。")
                : QString::fromUtf8(u8"高质量预缓冲已就绪。"),
            3500);
    }
    _publishTranslationPlaybackStrategyDiagnostics(
        _p->translationPlaybackMode == TranslationPlaybackStrategy::highQualityModeId());
}

} // namespace cgplay
