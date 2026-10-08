#pragma once

#include "SubtitleGenerationService.h"

#include <QJsonObject>
#include <QString>

#include <atomic>
#include <memory>

namespace cgplay {

struct TranslationEnhancementScheduleRequest
{
    QString playbackMode;
    QString mediaPath;
    QString quickTranslatedVttPath;
    QString refinedTranslatedVttPath;
    QString onlineSubtitleCachePath;
    QString onlineTranslatedCachePath;
    QString onlineApiKey;
    QString onlineProvider = QStringLiteral("opensubtitles-compatible");
    QString onlineBaseUrl;
    QString onlineLanguage = QStringLiteral("zh,ja,en");
    QString onlineHelperScriptPath;
    QString localReferenceSubtitlePath;
    QString ocrSubtitleCachePath;
    QString ocrTranslatedCachePath;
    QString ocrHelperScriptPath;
    QString longAsrSourceCachePath;
    QString longAsrTranslatedCachePath;
    QString longAsrHelperScriptPath;
    QString longAsrJsonPath;
    QString translationApiKey;
    QString translationBaseUrl;
    QString translationModel;
    QString translationProviderKind;
    QString workbenchConfigSource;
    QString enhancedTranslatedVttPath;
    QString repairTranslatedVttPath;
    bool manualRequested = false;
    bool onlineMockMode = false;
    bool ocrMockMode = false;
    bool lowConfidenceRepairMockMode = false;
    bool playbackAlreadyRunning = false;
    bool quickGenerationBusy = false;
    bool continuationScheduled = false;
    bool highQualityWaitActive = false;
    bool onlineConfigured = false;
    bool workbenchProviderAvailable = false;
    bool workbenchProviderSupportsText = false;
    bool workbenchProviderSupportsImages = false;
    bool workbenchCanSearchOnlineSubtitles = false;
    bool workbenchCanReadImageText = false;
    bool workbenchCanTranslate = false;
    bool workbenchNoOnlineSearchCapability = false;
    bool translationProviderAvailable = false;
    bool asrProviderAvailable = false;
    bool localReferenceProviderAvailable = false;
    bool ocrConfigured = false;
    bool ocrAutoEnabledForHighQualityCurrentMedia = false;
    bool longAsrConfigured = false;
    bool longAsrAutoEnabledForNoSubtitleHighQuality = false;
    bool fusionConfigured = false;
    bool lowConfidenceRepairConfigured = false;
    bool executeWorkers = true;
    bool allowManualWhilePlayback = false;
    double currentSeconds = 0.0;
    double quickCoverageEndSeconds = 0.0;
    double highQualityTargetCoverageSeconds = 0.0;
    int quickCueCount = 0;
    int lowConfidenceQuickCueCount = 0;
    double lowConfidenceQuickCueRatio = 0.0;
    QString ocrAutoEnableReason;
    std::shared_ptr<std::atomic_bool> cancelRequested;
};

class TranslationEnhancementScheduler final
{
public:
    QJsonObject policy() const;
    QJsonObject evaluate(const TranslationEnhancementScheduleRequest& request) const;

private:
    QJsonObject _layerState(
        const QString& layerId,
        bool configured,
        bool manualAllowed,
        const QString& outputPath) const;
    QJsonObject _runManualOnlineWorker(
        const TranslationEnhancementScheduleRequest& request,
        bool manualAllowed) const;
    QJsonObject _runManualOnlineTranslationWorker(
        const TranslationEnhancementScheduleRequest& request,
        bool manualAllowed,
        const QJsonObject& onlineWorkerResult) const;
    QJsonObject _runManualOcrWorker(
        const TranslationEnhancementScheduleRequest& request,
        bool manualAllowed) const;
    QJsonObject _runManualLongAsrWorker(
        const TranslationEnhancementScheduleRequest& request,
        bool manualAllowed) const;
    QJsonObject _runManualRepairWorker(
        const TranslationEnhancementScheduleRequest& request,
        bool manualAllowed) const;
    QJsonObject _diagnoseOnlineSourceIntake(
        const TranslationEnhancementScheduleRequest& request,
        bool manualAllowed,
        const QJsonObject& onlineWorkerResult) const;
    bool _writeOnlineIntakeDiagnostics(
        const QString& onlineCachePath,
        const QJsonObject& diagnostics,
        QString* outputPath,
        QString* errorMessage) const;
    bool _writeMockOnlineCache(
        const QString& quickTranslatedVttPath,
        const QString& onlineCachePath,
        double fallbackStartSeconds,
        QString* errorMessage) const;
    bool _writeMockTranslatedCache(
        const QString& sourceCachePath,
        const QString& translatedCachePath,
        const QString& textPrefix,
        QString* errorMessage) const;
    bool _writeMockRepairCache(
        const QString& quickTranslatedVttPath,
        const QString& repairCachePath,
        QString* errorMessage) const;
    bool _writeOcrTranslatedCache(
        const TranslationEnhancementScheduleRequest& request,
        const QString& ocrSourceCachePath,
        const QString& ocrTranslatedCachePath,
        int* translatedCueCount,
        QString* errorMessage) const;
    bool _writeLocalRepairCache(
        const QString& quickTranslatedVttPath,
        const QString& repairCachePath,
        int* changedCueCount,
        QString* errorMessage) const;
    bool _writeRepairCache(
        const QVector<GeneratedSubtitleCue>& cues,
        const QString& repairCachePath,
        QString* errorMessage) const;
};

} // namespace cgplay
