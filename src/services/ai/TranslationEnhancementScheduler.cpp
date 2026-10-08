#include "TranslationEnhancementScheduler.h"
#include "TranslationEnhancementSupport.h"

#include "common/jobs/JobSystem.h"
#include "TranslationFusionController.h"
#include "TranslationPlaybackStrategy.h"
#include "TranslationTextPolicy.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QStandardPaths>
#include <QStringConverter>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QEventLoop>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cgplay {

using namespace translation_enhancement_support;

QJsonObject TranslationEnhancementScheduler::policy() const
{
    return QJsonObject{
        { QStringLiteral("entry"), QStringLiteral("manual-background-enhancement-boundary") },
        { QStringLiteral("defaultEnabled"), false },
        { QStringLiteral("quickPathAutoRunAllowed"), false },
        { QStringLiteral("manualModes"), QJsonArray{
              TranslationPlaybackStrategy::highQualityModeId(),
              QStringLiteral("manual")
          } },
        { QStringLiteral("canPausePlayback"), false },
        { QStringLiteral("canChangeQuickConstants"), false },
        { QStringLiteral("canUseQuickApiQuota"), false },
        { QStringLiteral("canUseQuickWorkerLane"), false },
        { QStringLiteral("canWriteQuickZhSidecar"), false },
        { QStringLiteral("outputPolicy"), QStringLiteral("independent-.online.source-.online.translated.zh-.ocr.source-.ocr.translated.zh-.enhanced.zh-or-.repair.zh-cache-only") },
        { QStringLiteral("apiModel"), QStringLiteral("two-api-configuration: workbench-api-for-search-vision-text-fix-fusion; asr-api-for-audio-transcription-only") },
        { QStringLiteral("onlineWorkerPolicy"), QStringLiteral("manual-high-quality-only-local-reference-workbench-search-or-explicit-online-search-cache-plus-independent-translated-cache") },
        { QStringLiteral("onlineSearchPolicy"), QStringLiteral("manual-high-quality-workbench-api-search-first-with-minimal-metadata; no-third-online-api-required") },
        { QStringLiteral("ocrWorkerPolicy"), QStringLiteral("manual-high-quality-workbench-vision-frame-crops-first; local-ocr-fallback-independent-cache-plus-visible-Chinese-translated-cache") },
        { QStringLiteral("longAsrWorkerPolicy"), QStringLiteral("manual-high-quality-only-independent-longasr-source-and-hq-translated-cache-after-local-online-visual-sources-fail") },
        { QStringLiteral("repairWorkerPolicy"), QStringLiteral("manual-high-quality-only-independent-repair-cache") },
        { QStringLiteral("mergePolicy"), QStringLiteral("matching-quick-cue-only-with-source-reliability-ranking") },
        { QStringLiteral("sourceReliabilityOrder"), QJsonArray{
              QStringLiteral("human-local-subtitle"),
              QStringLiteral("embedded-subtitle"),
              QStringLiteral("external-subtitle"),
              QStringLiteral("online-high-confidence-aligned"),
              QStringLiteral("hard-sub-ocr"),
              QStringLiteral("long-asr-corrected"),
              QStringLiteral("repair"),
              QStringLiteral("refined"),
              QStringLiteral("quick-fallback")
          } },
        { QStringLiteral("fallbackPolicy"), QStringLiteral("degrade-to-quick") },
        { QStringLiteral("guardConsumersReadOnly"), true },
        { QStringLiteral("wholeTrackReplaceQuickAllowed"), false },
        { QStringLiteral("quickBaselineIsSourceOfTruth"), true }
    };
}

QJsonObject TranslationEnhancementScheduler::evaluate(
    const TranslationEnhancementScheduleRequest& request) const
{
    const QString mode = TranslationPlaybackStrategy::normalizeModeId(request.playbackMode);
    const QJsonObject mediaIdentity = SubtitleGenerationService::mediaIdentity(request.mediaPath);
    const QString mediaFingerprint =
        mediaIdentity.value(QStringLiteral("mediaFingerprint")).toString();
    const bool highQualityMode = mode == TranslationPlaybackStrategy::highQualityModeId();
    const bool manualAllowed =
        highQualityMode &&
        request.manualRequested &&
        (!request.playbackAlreadyRunning || request.allowManualWhilePlayback);
    const bool workerExecutionAllowed = manualAllowed && request.executeWorkers;
    const bool configured =
        request.onlineConfigured ||
        request.ocrConfigured ||
        request.longAsrConfigured ||
        request.fusionConfigured ||
        request.lowConfidenceRepairConfigured ||
        request.onlineMockMode ||
        request.ocrMockMode ||
        request.lowConfidenceRepairMockMode;
    QJsonObject onlineWorkerResult;
    QJsonObject onlineTranslationResult;
    QJsonObject ocrWorkerResult;
    const bool visualSubtitleFirst =
        request.ocrAutoEnabledForHighQualityCurrentMedia &&
        !request.localReferenceProviderAvailable;
    if (visualSubtitleFirst) {
        ocrWorkerResult = _runManualOcrWorker(request, workerExecutionAllowed);
        if (ocrWorkerResult.value(QStringLiteral("success")).toBool()) {
            onlineWorkerResult = QJsonObject{
                { QStringLiteral("attempted"), false },
                { QStringLiteral("success"), false },
                { QStringLiteral("fallbackReason"), QStringLiteral("skipped-hard-sub-visual-source-ready") }
            };
            onlineTranslationResult = QJsonObject{
                { QStringLiteral("attempted"), false },
                { QStringLiteral("success"), false },
                { QStringLiteral("degradedReason"), QStringLiteral("skipped-hard-sub-visual-source-ready") }
            };
        } else {
            onlineWorkerResult = _runManualOnlineWorker(request, workerExecutionAllowed);
            onlineTranslationResult =
                _runManualOnlineTranslationWorker(request, workerExecutionAllowed, onlineWorkerResult);
        }
    } else {
        onlineWorkerResult = _runManualOnlineWorker(request, workerExecutionAllowed);
        onlineTranslationResult =
            _runManualOnlineTranslationWorker(request, workerExecutionAllowed, onlineWorkerResult);
        ocrWorkerResult = _runManualOcrWorker(request, workerExecutionAllowed);
    }
    const bool higherPrioritySubtitleSourceReady =
        onlineTranslationResult.value(QStringLiteral("success")).toBool() ||
        ocrWorkerResult.value(QStringLiteral("success")).toBool();
    const QJsonObject longAsrWorkerResult = higherPrioritySubtitleSourceReady
        ? QJsonObject{
              { QStringLiteral("attempted"), false },
              { QStringLiteral("success"), false },
              { QStringLiteral("fallbackReason"), QStringLiteral("skipped-higher-priority-subtitle-source-ready") }
          }
        : _runManualLongAsrWorker(request, workerExecutionAllowed);
    const QJsonObject repairWorkerResult =
        _runManualRepairWorker(request, workerExecutionAllowed);
    const QJsonObject onlineSourceIntake =
        _diagnoseOnlineSourceIntake(request, workerExecutionAllowed, onlineWorkerResult);
    const QString onlineProviderKind = onlineReferenceProviderKind(request);
    const QString onlineFallbackReason = onlineReferenceFallbackReason(request);
    const bool translatedOnlineReady =
        onlineTranslationResult.value(QStringLiteral("translatedOnlineCacheWritten")).toBool();
    const bool longAsrCandidateAllowed =
        request.longAsrAutoEnabledForNoSubtitleHighQuality ||
        (!request.localReferenceProviderAvailable &&
         !request.onlineConfigured &&
         !request.ocrConfigured &&
         !request.ocrAutoEnabledForHighQualityCurrentMedia);
    TranslationManualFusionResult manualFusionResult;
    if (workerExecutionAllowed) {
        TranslationManualFusionRequest fusionRequest;
        fusionRequest.manualRequested = true;
        fusionRequest.quickTranslatedVttPath = request.quickTranslatedVttPath;
        fusionRequest.outputEnhancedVttPath = request.enhancedTranslatedVttPath;
        fusionRequest.candidateCachePaths = QStringList{
            request.onlineTranslatedCachePath,
            request.ocrTranslatedCachePath,
            request.refinedTranslatedVttPath,
            request.repairTranslatedVttPath
        };
        if (longAsrCandidateAllowed) {
            fusionRequest.candidateCachePaths.insert(2, request.longAsrTranslatedCachePath);
        }
        TranslationFusionController fusionController;
        manualFusionResult = fusionController.writeManualFusionCache(fusionRequest);
    }
    writeCacheMediaIdentityIfExists(
        {
            request.onlineSubtitleCachePath,
            request.onlineTranslatedCachePath,
            request.ocrSubtitleCachePath,
            request.ocrTranslatedCachePath,
            request.longAsrSourceCachePath,
            request.longAsrTranslatedCachePath,
            request.enhancedTranslatedVttPath,
            request.repairTranslatedVttPath
        },
        request.mediaPath);

    QJsonArray layers;
    layers.append(_layerState(
        QStringLiteral("online-subtitle"),
        request.onlineConfigured,
        manualAllowed,
        QString()));
    layers.append(_layerState(
        QStringLiteral("ocr-subtitle"),
        request.ocrConfigured,
        manualAllowed,
        QString()));
    layers.append(_layerState(
        QStringLiteral("long-window-asr"),
        request.longAsrConfigured,
        manualAllowed,
        request.longAsrTranslatedCachePath));
    layers.append(_layerState(
        QStringLiteral("fusion"),
        request.fusionConfigured,
        manualAllowed,
        request.enhancedTranslatedVttPath));
    layers.append(_layerState(
        QStringLiteral("low-confidence-repair"),
        request.lowConfidenceRepairConfigured,
        manualAllowed,
        request.repairTranslatedVttPath));

    const QString state = manualAllowed
        ? (manualFusionResult.success
              ? QStringLiteral("manual-local-fusion-cache-written")
              : QStringLiteral("manual-enhancement-safe-fallback-quick"))
        : (highQualityMode ? QStringLiteral("manual-mode-degraded-to-quick") : QStringLiteral("default-disabled"));
    const bool repairSucceeded =
        repairWorkerResult.value(QStringLiteral("success")).toBool();
    const int repairChangedCueCount =
        repairWorkerResult.value(QStringLiteral("terminologyAppliedCueCount")).toInt();
    const bool enhancementAttempted =
        onlineWorkerResult.value(QStringLiteral("attempted")).toBool() ||
        onlineTranslationResult.value(QStringLiteral("attempted")).toBool() ||
        ocrWorkerResult.value(QStringLiteral("attempted")).toBool() ||
        longAsrWorkerResult.value(QStringLiteral("attempted")).toBool() ||
        repairWorkerResult.value(QStringLiteral("attempted")).toBool() ||
        manualFusionResult.attempted;
    const bool enhancementSucceeded = manualFusionResult.success || repairSucceeded ||
        onlineTranslationResult.value(QStringLiteral("success")).toBool() ||
        ocrWorkerResult.value(QStringLiteral("success")).toBool();
    const bool longAsrSucceeded =
        longAsrWorkerResult.value(QStringLiteral("success")).toBool();
    const bool anyEnhancementSucceeded =
        enhancementSucceeded || longAsrSucceeded;
    const bool visibleEnhancementSucceeded =
        manualFusionResult.success || repairSucceeded;
    const QString fusionPrimarySource =
        manualFusionResult.primaryAcceptedSourceType.isEmpty()
        ? QStringLiteral("enhanced-fusion-cache")
        : manualFusionResult.primaryAcceptedSourceType;
    const QString activeSource = activeEnhancementSource(
        manualFusionResult.success,
        fusionPrimarySource,
        repairSucceeded,
        longAsrSucceeded,
        ocrWorkerResult.value(QStringLiteral("success")).toBool(),
        onlineTranslationResult.value(QStringLiteral("success")).toBool());
    const QString fallbackReason = enhancementFallbackReason(
        visibleEnhancementSucceeded,
        manualAllowed,
        request.executeWorkers,
        anyEnhancementSucceeded,
        highQualityMode);
    const QJsonObject quickState = cacheState(request.quickTranslatedVttPath, {}, request.currentSeconds);
    const QJsonObject enhancedState = cacheState(request.enhancedTranslatedVttPath, request.quickTranslatedVttPath, request.currentSeconds);
    const QJsonObject ocrState = cacheState(request.ocrSubtitleCachePath, request.quickTranslatedVttPath, request.currentSeconds);
    const QJsonObject ocrTranslatedState = cacheState(request.ocrTranslatedCachePath, request.quickTranslatedVttPath, request.currentSeconds);
    const QJsonObject onlineState = cacheState(request.onlineTranslatedCachePath, request.quickTranslatedVttPath, request.currentSeconds);
    const QJsonObject longAsrState = cacheState(request.longAsrTranslatedCachePath, request.quickTranslatedVttPath, request.currentSeconds);
    const QJsonObject localReferenceState = cacheState(request.localReferenceSubtitlePath, request.quickTranslatedVttPath, request.currentSeconds);
    QJsonObject candidateScan =
        ocrWorkerResult.value(QStringLiteral("candidateScan")).toObject();
    if (candidateScan.isEmpty()) {
        candidateScan = readJsonObjectFile(
            request.ocrSubtitleCachePath + QStringLiteral(".candidate_scan.json"));
    }
    const double legacyVisualOnsetCorrectionSeconds =
        candidateScan.value(QStringLiteral("detectorVersion")).toString() ==
            QStringLiteral("subtitle-region-detector-v1")
        ? std::max(
              0.0,
              candidateScan.value(QStringLiteral("sampleIntervalSeconds")).toDouble(0.0) * 0.5)
        : 0.0;
    const int longAsrDisplayableCueCount = longAsrCandidateAllowed
        ? longAsrState.value(QStringLiteral("displayableCueCount")).toInt()
        : 0;
    const double longAsrDisplayableCoverageEndSeconds = longAsrCandidateAllowed
        ? longAsrState.value(QStringLiteral("translatedDisplayableCoverageEndSeconds")).toDouble()
        : 0.0;
    const QVector<GeneratedSubtitleCue> quickTranslatedCues =
        SubtitleGenerationService::readSubtitleFile(request.quickTranslatedVttPath);
    const QVector<GeneratedSubtitleCue> enhancedTranslatedCues =
        SubtitleGenerationService::readSubtitleFile(request.enhancedTranslatedVttPath);
    QVector<GeneratedSubtitleCue> ocrSourceCues =
        SubtitleGenerationService::readSubtitleFile(request.ocrSubtitleCachePath);
    QVector<GeneratedSubtitleCue> ocrTranslatedCues =
        SubtitleGenerationService::readSubtitleFile(request.ocrTranslatedCachePath);
    if (legacyVisualOnsetCorrectionSeconds > 0.0) {
        const auto correctLegacyOnsets = [legacyVisualOnsetCorrectionSeconds](
            QVector<GeneratedSubtitleCue>& cues) {
            for (GeneratedSubtitleCue& cue : cues) {
                cue.startSeconds = std::min(
                    cue.endSeconds,
                    cue.startSeconds + legacyVisualOnsetCorrectionSeconds);
            }
        };
        correctLegacyOnsets(ocrSourceCues);
        correctLegacyOnsets(ocrTranslatedCues);
    }
    const auto cueTextAtCurrentSeconds =
        [&request](const QVector<GeneratedSubtitleCue>& cues,
                   double leadToleranceSeconds,
                   QString* cueText,
                   double* cueStart,
                   double* cueEnd) {
        const GeneratedSubtitleCue* bestCue = nullptr;
        for (const GeneratedSubtitleCue& cue : cues) {
            if (cue.startSeconds <= request.currentSeconds + leadToleranceSeconds &&
                cue.endSeconds + 0.02 > request.currentSeconds) {
                if (!bestCue || cue.startSeconds > bestCue->startSeconds) {
                    bestCue = &cue;
                }
            }
        }
        if (bestCue) {
            if (cueText) {
                *cueText = bestCue->translatedText.trimmed();
            }
            if (cueStart) {
                *cueStart = bestCue->startSeconds;
            }
            if (cueEnd) {
                *cueEnd = bestCue->endSeconds;
            }
            return true;
        }
        if (cueText) {
            cueText->clear();
        }
        if (cueStart) {
            *cueStart = -1.0;
        }
        if (cueEnd) {
            *cueEnd = -1.0;
        }
        return false;
    };
    QString visualSourceDisplayedText;
    QString quickDisplayedText;
    QString visualEnhancedDisplayedText;
    QString visualDisplayedText;
    double quickCueStartSeconds = -1.0;
    double quickCueEndSeconds = -1.0;
    double visualEnhancedCueStartSeconds = -1.0;
    double visualEnhancedCueEndSeconds = -1.0;
    double visualSourceCueStartSeconds = -1.0;
    double visualSourceCueEndSeconds = -1.0;
    double visualTranslatedCueStartSeconds = -1.0;
    double visualTranslatedCueEndSeconds = -1.0;
    const bool quickCueAtTime =
        cueTextAtCurrentSeconds(quickTranslatedCues, 0.005, &quickDisplayedText, &quickCueStartSeconds, &quickCueEndSeconds);
    const bool visualSourceCueAtTime = cueTextAtCurrentSeconds(
        ocrSourceCues,
        0.02,
        &visualSourceDisplayedText,
        &visualSourceCueStartSeconds,
        &visualSourceCueEndSeconds);
    const bool visualEnhancedCueAtTime = cueTextAtCurrentSeconds(
        enhancedTranslatedCues,
        0.02,
        &visualEnhancedDisplayedText,
        &visualEnhancedCueStartSeconds,
        &visualEnhancedCueEndSeconds);
    const bool visualTranslatedCueAtTime = cueTextAtCurrentSeconds(
        ocrTranslatedCues,
        0.02,
        &visualDisplayedText,
        &visualTranslatedCueStartSeconds,
        &visualTranslatedCueEndSeconds);
    const bool visualSourceCueUsableAtTime =
        visualSourceCueAtTime && !translation_text::isLikelyVisualWatermarkText(visualSourceDisplayedText);
    const bool visualTranslatedCueUsableAtTime =
        visualTranslatedCueAtTime && isFinalDisplayableText(visualDisplayedText);
    const bool primaryVisualTrackAvailable =
        ocrState.value(QStringLiteral("displayableCueCount")).toInt() > 0 ||
        ocrTranslatedState.value(QStringLiteral("displayableCueCount")).toInt() > 0;
    const bool enhancedCueAllowedAsVisualAtTime =
        visualEnhancedCueAtTime &&
        (!primaryVisualTrackAvailable ||
         (!visualSourceCueUsableAtTime &&
          !visualTranslatedCueUsableAtTime &&
          highQualityMode &&
          manualAllowed)) &&
        (fusionPrimarySource.contains(QStringLiteral("ocr"), Qt::CaseInsensitive) ||
         fusionPrimarySource.contains(QStringLiteral("visual"), Qt::CaseInsensitive) ||
         fusionPrimarySource.contains(QStringLiteral("online"), Qt::CaseInsensitive) ||
         enhancedState.value(QStringLiteral("displayableCueCount")).toInt() > 0);
    const bool visualCueAtTime =
        visualSourceCueUsableAtTime || visualTranslatedCueUsableAtTime || enhancedCueAllowedAsVisualAtTime;
    const double activeVisualCueStartSeconds =
        visualSourceCueUsableAtTime ? visualSourceCueStartSeconds
            : (visualTranslatedCueUsableAtTime ? visualTranslatedCueStartSeconds
                                         : (enhancedCueAllowedAsVisualAtTime ? visualEnhancedCueStartSeconds : -1.0));
    const double activeVisualCueEndSeconds =
        visualSourceCueUsableAtTime ? visualSourceCueEndSeconds
            : (visualTranslatedCueUsableAtTime ? visualTranslatedCueEndSeconds
                                         : (enhancedCueAllowedAsVisualAtTime ? visualEnhancedCueEndSeconds : -1.0));
    const bool visualTextFound =
        ocrState.value(QStringLiteral("displayableCueCount")).toInt() > 0 ||
        ocrTranslatedState.value(QStringLiteral("displayableCueCount")).toInt() > 0 ||
        (enhancedCueAllowedAsVisualAtTime && enhancedState.value(QStringLiteral("displayableCueCount")).toInt() > 0) ||
        visualCueAtTime ||
        ocrWorkerResult.value(QStringLiteral("ocrCueCount")).toInt() > 0 ||
        ocrWorkerResult.value(QStringLiteral("ocrTranslatedCueCount")).toInt() > 0;
    const bool visualAttempted =
        ocrWorkerResult.value(QStringLiteral("attempted")).toBool() ||
        request.ocrConfigured ||
        request.ocrAutoEnabledForHighQualityCurrentMedia;
    const bool visualTrackGenerated =
        ocrWorkerResult.value(QStringLiteral("visualSubtitleTrackGenerated")).toBool(false) ||
        ocrWorkerResult.value(QStringLiteral("visualCueCount")).toInt() > 0 ||
        ocrState.value(QStringLiteral("displayableCueCount")).toInt() > 0 ||
        ocrTranslatedState.value(QStringLiteral("displayableCueCount")).toInt() > 0 ||
        (enhancedCueAllowedAsVisualAtTime && enhancedState.value(QStringLiteral("displayableCueCount")).toInt() > 0);
    double visualCoverageStartSeconds = -1.0;
    double visualCoverageEndSeconds = 0.0;
    const auto includeVisualCoverage = [&](const QJsonObject& state) {
        if (state.value(QStringLiteral("displayableCueCount")).toInt() <= 0) {
            return;
        }
        const double start = state.value(QStringLiteral("translatedDisplayableCoverageStartSeconds")).toDouble(-1.0);
        const double end = state.value(QStringLiteral("translatedDisplayableCoverageEndSeconds")).toDouble(0.0);
        if (start >= 0.0 && end > start) {
            visualCoverageStartSeconds = visualCoverageStartSeconds < 0.0
                ? start
                : std::min(visualCoverageStartSeconds, start);
            visualCoverageEndSeconds = std::max(visualCoverageEndSeconds, end);
        }
    };
    includeVisualCoverage(ocrState);
    includeVisualCoverage(ocrTranslatedState);
    if (enhancedCueAllowedAsVisualAtTime) {
        includeVisualCoverage(enhancedState);
    }
    if (ocrWorkerResult.value(QStringLiteral("visualCueCount")).toInt() > 0) {
        const double workerStart =
            ocrWorkerResult.value(QStringLiteral("visualCoverageStart")).toDouble(-1.0);
        const double workerEnd =
            ocrWorkerResult.value(QStringLiteral("visualCoverageEnd")).toDouble(0.0);
        if (workerStart >= 0.0 && workerEnd > workerStart) {
            visualCoverageStartSeconds = visualCoverageStartSeconds < 0.0
                ? workerStart
                : std::min(visualCoverageStartSeconds, workerStart);
            visualCoverageEndSeconds = std::max(visualCoverageEndSeconds, workerEnd);
        }
    }
    const bool candidateScanFingerprintMatches =
        candidateScan.isEmpty() ||
        candidateScan.value(QStringLiteral("mediaFingerprint")).toString() == mediaFingerprint;
    double candidateScanCoverageStartSeconds = -1.0;
    double candidateScanCoverageEndSeconds = 0.0;
    if (!candidateScan.isEmpty() && candidateScanFingerprintMatches) {
        candidateScanCoverageStartSeconds =
            candidateScan.value(QStringLiteral("scanStartSeconds")).toDouble(
                candidateScan.value(QStringLiteral("startSeconds")).toDouble(0.0));
        candidateScanCoverageEndSeconds =
            candidateScan.value(QStringLiteral("scanEndSeconds")).toDouble(-1.0);
        if (candidateScanCoverageEndSeconds <= candidateScanCoverageStartSeconds) {
            const double duration = candidateScan.value(QStringLiteral("durationSec")).toDouble(0.0);
            if (duration > 0.0) {
                candidateScanCoverageEndSeconds = candidateScanCoverageStartSeconds + duration;
            }
        }
    }
    const bool candidateScanCoverageKnown =
        !candidateScan.isEmpty() &&
        candidateScanFingerprintMatches &&
        candidateScanCoverageStartSeconds >= 0.0 &&
        candidateScanCoverageEndSeconds > candidateScanCoverageStartSeconds;
    const bool visualCoverageKnown =
        visualTrackGenerated &&
        visualCoverageStartSeconds >= 0.0 &&
        visualCoverageEndSeconds > visualCoverageStartSeconds;
    double visualAuthorityCoverageStartSeconds = visualCoverageStartSeconds;
    double visualAuthorityCoverageEndSeconds = visualCoverageEndSeconds;
    if (candidateScanCoverageKnown) {
        visualAuthorityCoverageStartSeconds = visualAuthorityCoverageStartSeconds < 0.0
            ? candidateScanCoverageStartSeconds
            : std::min(visualAuthorityCoverageStartSeconds, candidateScanCoverageStartSeconds);
        visualAuthorityCoverageEndSeconds =
            std::max(visualAuthorityCoverageEndSeconds, candidateScanCoverageEndSeconds);
    }
    const bool visualAuthorityCoverageKnown =
        (visualCoverageKnown || candidateScanCoverageKnown) &&
        visualAuthorityCoverageStartSeconds >= 0.0 &&
        visualAuthorityCoverageEndSeconds > visualAuthorityCoverageStartSeconds;
    const bool currentTimeWithinVisualCoverage =
        visualAuthorityCoverageKnown &&
        request.currentSeconds + 0.005 >= visualAuthorityCoverageStartSeconds &&
        request.currentSeconds <= visualAuthorityCoverageEndSeconds + 0.005;
    const bool visualTrackAuthoritative =
        highQualityMode &&
        manualAllowed &&
        (request.workbenchCanReadImageText || request.ocrConfigured || request.ocrAutoEnabledForHighQualityCurrentMedia) &&
        (visualAttempted || visualTrackGenerated) &&
        (visualTextFound || visualTrackGenerated);
    const bool visualTrackAuthoritativeAtTime =
        visualTrackAuthoritative &&
        currentTimeWithinVisualCoverage;
    const bool visualNoTextSuppressQuickAsr =
        highQualityMode &&
        manualAllowed &&
        visualTrackAuthoritativeAtTime &&
        !visualCueAtTime;
    const QString visualSourceDisplayNormalized =
        visualSourceCueUsableAtTime ? normalizeOcrChineseForZhHans(visualSourceDisplayedText) : QString();
    const QString visualEnhancedDisplayNormalized =
        visualEnhancedCueAtTime ? normalizeOcrChineseForZhHans(visualEnhancedDisplayedText) : QString();
    const QString visualTranslatedDisplayNormalized =
        visualTranslatedCueUsableAtTime ? normalizeOcrChineseForZhHans(visualDisplayedText) : QString();
    const FinalDisplaySelection finalSelection = selectCurrentFrameFinalDisplay(
        visualCueAtTime,
        visualSourceDisplayNormalized,
        visualTranslatedDisplayNormalized,
        visualEnhancedDisplayNormalized,
        quickCueAtTime,
        quickDisplayedText,
        visualTrackAuthoritativeAtTime);
    const QString finalDisplayedText = finalSelection.text;
    const QString chosenReason = finalSelection.reason;
    const VisualFallbackReport visualFallback = visualFallbackReport(
        visualCueAtTime,
        visualTrackAuthoritativeAtTime,
        visualTrackAuthoritative,
        request.workbenchCanReadImageText,
        request.ocrConfigured,
        request.ocrAutoEnabledForHighQualityCurrentMedia,
        ocrWorkerResult.value(QStringLiteral("degradedReason")).toString(QStringLiteral("local-ocr-no-displayable-text")),
        finalDisplayedText);
    const QString noVisualCueFallbackReason = visualFallback.noVisualCueFallbackReason;
    const QString visualFallbackReason = visualFallback.visualFallbackReason;
    const QString workerSourceKind =
        ocrWorkerResult.value(QStringLiteral("currentCueSourceKind")).toString(
            ocrWorkerResult.value(QStringLiteral("sourceKind")).toString());
    const bool currentFrameUsesWorkbenchVision =
        visualCueAtTime &&
        request.workbenchCanReadImageText &&
        (workerSourceKind == QStringLiteral("workbench-vision") ||
         ocrWorkerResult.value(QStringLiteral("visionApiSucceeded")).toBool(false));
    const QString visualCueSourceKind = reportedVisualCueSourceKind(
        visualSourceCueUsableAtTime,
        visualTranslatedCueUsableAtTime,
        visualEnhancedCueAtTime);
    const QString currentFrameSourceKind = reportedCurrentFrameSourceKind(
        visualCueAtTime,
        currentFrameUsesWorkbenchVision,
        workerSourceKind,
        visualCueSourceKind,
        visualTrackAuthoritativeAtTime);
    const QString currentFrameOcrRawText =
        ocrWorkerResult.value(QStringLiteral("ocrRawText")).toString();
    const QString currentFrameVisualRawText =
        !ocrWorkerResult.value(QStringLiteral("visualRawText")).toString().trimmed().isEmpty()
            ? ocrWorkerResult.value(QStringLiteral("visualRawText")).toString()
            : (currentFrameSourceKind == QStringLiteral("workbench-vision")
                  ? (!visualSourceDisplayedText.isEmpty() ? visualSourceDisplayedText : visualEnhancedDisplayedText)
                  : QString());
    const QJsonObject visualPrecheck{
        { QStringLiteral("workbenchConfigSource"), request.workbenchConfigSource },
        { QStringLiteral("workbenchCanReadImageText"), request.workbenchCanReadImageText },
        { QStringLiteral("visualTextDetected"),
          visualCueAtTime ? QStringLiteral("true") : (visualAttempted ? QStringLiteral("false") : QStringLiteral("unknown")) },
        { QStringLiteral("visualTextDetectedBool"), visualCueAtTime },
        { QStringLiteral("visualTextDetectedAny"), visualTextFound },
        { QStringLiteral("visualCoverageStart"), visualCoverageStartSeconds },
        { QStringLiteral("visualCoverageEnd"), visualCoverageEndSeconds },
        { QStringLiteral("visualCoverageKnown"), visualCoverageKnown },
        { QStringLiteral("candidateScanCoverageStart"), candidateScanCoverageStartSeconds },
        { QStringLiteral("candidateScanCoverageEnd"), candidateScanCoverageEndSeconds },
        { QStringLiteral("candidateScanCoverageKnown"), candidateScanCoverageKnown },
        { QStringLiteral("candidateScanFingerprintMatches"), candidateScanFingerprintMatches },
        { QStringLiteral("visualAuthorityCoverageStart"), visualAuthorityCoverageStartSeconds },
        { QStringLiteral("visualAuthorityCoverageEnd"), visualAuthorityCoverageEndSeconds },
        { QStringLiteral("visualAuthorityCoverageKnown"), visualAuthorityCoverageKnown },
        { QStringLiteral("currentTimeWithinVisualCoverage"), currentTimeWithinVisualCoverage },
        { QStringLiteral("visualSourceCueAtTime"), visualSourceCueUsableAtTime },
        { QStringLiteral("visualRawSourceCueRejectedAsWatermark"), visualSourceCueAtTime && !visualSourceCueUsableAtTime },
        { QStringLiteral("visualEnhancedCueAtTime"), visualEnhancedCueAtTime },
        { QStringLiteral("enhancedCueAllowedAsVisualAtTime"), enhancedCueAllowedAsVisualAtTime },
        { QStringLiteral("primaryVisualTrackAvailable"), primaryVisualTrackAvailable },
        { QStringLiteral("visualCueAtTime"), visualCueAtTime },
        { QStringLiteral("visualTrackGenerated"), visualTrackGenerated },
        { QStringLiteral("visualTrackAuthoritative"), visualTrackAuthoritative },
        { QStringLiteral("visualTrackAuthoritativeAtTime"), visualTrackAuthoritativeAtTime },
        { QStringLiteral("activeVisualCueAtTime"), visualCueAtTime },
        { QStringLiteral("strictVisualCueTiming"), true },
        { QStringLiteral("activeVisualCueStart"), activeVisualCueStartSeconds },
        { QStringLiteral("activeVisualCueEnd"), activeVisualCueEndSeconds },
        { QStringLiteral("quickCueStart"), quickCueStartSeconds },
        { QStringLiteral("quickCueEnd"), quickCueEndSeconds },
        { QStringLiteral("quickCueAtTime"), quickCueAtTime },
        { QStringLiteral("quickDisplayedText"), quickDisplayedText },
        { QStringLiteral("visualSourceDisplayedText"), visualSourceDisplayedText },
        { QStringLiteral("visualEnhancedDisplayedText"), visualEnhancedDisplayedText },
        { QStringLiteral("rawSourceText"), visualSourceDisplayedText },
        { QStringLiteral("translatedText"), visualDisplayedText },
        { QStringLiteral("ocrRawText"), currentFrameOcrRawText },
        { QStringLiteral("visualRawText"), currentFrameVisualRawText },
        { QStringLiteral("visualDisplayedText"), visualDisplayedText },
        { QStringLiteral("finalDisplayedText"), finalDisplayedText },
        { QStringLiteral("visibleDisplayText"), finalDisplayedText },
        { QStringLiteral("visualNoTextSuppressQuickAsr"), visualNoTextSuppressQuickAsr },
        { QStringLiteral("asrSuppressedByVisualNoText"), visualNoTextSuppressQuickAsr && quickCueAtTime },
        { QStringLiteral("staleCueCleared"), visualNoTextSuppressQuickAsr },
        { QStringLiteral("sourceKind"), currentFrameSourceKind },
        { QStringLiteral("currentCueSourceKind"), workerSourceKind },
        { QStringLiteral("chosenSource"), currentFrameSourceKind },
        { QStringLiteral("provider"),
          request.workbenchCanReadImageText ? QStringLiteral("workbench-vision") : QStringLiteral("local_subtitle_ocr") },
        { QStringLiteral("model"), request.workbenchCanReadImageText ? request.translationModel : QStringLiteral("local-ocr-helper") },
        { QStringLiteral("sampleWindow"), QStringLiteral("first-3-4-minutes-or-current-hq-target-window") },
        { QStringLiteral("region"), QStringLiteral("bottom-subtitle-band") },
        { QStringLiteral("confidence"),
          visualCueAtTime ? QJsonValue(0.80) : (visualAttempted ? QJsonValue(0.0) : QJsonValue::Null) },
        { QStringLiteral("chosenReason"),
          visualPrecheckChosenReason(chosenReason, visualFallbackReason) },
        { QStringLiteral("fallbackReason"), visualFallbackReason },
        { QStringLiteral("ocrIntakeJsonPath"),
          ocrWorkerResult.value(QStringLiteral("ocrIntakeJsonPath")).toString(ocrIntakeJsonPath(request.ocrSubtitleCachePath)) }
    };
    const double hqCoverageEndSeconds = std::max({
        enhancedState.value(QStringLiteral("translatedDisplayableCoverageEndSeconds")).toDouble(),
        ocrTranslatedState.value(QStringLiteral("translatedDisplayableCoverageEndSeconds")).toDouble(),
        onlineState.value(QStringLiteral("translatedDisplayableCoverageEndSeconds")).toDouble(),
        longAsrDisplayableCoverageEndSeconds
    });
    const double hqActualFinalCoverageThroughSeconds = std::max({
        enhancedState.value(QStringLiteral("actualFinalCoverageThroughSeconds")).toDouble(request.currentSeconds),
        ocrTranslatedState.value(QStringLiteral("actualFinalCoverageThroughSeconds")).toDouble(request.currentSeconds),
        onlineState.value(QStringLiteral("actualFinalCoverageThroughSeconds")).toDouble(request.currentSeconds),
        longAsrState.value(QStringLiteral("actualFinalCoverageThroughSeconds")).toDouble(request.currentSeconds)
    });
    const double hqProgressOverreportedSeconds =
        std::max(0.0, hqCoverageEndSeconds - hqActualFinalCoverageThroughSeconds);
    const bool hqProgressTruthPass = hqProgressOverreportedSeconds <= 0.25;
    const double hqCoverageSeconds = std::max(0.0, hqActualFinalCoverageThroughSeconds - request.currentSeconds);
    const int hqTranslatedCueCount =
        enhancedState.value(QStringLiteral("displayableCueCount")).toInt() +
        ocrTranslatedState.value(QStringLiteral("displayableCueCount")).toInt() +
        onlineState.value(QStringLiteral("displayableCueCount")).toInt() +
        longAsrDisplayableCueCount;
    const QString activeHqSource = activeHighQualitySource(
        enhancedState.value(QStringLiteral("displayableCueCount")).toInt(),
        enhancedState.value(QStringLiteral("translatedDisplayableCoverageEndSeconds")).toDouble(),
        manualFusionResult.primaryAcceptedSourceType,
        onlineState.value(QStringLiteral("displayableCueCount")).toInt(),
        onlineState.value(QStringLiteral("translatedDisplayableCoverageEndSeconds")).toDouble(),
        ocrTranslatedState.value(QStringLiteral("displayableCueCount")).toInt(),
        ocrTranslatedState.value(QStringLiteral("translatedDisplayableCoverageEndSeconds")).toDouble(),
        longAsrDisplayableCueCount,
        longAsrDisplayableCoverageEndSeconds);
    const QString needReason = highQualityNeedReason(request, quickState, enhancedState);
    const bool hqTargetCoverageReady =
        activeHqSource != QStringLiteral("quick-fallback") &&
        hqTranslatedCueCount > 0 &&
        hqProgressTruthPass &&
        hqCoverageSeconds + 0.5 >= request.highQualityTargetCoverageSeconds;
    const double hqCoverageShortfallSeconds =
        std::max(0.0, request.highQualityTargetCoverageSeconds - hqCoverageSeconds);
    const bool currentMediaEnhancedReady =
        needReason == QStringLiteral("enhanced-cache-ready") && hqTargetCoverageReady;
    QJsonArray acceptanceClasses;
    acceptanceClasses.append(videoClassEntry(
        QStringLiteral("local-or-embedded-subtitle"),
        QStringLiteral("not-proven"),
        QStringLiteral("no-reference-comparison-run"),
        QStringLiteral("quick-or-source-priority"),
        false));
    acceptanceClasses.append(videoClassEntry(
        QStringLiteral("hard-sub-ocr"),
        ocrTranslatedState.value(QStringLiteral("displayableCueCount")).toInt() > 0
            ? QStringLiteral("measured-source-available-not-80-proven")
            : QStringLiteral("not-proven"),
        ocrTranslatedState.value(QStringLiteral("displayableCueCount")).toInt() > 0
            ? QStringLiteral("ocr-translated-cache-present-cue-level-comparison-required")
            : QStringLiteral("ocr-translated-cache-missing-or-blocked"),
        ocrTranslatedState.value(QStringLiteral("displayableCueCount")).toInt() > 0
            ? QStringLiteral("hard-sub-ocr")
            : QStringLiteral("quick-fallback"),
        ocrTranslatedState.value(QStringLiteral("displayableCueCount")).toInt() > 0));
    acceptanceClasses.append(videoClassEntry(
        QStringLiteral("online-or-reference-subtitle"),
        onlineState.value(QStringLiteral("displayableCueCount")).toInt() > 0
            ? QStringLiteral("measured-source-available-not-80-proven")
            : QStringLiteral("not-proven"),
        onlineState.value(QStringLiteral("displayableCueCount")).toInt() > 0
            ? QStringLiteral("online-translated-cache-present-cue-level-comparison-required")
            : onlineFallbackReason,
        onlineState.value(QStringLiteral("displayableCueCount")).toInt() > 0
            ? onlineProviderKind
            : QStringLiteral("quick-fallback"),
        onlineState.value(QStringLiteral("displayableCueCount")).toInt() > 0));
    acceptanceClasses.append(videoClassEntry(
        QStringLiteral("clean-no-subtitle-asr"),
        longAsrDisplayableCueCount > 0
            ? QStringLiteral("measured-chain-available-not-80-proven")
            : QStringLiteral("not-proven"),
        longAsrDisplayableCueCount > 0
            ? QStringLiteral("long-asr-hq-translated-cache-present-reference-comparison-required")
            : longAsrWorkerResult.value(QStringLiteral("fallbackReason")).toString(QStringLiteral("long-asr-not-run")),
        longAsrDisplayableCueCount > 0
            ? QStringLiteral("long-asr-corrected")
            : QStringLiteral("quick-fallback"),
        longAsrDisplayableCueCount > 0));
    acceptanceClasses.append(videoClassEntry(
        QStringLiteral("noisy-no-subtitle-asr"),
        QStringLiteral("not-proven"),
        QStringLiteral("no-noisy-reference-sample-run"),
        QStringLiteral("quick-fallback"),
        false));
    acceptanceClasses.append(videoClassEntry(
        QStringLiteral("provider-or-cache-fallback"),
        anyEnhancementSucceeded ? QStringLiteral("fallback-safe") : QStringLiteral("fallback-quick"),
        fallbackReason.isEmpty() ? QStringLiteral("enhancement-cache-ready") : fallbackReason,
        activeSource,
        false));
    const QString languageEvidenceText =
        !currentFrameVisualRawText.trimmed().isEmpty()
            ? currentFrameVisualRawText
            : (!visualSourceDisplayedText.trimmed().isEmpty()
                  ? visualSourceDisplayedText
                  : (!visualDisplayedText.trimmed().isEmpty()
                        ? visualDisplayedText
                        : quickDisplayedText));
    const QString languageDetected =
        detectSubtitleLanguageForDiagnostics(languageEvidenceText);
    const QString chosenPrimarySource =
        localReferenceState.value(QStringLiteral("displayableCueCount")).toInt() > 0
            ? QStringLiteral("local-or-embedded-subtitle")
            : (onlineState.value(QStringLiteral("displayableCueCount")).toInt() > 0
                  ? QStringLiteral("workbench-online-subtitle")
                  : (visualTrackGenerated
                        ? (request.workbenchCanReadImageText
                              ? QStringLiteral("workbench-vision")
                              : QStringLiteral("local-ocr-fallback"))
                        : (longAsrDisplayableCueCount > 0
                              ? QStringLiteral("asr-fallback")
                              : QStringLiteral("quick-baseline"))));
    const QJsonObject sourceDecision{
        { QStringLiteral("mediaPath"), request.mediaPath },
        { QStringLiteral("mediaFingerprint"), mediaFingerprint },
        { QStringLiteral("mediaIdentity"), mediaIdentity },
        { QStringLiteral("detectedTracks"), QJsonArray{
              QJsonObject{
                  { QStringLiteral("kind"), QStringLiteral("localReference") },
                  { QStringLiteral("path"), request.localReferenceSubtitlePath },
                  { QStringLiteral("available"), request.localReferenceProviderAvailable },
                  { QStringLiteral("cueCount"), localReferenceState.value(QStringLiteral("displayableCueCount")).toInt() }
              },
              QJsonObject{
                  { QStringLiteral("kind"), QStringLiteral("quickBaseline") },
                  { QStringLiteral("path"), request.quickTranslatedVttPath },
                  { QStringLiteral("cueCount"), quickState.value(QStringLiteral("displayableCueCount")).toInt() }
              }
          } },
        { QStringLiteral("onlineSearchResult"), QJsonObject{
              { QStringLiteral("attempted"), onlineWorkerResult.value(QStringLiteral("onlineSearchAttempted")).toBool() },
              { QStringLiteral("candidateCount"), onlineWorkerResult.value(QStringLiteral("onlineSearchResultCount")).toInt() },
              { QStringLiteral("acceptedCount"), onlineWorkerResult.value(QStringLiteral("onlineSearchAcceptedCount")).toInt() },
              { QStringLiteral("fallbackReason"), onlineFallbackReason }
          } },
        { QStringLiteral("visionAttempt"), QJsonObject{
              { QStringLiteral("attempted"), ocrWorkerResult.value(QStringLiteral("visionApiAttempted")).toBool() || request.ocrConfigured || request.ocrAutoEnabledForHighQualityCurrentMedia },
              { QStringLiteral("workbenchCanReadImageText"), request.workbenchCanReadImageText },
              { QStringLiteral("succeeded"), ocrWorkerResult.value(QStringLiteral("visionApiSucceeded")).toBool() },
              { QStringLiteral("model"), request.workbenchCanReadImageText ? request.translationModel : QStringLiteral("local-ocr-helper") },
              { QStringLiteral("cueCount"), ocrState.value(QStringLiteral("displayableCueCount")).toInt() },
              { QStringLiteral("coverageStart"), visualCoverageStartSeconds },
              { QStringLiteral("coverageEnd"), visualCoverageEndSeconds }
          } },
        { QStringLiteral("ocrFallback"), QJsonObject{
              { QStringLiteral("attempted"), request.ocrConfigured || request.ocrAutoEnabledForHighQualityCurrentMedia },
              { QStringLiteral("sourceKind"), request.workbenchCanReadImageText ? QStringLiteral("workbench-vision-or-local-fallback") : QStringLiteral("local-ocr-fallback") },
              { QStringLiteral("cueCount"), ocrTranslatedState.value(QStringLiteral("displayableCueCount")).toInt() },
              { QStringLiteral("fallbackReason"), ocrWorkerResult.value(QStringLiteral("degradedReason")).toString() }
          } },
        { QStringLiteral("asrFallback"), QJsonObject{
              { QStringLiteral("configured"), request.longAsrConfigured },
              { QStringLiteral("usedAsPrimary"), chosenPrimarySource == QStringLiteral("asr-fallback") },
              { QStringLiteral("allowedOnlyAfterNoSubtitleOrVisionFailure"), true },
              { QStringLiteral("candidateAllowed"), longAsrCandidateAllowed },
              { QStringLiteral("cueCount"), longAsrDisplayableCueCount },
              { QStringLiteral("cachedCueCount"), longAsrState.value(QStringLiteral("displayableCueCount")).toInt() }
          } },
        { QStringLiteral("chosenPrimarySource"), chosenPrimarySource },
        { QStringLiteral("reason"),
          visualTrackGenerated
              ? QStringLiteral("visual-or-subtitle-source-preferred-before-asr")
              : (chosenPrimarySource == QStringLiteral("asr-fallback")
                    ? QStringLiteral("subtitle-online-vision-unavailable-asr-fallback")
                    : QStringLiteral("quick-baseline-until-high-quality-source-ready")) },
        { QStringLiteral("languageDetected"), languageDetected },
        { QStringLiteral("sourceKindBreakdown"), QJsonObject{
              { QStringLiteral("localReferenceCueCount"), localReferenceState.value(QStringLiteral("displayableCueCount")).toInt() },
              { QStringLiteral("onlineCueCount"), onlineState.value(QStringLiteral("displayableCueCount")).toInt() },
              { QStringLiteral("visualCueCount"), ocrTranslatedState.value(QStringLiteral("displayableCueCount")).toInt() + ocrState.value(QStringLiteral("displayableCueCount")).toInt() },
              { QStringLiteral("asrCueCount"), longAsrDisplayableCueCount },
              { QStringLiteral("cachedAsrCueCount"), longAsrState.value(QStringLiteral("displayableCueCount")).toInt() },
              { QStringLiteral("quickCueCount"), quickState.value(QStringLiteral("displayableCueCount")).toInt() }
          } }
    };
    const QJsonObject currentMediaHighQuality{
        { QStringLiteral("mediaPath"), request.mediaPath },
        { QStringLiteral("mediaFingerprint"), mediaFingerprint },
        { QStringLiteral("mediaIdentity"), mediaIdentity },
        { QStringLiteral("sourceDecision"), sourceDecision },
        { QStringLiteral("languageDetected"), languageDetected },
        { QStringLiteral("workbenchConfigSource"), request.workbenchConfigSource },
        { QStringLiteral("executeWorkers"), request.executeWorkers },
        { QStringLiteral("manualTriggerAccepted"), manualAllowed },
        { QStringLiteral("workerExecutionAllowed"), workerExecutionAllowed },
        { QStringLiteral("playbackAlreadyRunning"), request.playbackAlreadyRunning },
        { QStringLiteral("allowManualWhilePlayback"), request.allowManualWhilePlayback },
        { QStringLiteral("quick"), quickState },
        { QStringLiteral("enhanced"), enhancedState },
        { QStringLiteral("ocrSource"), ocrState },
        { QStringLiteral("ocrTranslated"), ocrTranslatedState },
        { QStringLiteral("onlineTranslated"), onlineState },
        { QStringLiteral("localReferenceSubtitlePath"), request.localReferenceSubtitlePath },
        { QStringLiteral("localReferenceProviderAvailable"), request.localReferenceProviderAvailable },
        { QStringLiteral("longAsrTranslated"), longAsrState },
        { QStringLiteral("visualPrecheck"), visualPrecheck },
        { QStringLiteral("visualTextDetected"), visualPrecheck.value(QStringLiteral("visualTextDetected")) },
        { QStringLiteral("sourceKind"), currentFrameSourceKind },
        { QStringLiteral("chosenSource"), currentFrameSourceKind },
        { QStringLiteral("visualSourceCueAtTime"), visualSourceCueUsableAtTime },
        { QStringLiteral("visualRawSourceCueRejectedAsWatermark"), visualSourceCueAtTime && !visualSourceCueUsableAtTime },
        { QStringLiteral("visualEnhancedCueAtTime"), visualEnhancedCueAtTime },
        { QStringLiteral("visualCueAtTime"), visualCueAtTime },
        { QStringLiteral("activeVisualCueAtTime"), visualCueAtTime },
        { QStringLiteral("strictVisualCueTiming"), true },
        { QStringLiteral("visualCoverageStart"), visualCoverageStartSeconds },
        { QStringLiteral("visualCoverageEnd"), visualCoverageEndSeconds },
        { QStringLiteral("visualCoverageKnown"), visualCoverageKnown },
        { QStringLiteral("candidateScanCoverageStart"), candidateScanCoverageStartSeconds },
        { QStringLiteral("candidateScanCoverageEnd"), candidateScanCoverageEndSeconds },
        { QStringLiteral("candidateScanCoverageKnown"), candidateScanCoverageKnown },
        { QStringLiteral("candidateScanFingerprintMatches"), candidateScanFingerprintMatches },
        { QStringLiteral("visualAuthorityCoverageStart"), visualAuthorityCoverageStartSeconds },
        { QStringLiteral("visualAuthorityCoverageEnd"), visualAuthorityCoverageEndSeconds },
        { QStringLiteral("visualAuthorityCoverageKnown"), visualAuthorityCoverageKnown },
        { QStringLiteral("currentTimeWithinVisualCoverage"), currentTimeWithinVisualCoverage },
        { QStringLiteral("activeVisualCueStart"), activeVisualCueStartSeconds },
        { QStringLiteral("activeVisualCueEnd"), activeVisualCueEndSeconds },
        { QStringLiteral("quickCueStart"), quickCueStartSeconds },
        { QStringLiteral("quickCueEnd"), quickCueEndSeconds },
        { QStringLiteral("quickCueAtTime"), quickCueAtTime },
        { QStringLiteral("quickDisplayedText"), quickDisplayedText },
        { QStringLiteral("visualSourceDisplayedText"), visualSourceDisplayedText },
        { QStringLiteral("visualEnhancedDisplayedText"), visualEnhancedDisplayedText },
        { QStringLiteral("ocrRawText"), visualPrecheck.value(QStringLiteral("ocrRawText")) },
        { QStringLiteral("visualRawText"), visualPrecheck.value(QStringLiteral("visualRawText")) },
        { QStringLiteral("visualDisplayedText"), visualDisplayedText },
        { QStringLiteral("finalDisplayedText"), finalDisplayedText },
        { QStringLiteral("visualNoTextSuppressQuickAsr"), visualNoTextSuppressQuickAsr },
        { QStringLiteral("asrSuppressedByVisualNoText"), visualNoTextSuppressQuickAsr && quickCueAtTime },
        { QStringLiteral("staleCueCleared"), visualNoTextSuppressQuickAsr },
        { QStringLiteral("visualTrackGenerated"), visualTrackGenerated },
        { QStringLiteral("visualTrackAuthoritative"), visualTrackAuthoritative },
        { QStringLiteral("visualTrackAuthoritativeAtTime"), visualTrackAuthoritativeAtTime },
        { QStringLiteral("hqCoverageSeconds"), hqCoverageSeconds },
        { QStringLiteral("hqCoverageEndSeconds"), hqCoverageEndSeconds },
        { QStringLiteral("reportedTranslatedThroughSeconds"), hqCoverageEndSeconds },
        { QStringLiteral("actualFinalCoverageThroughSeconds"), hqActualFinalCoverageThroughSeconds },
        { QStringLiteral("progressOverreportedSeconds"), hqProgressOverreportedSeconds },
        { QStringLiteral("progressTruthPass"), hqProgressTruthPass },
        { QStringLiteral("targetCoverageSeconds"), request.highQualityTargetCoverageSeconds },
        { QStringLiteral("hqTargetCoverageReady"), hqTargetCoverageReady },
        { QStringLiteral("hqCoverageShortfallSeconds"), hqCoverageShortfallSeconds },
        { QStringLiteral("activeHqSource"), activeHqSource },
        { QStringLiteral("hqTranslatedCueCount"), hqTranslatedCueCount },
        { QStringLiteral("lowConfidenceCueCount"), request.lowConfidenceQuickCueCount },
        { QStringLiteral("lowConfidenceCueRatio"), request.lowConfidenceQuickCueRatio },
        { QStringLiteral("repairAttemptCount"), repairWorkerResult.value(QStringLiteral("attempted")).toBool() ? 1 : 0 },
        { QStringLiteral("qualityScore"), QStringLiteral("not-proven") },
        { QStringLiteral("needReason"), needReason },
        { QStringLiteral("enhancedReadyForCurrentMedia"), currentMediaEnhancedReady },
        { QStringLiteral("activeSource"), activeSource },
        { QStringLiteral("fallbackReason"), fallbackReason.isEmpty() ? needReason : fallbackReason },
        { QStringLiteral("accuracy80State"), QStringLiteral("not-proven") },
        { QStringLiteral("accuracy80Reason"), QStringLiteral("cross-video-reference-evaluation-not-complete") },
        { QStringLiteral("acceptanceClasses"), acceptanceClasses }
    };
    const QJsonObject providerConfigState{
        { QStringLiteral("mediaPath"), request.mediaPath },
        { QStringLiteral("mediaFingerprint"), mediaFingerprint },
        { QStringLiteral("mediaIdentity"), mediaIdentity },
        { QStringLiteral("workbenchConfigSource"), request.workbenchConfigSource },
        { QStringLiteral("onlineConfigured"), request.onlineConfigured },
        { QStringLiteral("onlineReferenceProviderKind"), onlineProviderKind },
        { QStringLiteral("onlineReferenceFallbackReason"), onlineFallbackReason },
        { QStringLiteral("workbenchProviderAvailable"), request.workbenchProviderAvailable },
        { QStringLiteral("workbenchCanSearchOnlineSubtitles"), request.workbenchCanSearchOnlineSubtitles },
        { QStringLiteral("workbenchCanSearchSubtitles"), request.workbenchCanSearchOnlineSubtitles },
        { QStringLiteral("workbenchCanReadImageText"), request.workbenchCanReadImageText },
        { QStringLiteral("workbenchCanTranslate"), request.workbenchCanTranslate },
        { QStringLiteral("workbenchNoOnlineSearchCapability"), request.workbenchNoOnlineSearchCapability },
        { QStringLiteral("translationProviderAvailable"), request.translationProviderAvailable },
        { QStringLiteral("translationProviderKind"), request.translationProviderKind },
        { QStringLiteral("translationModel"), request.translationModel },
        { QStringLiteral("asrProviderAvailable"), request.asrProviderAvailable },
        { QStringLiteral("localReferenceProviderAvailable"), request.localReferenceProviderAvailable },
        { QStringLiteral("onlineApiConfigured"),
          onlineWorkerResult.value(QStringLiteral("onlineApiConfigured")).toBool() },
        { QStringLiteral("ocrConfigured"), request.ocrConfigured },
        { QStringLiteral("ocrAutoEnabledForHighQualityCurrentMedia"),
          request.ocrAutoEnabledForHighQualityCurrentMedia },
        { QStringLiteral("ocrAutoEnableReason"), request.ocrAutoEnableReason },
        { QStringLiteral("longAsrConfigured"), request.longAsrConfigured },
        { QStringLiteral("longAsrCandidateAllowed"), longAsrCandidateAllowed },
        { QStringLiteral("longAsrAutoEnabledForNoSubtitleHighQuality"),
          request.longAsrAutoEnabledForNoSubtitleHighQuality },
        { QStringLiteral("ocrTranslatedCachePath"), request.ocrTranslatedCachePath },
        { QStringLiteral("longAsrSourceCachePath"), request.longAsrSourceCachePath },
        { QStringLiteral("longAsrTranslatedCachePath"), request.longAsrTranslatedCachePath },
        { QStringLiteral("ocrTranslationReason"),
          ocrWorkerResult.value(QStringLiteral("ocrTranslationReason")).toString() },
        { QStringLiteral("repairConfigured"), request.lowConfidenceRepairConfigured },
        { QStringLiteral("localRepairAvailable"), true },
        { QStringLiteral("fusionConfigured"), request.fusionConfigured },
        { QStringLiteral("onlineWorkerReason"),
          onlineWorkerResult.value(QStringLiteral("degradedReason")).toString() },
        { QStringLiteral("onlineSearchAttempted"),
          onlineWorkerResult.value(QStringLiteral("onlineSearchAttempted")).toBool() },
        { QStringLiteral("onlineSearchResultCount"),
          onlineWorkerResult.value(QStringLiteral("onlineSearchResultCount")).toInt() },
        { QStringLiteral("onlineSearchAcceptedCount"),
          onlineWorkerResult.value(QStringLiteral("onlineSearchAcceptedCount")).toInt() },
        { QStringLiteral("visionApiAttempted"),
          ocrWorkerResult.value(QStringLiteral("visionApiAttempted")).toBool() },
        { QStringLiteral("visionApiSucceeded"),
          ocrWorkerResult.value(QStringLiteral("visionApiSucceeded")).toBool() },
        { QStringLiteral("visionApiFailed"),
          ocrWorkerResult.value(QStringLiteral("visionApiFailed")).toBool() },
        { QStringLiteral("visionModel"),
          ocrWorkerResult.value(QStringLiteral("visionModel")).toString(request.translationModel) },
        { QStringLiteral("visionFrameCount"),
          ocrWorkerResult.value(QStringLiteral("visionFrameCount")).toInt() },
        { QStringLiteral("visionCueCount"),
          ocrWorkerResult.value(QStringLiteral("visionCueCount")).toInt() },
        { QStringLiteral("onlineTranslationReason"),
          onlineTranslationResult.value(QStringLiteral("degradedReason")).toString() },
        { QStringLiteral("ocrReason"),
          ocrWorkerResult.value(QStringLiteral("degradedReason")).toString() },
        { QStringLiteral("longAsrReason"),
          longAsrWorkerResult.value(QStringLiteral("fallbackReason")).toString() },
        { QStringLiteral("repairReason"),
          repairWorkerResult.value(QStringLiteral("degradedReason")).toString() }
    };
    return QJsonObject{
        { QStringLiteral("policy"), policy() },
        { QStringLiteral("selectedMode"), mode },
        { QStringLiteral("mediaPath"), request.mediaPath },
        { QStringLiteral("mediaFingerprint"), mediaFingerprint },
        { QStringLiteral("mediaIdentity"), mediaIdentity },
        { QStringLiteral("activeMode"), mode },
        { QStringLiteral("activeSource"), activeSource },
        { QStringLiteral("workbenchConfigSource"), request.workbenchConfigSource },
        { QStringLiteral("enhancementAttempted"), enhancementAttempted },
        { QStringLiteral("enhancementSucceeded"), anyEnhancementSucceeded },
        { QStringLiteral("fallbackReason"), fallbackReason },
        { QStringLiteral("changedCueCount"),
          manualFusionResult.success ? manualFusionResult.changedCueCount : repairChangedCueCount },
        { QStringLiteral("providerConfigState"), providerConfigState },
        { QStringLiteral("manualRequested"), request.manualRequested },
        { QStringLiteral("manualTriggerAccepted"), manualAllowed },
        { QStringLiteral("executeWorkers"), request.executeWorkers },
        { QStringLiteral("workerExecutionAllowed"), workerExecutionAllowed },
        { QStringLiteral("currentMediaHighQuality"), currentMediaHighQuality },
        { QStringLiteral("visualNoTextSuppressQuickAsr"), visualNoTextSuppressQuickAsr },
        { QStringLiteral("visualTrackGenerated"), visualTrackGenerated },
        { QStringLiteral("visualTrackAuthoritative"), visualTrackAuthoritative },
        { QStringLiteral("accuracyAcceptance"), QJsonObject{
              { QStringLiteral("target"), QStringLiteral("cross-video-80-percent-semantic-accuracy") },
              { QStringLiteral("state"), QStringLiteral("not-proven") },
              { QStringLiteral("mustNotClaim80Percent"), true },
              { QStringLiteral("classes"), acceptanceClasses }
          } },
        { QStringLiteral("configuredEnhancementLayerPresent"), configured },
        { QStringLiteral("taskState"), state },
        { QStringLiteral("willStartBackgroundWorker"), workerExecutionAllowed },
        { QStringLiteral("willStartOnlineSearch"),
          onlineWorkerResult.value(QStringLiteral("onlineNetworkStarted")).toBool() },
        { QStringLiteral("willRunManualOnlineWorker"),
          onlineWorkerResult.value(QStringLiteral("attempted")).toBool() },
        { QStringLiteral("manualOnlineWorker"), onlineWorkerResult },
        { QStringLiteral("manualOnlineTranslationWorker"), onlineTranslationResult },
        { QStringLiteral("manualOcrWorker"), ocrWorkerResult },
        { QStringLiteral("manualLongAsrWorker"), longAsrWorkerResult },
        { QStringLiteral("manualRepairWorker"), repairWorkerResult },
        { QStringLiteral("onlineSourceIntake"), onlineSourceIntake },
        { QStringLiteral("onlineWorkerDefaultDisabled"), true },
        { QStringLiteral("onlineManualAllowed"),
          onlineWorkerResult.value(QStringLiteral("onlineManualAllowed")).toBool() },
        { QStringLiteral("networkDisabledInQuick"), !manualAllowed },
        { QStringLiteral("onlineReferenceProviderKind"), onlineProviderKind },
        { QStringLiteral("onlineReferenceFallbackReason"), onlineFallbackReason },
        { QStringLiteral("workbenchProviderAvailable"), request.workbenchProviderAvailable },
        { QStringLiteral("workbenchCanSearchOnlineSubtitles"), request.workbenchCanSearchOnlineSubtitles },
        { QStringLiteral("workbenchCanSearchSubtitles"), request.workbenchCanSearchOnlineSubtitles },
        { QStringLiteral("workbenchCanReadImageText"), request.workbenchCanReadImageText },
        { QStringLiteral("workbenchCanTranslate"), request.workbenchCanTranslate },
        { QStringLiteral("workbenchNoOnlineSearchCapability"), request.workbenchNoOnlineSearchCapability },
        { QStringLiteral("visualPrecheck"), visualPrecheck },
        { QStringLiteral("onlineSearchAttempted"),
          onlineWorkerResult.value(QStringLiteral("onlineSearchAttempted")).toBool() },
        { QStringLiteral("onlineSearchEnabled"),
          onlineWorkerResult.value(QStringLiteral("onlineSearchEnabled")).toBool(request.onlineConfigured) },
        { QStringLiteral("onlineProvider"),
          onlineWorkerResult.value(QStringLiteral("onlineProvider")).toString(onlineProviderKind) },
        { QStringLiteral("onlineQuery"),
          onlineWorkerResult.value(QStringLiteral("query")).isObject()
              ? onlineWorkerResult.value(QStringLiteral("query")).toObject()
              : QJsonObject{} },
        { QStringLiteral("onlineCandidateCount"),
          onlineWorkerResult.value(QStringLiteral("candidateCount")).toInt() },
        { QStringLiteral("onlineSelectedCandidate"),
          onlineWorkerResult.value(QStringLiteral("selectedCandidate")) },
        { QStringLiteral("onlineSourceKind"),
          onlineWorkerResult.value(QStringLiteral("onlineSourceKind")).toString(QStringLiteral("online")) },
        { QStringLiteral("onlineLanguage"),
          onlineWorkerResult.value(QStringLiteral("onlineLanguage")).toString() },
        { QStringLiteral("onlineCoverage"),
          onlineWorkerResult.value(QStringLiteral("onlineCoverage")).toDouble() },
        { QStringLiteral("onlineAlignmentScore"),
          onlineWorkerResult.value(QStringLiteral("alignmentScore")).toDouble() },
        { QStringLiteral("onlineOffsetMs"),
          onlineWorkerResult.value(QStringLiteral("offsetMs")).toInt() },
        { QStringLiteral("onlineRejectReason"),
          onlineWorkerResult.value(QStringLiteral("rejectReason")).toString() },
        { QStringLiteral("onlineSearchSucceeded"),
          onlineWorkerResult.value(QStringLiteral("onlineSearchSucceeded")).toBool() },
        { QStringLiteral("onlineSearchResultCount"),
          onlineWorkerResult.value(QStringLiteral("onlineSearchResultCount")).toInt() },
        { QStringLiteral("onlineSearchAcceptedCount"),
          onlineWorkerResult.value(QStringLiteral("onlineSearchAcceptedCount")).toInt() },
        { QStringLiteral("onlineApiConfigured"),
          onlineWorkerResult.value(QStringLiteral("onlineApiConfigured")).toBool() },
        { QStringLiteral("onlineNetworkStarted"),
          onlineWorkerResult.value(QStringLiteral("onlineNetworkStarted")).toBool() },
        { QStringLiteral("onlineCacheWritten"),
          onlineWorkerResult.value(QStringLiteral("onlineCacheWritten")).toBool() },
        { QStringLiteral("onlineTranslatedCachePath"), request.onlineTranslatedCachePath },
        { QStringLiteral("onlineTranslatedCacheWritten"),
          onlineTranslationResult.value(QStringLiteral("translatedOnlineCacheWritten")).toBool() },
        { QStringLiteral("onlineTranslatedCueCount"),
          onlineTranslationResult.value(QStringLiteral("translatedOnlineCueCount")).toInt() },
        { QStringLiteral("onlineTranslatedDisplayableCoverageEndSeconds"),
          onlineTranslationResult.value(QStringLiteral("translatedDisplayableCoverageEndSeconds")).toDouble() },
        { QStringLiteral("degradedReason"),
          onlineWorkerResult.value(QStringLiteral("degradedReason")).toString() },
        { QStringLiteral("willStartOcr"),
          ocrWorkerResult.value(QStringLiteral("attempted")).toBool() },
        { QStringLiteral("ocrCacheWritten"),
          ocrWorkerResult.value(QStringLiteral("ocrCacheWritten")).toBool() },
        { QStringLiteral("ocrTranslatedCachePath"), request.ocrTranslatedCachePath },
        { QStringLiteral("ocrTranslatedCacheWritten"),
          ocrWorkerResult.value(QStringLiteral("ocrTranslatedCacheWritten")).toBool() },
        { QStringLiteral("ocrTranslatedCueCount"),
          ocrWorkerResult.value(QStringLiteral("ocrTranslatedCueCount")).toInt() },
        { QStringLiteral("willStartFusion"), manualFusionResult.success },
        { QStringLiteral("willRunLocalFusionWorker"), manualAllowed },
        { QStringLiteral("fusionDeferredForOnlineSourceIntake"), false },
        { QStringLiteral("fusionUsedTranslatedOnlineCache"), translatedOnlineReady },
        { QStringLiteral("localFusionWorker"), manualFusionResult.toJson() },
        { QStringLiteral("willStartLowConfidenceRepair"),
          repairWorkerResult.value(QStringLiteral("attempted")).toBool() },
        { QStringLiteral("repairCacheWritten"),
          repairWorkerResult.value(QStringLiteral("repairCacheWritten")).toBool() },
        { QStringLiteral("defaultAutoRun"), false },
        { QStringLiteral("quickPathAutoRun"), false },
        { QStringLiteral("quickPathBehaviorChanged"), false },
        { QStringLiteral("quickGenerationBusy"), request.quickGenerationBusy },
        { QStringLiteral("continuationScheduled"), request.continuationScheduled },
        { QStringLiteral("highQualityWaitActive"), request.highQualityWaitActive },
        { QStringLiteral("playbackAlreadyRunning"), request.playbackAlreadyRunning },
        { QStringLiteral("currentSeconds"), request.currentSeconds },
        { QStringLiteral("quickCoverageEndSeconds"), request.quickCoverageEndSeconds },
        { QStringLiteral("quickCueCount"), request.quickCueCount },
        { QStringLiteral("quickTranslatedVttPath"), request.quickTranslatedVttPath },
        { QStringLiteral("refinedTranslatedVttPath"), request.refinedTranslatedVttPath },
        { QStringLiteral("onlineSubtitleCachePath"), request.onlineSubtitleCachePath },
        { QStringLiteral("onlineSourceCacheFound"),
          onlineSourceIntake.value(QStringLiteral("onlineSourceCacheFound")).toBool() },
        { QStringLiteral("onlineSourceCueCount"),
          onlineSourceIntake.value(QStringLiteral("onlineSourceCueCount")).toInt() },
        { QStringLiteral("onlineSourceCoverageSeconds"),
          onlineSourceIntake.value(QStringLiteral("onlineSourceCoverageSeconds")).toDouble() },
        { QStringLiteral("matchedQuickCueCount"),
          onlineSourceIntake.value(QStringLiteral("matchedQuickCueCount")).toInt() },
        { QStringLiteral("unmatchedOnlineCueCount"),
          onlineSourceIntake.value(QStringLiteral("unmatchedOnlineCueCount")).toInt() },
        { QStringLiteral("timingAlignmentPassed"),
          onlineSourceIntake.value(QStringLiteral("timingAlignmentPassed")).toBool() },
        { QStringLiteral("sourceOnlyDisplayable"),
          onlineSourceIntake.value(QStringLiteral("sourceOnlyDisplayable")).toBool() },
        { QStringLiteral("displayCoverageDelta"),
          onlineSourceIntake.value(QStringLiteral("displayCoverageDelta")).toDouble() },
        { QStringLiteral("quickCoverageChanged"),
          onlineSourceIntake.value(QStringLiteral("quickCoverageChanged")).toBool() },
        { QStringLiteral("rejectedForDisplay"),
          onlineSourceIntake.value(QStringLiteral("rejectedForDisplay")).toString() },
        { QStringLiteral("ocrSubtitleCachePath"), request.ocrSubtitleCachePath },
        { QStringLiteral("enhancedTranslatedVttPath"), request.enhancedTranslatedVttPath },
        { QStringLiteral("repairTranslatedVttPath"), request.repairTranslatedVttPath },
        { QStringLiteral("writesQuickZhSidecar"), false },
        { QStringLiteral("usesQuickApiQuota"), false },
        { QStringLiteral("usesQuickWorkerLane"), false },
        { QStringLiteral("writesIndependentEnhancedCacheOnly"),
          manualFusionResult.success },
        { QStringLiteral("cueLevelMergeOnly"), true },
        { QStringLiteral("wholeTrackReplaceQuickAllowed"), false },
        { QStringLiteral("fallbackToQuick"), true },
        { QStringLiteral("degradedToQuick"), manualAllowed ? !manualFusionResult.success : true },
        { QStringLiteral("longWindowAsr"), longAsrWorkerResult },
        { QStringLiteral("asrFallbackPolicy"), QStringLiteral("run-only-after-local-online-visual-subtitle-sources-fail-or-are-unavailable") },
        { QStringLiteral("cancelable"), manualAllowed },
        { QStringLiteral("cancelDecision"), manualAllowed
              ? QStringLiteral("cancelable-local-worker-boundary")
              : QStringLiteral("no-active-enhancement-task") },
        { QStringLiteral("guardConsumersStayReadOnly"), true },
        { QStringLiteral("layers"), layers }
    };
}

QJsonObject TranslationEnhancementScheduler::_runManualOnlineWorker(
    const TranslationEnhancementScheduleRequest& request,
    bool manualAllowed) const
{
    const QString providerKind = onlineReferenceProviderKind(request);
    const QString fallbackReason = onlineReferenceFallbackReason(request);
    const QString onlineProvider = request.onlineProvider.trimmed().isEmpty()
        ? QStringLiteral("opensubtitles-compatible")
        : request.onlineProvider.trimmed();
    const bool apiConfigured =
        request.onlineConfigured &&
        (request.onlineMockMode || !request.onlineApiKey.trimmed().isEmpty());
    QJsonObject result{
        { QStringLiteral("attempted"), false },
        { QStringLiteral("success"), false },
        { QStringLiteral("onlineSearchEnabled"), request.onlineConfigured },
        { QStringLiteral("onlineProvider"), onlineProvider },
        { QStringLiteral("onlineEndpointConfigured"), !request.onlineBaseUrl.trimmed().isEmpty() },
        { QStringLiteral("query"), QJsonObject{
              { QStringLiteral("filename"), QFileInfo(request.mediaPath).fileName() },
              { QStringLiteral("duration"), qRound(request.currentSeconds + request.highQualityTargetCoverageSeconds) },
              { QStringLiteral("language"), request.onlineLanguage.trimmed().isEmpty()
                    ? QStringLiteral("zh,ja,en") : request.onlineLanguage.trimmed() }
          } },
        { QStringLiteral("candidateCount"), 0 },
        { QStringLiteral("selectedCandidate"), QJsonValue::Null },
        { QStringLiteral("onlineLanguage"), QString() },
        { QStringLiteral("onlineCoverage"), 0.0 },
        { QStringLiteral("alignmentScore"), 0.0 },
        { QStringLiteral("offsetMs"), 0 },
        { QStringLiteral("onlineSearchAttempted"), false },
        { QStringLiteral("onlineSearchSucceeded"), false },
        { QStringLiteral("onlineSearchResultCount"), 0 },
        { QStringLiteral("onlineSearchAcceptedCount"), 0 },
        { QStringLiteral("onlineSearchReportPath"), onlineSearchJsonPath(request.onlineSubtitleCachePath) },
        { QStringLiteral("onlineReferenceProviderKind"), providerKind },
        { QStringLiteral("onlineReferenceFallbackReason"), fallbackReason },
        { QStringLiteral("workbenchProviderAvailable"), request.workbenchProviderAvailable },
        { QStringLiteral("workbenchCanSearchOnlineSubtitles"), request.workbenchCanSearchOnlineSubtitles },
        { QStringLiteral("workbenchCanSearchSubtitles"), request.workbenchCanSearchOnlineSubtitles },
        { QStringLiteral("workbenchNoOnlineSearchCapability"), request.workbenchNoOnlineSearchCapability },
        { QStringLiteral("networkDisabledInQuick"), !manualAllowed },
        { QStringLiteral("onlineWorkerDefaultDisabled"), true },
        { QStringLiteral("onlineManualAllowed"), manualAllowed },
        { QStringLiteral("onlineApiConfigured"), apiConfigured },
        { QStringLiteral("onlineNetworkStarted"), false },
        { QStringLiteral("onlineCacheWritten"), false },
        { QStringLiteral("onlineCachePath"), request.onlineSubtitleCachePath },
        { QStringLiteral("onlineSourceKind"), QStringLiteral("opensubtitles-rest") },
        { QStringLiteral("mockMode"), request.onlineMockMode },
        { QStringLiteral("writesQuickZhSidecar"), false },
        { QStringLiteral("usesQuickApiQuota"), false },
        { QStringLiteral("usesQuickWorkerLane"), false },
        { QStringLiteral("usesAsrOcrTranslationRefineRepair"), false },
        { QStringLiteral("usesWebScraping"), false },
        { QStringLiteral("wholeTrackReplaceQuickAllowed"), false },
        { QStringLiteral("quickPathBehaviorChanged"), false },
        { QStringLiteral("fallbackToQuick"), true }
    };

    if (!manualAllowed) {
        result.insert(QStringLiteral("degradedReason"),
                      request.playbackMode == TranslationPlaybackStrategy::highQualityModeId()
                          ? QStringLiteral("manual-trigger-required-or-playback-running")
                          : QStringLiteral("disabled-in-quick-playback"));
        return result;
    }
    result.insert(QStringLiteral("attempted"), true);
    result.insert(QStringLiteral("onlineSearchAttempted"), true);

    if (providerKind == QStringLiteral("local-reference-provider")) {
        const QVector<GeneratedSubtitleCue> referenceCues =
            SubtitleGenerationService::readSubtitleFile(request.localReferenceSubtitlePath);
        QString error;
        const bool written = !referenceCues.isEmpty() &&
            _writeRepairCache(referenceCues, request.onlineSubtitleCachePath, &error);
        const QJsonObject report{
            { QStringLiteral("providerKind"), providerKind },
            { QStringLiteral("onlineProvider"), providerKind },
            { QStringLiteral("onlineSearchEnabled"), true },
            { QStringLiteral("mediaPath"), request.mediaPath },
            { QStringLiteral("localReferenceSubtitlePath"), request.localReferenceSubtitlePath },
            { QStringLiteral("sentMinimalQueryOnly"), true },
            { QStringLiteral("uploadedMediaPayload"), false },
            { QStringLiteral("resultCount"), written ? 1 : 0 },
            { QStringLiteral("acceptedCount"), written ? 1 : 0 },
            { QStringLiteral("candidateCount"), written ? 1 : 0 },
            { QStringLiteral("selectedCandidate"), written ? QJsonValue(QJsonObject{
                  { QStringLiteral("provider"), providerKind },
                  { QStringLiteral("confidence"), 1.0 },
                  { QStringLiteral("language"), QStringLiteral("local-reference") },
                  { QStringLiteral("sourceUrl"), request.localReferenceSubtitlePath },
                  { QStringLiteral("license"), QStringLiteral("local-user-file") },
                  { QStringLiteral("status"), QStringLiteral("accepted") }
              }) : QJsonValue::Null },
            { QStringLiteral("alignmentScore"), written ? 1.0 : 0.0 },
            { QStringLiteral("offsetMs"), 0 },
            { QStringLiteral("fallbackReason"), written ? QStringLiteral("local-reference-accepted") : QStringLiteral("local-reference-missing-or-unreadable") }
        };
        writeJsonFile(onlineSearchJsonPath(request.onlineSubtitleCachePath), report);
        result.insert(QStringLiteral("success"), written);
        result.insert(QStringLiteral("onlineSearchSucceeded"), written);
        result.insert(QStringLiteral("onlineSearchResultCount"), written ? 1 : 0);
        result.insert(QStringLiteral("onlineSearchAcceptedCount"), written ? 1 : 0);
        result.insert(QStringLiteral("onlineCacheWritten"), written);
        result.insert(QStringLiteral("onlineNetworkStarted"), false);
        result.insert(QStringLiteral("candidateCount"), written ? 1 : 0);
        result.insert(QStringLiteral("selectedCandidate"), written ? QJsonValue(QJsonObject{
            { QStringLiteral("provider"), providerKind },
            { QStringLiteral("confidence"), 1.0 },
            { QStringLiteral("language"), QStringLiteral("local-reference") },
            { QStringLiteral("sourceUrl"), request.localReferenceSubtitlePath },
            { QStringLiteral("license"), QStringLiteral("local-user-file") },
            { QStringLiteral("status"), QStringLiteral("accepted") }
        }) : QJsonValue::Null);
        result.insert(QStringLiteral("onlineLanguage"), written ? QStringLiteral("local-reference") : QString());
        result.insert(QStringLiteral("alignmentScore"), written ? 1.0 : 0.0);
        result.insert(QStringLiteral("offsetMs"), 0);
        result.insert(QStringLiteral("usesWebScraping"), false);
        result.insert(QStringLiteral("degradedReason"),
                      written ? QStringLiteral("local-reference-source-cache-written")
                              : (error.isEmpty() ? QStringLiteral("local-reference-missing") : error));
        return result;
    }

    if (providerKind == QStringLiteral("workbench-online-reference-provider")) {
        if (request.onlineSubtitleCachePath.trimmed().isEmpty()) {
            result.insert(QStringLiteral("degradedReason"), QStringLiteral("online-cache-path-missing"));
            return result;
        }
        const QString helper = request.onlineHelperScriptPath.trimmed().isEmpty()
            ? defaultOnlineHelperPath()
            : request.onlineHelperScriptPath;
        if (!QFileInfo::exists(helper)) {
            result.insert(QStringLiteral("degradedReason"), QStringLiteral("online-helper-script-missing"));
            result.insert(QStringLiteral("helperScriptPath"), helper);
            return result;
        }
        const QString python = QProcessEnvironment::systemEnvironment()
            .value(QStringLiteral("SUBTITLE_ONLINE_PYTHON"), QStringLiteral("python"));
        QStringList arguments{
            helper,
            QStringLiteral("--media"), request.mediaPath,
            QStringLiteral("--output"), request.onlineSubtitleCachePath,
            QStringLiteral("--search-report"), onlineSearchJsonPath(request.onlineSubtitleCachePath),
            QStringLiteral("--language"), request.onlineLanguage.trimmed().isEmpty()
                ? QStringLiteral("zh,ja,en")
                : request.onlineLanguage.trimmed(),
            QStringLiteral("--provider"), QStringLiteral("workbench-api"),
            QStringLiteral("--duration"), QString::number(qMax(0, qRound(request.currentSeconds + request.highQualityTargetCoverageSeconds))),
            QStringLiteral("--file-size"), QString::number(qMax<qint64>(0, QFileInfo(request.mediaPath).size())),
            QStringLiteral("--base-url"), request.translationBaseUrl.trimmed(),
            QStringLiteral("--model"), request.translationModel.trimmed()
        };

        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
        env.insert(QStringLiteral("SUBTITLE_WORKBENCH_API_KEY"), request.translationApiKey.trimmed());
        env.insert(QStringLiteral("SUBTITLE_WORKBENCH_BASE_URL"), request.translationBaseUrl.trimmed());
        env.insert(QStringLiteral("SUBTITLE_WORKBENCH_MODEL"), request.translationModel.trimmed());
        process.setProcessEnvironment(env);
        process.start(python, arguments);
        result.insert(QStringLiteral("onlineNetworkStarted"), process.waitForStarted(5000));
        if (!result.value(QStringLiteral("onlineNetworkStarted")).toBool()) {
            result.insert(QStringLiteral("degradedReason"), QStringLiteral("workbench-online-worker-start-failed"));
            result.insert(QStringLiteral("helperScriptPath"), helper);
            return result;
        }
        JobContext job(70000);
        const ProcessOutcome processOutcome = job.waitForProcess(process, 25, 70000);
        if (processOutcome.state == JobState::TimedOut) {
            result.insert(QStringLiteral("degradedReason"), QStringLiteral("workbench-online-worker-timeout"));
            return result;
        }
        const QString stdoutText = QString::fromUtf8(processOutcome.standardOutput).trimmed();
        const QJsonDocument doc = QJsonDocument::fromJson(stdoutText.toUtf8());
        const QJsonObject helperJson = doc.isObject() ? doc.object() : QJsonObject{};
        const bool cacheWritten =
            processOutcome.succeeded() &&
            QFileInfo::exists(request.onlineSubtitleCachePath) &&
            !SubtitleGenerationService::readSubtitleFile(request.onlineSubtitleCachePath).isEmpty();
        result.insert(QStringLiteral("success"), cacheWritten);
        result.insert(QStringLiteral("onlineSearchSucceeded"), cacheWritten);
        result.insert(QStringLiteral("onlineCacheWritten"), cacheWritten);
        result.insert(QStringLiteral("onlineProvider"), QStringLiteral("workbench-api"));
        result.insert(QStringLiteral("onlineSourceKind"), QStringLiteral("workbench-online-reference"));
        result.insert(QStringLiteral("workbenchConfigSource"), request.workbenchConfigSource);
        result.insert(QStringLiteral("workbenchCanSearchSubtitles"), true);
        result.insert(QStringLiteral("workbenchNoOnlineSearchCapability"), false);
        result.insert(QStringLiteral("onlineSearchResultCount"),
                      helperJson.value(QStringLiteral("onlineSearchResultCount")).toInt(cacheWritten ? 1 : 0));
        result.insert(QStringLiteral("onlineSearchAcceptedCount"),
                      helperJson.value(QStringLiteral("onlineSearchAcceptedCount")).toInt(cacheWritten ? 1 : 0));
        result.insert(QStringLiteral("candidateCount"),
                      helperJson.value(QStringLiteral("candidateCount")).toInt());
        result.insert(QStringLiteral("selectedCandidate"),
                      helperJson.value(QStringLiteral("selectedCandidate")));
        result.insert(QStringLiteral("onlineLanguage"),
                      helperJson.value(QStringLiteral("onlineLanguage")).toString());
        result.insert(QStringLiteral("onlineCoverage"),
                      helperJson.value(QStringLiteral("onlineCoverage")).toDouble());
        result.insert(QStringLiteral("alignmentScore"),
                      helperJson.value(QStringLiteral("alignmentScore")).toDouble());
        result.insert(QStringLiteral("offsetMs"),
                      helperJson.value(QStringLiteral("offsetMs")).toInt());
        result.insert(QStringLiteral("rejectReason"),
                      helperJson.value(QStringLiteral("rejectReason")).toString());
        result.insert(QStringLiteral("fallbackReason"),
                      helperJson.value(QStringLiteral("fallbackReason")).toString());
        result.insert(QStringLiteral("helperExitCode"), processOutcome.exitCode);
        result.insert(QStringLiteral("helperScriptPath"), helper);
        result.insert(QStringLiteral("helperResult"), helperJson);
        result.insert(QStringLiteral("degradedReason"),
                      cacheWritten
                          ? QStringLiteral("workbench-online-source-cache-written")
                          : helperJson.value(QStringLiteral("error")).toString(
                                QStringLiteral("workbench-online-search-failed")));
        return result;
    }

    if (!request.onlineConfigured) {
        const QString reason = request.workbenchProviderAvailable
            ? QStringLiteral("workbench-online-search-api-not-configured")
            : QStringLiteral("online-search-provider-not-configured");
        result.insert(QStringLiteral("degradedReason"), reason);
        result.insert(QStringLiteral("fallbackReason"), reason);
        writeJsonFile(onlineSearchJsonPath(request.onlineSubtitleCachePath), QJsonObject{
            { QStringLiteral("success"), false },
            { QStringLiteral("onlineSearchEnabled"), false },
            { QStringLiteral("onlineProvider"), request.workbenchProviderAvailable ? QStringLiteral("workbench-api") : QStringLiteral("not-configured") },
            { QStringLiteral("workbenchProviderAvailable"), request.workbenchProviderAvailable },
            { QStringLiteral("workbenchCanSearchSubtitles"), request.workbenchCanSearchOnlineSubtitles },
            { QStringLiteral("workbenchNoOnlineSearchCapability"), request.workbenchNoOnlineSearchCapability },
            { QStringLiteral("candidateCount"), 0 },
            { QStringLiteral("rejectReason"), reason },
            { QStringLiteral("fallbackReason"), reason },
            { QStringLiteral("uploadedMediaPayload"), false }
        });
        return result;
    }
    if (request.onlineSubtitleCachePath.trimmed().isEmpty()) {
        result.insert(QStringLiteral("degradedReason"), QStringLiteral("online-cache-path-missing"));
        return result;
    }

    if (request.onlineMockMode) {
        QString error;
        const bool written = _writeMockOnlineCache(
            request.quickTranslatedVttPath,
            request.onlineSubtitleCachePath,
            request.currentSeconds,
            &error);
        result.insert(QStringLiteral("success"), written);
        result.insert(QStringLiteral("onlineSearchSucceeded"), written);
        result.insert(QStringLiteral("onlineSearchResultCount"), written ? 1 : 0);
        result.insert(QStringLiteral("onlineSearchAcceptedCount"), written ? 1 : 0);
        result.insert(QStringLiteral("onlineCacheWritten"), written);
        writeJsonFile(onlineSearchJsonPath(request.onlineSubtitleCachePath), QJsonObject{
            { QStringLiteral("providerKind"), QStringLiteral("mock-online-reference-provider") },
            { QStringLiteral("onlineProvider"), QStringLiteral("mock-online-reference-provider") },
            { QStringLiteral("onlineSearchEnabled"), true },
            { QStringLiteral("attempted"), true },
            { QStringLiteral("success"), written },
            { QStringLiteral("resultCount"), written ? 1 : 0 },
            { QStringLiteral("acceptedCount"), written ? 1 : 0 },
            { QStringLiteral("candidateCount"), written ? 1 : 0 },
            { QStringLiteral("selectedCandidate"), written ? QJsonValue(QJsonObject{
                  { QStringLiteral("provider"), QStringLiteral("mock-online-reference-provider") },
                  { QStringLiteral("confidence"), 0.95 },
                  { QStringLiteral("language"), QStringLiteral("zh") },
                  { QStringLiteral("status"), QStringLiteral("mock") }
              }) : QJsonValue::Null },
            { QStringLiteral("alignmentScore"), written ? 0.95 : 0.0 },
            { QStringLiteral("offsetMs"), 0 },
            { QStringLiteral("uploadedMediaPayload"), false },
            { QStringLiteral("fallbackReason"), written ? QStringLiteral("mock-online-source-cache-written") : error }
        });
        result.insert(QStringLiteral("candidateCount"), written ? 1 : 0);
        result.insert(QStringLiteral("selectedCandidate"), written ? QJsonValue(QJsonObject{
            { QStringLiteral("provider"), QStringLiteral("mock-online-reference-provider") },
            { QStringLiteral("confidence"), 0.95 },
            { QStringLiteral("language"), QStringLiteral("zh") },
            { QStringLiteral("status"), QStringLiteral("mock") }
        }) : QJsonValue::Null);
        result.insert(QStringLiteral("onlineLanguage"), written ? QStringLiteral("zh") : QString());
        result.insert(QStringLiteral("alignmentScore"), written ? 0.95 : 0.0);
        result.insert(QStringLiteral("offsetMs"), 0);
        result.insert(QStringLiteral("degradedReason"),
                      written ? QStringLiteral("mock-online-source-cache-written")
                              : (error.isEmpty() ? QStringLiteral("mock-online-cache-write-failed") : error));
        return result;
    }

    if (request.onlineApiKey.trimmed().isEmpty()) {
        result.insert(QStringLiteral("degradedReason"), QStringLiteral("online-search-api-key-missing"));
        result.insert(QStringLiteral("fallbackReason"), QStringLiteral("online-search-api-key-missing"));
        writeJsonFile(onlineSearchJsonPath(request.onlineSubtitleCachePath), QJsonObject{
            { QStringLiteral("success"), false },
            { QStringLiteral("onlineSearchEnabled"), true },
            { QStringLiteral("onlineProvider"), onlineProvider },
            { QStringLiteral("candidateCount"), 0 },
            { QStringLiteral("candidates"), QJsonArray{} },
            { QStringLiteral("selectedCandidate"), QJsonValue::Null },
            { QStringLiteral("rejectReason"), QStringLiteral("online-search-api-key-missing") },
            { QStringLiteral("fallbackReason"), QStringLiteral("online-search-api-key-missing") },
            { QStringLiteral("uploadedMediaPayload"), false }
        });
        return result;
    }

    const QString helper = request.onlineHelperScriptPath.trimmed().isEmpty()
        ? defaultOnlineHelperPath()
        : request.onlineHelperScriptPath;
    if (!QFileInfo::exists(helper)) {
        result.insert(QStringLiteral("degradedReason"), QStringLiteral("online-helper-script-missing"));
        result.insert(QStringLiteral("helperScriptPath"), helper);
        return result;
    }

    const QString python = QProcessEnvironment::systemEnvironment()
        .value(QStringLiteral("SUBTITLE_ONLINE_PYTHON"), QStringLiteral("python"));
    QStringList arguments{
        helper,
        QStringLiteral("--media"), request.mediaPath,
        QStringLiteral("--output"), request.onlineSubtitleCachePath,
        QStringLiteral("--search-report"), onlineSearchJsonPath(request.onlineSubtitleCachePath),
        QStringLiteral("--language"), request.onlineLanguage.trimmed().isEmpty()
            ? QStringLiteral("zh,ja,en")
            : request.onlineLanguage.trimmed(),
        QStringLiteral("--provider"), onlineProvider,
        QStringLiteral("--duration"), QString::number(qMax(0, qRound(request.currentSeconds + request.highQualityTargetCoverageSeconds))),
        QStringLiteral("--file-size"), QString::number(qMax<qint64>(0, QFileInfo(request.mediaPath).size()))
    };
    if (!request.onlineBaseUrl.trimmed().isEmpty()) {
        arguments << QStringLiteral("--base-url") << request.onlineBaseUrl.trimmed();
    }

    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("SUBTITLE_ONLINE_API_KEY"), request.onlineApiKey.trimmed());
    if (!request.onlineBaseUrl.trimmed().isEmpty()) {
        env.insert(QStringLiteral("SUBTITLE_ONLINE_BASE_URL"), request.onlineBaseUrl.trimmed());
    }
    process.setProcessEnvironment(env);
    process.start(python, arguments);
    result.insert(QStringLiteral("onlineNetworkStarted"), process.waitForStarted(5000));
    if (!result.value(QStringLiteral("onlineNetworkStarted")).toBool()) {
        result.insert(QStringLiteral("degradedReason"), QStringLiteral("online-worker-start-failed"));
        result.insert(QStringLiteral("helperScriptPath"), helper);
        return result;
    }
    JobContext job(45000);
    const ProcessOutcome processOutcome = job.waitForProcess(process, 25, 45000);
    if (processOutcome.state == JobState::TimedOut) {
        result.insert(QStringLiteral("degradedReason"), QStringLiteral("online-worker-timeout"));
        return result;
    }

    const QString stdoutText = QString::fromUtf8(processOutcome.standardOutput).trimmed();
    const QJsonDocument doc = QJsonDocument::fromJson(stdoutText.toUtf8());
    const QJsonObject helperJson = doc.isObject() ? doc.object() : QJsonObject{};
    const bool cacheWritten =
        processOutcome.succeeded() &&
        QFileInfo::exists(request.onlineSubtitleCachePath) &&
        !SubtitleGenerationService::readSubtitleFile(request.onlineSubtitleCachePath).isEmpty();
    result.insert(QStringLiteral("success"), cacheWritten);
    result.insert(QStringLiteral("onlineSearchSucceeded"), cacheWritten);
    result.insert(QStringLiteral("onlineCacheWritten"), cacheWritten);
    result.insert(QStringLiteral("onlineSearchResultCount"),
                  helperJson.value(QStringLiteral("onlineSearchResultCount")).toInt(cacheWritten ? 1 : 0));
    result.insert(QStringLiteral("onlineSearchAcceptedCount"),
                  helperJson.value(QStringLiteral("onlineSearchAcceptedCount")).toInt(cacheWritten ? 1 : 0));
    result.insert(QStringLiteral("onlineSearchEnabled"),
                  helperJson.value(QStringLiteral("onlineSearchEnabled")).toBool(true));
    result.insert(QStringLiteral("onlineProvider"),
                  helperJson.value(QStringLiteral("onlineProvider")).toString(onlineProvider));
    result.insert(QStringLiteral("query"),
                  helperJson.value(QStringLiteral("query")).isObject()
                      ? helperJson.value(QStringLiteral("query")).toObject()
                      : result.value(QStringLiteral("query")).toObject());
    result.insert(QStringLiteral("candidateCount"),
                  helperJson.value(QStringLiteral("candidateCount")).toInt(
                      helperJson.value(QStringLiteral("onlineSearchResultCount")).toInt(cacheWritten ? 1 : 0)));
    result.insert(QStringLiteral("selectedCandidate"),
                  helperJson.value(QStringLiteral("selectedCandidate")));
    result.insert(QStringLiteral("onlineLanguage"),
                  helperJson.value(QStringLiteral("onlineLanguage")).toString());
    result.insert(QStringLiteral("onlineCoverage"),
                  helperJson.value(QStringLiteral("onlineCoverage")).toDouble());
    result.insert(QStringLiteral("alignmentScore"),
                  helperJson.value(QStringLiteral("alignmentScore")).toDouble());
    result.insert(QStringLiteral("offsetMs"),
                  helperJson.value(QStringLiteral("offsetMs")).toInt());
    result.insert(QStringLiteral("rejectReason"),
                  helperJson.value(QStringLiteral("rejectReason")).toString());
    result.insert(QStringLiteral("fallbackReason"),
                  helperJson.value(QStringLiteral("fallbackReason")).toString());
    result.insert(QStringLiteral("helperExitCode"), processOutcome.exitCode);
    result.insert(QStringLiteral("helperScriptPath"), helper);
    result.insert(QStringLiteral("helperResult"), helperJson);
    result.insert(QStringLiteral("degradedReason"),
                  cacheWritten
                      ? QStringLiteral("online-source-cache-written")
                      : helperJson.value(QStringLiteral("error")).toString(
                            QStringLiteral("online-worker-failed")));
    return result;
}

QJsonObject TranslationEnhancementScheduler::_runManualOnlineTranslationWorker(
    const TranslationEnhancementScheduleRequest& request,
    bool manualAllowed,
    const QJsonObject& onlineWorkerResult) const
{
    QJsonObject result{
        { QStringLiteral("attempted"), false },
        { QStringLiteral("success"), false },
        { QStringLiteral("manualOnly"), true },
        { QStringLiteral("onlineManualAllowed"), manualAllowed },
        { QStringLiteral("translatedOnlineCachePath"), request.onlineTranslatedCachePath },
        { QStringLiteral("sourceOnlineCachePath"), request.onlineSubtitleCachePath },
        { QStringLiteral("translatedOnlineCacheWritten"), false },
        { QStringLiteral("translatedOnlineCueCount"), 0 },
        { QStringLiteral("translatedDisplayableCoverageEndSeconds"), 0.0 },
        { QStringLiteral("sourceCoverageCountsAsDisplayable"), false },
        { QStringLiteral("writesQuickZhSidecar"), false },
        { QStringLiteral("usesQuickApiQuota"), false },
        { QStringLiteral("usesQuickWorkerLane"), false },
        { QStringLiteral("usesQuickTranslationApi"), false },
        { QStringLiteral("wholeTrackReplaceQuickAllowed"), false },
        { QStringLiteral("fallbackToQuick"), true }
    };
    if (!manualAllowed) {
        result.insert(QStringLiteral("degradedReason"),
                      request.playbackMode == TranslationPlaybackStrategy::highQualityModeId()
                          ? QStringLiteral("manual-trigger-required-or-playback-running")
                          : QStringLiteral("disabled-in-quick-playback"));
        return result;
    }
    result.insert(QStringLiteral("attempted"), true);
    if (request.onlineTranslatedCachePath.trimmed().isEmpty()) {
        result.insert(QStringLiteral("degradedReason"), QStringLiteral("online-translated-cache-path-missing"));
        return result;
    }
    if (!QFileInfo::exists(request.onlineSubtitleCachePath)) {
        result.insert(QStringLiteral("degradedReason"), QStringLiteral("online-source-cache-missing"));
        return result;
    }
    if (!request.onlineConfigured &&
        !request.localReferenceProviderAvailable &&
        !onlineWorkerResult.value(QStringLiteral("onlineCacheWritten")).toBool()) {
        result.insert(QStringLiteral("degradedReason"), QStringLiteral("online-reference-source-not-enabled"));
        return result;
    }

    if (request.onlineMockMode) {
        QString error;
        const bool written = _writeMockTranslatedCache(
            request.onlineSubtitleCachePath,
            request.onlineTranslatedCachePath,
            QStringLiteral("mock translated online cue"),
            &error);
        const QVector<GeneratedSubtitleCue> translatedCues =
            SubtitleGenerationService::readSubtitleFile(request.onlineTranslatedCachePath);
        double coverageEnd = 0.0;
        for (const auto& cue : translatedCues) {
            if (!cue.translatedText.trimmed().isEmpty()) {
                coverageEnd = std::max(coverageEnd, cue.endSeconds);
            }
        }
        result.insert(QStringLiteral("success"), written);
        result.insert(QStringLiteral("translatedOnlineCacheWritten"), written);
        result.insert(QStringLiteral("translatedOnlineCueCount"), translatedCues.size());
        result.insert(QStringLiteral("translatedDisplayableCoverageEndSeconds"), coverageEnd);
        result.insert(QStringLiteral("degradedReason"),
                      written ? QStringLiteral("mock-online-translated-cache-written")
                              : (error.isEmpty() ? QStringLiteral("mock-online-translation-cache-write-failed") : error));
        return result;
    }

    const QVector<GeneratedSubtitleCue> sourceCues =
        SubtitleGenerationService::readSubtitleFile(request.onlineSubtitleCachePath);
    if (sourceCues.isEmpty()) {
        result.insert(QStringLiteral("degradedReason"), QStringLiteral("online-source-cache-empty"));
        result.insert(QStringLiteral("onlineWorker"), onlineWorkerResult);
        return result;
    }

    QVector<GeneratedSubtitleCue> translatedCues;
    translatedCues.reserve(sourceCues.size());
    int translatedCueCount = 0;
    QString firstError;
    const QString helper = defaultLongAsrTranslateHelperPath();
    const bool helperReady = QFileInfo::exists(helper);
    const bool providerReady =
        request.translationProviderAvailable &&
        !request.translationBaseUrl.trimmed().isEmpty() &&
        !request.translationApiKey.trimmed().isEmpty();
    for (const GeneratedSubtitleCue& cue : sourceCues) {
        const QString sourceText = !cue.translatedText.trimmed().isEmpty()
            ? cue.translatedText.trimmed()
            : cue.sourceText.trimmed();
        if (sourceText.isEmpty()) {
            continue;
        }
        QString translatedText;
        QString translateError;
        if (helperReady) {
            translatedText = translateSubtitleTextWithHelper(
                helper,
                sourceText,
                QStringLiteral("auto"),
                QStringLiteral("zh"),
                &translateError);
        }
        if (translatedText.isEmpty() && providerReady) {
            const QString providerKindLower = request.translationProviderKind.trimmed().toLower();
            const QString effectiveModel =
                request.translationModel.trimmed().isEmpty() &&
                providerKindLower.contains(QStringLiteral("qwen"))
                    ? QStringLiteral("qwen-plus")
                    : request.translationModel.trimmed();
            if (!effectiveModel.trimmed().isEmpty()) {
                translatedText = translateSubtitleTextWithOpenAICompatible(
                    QStringLiteral("Translate subtitle text faithfully into Simplified Chinese. Return only the subtitle text. Do not add context, explanations, speakers, inferred subjects, or adjacent dialogue. If the source is already Chinese/Traditional Chinese, only normalize to Simplified Chinese punctuation and wording without rewriting or expanding it."),
                    sourceText,
                    request.translationBaseUrl.trimmed(),
                    request.translationApiKey.trimmed(),
                    effectiveModel,
                    &translateError);
            }
        }
        if (translatedText.isEmpty()) {
            if (firstError.isEmpty()) {
                firstError = translateError;
            }
            continue;
        }
        GeneratedSubtitleCue translatedCue = cue;
        translatedCue.sourceText = sourceText;
        translatedCue.translatedText = normalizeOcrChineseForZhHans(translatedText);
        translatedCues.push_back(translatedCue);
        ++translatedCueCount;
    }
    if (translatedCues.isEmpty()) {
        result.insert(QStringLiteral("degradedReason"),
                      firstError.isEmpty()
                          ? QStringLiteral("translation-helper-failed")
                          : firstError);
        result.insert(QStringLiteral("displayableCopyReason"), firstError);
        result.insert(QStringLiteral("onlineWorker"), onlineWorkerResult);
        return result;
    }

    QString writeError;
    const bool written = _writeRepairCache(translatedCues, request.onlineTranslatedCachePath, &writeError);
    double coverageEnd = 0.0;
    for (const auto& cue : translatedCues) {
        if (!cue.translatedText.trimmed().isEmpty()) {
            coverageEnd = std::max(coverageEnd, cue.endSeconds);
        }
    }
    result.insert(QStringLiteral("success"), written);
    result.insert(QStringLiteral("translatedOnlineCacheWritten"), written);
    result.insert(QStringLiteral("translatedOnlineCueCount"), translatedCueCount);
    result.insert(QStringLiteral("translatedDisplayableCoverageEndSeconds"), coverageEnd);
    result.insert(QStringLiteral("degradedReason"),
                  written
                      ? QStringLiteral("online-reference-translated-cache-written")
                      : (writeError.isEmpty() ? QStringLiteral("online-reference-translation-cache-write-failed") : writeError));
    result.insert(QStringLiteral("displayableCopyReason"), writeError);
    result.insert(QStringLiteral("onlineWorker"), onlineWorkerResult);
    return result;
}

QJsonObject TranslationEnhancementScheduler::_runManualOcrWorker(
    const TranslationEnhancementScheduleRequest& request,
    bool manualAllowed) const
{
    QJsonObject result{
        { QStringLiteral("attempted"), false },
        { QStringLiteral("success"), false },
        { QStringLiteral("manualOnly"), true },
        { QStringLiteral("ocrManualAllowed"), manualAllowed },
        { QStringLiteral("ocrConfigured"), request.ocrConfigured },
        { QStringLiteral("ocrAutoEnabledForHighQualityCurrentMedia"),
          request.ocrAutoEnabledForHighQualityCurrentMedia },
        { QStringLiteral("ocrCachePath"), request.ocrSubtitleCachePath },
        { QStringLiteral("ocrTranslatedCachePath"), request.ocrTranslatedCachePath },
        { QStringLiteral("ocrCacheWritten"), false },
        { QStringLiteral("ocrTranslatedCacheWritten"), false },
        { QStringLiteral("ocrCueCount"), 0 },
        { QStringLiteral("ocrTranslatedCueCount"), 0 },
        { QStringLiteral("ocrSourceOnlyDisplayable"), false },
        { QStringLiteral("sourceKind"), QStringLiteral("unknown") },
        { QStringLiteral("chosenSource"), QStringLiteral("unknown") },
        { QStringLiteral("visionSourceKind"), QStringLiteral("unknown") },
        { QStringLiteral("workbenchConfigSource"), request.workbenchConfigSource },
        { QStringLiteral("provider"), request.workbenchCanReadImageText ? QStringLiteral("workbench-vision") : QStringLiteral("local_subtitle_ocr") },
        { QStringLiteral("workbenchCanReadImageText"), request.workbenchCanReadImageText },
        { QStringLiteral("visionApiAttempted"), false },
        { QStringLiteral("visionApiSucceeded"), false },
        { QStringLiteral("visionApiFailed"), false },
        { QStringLiteral("visionModel"), request.workbenchCanReadImageText ? request.translationModel : QString() },
        { QStringLiteral("visualTextDetected"), QStringLiteral("unknown") },
        { QStringLiteral("writesQuickZhSidecar"), false },
        { QStringLiteral("writesEnhancedZhSidecar"), false },
        { QStringLiteral("writesIndependentOcrTranslatedCacheOnly"), false },
        { QStringLiteral("usesQuickApiQuota"), false },
        { QStringLiteral("usesQuickWorkerLane"), false },
        { QStringLiteral("wholeTrackReplaceQuickAllowed"), false },
        { QStringLiteral("fallbackToQuick"), true }
    };
    if (!manualAllowed) {
        result.insert(QStringLiteral("degradedReason"),
                      request.playbackMode == TranslationPlaybackStrategy::highQualityModeId()
                          ? QStringLiteral("manual-trigger-required-or-playback-running")
                          : QStringLiteral("disabled-in-quick-playback"));
        return result;
    }
    result.insert(QStringLiteral("attempted"), true);
    if (!request.ocrConfigured && !request.ocrMockMode) {
        result.insert(QStringLiteral("degradedReason"), QStringLiteral("ocr-worker-not-enabled"));
        return result;
    }
    if (request.ocrSubtitleCachePath.trimmed().isEmpty()) {
        result.insert(QStringLiteral("degradedReason"), QStringLiteral("ocr-cache-path-missing"));
        return result;
    }
    if (request.ocrMockMode) {
        QString error;
        const bool written = _writeMockTranslatedCache(
            request.quickTranslatedVttPath,
            request.ocrSubtitleCachePath,
            QStringLiteral("mock ocr source cue"),
            &error);
        QString translatedError;
        int translatedCueCount = 0;
        const bool translatedWritten = written && _writeMockTranslatedCache(
            request.ocrSubtitleCachePath,
            request.ocrTranslatedCachePath,
            QStringLiteral("mock ocr translated cue"),
            &translatedError);
        const QVector<GeneratedSubtitleCue> cues =
            SubtitleGenerationService::readSubtitleFile(request.ocrSubtitleCachePath);
        result.insert(QStringLiteral("success"), written);
        result.insert(QStringLiteral("ocrCacheWritten"), written);
        result.insert(QStringLiteral("ocrTranslatedCacheWritten"), translatedWritten);
        result.insert(QStringLiteral("ocrCueCount"), cues.size());
        result.insert(QStringLiteral("ocrTranslatedCueCount"), translatedWritten ? cues.size() : 0);
        result.insert(QStringLiteral("visualTextDetected"), written ? QStringLiteral("true") : QStringLiteral("false"));
        result.insert(QStringLiteral("writesIndependentOcrTranslatedCacheOnly"), translatedWritten);
        result.insert(QStringLiteral("ocrTranslationReason"),
                      translatedWritten ? QStringLiteral("mock-ocr-translated-cache-written") : translatedError);
        result.insert(QStringLiteral("degradedReason"),
                      written ? QStringLiteral("mock-ocr-source-cache-written")
                              : (error.isEmpty() ? QStringLiteral("mock-ocr-cache-write-failed") : error));
        return result;
    }
    const QVector<GeneratedSubtitleCue> existingOcrCues =
        SubtitleGenerationService::readSubtitleFile(request.ocrSubtitleCachePath);
    const QVector<GeneratedSubtitleCue> existingTranslatedOcrCues =
        SubtitleGenerationService::readSubtitleFile(request.ocrTranslatedCachePath);
    const QVector<GeneratedSubtitleCue> existingEnhancedCues =
        SubtitleGenerationService::readSubtitleFile(request.enhancedTranslatedVttPath);
    const auto hasCueNearCurrent = [&request](const QVector<GeneratedSubtitleCue>& cues) {
        for (const GeneratedSubtitleCue& cue : cues) {
            if (!cue.translatedText.trimmed().isEmpty() &&
                cue.startSeconds <= request.currentSeconds + 5.0 &&
                cue.endSeconds + 5.0 >= request.currentSeconds) {
                return true;
            }
        }
        return false;
    };
    if (!request.workbenchCanReadImageText &&
        !existingOcrCues.isEmpty() &&
        !existingTranslatedOcrCues.isEmpty() &&
        !existingEnhancedCues.isEmpty() &&
        hasCueNearCurrent(existingTranslatedOcrCues) &&
        displayableCoverageEndSeconds(existingTranslatedOcrCues) + 0.5 >=
            request.currentSeconds + request.highQualityTargetCoverageSeconds) {
        result.insert(QStringLiteral("success"), true);
        result.insert(QStringLiteral("ocrCacheWritten"), true);
        result.insert(QStringLiteral("ocrTranslatedCacheWritten"), true);
        result.insert(QStringLiteral("ocrCueCount"), existingOcrCues.size());
        result.insert(QStringLiteral("ocrTranslatedCueCount"), existingTranslatedOcrCues.size());
        result.insert(QStringLiteral("visualTextDetected"), QStringLiteral("true"));
        result.insert(QStringLiteral("targetCoverageSeconds"), request.highQualityTargetCoverageSeconds);
        result.insert(QStringLiteral("translatedDisplayableCoverageEndSeconds"),
                      displayableCoverageEndSeconds(existingTranslatedOcrCues));
        result.insert(QStringLiteral("ocrIntakeJsonPath"), ocrIntakeJsonPath(request.ocrSubtitleCachePath));
        result.insert(QStringLiteral("reusedExistingCache"), true);
        result.insert(QStringLiteral("writesIndependentOcrTranslatedCacheOnly"), true);
        result.insert(QStringLiteral("ocrTranslationReason"), QStringLiteral("existing-visible-chinese-ocr-cache-reused"));
        result.insert(QStringLiteral("degradedReason"), QStringLiteral("existing-ocr-enhanced-cache-reused"));
        return result;
    }
    const QString helper = request.ocrHelperScriptPath.trimmed().isEmpty()
        ? defaultOcrHelperPath()
        : request.ocrHelperScriptPath;
    if (!QFileInfo::exists(helper)) {
        result.insert(QStringLiteral("degradedReason"), QStringLiteral("ocr-helper-script-missing"));
        result.insert(QStringLiteral("helperScriptPath"), helper);
        return result;
    }
    const QString python = QProcessEnvironment::systemEnvironment()
        .value(QStringLiteral("SUBTITLE_OCR_PYTHON"), QStringLiteral("python"));
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString durationOverride = env.value(QStringLiteral("SUBTITLE_OCR_DURATION_SECONDS")).trimmed();
    const double targetDurationSeconds = std::max(24.0, request.highQualityTargetCoverageSeconds);
    const double durationSeconds = std::max(
        6.0,
        durationOverride.isEmpty() ? targetDurationSeconds : durationOverride.toDouble());
    const QString sampleIntervalOverride =
        env.value(QStringLiteral("SUBTITLE_OCR_SAMPLE_INTERVAL_SECONDS")).trimmed();
    const double defaultSampleIntervalSeconds = request.workbenchCanReadImageText ? 0.85 : 0.65;
    const double sampleIntervalSeconds = std::max(
        0.4,
        sampleIntervalOverride.isEmpty()
            ? defaultSampleIntervalSeconds
            : sampleIntervalOverride.toDouble());
    const int defaultMaxSamples =
        std::max(240, static_cast<int>(std::ceil(durationSeconds / sampleIntervalSeconds)) + 2);
    const int maxSamples = std::max(
        1,
        env.value(QStringLiteral("SUBTITLE_OCR_MAX_SAMPLES"),
                  QString::number(defaultMaxSamples)).toInt());
    const QString intakePath = ocrIntakeJsonPath(request.ocrSubtitleCachePath);
    QStringList arguments{
        helper,
        QStringLiteral("--media"), request.mediaPath,
        QStringLiteral("--output-json"), intakePath,
        QStringLiteral("--output-vtt"), request.ocrSubtitleCachePath,
        QStringLiteral("--language"), QStringLiteral("zh"),
        QStringLiteral("--start"), QString::number(std::max(0.0, request.currentSeconds), 'f', 3),
        QStringLiteral("--duration"), QString::number(durationSeconds, 'f', 1),
        QStringLiteral("--sample-interval"), QString::number(sampleIntervalSeconds, 'f', 2),
        QStringLiteral("--max-samples"), QString::number(maxSamples),
        QStringLiteral("--candidate-scan-json"), request.ocrSubtitleCachePath + QStringLiteral(".candidate_scan.json")
    };
    if (request.workbenchCanReadImageText) {
        const QString codexExecutable = defaultCodexExecutablePath();
        const QString requestedVisionProvider = QProcessEnvironment::systemEnvironment()
            .value(QStringLiteral("SUBTITLE_VISION_PROVIDER"), QStringLiteral("codex-cli"))
            .trimmed()
            .toLower();
        const bool useCodexCli = requestedVisionProvider != QStringLiteral("workbench") &&
            !codexExecutable.isEmpty();
        const QString visionTimeout =
            QProcessEnvironment::systemEnvironment()
                .value(QStringLiteral("SUBTITLE_WORKBENCH_VISION_TIMEOUT_SECONDS"),
                       useCodexCli ? QStringLiteral("75") : QStringLiteral("15"));
        const QString stopAfterFailures =
            QProcessEnvironment::systemEnvironment()
                .value(QStringLiteral("SUBTITLE_WORKBENCH_VISION_STOP_AFTER_FAILURES"), QStringLiteral("3"));
        arguments
            << QStringLiteral("--vision-provider") << (useCodexCli ? QStringLiteral("codex-cli") : QStringLiteral("workbench"))
            << QStringLiteral("--vision-base-url") << request.translationBaseUrl.trimmed()
            << QStringLiteral("--vision-model") << request.translationModel.trimmed()
            << QStringLiteral("--vision-timeout") << visionTimeout
            << QStringLiteral("--vision-stop-after-failures") << stopAfterFailures
            << QStringLiteral("--vision-fallback-local-ocr");
        if (useCodexCli) {
            arguments
                << QStringLiteral("--codex-executable") << codexExecutable
                << QStringLiteral("--codex-home") << subtitleCodexHomePath();
        }
    }
    QProcess process;
    QProcessEnvironment processEnv = QProcessEnvironment::systemEnvironment();
    processEnv.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    if (request.workbenchCanReadImageText) {
        processEnv.insert(QStringLiteral("SUBTITLE_WORKBENCH_API_KEY"), request.translationApiKey.trimmed());
        processEnv.insert(QStringLiteral("SUBTITLE_WORKBENCH_BASE_URL"), request.translationBaseUrl.trimmed());
        processEnv.insert(QStringLiteral("SUBTITLE_WORKBENCH_MODEL"), request.translationModel.trimmed());
        processEnv.insert(QStringLiteral("SUBTITLE_WORKBENCH_VISION_MODEL"), request.translationModel.trimmed());
        if (request.playbackAlreadyRunning &&
            processEnv.value(QStringLiteral("SUBTITLE_WORKBENCH_VISION_CONCURRENCY")).trimmed().isEmpty()) {
            processEnv.insert(QStringLiteral("SUBTITLE_WORKBENCH_VISION_CONCURRENCY"), QStringLiteral("2"));
        }
    }
    process.setProcessEnvironment(processEnv);
    process.start(python, arguments);
    result.insert(QStringLiteral("helperStarted"), process.waitForStarted(5000));
    if (!result.value(QStringLiteral("helperStarted")).toBool()) {
        result.insert(QStringLiteral("degradedReason"), QStringLiteral("ocr-worker-start-failed"));
        result.insert(QStringLiteral("helperScriptPath"), helper);
        return result;
    }
    result.insert(
        QStringLiteral("playbackPriorityThrottled"),
        lowerWorkerPriorityForPlayback(&process, request.playbackAlreadyRunning));
    const int timeoutMs = std::max(60000, static_cast<int>((durationSeconds / sampleIntervalSeconds) * 5000.0) + 30000);
    bool canceled = false;
    const ProcessOutcome processOutcome =
        waitForWorkerProcess(&process, timeoutMs, request.cancelRequested, &canceled);
    if (processOutcome.state == JobState::Canceled || processOutcome.state == JobState::TimedOut) {
        result.insert(QStringLiteral("degradedReason"), canceled
            ? QStringLiteral("ocr-worker-canceled")
            : QStringLiteral("ocr-worker-timeout"));
        return result;
    }
    const QString stdoutText = QString::fromUtf8(processOutcome.standardOutput).trimmed();
    const QJsonDocument doc = QJsonDocument::fromJson(stdoutText.toUtf8());
    const QJsonObject helperJson = doc.isObject() ? doc.object() : QJsonObject{};
    const QVector<GeneratedSubtitleCue> cues =
        SubtitleGenerationService::readSubtitleFile(request.ocrSubtitleCachePath);
    QString translatedError;
    int translatedCueCount = 0;
    const bool translatedWritten = _writeOcrTranslatedCache(
        request,
        request.ocrSubtitleCachePath,
        request.ocrTranslatedCachePath,
        &translatedCueCount,
        &translatedError);
    const bool sourceWritten =
        processOutcome.succeeded() &&
        QFileInfo::exists(request.ocrSubtitleCachePath) &&
        !cues.isEmpty();
    result.insert(QStringLiteral("success"), sourceWritten && translatedWritten);
    result.insert(QStringLiteral("ocrCacheWritten"), sourceWritten);
    result.insert(QStringLiteral("ocrTranslatedCacheWritten"), translatedWritten);
    result.insert(QStringLiteral("ocrCueCount"), cues.size());
    result.insert(QStringLiteral("ocrTranslatedCueCount"), translatedCueCount);
    result.insert(QStringLiteral("visualTextDetected"), sourceWritten ? QStringLiteral("true") : QStringLiteral("false"));
    result.insert(QStringLiteral("visualSubtitleTrackGenerated"),
                  helperJson.value(QStringLiteral("visualSubtitleTrackGenerated")).toBool(sourceWritten));
    result.insert(QStringLiteral("visualCueCount"),
                  helperJson.value(QStringLiteral("visualCueCount")).toInt(cues.size()));
    result.insert(QStringLiteral("visualCoverageStart"),
                  helperJson.value(QStringLiteral("visualCoverageStart")).toDouble());
    result.insert(QStringLiteral("visualCoverageEnd"),
                  helperJson.value(QStringLiteral("visualCoverageEnd")).toDouble());
    result.insert(QStringLiteral("visualDetectedFrameCount"),
                  helperJson.value(QStringLiteral("visualDetectedFrameCount")).toInt());
    result.insert(QStringLiteral("visualMissedFrameCount"),
                  helperJson.value(QStringLiteral("visualMissedFrameCount")).toInt());
    result.insert(QStringLiteral("missedLikelySubtitleFrames"),
                  helperJson.value(QStringLiteral("missedLikelySubtitleFrames")));
    result.insert(QStringLiteral("sharedRuntimeHeadlessDetector"),
                  helperJson.value(QStringLiteral("sharedRuntimeHeadlessDetector")).toBool(true));
    result.insert(QStringLiteral("detectorVersion"),
                  helperJson.value(QStringLiteral("detectorVersion")).toString());
    result.insert(QStringLiteral("candidateScanCachePath"),
                  helperJson.value(QStringLiteral("candidateScanCachePath")).toString());
    result.insert(QStringLiteral("candidateScan"),
                  helperJson.value(QStringLiteral("candidateScan")));
    result.insert(QStringLiteral("workbenchCanReadImageText"),
                  helperJson.value(QStringLiteral("workbenchCanReadImageText")).toBool(request.workbenchCanReadImageText));
    result.insert(QStringLiteral("visionApiAttempted"),
                  helperJson.value(QStringLiteral("visionApiAttempted")).toBool(false));
    result.insert(QStringLiteral("visionApiSucceeded"),
                  helperJson.value(QStringLiteral("visionApiSucceeded")).toBool(false));
    result.insert(QStringLiteral("visionApiFailed"),
                  helperJson.value(QStringLiteral("visionApiFailed")).toBool(false));
    result.insert(QStringLiteral("visionModel"),
                  helperJson.value(QStringLiteral("visionModel")).toString(request.translationModel));
    result.insert(QStringLiteral("visionProvider"),
                  helperJson.value(QStringLiteral("visionProvider")).toString());
    result.insert(QStringLiteral("visionFrameCount"),
                  helperJson.value(QStringLiteral("visionFrameCount")).toInt());
    result.insert(QStringLiteral("visionCueCount"),
                  helperJson.value(QStringLiteral("visionCueCount")).toInt());
    result.insert(QStringLiteral("fallbackReason"),
                  helperJson.value(QStringLiteral("fallbackReason")).toString());
    result.insert(QStringLiteral("visionError"),
                  helperJson.value(QStringLiteral("visionError")).toString());
    QString currentVisualRawText;
    QString currentOcrRawText;
    QString currentCueSourceKind;
    const QJsonArray helperCues = helperJson.value(QStringLiteral("cues")).toArray();
    for (const QJsonValue& cueValue : helperCues) {
        const QJsonObject cue = cueValue.toObject();
        const double cueStart = cue.value(QStringLiteral("start")).toDouble();
        const double cueEnd = cue.value(QStringLiteral("end")).toDouble();
        if (cueStart <= request.currentSeconds + 5.0 && cueEnd + 5.0 >= request.currentSeconds) {
            currentCueSourceKind = cue.value(QStringLiteral("sourceKind")).toString();
            currentVisualRawText = cue.value(QStringLiteral("visualRawText")).toString();
            currentOcrRawText = cue.value(QStringLiteral("ocrRawText")).toString();
            if (!currentVisualRawText.isEmpty() || !currentOcrRawText.isEmpty()) {
                break;
            }
        }
    }
    if (currentVisualRawText.isEmpty() && currentOcrRawText.isEmpty() && !helperCues.isEmpty()) {
        const QJsonObject firstCue = helperCues.first().toObject();
        currentCueSourceKind = firstCue.value(QStringLiteral("sourceKind")).toString();
        currentVisualRawText = firstCue.value(QStringLiteral("visualRawText")).toString();
        currentOcrRawText = firstCue.value(QStringLiteral("ocrRawText")).toString();
    }
    result.insert(QStringLiteral("currentCueSourceKind"), currentCueSourceKind);
    result.insert(QStringLiteral("visualRawText"), currentVisualRawText);
    result.insert(QStringLiteral("ocrRawText"), currentOcrRawText);
    const bool visionApiSucceeded = helperJson.value(QStringLiteral("visionApiSucceeded")).toBool(false);
    const bool visionApiAttempted = helperJson.value(QStringLiteral("visionApiAttempted")).toBool(false);
    const int localFallbackFrameCount = helperJson.value(QStringLiteral("localOcrFallbackFrameCount")).toInt();
    const QString helperSourceKind = helperJson.value(QStringLiteral("sourceKind")).toString();
    const QString resolvedSourceKind =
        visionApiSucceeded
            ? QStringLiteral("workbench-vision")
            : (!helperSourceKind.isEmpty()
                  ? (helperSourceKind == QStringLiteral("workbench-vision")
                        ? QStringLiteral("local-ocr-fallback")
                        : helperSourceKind)
                  : (localFallbackFrameCount > 0
                        ? QStringLiteral("local-ocr-fallback")
                        : (visionApiAttempted
                              ? QStringLiteral("workbench-vision-failed")
                              : QStringLiteral("unknown"))));
    result.insert(QStringLiteral("visionSourceKind"),
                  visionApiSucceeded
                      ? QStringLiteral("workbench-vision")
                      : (request.workbenchCanReadImageText
                            ? QStringLiteral("workbench-vision-fallback-local-ocr")
                            : QStringLiteral("local-ocr")));
    result.insert(QStringLiteral("sourceKind"), resolvedSourceKind);
    result.insert(QStringLiteral("chosenSource"), resolvedSourceKind);
    result.insert(QStringLiteral("ocrIntakeJsonPath"), intakePath);
    result.insert(QStringLiteral("helperExitCode"), processOutcome.exitCode);
    result.insert(QStringLiteral("helperScriptPath"), helper);
    result.insert(QStringLiteral("helperResult"), helperJson);
    result.insert(QStringLiteral("targetCoverageSeconds"), request.highQualityTargetCoverageSeconds);
    result.insert(QStringLiteral("requestedDurationSeconds"), durationSeconds);
    result.insert(QStringLiteral("sampleIntervalSeconds"), sampleIntervalSeconds);
    result.insert(QStringLiteral("maxSamples"), maxSamples);
    result.insert(QStringLiteral("translatedDisplayableCoverageEndSeconds"),
                  displayableCoverageEndSeconds(
                      SubtitleGenerationService::readSubtitleFile(request.ocrTranslatedCachePath)));
    result.insert(QStringLiteral("writesIndependentOcrTranslatedCacheOnly"), translatedWritten);
    result.insert(QStringLiteral("ocrTranslationReason"),
                  translatedWritten ? QStringLiteral("visible-chinese-ocr-copied-as-translated-cache") : translatedError);
    result.insert(QStringLiteral("degradedReason"),
                  sourceWritten
                      ? (translatedWritten ? QStringLiteral("ocr-source-and-translated-cache-written")
                                           : translatedError)
                      : helperJson.value(QStringLiteral("ocrError")).toString(QStringLiteral("ocr-worker-failed")));
    return result;
}

QJsonObject TranslationEnhancementScheduler::_runManualLongAsrWorker(
    const TranslationEnhancementScheduleRequest& request,
    bool manualAllowed) const
{
    QJsonObject result{
        { QStringLiteral("attempted"), false },
        { QStringLiteral("success"), false },
        { QStringLiteral("implemented"), true },
        { QStringLiteral("defaultEnabled"), false },
        { QStringLiteral("manualOnly"), true },
        { QStringLiteral("activeSource"), QStringLiteral("long-asr-corrected") },
        { QStringLiteral("sourcePath"), request.longAsrSourceCachePath },
        { QStringLiteral("translatedPath"), request.longAsrTranslatedCachePath },
        { QStringLiteral("reportPath"), request.longAsrJsonPath },
        { QStringLiteral("sourceCacheWritten"), false },
        { QStringLiteral("translatedCacheWritten"), false },
        { QStringLiteral("sourceCueCount"), 0 },
        { QStringLiteral("translatedCueCount"), 0 },
        { QStringLiteral("writesQuickZhSidecar"), false },
        { QStringLiteral("usesQuickWorkerLane"), false },
        { QStringLiteral("usesQuickApiQuota"), false },
        { QStringLiteral("writesIndependentHqTranslatedCacheOnly"), false },
        { QStringLiteral("asrProviderAvailable"), request.asrProviderAvailable },
        { QStringLiteral("translationProviderAvailable"), request.translationProviderAvailable },
        { QStringLiteral("translationProviderKind"), request.translationProviderKind },
        { QStringLiteral("translationModel"), request.translationModel },
        { QStringLiteral("longAsrAutoEnabledForNoSubtitleHighQuality"),
          request.longAsrAutoEnabledForNoSubtitleHighQuality },
        { QStringLiteral("fallbackToQuick"), true }
    };
    if (!manualAllowed) {
        result.insert(QStringLiteral("fallbackReason"),
                      request.playbackMode == TranslationPlaybackStrategy::highQualityModeId()
                          ? QStringLiteral("manual-trigger-required-or-playback-running")
                          : QStringLiteral("disabled-in-quick-playback"));
        return result;
    }
    if (!request.longAsrConfigured) {
        result.insert(QStringLiteral("fallbackReason"), QStringLiteral("long-asr-worker-not-enabled"));
        return result;
    }
    const bool longAsrAllowedForThisSourceDecision =
        request.longAsrAutoEnabledForNoSubtitleHighQuality ||
        (!request.localReferenceProviderAvailable &&
         !request.onlineConfigured &&
         !request.ocrConfigured &&
         !request.ocrAutoEnabledForHighQualityCurrentMedia);
    result.insert(QStringLiteral("longAsrCandidateAllowed"), longAsrAllowedForThisSourceDecision);
    if (!longAsrAllowedForThisSourceDecision) {
        result.insert(QStringLiteral("fallbackReason"),
                      QStringLiteral("long-asr-skipped-non-asr-subtitle-source-priority"));
        return result;
    }
    result.insert(QStringLiteral("attempted"), true);
    if (request.mediaPath.trimmed().isEmpty() ||
        request.longAsrSourceCachePath.trimmed().isEmpty() ||
        request.longAsrTranslatedCachePath.trimmed().isEmpty() ||
        request.longAsrJsonPath.trimmed().isEmpty()) {
        result.insert(QStringLiteral("fallbackReason"), QStringLiteral("long-asr-path-missing"));
        return result;
    }
    const QString helper = request.longAsrHelperScriptPath.trimmed().isEmpty()
        ? defaultLongAsrHelperPath()
        : request.longAsrHelperScriptPath;
    if (!QFileInfo::exists(helper)) {
        result.insert(QStringLiteral("fallbackReason"), QStringLiteral("long-asr-helper-script-missing"));
        result.insert(QStringLiteral("helperScriptPath"), helper);
        return result;
    }
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString python = env.value(QStringLiteral("SUBTITLE_LONG_ASR_PYTHON"),
                                     env.value(QStringLiteral("SUBTITLE_OCR_PYTHON"), QStringLiteral("python")));
    const QString durationOverride =
        env.value(QStringLiteral("SUBTITLE_LONG_ASR_DURATION_SECONDS")).trimmed();
    const double targetDurationSeconds = std::max(24.0, request.highQualityTargetCoverageSeconds);
    const double durationSeconds = std::max(
        6.0,
        durationOverride.isEmpty() ? targetDurationSeconds : durationOverride.toDouble());
    const QString sourceLanguage =
        env.value(QStringLiteral("SUBTITLE_LONG_ASR_SOURCE_LANGUAGE"), QStringLiteral("ja"));
    const QString targetLanguage =
        env.value(QStringLiteral("SUBTITLE_LONG_ASR_TARGET_LANGUAGE"), QStringLiteral("zh"));
    QStringList arguments{
        helper,
        QStringLiteral("--media"), request.mediaPath,
        QStringLiteral("--output-json"), request.longAsrJsonPath,
        QStringLiteral("--output-source-vtt"), request.longAsrSourceCachePath,
        QStringLiteral("--output-translated-vtt"), request.longAsrTranslatedCachePath,
        QStringLiteral("--start"), QString::number(std::max(0.0, request.currentSeconds), 'f', 3),
        QStringLiteral("--duration"), QString::number(durationSeconds, 'f', 1),
        QStringLiteral("--source-language"), sourceLanguage,
        QStringLiteral("--target-language"), targetLanguage
    };
    QProcess process;
    QProcessEnvironment processEnv = QProcessEnvironment::systemEnvironment();
    if (!request.translationApiKey.trimmed().isEmpty()) {
        processEnv.insert(QStringLiteral("SUBTITLE_TRANSLATION_API_KEY"), request.translationApiKey.trimmed());
    }
    if (!request.translationBaseUrl.trimmed().isEmpty()) {
        processEnv.insert(QStringLiteral("SUBTITLE_TRANSLATION_BASE_URL"), request.translationBaseUrl.trimmed());
    }
    if (!request.translationModel.trimmed().isEmpty()) {
        processEnv.insert(QStringLiteral("SUBTITLE_TRANSLATION_MODEL"), request.translationModel.trimmed());
    }
    if (processEnv.value(QStringLiteral("SUBTITLE_LONG_ASR_TRANSCRIBE_TIMEOUT_SECONDS")).trimmed().isEmpty()) {
        processEnv.insert(
            QStringLiteral("SUBTITLE_LONG_ASR_TRANSCRIBE_TIMEOUT_SECONDS"),
            QString::number(std::max(180, static_cast<int>(durationSeconds) + 180)));
    }
    processEnv.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    processEnv.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    process.setProcessEnvironment(processEnv);
    process.start(python, arguments);
    result.insert(QStringLiteral("helperStarted"), process.waitForStarted(5000));
    if (!result.value(QStringLiteral("helperStarted")).toBool()) {
        result.insert(QStringLiteral("fallbackReason"), QStringLiteral("long-asr-worker-start-failed"));
        result.insert(QStringLiteral("helperScriptPath"), helper);
        return result;
    }
    result.insert(
        QStringLiteral("playbackPriorityThrottled"),
        lowerWorkerPriorityForPlayback(&process, request.playbackAlreadyRunning));
    const int timeoutMs = std::max(
        90000,
        env.value(QStringLiteral("SUBTITLE_LONG_ASR_TIMEOUT_MS"),
                  QString::number(std::max(240000, static_cast<int>(durationSeconds * 1000.0) + 240000)))
            .toInt());
    bool canceled = false;
    const ProcessOutcome processOutcome =
        waitForWorkerProcess(&process, timeoutMs, request.cancelRequested, &canceled);
    if (processOutcome.state == JobState::Canceled || processOutcome.state == JobState::TimedOut) {
        result.insert(QStringLiteral("fallbackReason"), canceled
            ? QStringLiteral("long-asr-worker-canceled")
            : QStringLiteral("long-asr-worker-timeout"));
        return result;
    }
    const QString stdoutText = QString::fromUtf8(processOutcome.standardOutput).trimmed();
    const QJsonDocument doc = QJsonDocument::fromJson(stdoutText.toUtf8());
    QJsonObject helperJson = doc.isObject() ? doc.object() : QJsonObject{};
    if (helperJson.isEmpty() && QFileInfo::exists(request.longAsrJsonPath)) {
        QFile file(request.longAsrJsonPath);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            helperJson = QJsonDocument::fromJson(file.readAll()).object();
        }
    }
    const QVector<GeneratedSubtitleCue> sourceCues =
        SubtitleGenerationService::readSubtitleFile(request.longAsrSourceCachePath);
    const QVector<GeneratedSubtitleCue> translatedCues =
        SubtitleGenerationService::readSubtitleFile(request.longAsrTranslatedCachePath);
    const bool sourceWritten =
        QFileInfo::exists(request.longAsrSourceCachePath) && !sourceCues.isEmpty();
    const bool translatedWritten =
        QFileInfo::exists(request.longAsrTranslatedCachePath) && !translatedCues.isEmpty();
    result.insert(QStringLiteral("success"), sourceWritten && translatedWritten);
    result.insert(QStringLiteral("sourceCacheWritten"), sourceWritten);
    result.insert(QStringLiteral("translatedCacheWritten"), translatedWritten);
    result.insert(QStringLiteral("sourceCueCount"), sourceCues.size());
    result.insert(QStringLiteral("translatedCueCount"), translatedCues.size());
    result.insert(QStringLiteral("helperExitCode"), processOutcome.exitCode);
    result.insert(QStringLiteral("helperScriptPath"), helper);
    result.insert(QStringLiteral("helperResult"), helperJson);
    result.insert(QStringLiteral("targetCoverageSeconds"), request.highQualityTargetCoverageSeconds);
    result.insert(QStringLiteral("requestedDurationSeconds"), durationSeconds);
    result.insert(QStringLiteral("translatedDisplayableCoverageEndSeconds"),
                  displayableCoverageEndSeconds(translatedCues));
    result.insert(QStringLiteral("writesIndependentHqTranslatedCacheOnly"), translatedWritten);
    QString fallback = helperJson.value(QStringLiteral("blocker"))
        .toString(helperJson.value(QStringLiteral("error"))
                      .toString(QStringLiteral("long-asr-worker-failed")));
    if (fallback.contains(QStringLiteral("cublas"), Qt::CaseInsensitive) ||
        fallback.contains(QStringLiteral("cuda"), Qt::CaseInsensitive)) {
        fallback = QStringLiteral("longasr-dependency-missing");
    } else if (sourceWritten && !translatedWritten) {
        fallback = request.translationProviderAvailable
            ? QStringLiteral("translation-provider-available-but-longasr-translation-failed")
            : QStringLiteral("translation-provider-unavailable");
    } else if (!sourceWritten) {
        fallback = request.asrProviderAvailable
            ? QStringLiteral("asr-provider-available-but-longasr-source-failed")
            : QStringLiteral("asr-provider-unavailable-or-longasr-dependency-missing");
    }
    result.insert(QStringLiteral("fallbackReason"),
                  sourceWritten && translatedWritten
                      ? QStringLiteral("long-asr-source-and-hq-translation-written")
                      : fallback);
    return result;
}

QJsonObject TranslationEnhancementScheduler::_runManualRepairWorker(
    const TranslationEnhancementScheduleRequest& request,
    bool manualAllowed) const
{
    QJsonObject result{
        { QStringLiteral("attempted"), false },
        { QStringLiteral("success"), false },
        { QStringLiteral("manualOnly"), true },
        { QStringLiteral("repairManualAllowed"), manualAllowed },
        { QStringLiteral("repairCachePath"), request.repairTranslatedVttPath },
        { QStringLiteral("repairCacheWritten"), false },
        { QStringLiteral("repairCueCount"), 0 },
        { QStringLiteral("terminologyAppliedCueCount"), 0 },
        { QStringLiteral("quickTimingPreserved"), false },
        { QStringLiteral("quickCueCountPreserved"), false },
        { QStringLiteral("writesQuickZhSidecar"), false },
        { QStringLiteral("usesQuickApiQuota"), false },
        { QStringLiteral("usesQuickWorkerLane"), false },
        { QStringLiteral("wholeTrackReplaceQuickAllowed"), false },
        { QStringLiteral("fallbackToQuick"), true }
    };
    if (!manualAllowed) {
        result.insert(QStringLiteral("degradedReason"),
                      request.playbackMode == TranslationPlaybackStrategy::highQualityModeId()
                          ? QStringLiteral("manual-trigger-required-or-playback-running")
                          : QStringLiteral("disabled-in-quick-playback"));
        return result;
    }
    result.insert(QStringLiteral("attempted"), true);
    if (request.repairTranslatedVttPath.trimmed().isEmpty()) {
        result.insert(QStringLiteral("degradedReason"), QStringLiteral("repair-cache-path-missing"));
        return result;
    }
    if (request.lowConfidenceRepairMockMode) {
        QString error;
        const bool written = _writeMockRepairCache(
            request.quickTranslatedVttPath,
            request.repairTranslatedVttPath,
            &error);
        const QVector<GeneratedSubtitleCue> cues =
            SubtitleGenerationService::readSubtitleFile(request.repairTranslatedVttPath);
        const QVector<GeneratedSubtitleCue> quickCues =
            SubtitleGenerationService::readSubtitleFile(request.quickTranslatedVttPath);
        result.insert(QStringLiteral("success"), written);
        result.insert(QStringLiteral("repairCacheWritten"), written);
        result.insert(QStringLiteral("repairCueCount"), cues.size());
        result.insert(QStringLiteral("terminologyAppliedCueCount"), written && !cues.isEmpty() ? 1 : 0);
        result.insert(QStringLiteral("quickTimingPreserved"), written);
        result.insert(QStringLiteral("quickCueCountPreserved"), written && !quickCues.isEmpty() && cues.size() == quickCues.size());
        result.insert(QStringLiteral("degradedReason"),
                      written ? QStringLiteral("mock-repair-terminology-cache-written")
                              : (error.isEmpty() ? QStringLiteral("mock-repair-cache-write-failed") : error));
        return result;
    }
    int changedCueCount = 0;
    QString error;
    const bool written = _writeLocalRepairCache(
        request.quickTranslatedVttPath,
        request.repairTranslatedVttPath,
        &changedCueCount,
        &error);
    const QVector<GeneratedSubtitleCue> cues =
        SubtitleGenerationService::readSubtitleFile(request.repairTranslatedVttPath);
    const QVector<GeneratedSubtitleCue> quickCues =
        SubtitleGenerationService::readSubtitleFile(request.quickTranslatedVttPath);
    result.insert(QStringLiteral("success"), written && changedCueCount > 0);
    result.insert(QStringLiteral("repairCacheWritten"), written);
    result.insert(QStringLiteral("repairCueCount"), cues.size());
    result.insert(QStringLiteral("terminologyAppliedCueCount"), changedCueCount);
    result.insert(QStringLiteral("quickTimingPreserved"), written);
    result.insert(QStringLiteral("quickCueCountPreserved"), written && !quickCues.isEmpty() && cues.size() == quickCues.size());
    result.insert(QStringLiteral("providerState"),
                  request.lowConfidenceRepairConfigured
                      ? QStringLiteral("configured-local-rule-repair")
                      : QStringLiteral("no-provider-local-rule-repair"));
    result.insert(QStringLiteral("degradedReason"),
                  written && changedCueCount > 0
                      ? QStringLiteral("local-semantic-repair-cache-written")
                      : (error.isEmpty() ? QStringLiteral("local-semantic-repair-no-change") : error));
    return result;
}

QJsonObject TranslationEnhancementScheduler::_diagnoseOnlineSourceIntake(
    const TranslationEnhancementScheduleRequest& request,
    bool manualAllowed,
    const QJsonObject& onlineWorkerResult) const
{
    const bool cacheFound =
        !request.onlineSubtitleCachePath.trimmed().isEmpty() &&
        QFileInfo::exists(request.onlineSubtitleCachePath);
    QJsonObject result{
        { QStringLiteral("enabled"), manualAllowed },
        { QStringLiteral("mode"), QStringLiteral("manual-source-cache-intake-diagnostics") },
        { QStringLiteral("manualOnly"), true },
        { QStringLiteral("diagnosticOnly"), true },
        { QStringLiteral("canAffectQuickPath"), false },
        { QStringLiteral("readsOnlineSourceCache"), manualAllowed },
        { QStringLiteral("onlineSourceCachePath"), request.onlineSubtitleCachePath },
        { QStringLiteral("onlineSourceCacheFound"), cacheFound },
        { QStringLiteral("onlineSourceCueCount"), 0 },
        { QStringLiteral("onlineSourceCoverageSeconds"), 0.0 },
        { QStringLiteral("onlineSourceCoverageEndSeconds"), 0.0 },
        { QStringLiteral("matchedQuickCueCount"), 0 },
        { QStringLiteral("unmatchedOnlineCueCount"), 0 },
        { QStringLiteral("timingAlignmentPassed"), false },
        { QStringLiteral("sourceOnlyDisplayable"), false },
        { QStringLiteral("displayCoverageDelta"), 0.0 },
        { QStringLiteral("quickCoverageChanged"), false },
        { QStringLiteral("quickCueCount"), request.quickCueCount },
        { QStringLiteral("quickCoverageEndSeconds"), request.quickCoverageEndSeconds },
        { QStringLiteral("rejectedForDisplay"), QStringLiteral("source-only-not-displayable") },
        { QStringLiteral("handoffToFusion"), false },
        { QStringLiteral("writesQuickZhSidecar"), false },
        { QStringLiteral("writesEnhancedZhSidecar"), false },
        { QStringLiteral("usesQuickApiQuota"), false },
        { QStringLiteral("usesQuickWorkerLane"), false },
        { QStringLiteral("usesAsrOcrTranslationRefineRepair"), false },
        { QStringLiteral("onlineWorker"), onlineWorkerResult }
    };

    if (!manualAllowed) {
        result.insert(QStringLiteral("skipReason"),
                      request.playbackMode == TranslationPlaybackStrategy::highQualityModeId()
                          ? QStringLiteral("manual-trigger-required-or-playback-running")
                          : QStringLiteral("disabled-in-quick-playback"));
        return result;
    }
    if (!cacheFound) {
        result.insert(QStringLiteral("skipReason"), QStringLiteral("online-source-cache-missing"));
        QString diagnosticsPath;
        QString diagnosticsError;
        const bool diagnosticsWritten =
            _writeOnlineIntakeDiagnostics(
                request.onlineSubtitleCachePath,
                result,
                &diagnosticsPath,
                &diagnosticsError);
        result.insert(QStringLiteral("diagnosticsPath"), diagnosticsPath);
        result.insert(QStringLiteral("diagnosticsWritten"), diagnosticsWritten);
        result.insert(QStringLiteral("diagnosticsError"), diagnosticsError);
        return result;
    }

    const QVector<GeneratedSubtitleCue> quickCues =
        SubtitleGenerationService::readSubtitleFile(request.quickTranslatedVttPath);
    const QVector<GeneratedSubtitleCue> onlineCues =
        SubtitleGenerationService::readSubtitleFile(request.onlineSubtitleCachePath);
    double onlineCoverageSeconds = 0.0;
    double onlineCoverageEndSeconds = 0.0;
    for (const GeneratedSubtitleCue& cue : onlineCues) {
        if (cue.endSeconds > cue.startSeconds) {
            onlineCoverageSeconds += cue.endSeconds - cue.startSeconds;
            onlineCoverageEndSeconds = std::max(onlineCoverageEndSeconds, cue.endSeconds);
        }
    }

    auto timingsAlign = [](const GeneratedSubtitleCue& quickCue, const GeneratedSubtitleCue& onlineCue) {
        const double quickDuration = std::max(0.05, quickCue.endSeconds - quickCue.startSeconds);
        const double onlineDuration = std::max(0.05, onlineCue.endSeconds - onlineCue.startSeconds);
        const double overlapStart = std::max(quickCue.startSeconds, onlineCue.startSeconds);
        const double overlapEnd = std::min(quickCue.endSeconds, onlineCue.endSeconds);
        const double overlap = std::max(0.0, overlapEnd - overlapStart);
        const double quickCenter = (quickCue.startSeconds + quickCue.endSeconds) * 0.5;
        const double onlineCenter = (onlineCue.startSeconds + onlineCue.endSeconds) * 0.5;
        return (std::abs(quickCue.startSeconds - onlineCue.startSeconds) <= 0.50 &&
                std::abs(quickCue.endSeconds - onlineCue.endSeconds) <= 0.75) ||
            overlap / std::min(quickDuration, onlineDuration) >= 0.50 ||
            (onlineCenter >= quickCue.startSeconds - 0.75 &&
             onlineCenter <= quickCue.endSeconds + 0.75) ||
            (onlineDuration <= 1.50 &&
             std::abs(quickCenter - onlineCenter) <= std::max(1.25, quickDuration * 0.50));
    };
    QSet<int> matchedQuickIndexes;
    int matchedOnlineCueCount = 0;
    for (const GeneratedSubtitleCue& onlineCue : onlineCues) {
        for (int i = 0; i < quickCues.size(); ++i) {
            if (matchedQuickIndexes.contains(i)) {
                continue;
            }
            if (timingsAlign(quickCues.at(i), onlineCue)) {
                matchedQuickIndexes.insert(i);
                ++matchedOnlineCueCount;
                break;
            }
        }
    }

    const int onlineCueCount = static_cast<int>(onlineCues.size());
    const int unmatchedOnlineCueCount =
        std::max(0, onlineCueCount - matchedOnlineCueCount);
    const bool timingAlignmentPassed =
        !onlineCues.isEmpty() &&
        matchedOnlineCueCount > 0 &&
        unmatchedOnlineCueCount == 0 &&
        matchedOnlineCueCount <= quickCues.size();
    result.insert(QStringLiteral("skipReason"), QString());
    result.insert(QStringLiteral("onlineSourceCueCount"), onlineCueCount);
    result.insert(QStringLiteral("onlineSourceCoverageSeconds"), onlineCoverageSeconds);
    result.insert(QStringLiteral("onlineSourceCoverageEndSeconds"), onlineCoverageEndSeconds);
    result.insert(QStringLiteral("matchedQuickCueCount"), matchedOnlineCueCount);
    result.insert(QStringLiteral("unmatchedOnlineCueCount"), unmatchedOnlineCueCount);
    result.insert(QStringLiteral("timingAlignmentPassed"), timingAlignmentPassed);
    result.insert(QStringLiteral("quickCueCount"), quickCues.size());
    result.insert(QStringLiteral("quickCoverageEndSeconds"),
                  request.quickCoverageEndSeconds);

    QString diagnosticsPath;
    QString diagnosticsError;
    const bool diagnosticsWritten =
        _writeOnlineIntakeDiagnostics(
            request.onlineSubtitleCachePath,
            result,
            &diagnosticsPath,
            &diagnosticsError);
    result.insert(QStringLiteral("diagnosticsPath"), diagnosticsPath);
    result.insert(QStringLiteral("diagnosticsWritten"), diagnosticsWritten);
    result.insert(QStringLiteral("diagnosticsError"), diagnosticsError);
    return result;
}

bool TranslationEnhancementScheduler::_writeOnlineIntakeDiagnostics(
    const QString& onlineCachePath,
    const QJsonObject& diagnostics,
    QString* outputPath,
    QString* errorMessage) const
{
    if (onlineCachePath.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("online-cache-path-missing");
        }
        return false;
    }
    QString path = onlineCachePath;
    if (path.endsWith(QStringLiteral(".online.source.vtt"))) {
        path.chop(QStringLiteral(".online.source.vtt").size());
        path += QStringLiteral(".online.intake.json");
    } else {
        path += QStringLiteral(".intake.json");
    }
    if (outputPath) {
        *outputPath = path;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    file.write(QJsonDocument(diagnostics).toJson(QJsonDocument::Indented));
    return true;
}

bool TranslationEnhancementScheduler::_writeMockOnlineCache(
    const QString& quickTranslatedVttPath,
    const QString& onlineCachePath,
    double fallbackStartSeconds,
    QString* errorMessage) const
{
    QVector<GeneratedSubtitleCue> quickCues =
        SubtitleGenerationService::readSubtitleFile(quickTranslatedVttPath);
    if (quickCues.isEmpty()) {
        GeneratedSubtitleCue cue;
        cue.index = 1;
        cue.startSeconds = std::max(0.0, fallbackStartSeconds);
        cue.endSeconds = cue.startSeconds + 4.0;
        cue.sourceText = QStringLiteral("mock online source cue");
        cue.translatedText = cue.sourceText;
        quickCues.push_back(cue);
    }
    QDir().mkpath(QFileInfo(onlineCachePath).absolutePath());
    QFile file(onlineCachePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("unable-to-open-online-cache");
        }
        return false;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "WEBVTT\n\n";
    int index = 1;
    for (const GeneratedSubtitleCue& cue : quickCues) {
        if (cue.endSeconds <= cue.startSeconds) {
            continue;
        }
        stream << onlineVttTime(cue.startSeconds) << " --> "
               << onlineVttTime(cue.endSeconds) << "\n"
               << "mock online source cue " << index++ << "\n\n";
    }
    return true;
}

bool TranslationEnhancementScheduler::_writeMockTranslatedCache(
    const QString& sourceCachePath,
    const QString& translatedCachePath,
    const QString& textPrefix,
    QString* errorMessage) const
{
    const QVector<GeneratedSubtitleCue> sourceCues =
        SubtitleGenerationService::readSubtitleFile(sourceCachePath);
    if (sourceCues.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("source-cache-missing-or-empty");
        }
        return false;
    }
    QDir().mkpath(QFileInfo(translatedCachePath).absolutePath());
    QFile file(translatedCachePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("unable-to-open-translated-cache");
        }
        return false;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "WEBVTT\n\n";
    int index = 1;
    for (const GeneratedSubtitleCue& cue : sourceCues) {
        if (cue.endSeconds <= cue.startSeconds) {
            continue;
        }
        stream << onlineVttTime(cue.startSeconds) << " --> "
               << onlineVttTime(cue.endSeconds) << "\n"
               << textPrefix << " " << index++ << "\n\n";
    }
    return true;
}

bool TranslationEnhancementScheduler::_writeMockRepairCache(
    const QString& quickTranslatedVttPath,
    const QString& repairCachePath,
    QString* errorMessage) const
{
    QVector<GeneratedSubtitleCue> quickCues =
        SubtitleGenerationService::readSubtitleFile(quickTranslatedVttPath);
    if (quickCues.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("quick-baseline-missing");
        }
        return false;
    }
    QDir().mkpath(QFileInfo(repairCachePath).absolutePath());
    QFile file(repairCachePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("unable-to-open-repair-cache");
        }
        return false;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "WEBVTT\n\n";
    int index = 1;
    for (const GeneratedSubtitleCue& cue : quickCues) {
        if (cue.endSeconds <= cue.startSeconds || cue.translatedText.trimmed().isEmpty()) {
            continue;
        }
        QString text = cue.translatedText.trimmed();
        if (index == 1) {
            text = QStringLiteral("mock repaired terminology cue 1");
        }
        stream << onlineVttTime(cue.startSeconds) << " --> "
               << onlineVttTime(cue.endSeconds) << "\n"
               << text << "\n\n";
        ++index;
    }
    return true;
}

bool TranslationEnhancementScheduler::_writeOcrTranslatedCache(
    const TranslationEnhancementScheduleRequest& request,
    const QString& ocrSourceCachePath,
    const QString& ocrTranslatedCachePath,
    int* translatedCueCount,
    QString* errorMessage) const
{
    const QVector<GeneratedSubtitleCue> sourceCues =
        SubtitleGenerationService::readSubtitleFile(ocrSourceCachePath);
    if (sourceCues.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("ocr-source-cache-missing-or-empty");
        }
        return false;
    }
    QVector<GeneratedSubtitleCue> translatedCues;
    translatedCues.reserve(sourceCues.size());
    const QString helper = defaultLongAsrTranslateHelperPath();
    const bool helperReady = QFileInfo::exists(helper);
    const bool providerReady =
        request.translationProviderAvailable &&
        !request.translationBaseUrl.trimmed().isEmpty() &&
        !request.translationApiKey.trimmed().isEmpty();
    QString firstError;
    for (GeneratedSubtitleCue cue : sourceCues) {
        const QString sourceText = !cue.translatedText.trimmed().isEmpty()
            ? cue.translatedText.trimmed()
            : cue.sourceText.trimmed();
        if (sourceText.isEmpty() || translation_text::containsKanaOrReplacement(sourceText)) {
            continue;
        }
        QString translatedText;
        if (translation_text::containsHan(sourceText)) {
            translatedText = normalizeOcrChineseForZhHans(sourceText);
        } else {
            QString translateError;
            if (helperReady) {
                translatedText = translateSubtitleTextWithHelper(
                    helper,
                    sourceText,
                    QStringLiteral("auto"),
                    QStringLiteral("zh"),
                    &translateError);
            }
            if (translatedText.isEmpty() && providerReady) {
                const QString providerKindLower = request.translationProviderKind.trimmed().toLower();
                const QString effectiveModel =
                    request.translationModel.trimmed().isEmpty() &&
                    providerKindLower.contains(QStringLiteral("qwen"))
                        ? QStringLiteral("qwen-plus")
                        : request.translationModel.trimmed();
                if (!effectiveModel.trimmed().isEmpty()) {
                    translatedText = translateSubtitleTextWithOpenAICompatible(
                        QStringLiteral("Translate OCR-read subtitle text faithfully into Simplified Chinese. Return only the subtitle text. Never return the source language. Do not add context, explanations, speakers, inferred subjects, or nearby dialogue. If the source is Chinese/Traditional Chinese, only normalize to Simplified Chinese and punctuation; do not rewrite, expand, or make it more dramatic."),
                        sourceText,
                        request.translationBaseUrl.trimmed(),
                        request.translationApiKey.trimmed(),
                        effectiveModel,
                        &translateError);
                }
            }
            if (translatedText.isEmpty()) {
                if (firstError.isEmpty()) {
                    firstError = translateError.isEmpty()
                        ? QStringLiteral("ocr-source-needs-translation-provider")
                        : translateError;
                }
                continue;
            }
        }
        cue.sourceText = sourceText;
        cue.translatedText = normalizeOcrChineseForZhHans(translatedText);
        translatedCues.push_back(cue);
    }
    if (translatedCueCount) {
        *translatedCueCount = translatedCues.size();
    }
    if (translatedCues.isEmpty()) {
        if (errorMessage) {
            *errorMessage = firstError.isEmpty()
                ? QStringLiteral("ocr-source-needs-translation-provider")
                : firstError;
        }
        return false;
    }
    return _writeRepairCache(translatedCues, ocrTranslatedCachePath, errorMessage);
}

bool TranslationEnhancementScheduler::_writeLocalRepairCache(
    const QString& quickTranslatedVttPath,
    const QString& repairCachePath,
    int* changedCueCount,
    QString* errorMessage) const
{
    QVector<GeneratedSubtitleCue> quickCues =
        SubtitleGenerationService::readSubtitleFile(quickTranslatedVttPath);
    if (quickCues.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("quick-baseline-missing");
        }
        return false;
    }
    int changed = 0;
    for (GeneratedSubtitleCue& cue : quickCues) {
        bool cueChanged = false;
        const QString repaired = semanticRepairText(cue.translatedText, &cueChanged);
        if (cueChanged && !repaired.trimmed().isEmpty()) {
            cue.translatedText = repaired;
            ++changed;
        }
    }
    if (changedCueCount) {
        *changedCueCount = changed;
    }
    if (changed <= 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("local-semantic-repair-no-change");
        }
        return false;
    }
    return _writeRepairCache(quickCues, repairCachePath, errorMessage);
}

bool TranslationEnhancementScheduler::_writeRepairCache(
    const QVector<GeneratedSubtitleCue>& cues,
    const QString& repairCachePath,
    QString* errorMessage) const
{
    QDir().mkpath(QFileInfo(repairCachePath).absolutePath());
    QFile file(repairCachePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("unable-to-open-repair-cache");
        }
        return false;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "WEBVTT\n\n";
    for (const GeneratedSubtitleCue& cue : cues) {
        const QString text = cue.translatedText.trimmed();
        if (cue.endSeconds <= cue.startSeconds || text.isEmpty()) {
            continue;
        }
        stream << onlineVttTime(cue.startSeconds) << " --> "
               << onlineVttTime(cue.endSeconds) << "\n"
               << text << "\n\n";
    }
    return true;
}

QJsonObject TranslationEnhancementScheduler::_layerState(
    const QString& layerId,
    bool configured,
    bool manualAllowed,
    const QString& outputPath) const
{
    return QJsonObject{
        { QStringLiteral("layerId"), layerId },
        { QStringLiteral("configured"), configured },
        { QStringLiteral("manualTriggerAllowed"), manualAllowed },
        { QStringLiteral("defaultEnabled"), false },
        { QStringLiteral("quickPathAutoRun"), false },
        { QStringLiteral("state"), manualAllowed
              ? QStringLiteral("accepted-manual-local-boundary")
              : QStringLiteral("disabled-or-deferred") },
        { QStringLiteral("outputPath"), outputPath },
        { QStringLiteral("writesQuickZhSidecar"), false },
        { QStringLiteral("mergeMode"), QStringLiteral("matching-quick-cue-only") },
        { QStringLiteral("fallback"), QStringLiteral("quick") }
    };
}

} // namespace cgplay
