#pragma once

#include "SubtitleGenerationService.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QStringList>

namespace cgplay {

struct TranslationManualFusionRequest
{
    QString quickTranslatedVttPath;
    QString outputEnhancedVttPath;
    QStringList candidateCachePaths;
    bool manualRequested = false;
};

struct TranslationManualFusionResult
{
    bool attempted = false;
    bool success = false;
    QString outputPath;
    QString reason;
    int quickCueCount = 0;
    int outputCueCount = 0;
    int matchedCueCount = 0;
    int changedCueCount = 0;
    int ignoredUnmatchedCueCount = 0;
    int rejectedCandidateCueCount = 0;
    int candidateCacheCount = 0;
    QStringList usedCachePaths;
    QJsonObject sourceSelectionCounts;
    QString primaryAcceptedSourceType;
    QJsonArray cueSelectionReasons;
    QJsonObject toJson() const;
};

class TranslationFusionController final
{
public:
    QJsonObject describeBoundary(const SubtitleGenerationResult& result) const;
    QJsonObject describeCueLevelPolicy() const;
    QJsonObject describeLayerPolicy(const QString& layerId, const QString& cachePath, int cueCount) const;
    TranslationManualFusionResult writeManualFusionCache(const TranslationManualFusionRequest& request) const;

private:
    int _findMatchingCue(
        const QVector<GeneratedSubtitleCue>& candidates,
        const GeneratedSubtitleCue& quickCue,
        QSet<int>* usedIndexes,
        const QString& sourceType) const;
    bool _timingsAlign(
        const GeneratedSubtitleCue& quickCue,
        const GeneratedSubtitleCue& candidateCue,
        bool looseSourceTiming) const;
    bool _writeVtt(
        const QString& path,
        const QVector<GeneratedSubtitleCue>& cues,
        QString* errorMessage) const;
};

} // namespace cgplay
