#include "TranslationBaselineController.h"

namespace cgplay {

QJsonObject TranslationBaselineController::describeQuickBaseline(const SubtitleGenerationResult& result) const
{
    return QJsonObject{
        { QStringLiteral("baseline"), QStringLiteral("quick-.zh-vtt-srt") },
        { QStringLiteral("sourceOfTruth"), true },
        { QStringLiteral("mediaPath"), result.mediaPath },
        { QStringLiteral("quickTranslatedVttPath"), result.translatedVttPath },
        { QStringLiteral("quickCueCount"), result.cues.size() },
        { QStringLiteral("sourceCoverageEndSeconds"), result.sourceCoverageEndSeconds },
        { QStringLiteral("translatedCoverageEndSeconds"), result.translatedCoverageEndSeconds },
        { QStringLiteral("usedSubtitleSource"), result.usedSubtitleSource },
        { QStringLiteral("usedAudioAsr"), result.usedAudioAsr },
        { QStringLiteral("wholeTrackEnhancementReplaceAllowed"), false }
    };
}

QJsonObject TranslationBaselineController::describeRefinementBoundary(const SubtitleRefinementResult& result) const
{
    return QJsonObject{
        { QStringLiteral("baseline"), QStringLiteral("quick-.zh-vtt-srt") },
        { QStringLiteral("refinedPath"), result.refinedVttPath },
        { QStringLiteral("inputCueCount"), result.inputCueCount },
        { QStringLiteral("refinedCueCount"), result.refinedCueCount },
        { QStringLiteral("changedCueCount"), result.changedCueCount },
        { QStringLiteral("mergeMode"), QStringLiteral("cue-level-enhancement-with-quick-fallback") },
        { QStringLiteral("wholeTrackReplaceAllowed"), false }
    };
}

} // namespace cgplay
