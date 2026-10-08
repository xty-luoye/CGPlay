#include "TranslationFusionController.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringConverter>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <limits>

namespace cgplay {

namespace {

QString fusionVttTime(double seconds)
{
    const qint64 totalMs = std::max<qint64>(0, qRound64(seconds * 1000.0));
    const qint64 ms = totalMs % 1000;
    const qint64 totalSeconds = totalMs / 1000;
    const qint64 s = totalSeconds % 60;
    const qint64 totalMinutes = totalSeconds / 60;
    const qint64 m = totalMinutes % 60;
    const qint64 h = totalMinutes / 60;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(ms, 3, 10, QLatin1Char('0'));
}

bool cueHasText(const GeneratedSubtitleCue& cue)
{
    return !cue.translatedText.trimmed().isEmpty();
}

bool containsCjk(const QString& text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (u >= 0x4E00 && u <= 0x9FFF) {
            return true;
        }
    }
    return false;
}

bool containsKana(const QString& text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if ((u >= 0x3040 && u <= 0x30FF) || u == 0xFFFD) {
            return true;
        }
    }
    return false;
}

bool textExpressesUnsafe(const QString& text)
{
    const QString normalized = text.simplified();
    return normalized.contains(QStringLiteral("不安全")) ||
        normalized.contains(QStringLiteral("有危险")) ||
        (normalized.contains(QStringLiteral("危险")) &&
         !normalized.contains(QStringLiteral("没有危险")) &&
         !normalized.contains(QStringLiteral("沒有危險")) &&
         !normalized.contains(QStringLiteral("无危险")) &&
         !normalized.contains(QStringLiteral("無危險")) &&
         !normalized.contains(QStringLiteral("沒危險")) &&
         !normalized.contains(QStringLiteral("不危险")));
}

bool textExpressesSafe(const QString& text)
{
    const QString normalized = text.simplified();
    if (textExpressesUnsafe(normalized)) {
        return false;
    }
    return normalized.contains(QStringLiteral("没有危险")) ||
        normalized.contains(QStringLiteral("沒有危險")) ||
        normalized.contains(QStringLiteral("无危险")) ||
        normalized.contains(QStringLiteral("無危險")) ||
        normalized.contains(QStringLiteral("沒危險")) ||
        normalized.contains(QStringLiteral("不危险")) ||
        normalized.contains(QStringLiteral("很安全")) ||
        normalized.contains(QStringLiteral("这里安全")) ||
        normalized.contains(QStringLiteral("這裡安全"));
}

QString sourceTypeFromPath(const QString& path)
{
    if (path.contains(QStringLiteral(".local.translated.zh."), Qt::CaseInsensitive) ||
        path.contains(QStringLiteral(".embedded.translated.zh."), Qt::CaseInsensitive)) {
        return QStringLiteral("subtitle-source");
    }
    if (path.contains(QStringLiteral(".ocr.translated.zh."), Qt::CaseInsensitive) ||
        path.contains(QStringLiteral(".vision.translated.zh."), Qt::CaseInsensitive) ||
        path.contains(QStringLiteral(".workbench.vision."), Qt::CaseInsensitive)) {
        return QStringLiteral("hard-sub-ocr");
    }
    if (path.contains(QStringLiteral(".longasr.translated.zh."), Qt::CaseInsensitive) ||
        path.contains(QStringLiteral(".hq.translated.zh."), Qt::CaseInsensitive)) {
        return QStringLiteral("long-asr-corrected");
    }
    if (path.contains(QStringLiteral(".online.translated.zh."), Qt::CaseInsensitive)) {
        return QStringLiteral("online-subtitle");
    }
    if (path.contains(QStringLiteral(".repair.zh."), Qt::CaseInsensitive)) {
        return QStringLiteral("repair");
    }
    if (path.contains(QStringLiteral(".refined.zh."), Qt::CaseInsensitive)) {
        return QStringLiteral("refined");
    }
    return QStringLiteral("candidate");
}

int sourceTypePriority(const QString& sourceType)
{
    if (sourceType == QStringLiteral("subtitle-source")) {
        return 100;
    }
    if (sourceType == QStringLiteral("online-subtitle")) {
        return 95;
    }
    if (sourceType == QStringLiteral("hard-sub-ocr")) {
        return 90;
    }
    if (sourceType == QStringLiteral("long-asr-corrected")) {
        return 80;
    }
    if (sourceType == QStringLiteral("repair")) {
        return 60;
    }
    if (sourceType == QStringLiteral("refined")) {
        return 50;
    }
    return 50;
}

int sourcePriority(const QString& path)
{
    return sourceTypePriority(sourceTypeFromPath(path));
}

QString candidateRejectReason(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return QStringLiteral("empty-candidate");
    }
    if (containsKana(trimmed)) {
        return QStringLiteral("japanese-or-replacement-leftover");
    }
    if (!containsCjk(trimmed) && !trimmed.startsWith(QStringLiteral("mock "), Qt::CaseInsensitive)) {
        return QStringLiteral("not-chinese");
    }
    return QString();
}

} // namespace

QJsonObject TranslationManualFusionResult::toJson() const
{
    return QJsonObject{
        { QStringLiteral("attempted"), attempted },
        { QStringLiteral("success"), success },
        { QStringLiteral("outputPath"), outputPath },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("quickCueCount"), quickCueCount },
        { QStringLiteral("outputCueCount"), outputCueCount },
        { QStringLiteral("matchedCueCount"), matchedCueCount },
        { QStringLiteral("changedCueCount"), changedCueCount },
        { QStringLiteral("ignoredUnmatchedCueCount"), ignoredUnmatchedCueCount },
        { QStringLiteral("rejectedCandidateCueCount"), rejectedCandidateCueCount },
        { QStringLiteral("candidateCacheCount"), candidateCacheCount },
        { QStringLiteral("usedCachePaths"), QJsonArray::fromStringList(usedCachePaths) },
        { QStringLiteral("sourceSelectionCounts"), sourceSelectionCounts },
        { QStringLiteral("primaryAcceptedSourceType"), primaryAcceptedSourceType },
        { QStringLiteral("cueSelectionReasons"), cueSelectionReasons },
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
        { QStringLiteral("localOnly"), true },
        { QStringLiteral("usesApi"), false },
        { QStringLiteral("usesQuickWorkerLane"), false },
        { QStringLiteral("writesQuickZhSidecar"), false },
        { QStringLiteral("writesIndependentEnhancedCacheOnly"), success },
        { QStringLiteral("quickTimingPreserved"), success },
        { QStringLiteral("quickCueCountPreserved"), success && quickCueCount == outputCueCount },
        { QStringLiteral("wholeTrackReplaceQuickAllowed"), false },
        { QStringLiteral("fallbackToQuick"), true }
    };
}

QJsonObject TranslationFusionController::describeBoundary(const SubtitleGenerationResult& result) const
{
    QJsonArray layers;
    layers.append(describeLayerPolicy(QStringLiteral("enhanced-fusion"), result.fusionPath, result.fusionEnhancedCueCount));
    layers.append(describeLayerPolicy(QStringLiteral("low-confidence-repair"), result.lowConfidenceRepairPath, result.lowConfidenceRepairCueCount));
    layers.append(describeLayerPolicy(QStringLiteral("online-subtitle"), QString(), result.usedOnlineSubtitle ? result.sourceCueCount : 0));
    layers.append(describeLayerPolicy(QStringLiteral("ocr-subtitle"), QString(), result.ocrAcceptedCueCount));
    return QJsonObject{
        { QStringLiteral("enabledCachePresent"), result.fusionEnabled },
        { QStringLiteral("enhancedPath"), result.fusionPath },
        { QStringLiteral("enhancedCueCount"), result.fusionEnhancedCueCount },
        { QStringLiteral("quickFallbackCueCount"), result.fusionQuickFallbackCueCount },
        { QStringLiteral("mergeMode"), QStringLiteral("cue-level-only") },
        { QStringLiteral("quickFallbackRequired"), true },
        { QStringLiteral("quickWholeTrackReplaceAllowed"), false },
        { QStringLiteral("quickPathAutoRunAllowed"), false },
        { QStringLiteral("policy"), describeCueLevelPolicy() },
        { QStringLiteral("layers"), layers }
    };
}

QJsonObject TranslationFusionController::describeCueLevelPolicy() const
{
    return QJsonObject{
        { QStringLiteral("requiresMatchingQuickCue"), true },
        { QStringLiteral("matchingKeys"), QJsonArray{
              QStringLiteral("cue index when stable"),
              QStringLiteral("time overlap within existing quick cue timing")
          } },
        { QStringLiteral("unmatchedEnhancementDecision"), QStringLiteral("ignore-enhancement-and-display-quick") },
        { QStringLiteral("emptyEnhancementDecision"), QStringLiteral("display-quick") },
        { QStringLiteral("timingAuthority"), QStringLiteral("quick-baseline") },
        { QStringLiteral("canAddCue"), false },
        { QStringLiteral("canRemoveQuickCue"), false },
        { QStringLiteral("canMoveQuickTiming"), false },
        { QStringLiteral("wholeTrackReplaceAllowed"), false }
    };
}

QJsonObject TranslationFusionController::describeLayerPolicy(const QString& layerId, const QString& cachePath, int cueCount) const
{
    return QJsonObject{
        { QStringLiteral("layerId"), layerId },
        { QStringLiteral("cachePath"), cachePath },
        { QStringLiteral("cueCount"), cueCount },
        { QStringLiteral("mergeMode"), QStringLiteral("matching-quick-cue-only") },
        { QStringLiteral("fallback"), QStringLiteral("quick") },
        { QStringLiteral("quickPathAutoRunAllowed"), false },
        { QStringLiteral("wholeTrackReplaceAllowed"), false }
    };
}

TranslationManualFusionResult TranslationFusionController::writeManualFusionCache(
    const TranslationManualFusionRequest& request) const
{
    TranslationManualFusionResult result;
    result.attempted = request.manualRequested;
    result.outputPath = request.outputEnhancedVttPath;
    result.candidateCacheCount = request.candidateCachePaths.size();
    if (!request.manualRequested) {
        result.reason = QStringLiteral("manual-trigger-required");
        return result;
    }
    if (request.quickTranslatedVttPath.trimmed().isEmpty() ||
        !QFileInfo::exists(request.quickTranslatedVttPath)) {
        result.reason = QStringLiteral("quick-baseline-missing");
        return result;
    }
    if (request.outputEnhancedVttPath.trimmed().isEmpty()) {
        result.reason = QStringLiteral("enhanced-output-path-missing");
        return result;
    }

    QVector<GeneratedSubtitleCue> quickCues =
        SubtitleGenerationService::readSubtitleFile(request.quickTranslatedVttPath);
    result.quickCueCount = quickCues.size();
    if (quickCues.isEmpty()) {
        result.reason = QStringLiteral("quick-baseline-empty");
        return result;
    }

    QVector<GeneratedSubtitleCue> outputCues = quickCues;
    QSet<int> changedQuickIndexes;
    int candidateCueTotal = 0;
    struct CandidateChoice
    {
        int candidateIndex = -1;
        int score = -1;
        QString path;
        QString text;
        QString reason;
        QString sourceType;
    };
    QVector<CandidateChoice> bestChoices(outputCues.size());
    for (const QString& candidatePath : request.candidateCachePaths) {
        const QString normalizedCandidate = QFileInfo(candidatePath).absoluteFilePath();
        if (candidatePath.trimmed().isEmpty() ||
            normalizedCandidate == QFileInfo(request.outputEnhancedVttPath).absoluteFilePath() ||
            !QFileInfo::exists(candidatePath)) {
            continue;
        }
        const QVector<GeneratedSubtitleCue> candidateCues =
            SubtitleGenerationService::readSubtitleFile(candidatePath);
        if (candidateCues.isEmpty()) {
            continue;
        }
        candidateCueTotal += candidateCues.size();
        result.usedCachePaths.append(normalizedCandidate);
        const QString sourceType = sourceTypeFromPath(normalizedCandidate);
        const bool reusableSparseCandidate =
            sourceType == QStringLiteral("hard-sub-ocr") ||
            sourceType == QStringLiteral("online-subtitle") ||
            sourceType == QStringLiteral("long-asr-corrected");
        QSet<int> usedCandidateIndexes;
        for (int quickIndex = 0; quickIndex < outputCues.size(); ++quickIndex) {
            GeneratedSubtitleCue& quickCue = outputCues[quickIndex];
            if (!cueHasText(quickCue)) {
                continue;
            }
            const int candidateIndex =
                _findMatchingCue(candidateCues,
                                 quickCue,
                                 reusableSparseCandidate ? nullptr : &usedCandidateIndexes,
                                 sourceType);
            if (candidateIndex < 0) {
                result.cueSelectionReasons.append(QJsonObject{
                    { QStringLiteral("quickCueIndex"), quickIndex + 1 },
                    { QStringLiteral("candidatePath"), normalizedCandidate },
                    { QStringLiteral("sourceType"), sourceType },
                    { QStringLiteral("decision"), QStringLiteral("reject") },
                    { QStringLiteral("reason"), QStringLiteral("no-matching-quick-cue") }
                });
                continue;
            }
            const GeneratedSubtitleCue candidateCue = candidateCues.at(candidateIndex);
            const QString candidateText = candidateCue.translatedText.trimmed();
            const QString rejectReason = candidateRejectReason(candidateText);
            if (!rejectReason.isEmpty()) {
                ++result.rejectedCandidateCueCount;
                result.cueSelectionReasons.append(QJsonObject{
                    { QStringLiteral("quickCueIndex"), quickIndex + 1 },
                    { QStringLiteral("candidatePath"), normalizedCandidate },
                    { QStringLiteral("candidateCueIndex"), candidateIndex + 1 },
                    { QStringLiteral("decision"), QStringLiteral("reject") },
                    { QStringLiteral("reason"), rejectReason }
                });
                continue;
            }
            if (textExpressesUnsafe(quickCue.translatedText) && textExpressesSafe(candidateText)) {
                ++result.rejectedCandidateCueCount;
                result.cueSelectionReasons.append(QJsonObject{
                    { QStringLiteral("quickCueIndex"), quickIndex + 1 },
                    { QStringLiteral("candidatePath"), normalizedCandidate },
                    { QStringLiteral("candidateCueIndex"), candidateIndex + 1 },
                    { QStringLiteral("sourceType"), sourceType },
                    { QStringLiteral("decision"), QStringLiteral("reject") },
                    { QStringLiteral("reason"), QStringLiteral("safety-negation-polarity-conflict") }
                });
                continue;
            }
            if (!reusableSparseCandidate) {
                usedCandidateIndexes.insert(candidateIndex);
            }
            const int score = sourcePriority(normalizedCandidate) +
                (candidateText != quickCue.translatedText.trimmed() ? 10 : 0);
            if (score > bestChoices[quickIndex].score) {
                bestChoices[quickIndex].candidateIndex = candidateIndex;
                bestChoices[quickIndex].score = score;
                bestChoices[quickIndex].path = normalizedCandidate;
                bestChoices[quickIndex].text = candidateText;
                bestChoices[quickIndex].sourceType = sourceType;
                bestChoices[quickIndex].reason =
                    candidateText != quickCue.translatedText.trimmed()
                    ? QStringLiteral("better-candidate-text")
                    : QStringLiteral("valid-candidate-same-text");
            }
        }
    }

    for (int quickIndex = 0; quickIndex < outputCues.size(); ++quickIndex) {
        GeneratedSubtitleCue& quickCue = outputCues[quickIndex];
        const CandidateChoice& choice = bestChoices.at(quickIndex);
        if (choice.candidateIndex < 0) {
            result.cueSelectionReasons.append(QJsonObject{
                { QStringLiteral("quickCueIndex"), quickIndex + 1 },
                { QStringLiteral("decision"), QStringLiteral("fallback-quick") },
                { QStringLiteral("reason"), QStringLiteral("no-accepted-candidate") }
            });
            continue;
        }
        result.matchedCueCount += 1;
        changedQuickIndexes.insert(quickIndex);
        result.cueSelectionReasons.append(QJsonObject{
            { QStringLiteral("quickCueIndex"), quickIndex + 1 },
            { QStringLiteral("candidatePath"), choice.path },
            { QStringLiteral("candidateCueIndex"), choice.candidateIndex + 1 },
            { QStringLiteral("sourceType"), choice.sourceType },
            { QStringLiteral("decision"), QStringLiteral("accept") },
            { QStringLiteral("reason"), choice.reason },
            { QStringLiteral("score"), choice.score }
        });
        const QString countKey = choice.sourceType.isEmpty() ? QStringLiteral("candidate") : choice.sourceType;
        result.sourceSelectionCounts.insert(
            countKey,
            result.sourceSelectionCounts.value(countKey).toInt() + 1);
        if (result.primaryAcceptedSourceType.isEmpty() ||
            sourceTypePriority(countKey) > sourceTypePriority(result.primaryAcceptedSourceType)) {
            result.primaryAcceptedSourceType = countKey;
        }
        if (choice.text != quickCue.translatedText.trimmed()) {
            quickCue.translatedText = choice.text;
            result.changedCueCount += 1;
        }
    }

    result.ignoredUnmatchedCueCount = std::max(0, candidateCueTotal - result.matchedCueCount);
    if (result.usedCachePaths.isEmpty()) {
        result.reason = QStringLiteral("no-independent-enhancement-cache");
        return result;
    }
    if (changedQuickIndexes.isEmpty()) {
        result.reason = QStringLiteral("no-matching-enhancement-cue");
        return result;
    }
    for (int i = 0; i < outputCues.size(); ++i) {
        outputCues[i].index = i + 1;
        outputCues[i].startSeconds = quickCues.at(i).startSeconds;
        outputCues[i].endSeconds = quickCues.at(i).endSeconds;
    }

    QString writeError;
    if (!_writeVtt(request.outputEnhancedVttPath, outputCues, &writeError)) {
        result.reason = writeError.isEmpty() ? QStringLiteral("write-failed") : writeError;
        return result;
    }
    result.success = true;
    result.reason = QStringLiteral("manual-local-fusion-cache-written");
    result.outputCueCount = outputCues.size();
    return result;
}

int TranslationFusionController::_findMatchingCue(
    const QVector<GeneratedSubtitleCue>& candidates,
    const GeneratedSubtitleCue& quickCue,
    QSet<int>* usedIndexes,
    const QString& sourceType) const
{
    const bool looseSourceTiming =
        sourceType == QStringLiteral("hard-sub-ocr") ||
        sourceType == QStringLiteral("online-subtitle") ||
        sourceType == QStringLiteral("long-asr-corrected");
    if (quickCue.index > 0 && quickCue.index <= candidates.size()) {
        const int candidateIndex = quickCue.index - 1;
        if ((!usedIndexes || !usedIndexes->contains(candidateIndex)) &&
            _timingsAlign(quickCue, candidates.at(candidateIndex), looseSourceTiming)) {
            return candidateIndex;
        }
    }
    int bestIndex = -1;
    double bestDelta = std::numeric_limits<double>::max();
    for (int i = 0; i < candidates.size(); ++i) {
        if (usedIndexes && usedIndexes->contains(i)) {
            continue;
        }
        const GeneratedSubtitleCue& candidate = candidates.at(i);
        if (!_timingsAlign(quickCue, candidate, looseSourceTiming)) {
            continue;
        }
        const double delta =
            std::abs(quickCue.startSeconds - candidate.startSeconds) +
            std::abs(quickCue.endSeconds - candidate.endSeconds);
        if (delta < bestDelta) {
            bestDelta = delta;
            bestIndex = i;
        }
    }
    return bestIndex;
}

bool TranslationFusionController::_timingsAlign(
    const GeneratedSubtitleCue& quickCue,
    const GeneratedSubtitleCue& candidateCue,
    bool looseSourceTiming) const
{
    const double quickDuration = std::max(0.05, quickCue.endSeconds - quickCue.startSeconds);
    const double candidateDuration = std::max(0.05, candidateCue.endSeconds - candidateCue.startSeconds);
    const double overlapStart = std::max(quickCue.startSeconds, candidateCue.startSeconds);
    const double overlapEnd = std::min(quickCue.endSeconds, candidateCue.endSeconds);
    const double overlap = std::max(0.0, overlapEnd - overlapStart);
    const bool closeBoundary =
        std::abs(quickCue.startSeconds - candidateCue.startSeconds) <= 0.35 &&
        std::abs(quickCue.endSeconds - candidateCue.endSeconds) <= 0.50;
    const bool mostlyOverlaps =
        overlap / quickDuration >= 0.70 &&
        overlap / candidateDuration >= 0.70;
    if (closeBoundary || mostlyOverlaps) {
        return true;
    }
    if (!looseSourceTiming) {
        return false;
    }

    const bool meaningfulQuickOverlap =
        overlap >= 0.75 ||
        overlap / quickDuration >= 0.25;
    const bool nearStartBoundary =
        std::abs(quickCue.startSeconds - candidateCue.startSeconds) <= 2.50 &&
        candidateCue.endSeconds + 0.50 >= quickCue.startSeconds &&
        candidateCue.startSeconds <= quickCue.endSeconds + 2.00;
    const bool candidateCoversQuickStart =
        candidateCue.startSeconds <= quickCue.startSeconds + 2.00 &&
        candidateCue.endSeconds >= quickCue.startSeconds + std::min(1.00, quickDuration * 0.25);
    const double quickCenter = (quickCue.startSeconds + quickCue.endSeconds) * 0.5;
    const double candidateCenter = (candidateCue.startSeconds + candidateCue.endSeconds) * 0.5;
    const bool candidateCenterInsideQuick =
        candidateCenter >= quickCue.startSeconds - 0.75 &&
        candidateCenter <= quickCue.endSeconds + 0.75;
    const bool quickCenterNearShortCandidate =
        candidateDuration <= 1.50 &&
        std::abs(quickCenter - candidateCenter) <= std::max(1.25, quickDuration * 0.50);
    return meaningfulQuickOverlap ||
        nearStartBoundary ||
        candidateCoversQuickStart ||
        candidateCenterInsideQuick ||
        quickCenterNearShortCandidate;
}

bool TranslationFusionController::_writeVtt(
    const QString& path,
    const QVector<GeneratedSubtitleCue>& cues,
    QString* errorMessage) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("unable-to-open-enhanced-output");
        }
        return false;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "WEBVTT\n\n";
    for (const auto& cue : cues) {
        const QString text = cue.translatedText.trimmed();
        if (text.isEmpty() || cue.endSeconds <= cue.startSeconds) {
            continue;
        }
        stream << fusionVttTime(cue.startSeconds) << " --> "
               << fusionVttTime(cue.endSeconds) << "\n"
               << text << "\n\n";
    }
    return true;
}

} // namespace cgplay
