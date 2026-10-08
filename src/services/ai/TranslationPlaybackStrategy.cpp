#include "TranslationPlaybackStrategy.h"

namespace cgplay {

QString TranslationPlaybackStrategy::quickPlaybackModeId()
{
    return QStringLiteral("quick-playback");
}

QString TranslationPlaybackStrategy::highQualityModeId()
{
    return QStringLiteral("high-quality");
}

QString TranslationPlaybackStrategy::normalizeModeId(const QString& modeId)
{
    const QString normalized = modeId.trimmed().toLower();
    if (normalized == QStringLiteral("high-quality") ||
        normalized == QStringLiteral("manual-high-quality")) {
        return highQualityModeId();
    }
    return quickPlaybackModeId();
}

QString TranslationPlaybackStrategy::modeLabel(const QString& modeId)
{
    return normalizeModeId(modeId) == highQualityModeId()
        ? QStringLiteral("High quality")
        : QStringLiteral("Quick playback");
}

QJsonObject TranslationPlaybackModePolicy::toJson() const
{
    return QJsonObject{
        { QStringLiteral("id"), id },
        { QStringLiteral("uiModeId"), TranslationPlaybackStrategy::normalizeModeId(id) },
        { QStringLiteral("label"), TranslationPlaybackStrategy::modeLabel(id) },
        { QStringLiteral("legacyId"),
          TranslationPlaybackStrategy::normalizeModeId(id) == TranslationPlaybackStrategy::highQualityModeId()
              ? QStringLiteral("manual-high-quality")
              : QStringLiteral("quick-default") },
        { QStringLiteral("trigger"), trigger },
        { QStringLiteral("enhancedWarmupSecondsMin"), enhancedWarmupSecondsMin },
        { QStringLiteral("enhancedWarmupSecondsMax"), enhancedWarmupSecondsMax },
        { QStringLiteral("prePlaybackWaitAllowed"), false },
        { QStringLiteral("waitDuringPlaybackAllowed"), false },
        { QStringLiteral("waitIsCancelable"), true },
        { QStringLiteral("fallbackToQuickAllowed"), true },
        { QStringLiteral("manualOnly"), manualOnly },
        { QStringLiteral("defaultEnabled"), !manualOnly },
        { QStringLiteral("blocksQuickStart"), blocksQuickStart },
        { QStringLiteral("canPausePlayback"), canPausePlayback },
        { QStringLiteral("pausesPlayback"), canPausePlayback },
        { QStringLiteral("changesQuickConstants"), changesQuickConstants },
        { QStringLiteral("changesFrozenSpeedConstants"), changesQuickConstants },
        { QStringLiteral("fallbackPolicy"), fallbackPolicy }
    };
}

QJsonObject TranslationPlaybackStrategy::defaultQuickPolicy() const
{
    return TranslationPlaybackModePolicy{
        quickPlaybackModeId(),
        QStringLiteral("default"),
        0,
        0,
        false,
        false,
        false,
        false,
        QStringLiteral("show quick baseline immediately; enhanced/refined can only improve matching cues and must fall back to quick")
    }.toJson();
}

QJsonObject TranslationPlaybackStrategy::manualHighQualityPolicy() const
{
    QJsonObject policy = TranslationPlaybackModePolicy{
        highQualityModeId(),
        QStringLiteral("explicit-user-or-config-only"),
        600,
        720,
        true,
        false,
        false,
        false,
        QStringLiteral("manual high-quality mode may wait before playback; timeout/cancel falls back to quick and active playback is never paused")
    }.toJson();
    policy.insert(QStringLiteral("prePlaybackWaitAllowed"), true);
    policy.insert(QStringLiteral("prePlaybackWaitImplemented"), true);
    policy.insert(QStringLiteral("defaultEnabled"), false);
    return policy;
}

QJsonObject TranslationPlaybackStrategy::policyForMode(const QString& modeId) const
{
    return normalizeModeId(modeId) == highQualityModeId()
        ? manualHighQualityPolicy()
        : defaultQuickPolicy();
}

QJsonObject TranslationPlaybackStrategy::decideDefaultQuick(
    double currentSeconds,
    double quickCoverageEndSeconds,
    double enhancedCoverageEndSeconds) const
{
    const double quickAhead = quickCoverageEndSeconds - currentSeconds;
    const double enhancedAhead = enhancedCoverageEndSeconds - currentSeconds;
    return QJsonObject{
        { QStringLiteral("mode"), quickPlaybackModeId() },
        { QStringLiteral("modeLabel"), modeLabel(quickPlaybackModeId()) },
        { QStringLiteral("decision"), QStringLiteral("play-quick-now") },
        { QStringLiteral("waitBeforeDisplay"), false },
        { QStringLiteral("waitMs"), 0 },
        { QStringLiteral("quickCoverageAheadSeconds"), quickAhead },
        { QStringLiteral("enhancedCoverageAheadSeconds"), enhancedAhead },
        { QStringLiteral("useEnhancedWhenCueMatches"), enhancedAhead > 0.0 },
        { QStringLiteral("fallbackToQuickWhenEnhancedMissing"), true },
        { QStringLiteral("blocksQuickStart"), false },
        { QStringLiteral("pausePlayback"), false },
        { QStringLiteral("pausesPlayback"), false },
        { QStringLiteral("changesQuickConstants"), false },
        { QStringLiteral("changesFrozenSpeedConstants"), false }
    };
}

QJsonObject TranslationPlaybackStrategy::decideManualHighQuality(
    double currentSeconds,
    double quickCoverageEndSeconds,
    double enhancedCoverageEndSeconds,
    bool explicitlyRequested) const
{
    const double quickAhead = quickCoverageEndSeconds - currentSeconds;
    const double enhancedAhead = enhancedCoverageEndSeconds - currentSeconds;
    return QJsonObject{
        { QStringLiteral("mode"), highQualityModeId() },
        { QStringLiteral("modeLabel"), modeLabel(highQualityModeId()) },
        { QStringLiteral("decision"), explicitlyRequested ? QStringLiteral("manual-boundary-available") : QStringLiteral("not-active") },
        { QStringLiteral("explicitlyRequested"), explicitlyRequested },
        { QStringLiteral("defaultEnabled"), false },
        { QStringLiteral("waitBeforeDisplay"), explicitlyRequested },
        { QStringLiteral("waitMs"), 0 },
        { QStringLiteral("prePlaybackWaitImplemented"), true },
        { QStringLiteral("prePlaybackWaitCancelable"), true },
        { QStringLiteral("prePlaybackWaitTimeoutFallbackToQuick"), true },
        { QStringLiteral("quickCoverageAheadSeconds"), quickAhead },
        { QStringLiteral("enhancedCoverageAheadSeconds"), enhancedAhead },
        { QStringLiteral("targetEnhancedWarmupSecondsMin"), 600 },
        { QStringLiteral("targetEnhancedWarmupSecondsMax"), 720 },
        { QStringLiteral("fallbackToQuickWhenEnhancedMissing"), true },
        { QStringLiteral("blocksQuickStart"), false },
        { QStringLiteral("pausePlayback"), false },
        { QStringLiteral("pausesPlayback"), false },
        { QStringLiteral("changesQuickConstants"), false },
        { QStringLiteral("changesFrozenSpeedConstants"), false }
    };
}

QJsonObject TranslationPlaybackStrategy::decideForMode(
    const QString& modeId,
    double currentSeconds,
    double quickCoverageEndSeconds,
    double enhancedCoverageEndSeconds,
    bool playbackAlreadyRunning,
    bool explicitlyRequested) const
{
    const QString normalized = normalizeModeId(modeId);
    QJsonObject decision = normalized == highQualityModeId()
        ? decideManualHighQuality(
              currentSeconds,
              quickCoverageEndSeconds,
              enhancedCoverageEndSeconds,
              explicitlyRequested)
        : decideDefaultQuick(currentSeconds, quickCoverageEndSeconds, enhancedCoverageEndSeconds);
    decision.insert(QStringLiteral("selectedMode"), normalized);
    decision.insert(QStringLiteral("selectedModeLabel"), modeLabel(normalized));
    decision.insert(QStringLiteral("playbackAlreadyRunning"), playbackAlreadyRunning);
    decision.insert(QStringLiteral("waitDuringPlaybackAllowed"), false);
    if (normalized == highQualityModeId()) {
        decision.insert(
            QStringLiteral("decision"),
            playbackAlreadyRunning
                ? QStringLiteral("playback-running-fallback-quick")
                : QStringLiteral("pre-playback-wait-for-displayable-coverage"));
        decision.insert(QStringLiteral("waitBeforeDisplay"), explicitlyRequested && !playbackAlreadyRunning);
    }
    decision.insert(QStringLiteral("pausePlayback"), false);
    decision.insert(QStringLiteral("pausesPlayback"), false);
    decision.insert(QStringLiteral("quickBaselineIsSourceOfTruth"), true);
    decision.insert(QStringLiteral("apiConfigurationModel"), QStringLiteral("workbench-api-plus-asr-api"));
    decision.insert(QStringLiteral("workbenchApiRole"), QStringLiteral("subtitle-search-visual-text-reading-text-translation-repair-fusion"));
    decision.insert(QStringLiteral("asrApiRole"), QStringLiteral("audio-transcription-only-when-no-local-online-visual-subtitle-source"));
    decision.insert(QStringLiteral("cueLevelFallbackToQuick"), true);
    return decision;
}

} // namespace cgplay
