#pragma once

#include <QJsonObject>
#include <QString>

namespace cgplay {

struct TranslationPlaybackModePolicy
{
    QString id;
    QString trigger;
    int enhancedWarmupSecondsMin = 0;
    int enhancedWarmupSecondsMax = 0;
    bool manualOnly = false;
    bool blocksQuickStart = false;
    bool canPausePlayback = false;
    bool changesQuickConstants = false;
    QString fallbackPolicy;

    QJsonObject toJson() const;
};

class TranslationPlaybackStrategy final
{
public:
    static QString quickPlaybackModeId();
    static QString highQualityModeId();
    static QString normalizeModeId(const QString& modeId);
    static QString modeLabel(const QString& modeId);

    QJsonObject defaultQuickPolicy() const;
    QJsonObject manualHighQualityPolicy() const;
    QJsonObject policyForMode(const QString& modeId) const;
    QJsonObject decideDefaultQuick(
        double currentSeconds,
        double quickCoverageEndSeconds,
        double enhancedCoverageEndSeconds) const;
    QJsonObject decideManualHighQuality(
        double currentSeconds,
        double quickCoverageEndSeconds,
        double enhancedCoverageEndSeconds,
        bool explicitlyRequested) const;
    QJsonObject decideForMode(
        const QString& modeId,
        double currentSeconds,
        double quickCoverageEndSeconds,
        double enhancedCoverageEndSeconds,
        bool playbackAlreadyRunning,
        bool explicitlyRequested) const;
};

} // namespace cgplay
