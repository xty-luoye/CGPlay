#pragma once

#include <QJsonObject>
#include <QString>

namespace cgplay {

struct TranslationGuardSnapshot
{
    QString mediaPath;
    QString generatedSubtitlePath;
    QString translatedVttPath;
    QString refinedVttPath;
    QString enhancedVttPath;
    QString repairVttPath;
    QString playbackMode;
    double currentSeconds = 0.0;
    double sourceCoverageEndSeconds = 0.0;
    double translatedCoverageEndSeconds = 0.0;
    double displayCoverageEndSeconds = 0.0;
    double processedEndSeconds = 0.0;
    int sourceCueCount = 0;
    int translatedCueCount = 0;
    int sourceOnlyCueCount = 0;
    int quickFallbackCueCount = 0;
    int refinedMatchedCueCount = 0;
    int enhancedMatchedCueCount = 0;
    bool enhancedLoaded = false;
    QString enhancedRejectedReason;
    bool visualTextDetected = false;
    bool visualNoTextSuppressQuickAsr = false;
    bool currentCueFallbackSafe = true;
    bool generatedSubtitleBusy = false;
    bool continuationScheduled = false;
    bool continuationQueuedWhileBusy = false;
    bool hasExactTranslatedCue = false;
    bool overlayVisible = false;
    QString overlayText;
    bool refinedMerged = false;
    bool enhancedMerged = false;
    bool refinedWholeTrackTakeover = false;
    bool enhancedWholeTrackTakeover = false;
    QJsonObject serviceDiagnostics;
    QJsonObject playbackStrategyDiagnostics;
    QJsonObject playbackSampling;
};

class TranslationCoverageAuditSkill final
{
public:
    QJsonObject evaluate(const TranslationGuardSnapshot& snapshot) const;
};

class TranslationCacheGuardSkill final
{
public:
    QJsonObject evaluate(const TranslationGuardSnapshot& snapshot) const;
};

class TranslationNoDialogueGuardSkill final
{
public:
    QJsonObject evaluate(const TranslationGuardSnapshot& snapshot) const;
};

class TranslationSourcePriorityGuardSkill final
{
public:
    QJsonObject evaluate(const TranslationGuardSnapshot& snapshot) const;
};

class TranslationReportSkill final
{
public:
    QJsonObject aggregate(
        const TranslationGuardSnapshot& snapshot,
        const QJsonObject& coverageAudit,
        const QJsonObject& cacheGuard,
        const QJsonObject& noDialogueGuard,
        const QJsonObject& sourcePriorityAudit) const;
    bool writeReportFile(const QString& path, const QJsonObject& report, QString* errorMessage = nullptr) const;
};

} // namespace cgplay
