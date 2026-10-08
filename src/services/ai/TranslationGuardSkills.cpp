#include "TranslationGuardSkills.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>

namespace cgplay {

namespace {

bool hasProgressOverlayText(const QString& text)
{
    const QString trimmed = text.trimmed();
    return trimmed.contains(QStringLiteral("正在翻译后续字幕")) ||
        trimmed.contains(QStringLiteral("后续字幕生成中"));
}

bool isQuickBaselinePath(const QString& path)
{
    const QString lower = path.trimmed().toLower();
    return lower.endsWith(QStringLiteral(".zh.vtt")) ||
        lower.endsWith(QStringLiteral(".zh.srt"));
}

bool isEnhancementPath(const QString& path)
{
    const QString lower = path.trimmed().toLower();
    return lower.contains(QStringLiteral(".refined.")) ||
        lower.contains(QStringLiteral(".enhanced.")) ||
        lower.contains(QStringLiteral(".repair.")) ||
        lower.contains(QStringLiteral(".online.")) ||
        lower.contains(QStringLiteral(".ocr."));
}

QJsonObject stateObject(bool passed, const QString& message)
{
    return QJsonObject{
        { QStringLiteral("status"), passed ? QStringLiteral("PASS") : QStringLiteral("FAIL") },
        { QStringLiteral("passed"), passed },
        { QStringLiteral("message"), message }
    };
}

} // namespace

QJsonObject TranslationCoverageAuditSkill::evaluate(const TranslationGuardSnapshot& snapshot) const
{
    const bool displayCoverageUsesTranslated =
        snapshot.displayCoverageEndSeconds <= snapshot.translatedCoverageEndSeconds + 0.01;
    const bool sourceOnlyNotPlayable =
        displayCoverageUsesTranslated;
    const bool translatedCoverageHasPlayableCues =
        snapshot.translatedCueCount > 0 || snapshot.translatedCoverageEndSeconds <= 0.0;
    const bool passed =
        displayCoverageUsesTranslated &&
        sourceOnlyNotPlayable &&
        translatedCoverageHasPlayableCues;
    return QJsonObject{
        { QStringLiteral("skill"), QStringLiteral("CoverageAuditSkill") },
        { QStringLiteral("mode"), QStringLiteral("diagnostic-guard-only") },
        { QStringLiteral("apiUsage"), QStringLiteral("none") },
        { QStringLiteral("workerThreadUsage"), QStringLiteral("none") },
        { QStringLiteral("sidecarCacheOwnership"), QStringLiteral("none") },
        { QStringLiteral("uiImpact"), QStringLiteral("none") },
        { QStringLiteral("autoRun"), QStringLiteral("auto-readonly") },
        { QStringLiteral("canAffectQuickPath"), false },
        { QStringLiteral("sourceCoverageEndSeconds"), snapshot.sourceCoverageEndSeconds },
        { QStringLiteral("translatedCoverageEndSeconds"), snapshot.translatedCoverageEndSeconds },
        { QStringLiteral("displayCoverageEndSeconds"), snapshot.displayCoverageEndSeconds },
        { QStringLiteral("sourceCueCount"), snapshot.sourceCueCount },
        { QStringLiteral("translatedCueCount"), snapshot.translatedCueCount },
        { QStringLiteral("sourceOnlyCueCount"), snapshot.sourceOnlyCueCount },
        { QStringLiteral("displayCoverageUsesTranslatedCoverage"), displayCoverageUsesTranslated },
        { QStringLiteral("sourceOnlyNotPlayableCoverage"), sourceOnlyNotPlayable },
        { QStringLiteral("translatedCoverageHasPlayableCues"), translatedCoverageHasPlayableCues },
        { QStringLiteral("result"), stateObject(
              passed,
              passed
                  ? QStringLiteral("source and translated/displayable coverage remain separated")
                  : QStringLiteral("source-only coverage may be treated as displayable")) }
    };
}

QJsonObject TranslationCacheGuardSkill::evaluate(const TranslationGuardSnapshot& snapshot) const
{
    const bool quickBaselineLoaded =
        isQuickBaselinePath(snapshot.generatedSubtitlePath) &&
        !isEnhancementPath(snapshot.generatedSubtitlePath);
    const bool noWholeTrackTakeover =
        !snapshot.refinedWholeTrackTakeover && !snapshot.enhancedWholeTrackTakeover;
    const bool wholeTrackReplacement = false;
    const bool onlineLayerInactive =
        !snapshot.serviceDiagnostics.value(QStringLiteral("usedOnlineSubtitle")).toBool();
    const bool highQualityMode =
        snapshot.playbackMode == QStringLiteral("high-quality");
    const bool riskyAccuracyLayerInactive =
        highQualityMode ||
        (onlineLayerInactive &&
         !snapshot.serviceDiagnostics.value(QStringLiteral("ocrEnabled")).toBool() &&
         !snapshot.serviceDiagnostics.value(QStringLiteral("fusionEnabled")).toBool() &&
         !snapshot.serviceDiagnostics.value(QStringLiteral("lowConfidenceRepairEnabled")).toBool());
    const bool quickFallbackAvailable =
        snapshot.quickFallbackCueCount >= 0 &&
        snapshot.generatedSubtitlePath.trimmed().contains(QStringLiteral(".zh."));
    const bool passed =
        quickBaselineLoaded &&
        noWholeTrackTakeover &&
        riskyAccuracyLayerInactive &&
        quickFallbackAvailable &&
        snapshot.currentCueFallbackSafe;
    return QJsonObject{
        { QStringLiteral("skill"), QStringLiteral("CacheGuardSkill") },
        { QStringLiteral("mode"), QStringLiteral("diagnostic-guard-only") },
        { QStringLiteral("apiUsage"), QStringLiteral("none") },
        { QStringLiteral("workerThreadUsage"), QStringLiteral("none") },
        { QStringLiteral("sidecarCacheOwnership"), QStringLiteral("none; validates quick .zh priority") },
        { QStringLiteral("uiImpact"), QStringLiteral("none") },
        { QStringLiteral("autoRun"), QStringLiteral("auto-readonly") },
        { QStringLiteral("canAffectQuickPath"), false },
        { QStringLiteral("generatedSubtitlePath"), snapshot.generatedSubtitlePath },
        { QStringLiteral("translatedVttPath"), snapshot.translatedVttPath },
        { QStringLiteral("refinedVttPath"), snapshot.refinedVttPath },
        { QStringLiteral("enhancedVttPath"), snapshot.enhancedVttPath },
        { QStringLiteral("repairVttPath"), snapshot.repairVttPath },
        { QStringLiteral("quickBaselineLoaded"), quickBaselineLoaded },
        { QStringLiteral("quickZhIsDisplayBaseline"), true },
        { QStringLiteral("enhancedLoaded"), snapshot.enhancedLoaded },
        { QStringLiteral("enhancedCueMatches"), snapshot.enhancedMatchedCueCount },
        { QStringLiteral("enhancedRejectedReason"), snapshot.enhancedRejectedReason },
        { QStringLiteral("quickFallbackCueCount"), snapshot.quickFallbackCueCount },
        { QStringLiteral("refinedMerged"), snapshot.refinedMerged },
        { QStringLiteral("enhancedMerged"), snapshot.enhancedMerged },
        { QStringLiteral("refinedWholeTrackTakeover"), snapshot.refinedWholeTrackTakeover },
        { QStringLiteral("enhancedWholeTrackTakeover"), snapshot.enhancedWholeTrackTakeover },
        { QStringLiteral("wholeTrackReplacement"), wholeTrackReplacement },
        { QStringLiteral("currentCueFallbackSafe"), snapshot.currentCueFallbackSafe },
        { QStringLiteral("visualTextDetected"), snapshot.visualTextDetected },
        { QStringLiteral("visualNoTextSuppressQuickAsr"), snapshot.visualNoTextSuppressQuickAsr },
        { QStringLiteral("onlineSearchConfigured"),
          snapshot.serviceDiagnostics.value(QStringLiteral("onlineSearchEnabled")).toBool() },
        { QStringLiteral("usedOnlineSubtitle"),
          snapshot.serviceDiagnostics.value(QStringLiteral("usedOnlineSubtitle")).toBool() },
        { QStringLiteral("riskyAccuracyLayerInactive"), riskyAccuracyLayerInactive },
        { QStringLiteral("noWholeTrackTakeover"), noWholeTrackTakeover },
        { QStringLiteral("result"), stateObject(
              passed,
              passed
                  ? QStringLiteral("quick .zh remains baseline and enhancement layers did not replace it")
                  : QStringLiteral("cache priority or enhancement takeover risk detected")) }
    };
}

QJsonObject TranslationNoDialogueGuardSkill::evaluate(const TranslationGuardSnapshot& snapshot) const
{
    const bool noProgressOverlayDuringNoDialogue =
        snapshot.hasExactTranslatedCue || !hasProgressOverlayText(snapshot.overlayText);
    const bool exactCueVisibleWhenPresent =
        !snapshot.hasExactTranslatedCue ||
        (snapshot.overlayVisible && !snapshot.overlayText.trimmed().isEmpty());
    bool playbackSamplesClean = true;
    if (!snapshot.playbackSampling.isEmpty()) {
        playbackSamplesClean =
            snapshot.playbackSampling.value(QStringLiteral("exactCueHiddenSamples")).toInt() == 0 &&
            snapshot.playbackSampling.value(QStringLiteral("exactCueEmptyTextSamples")).toInt() == 0;
    }
    const bool passed =
        noProgressOverlayDuringNoDialogue &&
        exactCueVisibleWhenPresent &&
        playbackSamplesClean;
    return QJsonObject{
        { QStringLiteral("skill"), QStringLiteral("NoDialogueGuardSkill") },
        { QStringLiteral("mode"), QStringLiteral("diagnostic-guard-only") },
        { QStringLiteral("apiUsage"), QStringLiteral("none") },
        { QStringLiteral("workerThreadUsage"), QStringLiteral("none") },
        { QStringLiteral("sidecarCacheOwnership"), QStringLiteral("none") },
        { QStringLiteral("uiImpact"), QStringLiteral("read-only overlay audit") },
        { QStringLiteral("autoRun"), QStringLiteral("auto-readonly") },
        { QStringLiteral("canAffectQuickPath"), false },
        { QStringLiteral("hasExactTranslatedCue"), snapshot.hasExactTranslatedCue },
        { QStringLiteral("overlayVisible"), snapshot.overlayVisible },
        { QStringLiteral("overlayText"), snapshot.overlayText },
        { QStringLiteral("progressOverlayTextDetected"), hasProgressOverlayText(snapshot.overlayText) },
        { QStringLiteral("noProgressOverlayDuringNoDialogue"), noProgressOverlayDuringNoDialogue },
        { QStringLiteral("exactCueVisibleWhenPresent"), exactCueVisibleWhenPresent },
        { QStringLiteral("playbackSamplesClean"), playbackSamplesClean },
        { QStringLiteral("result"), stateObject(
              passed,
              passed
                  ? QStringLiteral("no-dialogue/no-exact-cue overlay is clean and exact cues stay visible")
                  : QStringLiteral("no-dialogue progress overlay or exact-cue visibility risk detected")) }
    };
}

QJsonObject TranslationSourcePriorityGuardSkill::evaluate(const TranslationGuardSnapshot& snapshot) const
{
    const bool usedSubtitleSource =
        snapshot.serviceDiagnostics.value(QStringLiteral("usedSubtitleSource")).toBool();
    const bool usedAudioAsr =
        snapshot.serviceDiagnostics.value(QStringLiteral("usedAudioAsr")).toBool();
    const int subtitleCandidateCount =
        snapshot.serviceDiagnostics.value(QStringLiteral("subtitleSourceCandidateCount")).toInt();
    const QString sourcePriorityRank =
        snapshot.serviceDiagnostics.value(QStringLiteral("sourcePriorityRank")).toString();
    QString activeHqSource =
        snapshot.playbackStrategyDiagnostics.value(QStringLiteral("activeHqSource")).toString(
            snapshot.playbackStrategyDiagnostics.value(QStringLiteral("activeSource")).toString());
    const QJsonObject enhancement =
        snapshot.playbackStrategyDiagnostics.value(QStringLiteral("enhancementScheduler")).toObject();
    const QString schedulerActiveSource =
        enhancement.value(QStringLiteral("activeSource")).toString();
    if ((activeHqSource.isEmpty() || activeHqSource == QStringLiteral("enhanced")) &&
        !schedulerActiveSource.isEmpty()) {
        activeHqSource = schedulerActiveSource;
    }
    const QJsonObject manualOcrWorker =
        enhancement.value(QStringLiteral("manualOcrWorker")).toObject();
    const bool ocrCandidateReady =
        manualOcrWorker.value(QStringLiteral("ocrTranslatedCacheWritten")).toBool() ||
        enhancement.value(QStringLiteral("ocrTranslatedCueCount")).toInt() > 0 ||
        activeHqSource == QStringLiteral("hard-sub-ocr");
    const bool onlineCandidateReady =
        enhancement.value(QStringLiteral("onlineCandidateCount")).toInt() > 0 ||
        activeHqSource == QStringLiteral("online-subtitle");
    const bool finalUsesHigherPriorityHqSource =
        activeHqSource == QStringLiteral("hard-sub-ocr") ||
        activeHqSource == QStringLiteral("online-subtitle");
    const bool localSubtitlePriorityPassed =
        !usedSubtitleSource ||
        (!usedAudioAsr && sourcePriorityRank.startsWith(QStringLiteral("1-")));
    const bool localCandidateDidNotFallToAsr =
        subtitleCandidateCount <= 0 || usedSubtitleSource || !usedAudioAsr;
    const bool ocrPriorityPassed =
        !(snapshot.playbackMode == QStringLiteral("high-quality") &&
          !usedSubtitleSource &&
          ocrCandidateReady) ||
        finalUsesHigherPriorityHqSource ||
        (snapshot.enhancedLoaded && schedulerActiveSource == QStringLiteral("hard-sub-ocr"));
    const bool asrLastResort =
        !usedAudioAsr ||
        (!usedSubtitleSource &&
         !(snapshot.playbackMode == QStringLiteral("high-quality") &&
           (ocrCandidateReady || onlineCandidateReady))) ||
        finalUsesHigherPriorityHqSource ||
        (snapshot.enhancedLoaded &&
         (schedulerActiveSource == QStringLiteral("hard-sub-ocr") ||
          schedulerActiveSource == QStringLiteral("online-subtitle")));
    const bool passed =
        localSubtitlePriorityPassed &&
        localCandidateDidNotFallToAsr &&
        ocrPriorityPassed &&
        asrLastResort;
    return QJsonObject{
        { QStringLiteral("skill"), QStringLiteral("SourcePriorityGuardSkill") },
        { QStringLiteral("mode"), QStringLiteral("diagnostic-guard-only") },
        { QStringLiteral("apiUsage"), QStringLiteral("none") },
        { QStringLiteral("workerThreadUsage"), QStringLiteral("none") },
        { QStringLiteral("sidecarCacheOwnership"), QStringLiteral("none") },
        { QStringLiteral("uiImpact"), QStringLiteral("none") },
        { QStringLiteral("autoRun"), QStringLiteral("auto-readonly") },
        { QStringLiteral("canAffectQuickPath"), false },
        { QStringLiteral("requiredPriority"), QJsonArray{
              QStringLiteral("1-local-embedded-external-subtitle"),
              QStringLiteral("1b-online-high-confidence-aligned-hq-candidate"),
              QStringLiteral("2-hard-sub-ocr-high-quality"),
              QStringLiteral("3-audio-asr-fallback")
          } },
        { QStringLiteral("onlinePolicy"), QStringLiteral("online is high-quality local-like candidate only; never overrides local/embedded/external quick source") },
        { QStringLiteral("subtitleSourceCandidateCount"), subtitleCandidateCount },
        { QStringLiteral("subtitleSourceKind"),
          snapshot.serviceDiagnostics.value(QStringLiteral("subtitleSourceKind")).toString() },
        { QStringLiteral("sourcePriorityRank"), sourcePriorityRank },
        { QStringLiteral("sourceSelectionReason"),
          snapshot.serviceDiagnostics.value(QStringLiteral("sourceSelectionReason")).toString() },
        { QStringLiteral("sourceRejectReason"),
          snapshot.serviceDiagnostics.value(QStringLiteral("sourceRejectReason")).toString() },
        { QStringLiteral("usedSubtitleSource"), usedSubtitleSource },
        { QStringLiteral("usedAudioAsr"), usedAudioAsr },
        { QStringLiteral("activeHqSource"), activeHqSource },
        { QStringLiteral("schedulerActiveSource"), schedulerActiveSource },
        { QStringLiteral("ocrCandidateReady"), ocrCandidateReady },
        { QStringLiteral("onlineCandidateReady"), onlineCandidateReady },
        { QStringLiteral("finalUsesHigherPriorityHqSource"), finalUsesHigherPriorityHqSource },
        { QStringLiteral("localSubtitlePriorityPassed"), localSubtitlePriorityPassed },
        { QStringLiteral("localCandidateDidNotFallToAsr"), localCandidateDidNotFallToAsr },
        { QStringLiteral("ocrPriorityOverAsrPassed"), ocrPriorityPassed },
        { QStringLiteral("asrLastResort"), asrLastResort },
        { QStringLiteral("result"), stateObject(
              passed,
              passed
                  ? QStringLiteral("source priority is local/embedded/external first, OCR high-quality second, ASR fallback last")
                  : QStringLiteral("source priority risk detected: ASR or a lower-priority source may override subtitle/OCR candidates")) }
    };
}

QJsonObject TranslationReportSkill::aggregate(
    const TranslationGuardSnapshot& snapshot,
    const QJsonObject& coverageAudit,
    const QJsonObject& cacheGuard,
    const QJsonObject& noDialogueGuard,
    const QJsonObject& sourcePriorityAudit) const
{
    const bool passed =
        coverageAudit.value(QStringLiteral("result")).toObject().value(QStringLiteral("passed")).toBool() &&
        cacheGuard.value(QStringLiteral("result")).toObject().value(QStringLiteral("passed")).toBool() &&
        noDialogueGuard.value(QStringLiteral("result")).toObject().value(QStringLiteral("passed")).toBool() &&
        sourcePriorityAudit.value(QStringLiteral("result")).toObject().value(QStringLiteral("passed")).toBool();
    return QJsonObject{
        { QStringLiteral("skill"), QStringLiteral("ReportSkill") },
        { QStringLiteral("mode"), QStringLiteral("diagnostic-guard-only") },
        { QStringLiteral("apiUsage"), QStringLiteral("none") },
        { QStringLiteral("workerThreadUsage"), QStringLiteral("none") },
        { QStringLiteral("sidecarCacheOwnership"), QStringLiteral("none; writes only smoke/runtime report JSON") },
        { QStringLiteral("uiImpact"), QStringLiteral("none") },
        { QStringLiteral("autoRun"), QStringLiteral("auto-readonly") },
        { QStringLiteral("canAffectQuickPath"), false },
        { QStringLiteral("summary"), stateObject(
              passed,
              passed
                  ? QStringLiteral("guard consumers reported no quick-path risk")
                  : QStringLiteral("one or more guard consumers reported a quick-path risk")) },
        { QStringLiteral("executionPlan"), QJsonObject{
              { QStringLiteral("quickPathOwner"), QStringLiteral("SubtitleGenerationService") },
              { QStringLiteral("translationAgentQuickPathRole"), QStringLiteral("diagnostic-boundary-only") },
              { QStringLiteral("canTriggerAsrOrTranslation"), false },
              { QStringLiteral("canTriggerOcrOnlineFusionRepairRefine"), false },
              { QStringLiteral("canChangePlaybackOrScheduling"), false },
              { QStringLiteral("canWriteSubtitleSidecars"), false },
              { QStringLiteral("quickZhIsDisplayBaseline"), true },
              { QStringLiteral("cueLevelEnhancementOnly"), true },
              { QStringLiteral("wholeTrackReplacement"), false },
              { QStringLiteral("currentCueFallbackSafe"), snapshot.currentCueFallbackSafe },
              { QStringLiteral("sourceCoverageSeparateFromDisplayableCoverage"), true }
          } },
        { QStringLiteral("runtime"), QJsonObject{
              { QStringLiteral("mediaPath"), snapshot.mediaPath },
              { QStringLiteral("playbackMode"), snapshot.playbackMode },
              { QStringLiteral("currentSeconds"), snapshot.currentSeconds },
              { QStringLiteral("enhancedLoaded"), snapshot.enhancedLoaded },
              { QStringLiteral("enhancedCueMatches"), snapshot.enhancedMatchedCueCount },
              { QStringLiteral("enhancedRejectedReason"), snapshot.enhancedRejectedReason },
              { QStringLiteral("quickFallbackCueCount"), snapshot.quickFallbackCueCount },
              { QStringLiteral("generatedSubtitleBusy"), snapshot.generatedSubtitleBusy },
              { QStringLiteral("continuationScheduled"), snapshot.continuationScheduled },
              { QStringLiteral("continuationQueuedWhileBusy"), snapshot.continuationQueuedWhileBusy }
          } },
        { QStringLiteral("playbackStrategy"), snapshot.playbackStrategyDiagnostics },
        { QStringLiteral("serviceDiagnostics"), snapshot.serviceDiagnostics },
        { QStringLiteral("coverageAudit"), coverageAudit },
        { QStringLiteral("cacheGuard"), cacheGuard },
        { QStringLiteral("noDialogueGuard"), noDialogueGuard },
        { QStringLiteral("sourcePriorityAudit"), sourcePriorityAudit }
    };
}

bool TranslationReportSkill::writeReportFile(
    const QString& path,
    const QJsonObject& report,
    QString* errorMessage) const
{
    if (path.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("empty report path");
        }
        return false;
    }
    QFileInfo info(path);
    QDir().mkpath(info.absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    return true;
}

} // namespace cgplay
