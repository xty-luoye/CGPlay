#pragma once

#include "TranslationEnhancementScheduler.h"
#include "common/jobs/JobSystem.h"

#include <QJsonObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVector>

namespace cgplay::translation_enhancement_support {

struct FinalDisplaySelection
{
    QString text;
    QString reason;
};

struct VisualFallbackReport
{
    QString noVisualCueFallbackReason;
    QString visualFallbackReason;
};

QString onlineVttTime(double seconds);
QString defaultOnlineHelperPath(const ComponentRuntimePaths& runtime = {});
QString defaultOcrHelperPath(const ComponentRuntimePaths& runtime = {});
QString defaultCodexExecutablePath();
QString subtitleCodexHomePath();
QString defaultLongAsrHelperPath(const ComponentRuntimePaths& runtime = {});
QString defaultLongAsrTranslateHelperPath(const ComponentRuntimePaths& runtime = {});
QString ocrIntakeJsonPath(const QString& ocrSourcePath);
QJsonObject readJsonObjectFile(const QString& path);
ProcessOutcome waitForWorkerProcess(QProcess* process, int timeoutMs,
    const std::shared_ptr<std::atomic_bool>& cancelRequested, bool* canceled);
bool lowerWorkerPriorityForPlayback(QProcess* process, bool playbackAlreadyRunning);
QString onlineSearchJsonPath(const QString& onlineSourcePath);
QString onlineReferenceProviderKind(const TranslationEnhancementScheduleRequest& request);
QString onlineReferenceFallbackReason(const TranslationEnhancementScheduleRequest& request);
QString translateSubtitleTextWithHelper(const QString& helperPath, const QString& text,
    const QString& sourceLanguage, const QString& targetLanguage, QString* errorMessage,
    const ComponentRuntimePaths& runtime = {});
QString translateSubtitleTextWithOpenAICompatible(const QString& systemPrompt, const QString& userPrompt,
    const QString& baseUrl, const QString& apiKey, const QString& model, QString* errorMessage);
bool writeJsonFile(const QString& path, const QJsonObject& payload);
bool isFinalDisplayableText(const QString& text);
QString activeEnhancementSource(bool manualFusionSucceeded, const QString& fusionPrimarySource,
    bool repairSucceeded, bool longAsrSucceeded, bool ocrSucceeded, bool onlineTranslationSucceeded);
QString enhancementFallbackReason(bool visibleEnhancementSucceeded, bool manualAllowed,
    bool executeWorkers, bool anyEnhancementSucceeded, bool highQualityMode);
VisualFallbackReport visualFallbackReport(bool visualCueAtTime, bool visualTrackAuthoritativeAtTime,
    bool visualTrackAuthoritative, bool workbenchCanReadImageText, bool ocrConfigured,
    bool ocrAutoEnabledForHighQualityCurrentMedia, const QString& ocrDegradedReason,
    const QString& finalDisplayedText);
QString visualPrecheckChosenReason(const QString& chosenReason, const QString& visualFallbackReason);
QString reportedVisualCueSourceKind(bool visualSourceCueUsableAtTime,
    bool visualTranslatedCueUsableAtTime, bool visualEnhancedCueAtTime);
QString reportedCurrentFrameSourceKind(bool visualCueAtTime, bool currentFrameUsesWorkbenchVision,
    const QString& workerSourceKind, const QString& visualCueSourceKind,
    bool visualTrackAuthoritativeAtTime);
QString activeHighQualitySource(int enhancedDisplayableCueCount, double enhancedCoverageEndSeconds,
    const QString& manualFusionPrimarySource, int onlineDisplayableCueCount,
    double onlineCoverageEndSeconds, int ocrTranslatedDisplayableCueCount,
    double ocrTranslatedCoverageEndSeconds, int longAsrDisplayableCueCount,
    double longAsrCoverageEndSeconds);
FinalDisplaySelection selectCurrentFrameFinalDisplay(bool visualCueAtTime,
    const QString& visualSourceDisplayNormalized, const QString& visualTranslatedDisplayNormalized,
    const QString& visualEnhancedDisplayNormalized, bool quickCueAtTime,
    const QString& quickDisplayedText, bool visualTrackAuthoritativeAtTime);
double displayableCoverageEndSeconds(const QVector<GeneratedSubtitleCue>& cues);
QJsonObject cacheState(const QString& path, const QString& quickPath = {}, double currentSeconds = 0.0);
QString semanticRepairText(const QString& quickText, bool* changed);
QString normalizeOcrChineseForZhHans(QString text);
QString detectSubtitleLanguageForDiagnostics(const QString& text);
QJsonObject cacheContentHashes(const QStringList& paths);
void commitGeneratedCacheMediaIdentities(const QStringList& paths,
    const QJsonObject& previousHashes, const QJsonObject& mediaIdentity);
QString highQualityNeedReason(const TranslationEnhancementScheduleRequest& request,
    const QJsonObject& quickState, const QJsonObject& enhancedState);
QJsonObject videoClassEntry(const QString& id, const QString& state, const QString& reason,
    const QString& activeSource = {}, bool measured = false);

} // namespace cgplay::translation_enhancement_support
