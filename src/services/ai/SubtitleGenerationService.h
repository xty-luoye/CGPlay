#pragma once

#include "ai/api/AIProviderTypes.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>

namespace cgplay {

class IAIProviderManager;
class ISettingsService;

struct GeneratedSubtitleCue
{
    int index = 0;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    QString sourceText;
    QString translatedText;
    bool lowConfidence = false;
    QString qualityReason;
    QJsonObject qualityMetrics;
};

struct SubtitleGenerationRequest
{
    QString mediaPath;
    QString outputDirectory;
    QString subtitleSourcePath;
    QString targetLanguage = QStringLiteral("zh-Hans");
    QString sourceLanguageHint = QStringLiteral("auto");
    QString providerId;
    QString transcriptionModel;
    QString translationModel;
    bool translate = true;
    bool forceRegenerate = false;
    bool mockWithoutApi = false;
    bool useApiTranscription = false;
    double mockCueStartSeconds = -1.0;
    double startSeconds = 0.0;
    double priorityStartSeconds = -1.0;
    double maxDurationSeconds = 0.0;
    std::shared_ptr<std::atomic_bool> cancelRequested;
};

struct SubtitleGenerationResult
{
    bool success = false;
    QString errorMessage;
    QString mediaPath;
    QString audioDirectory;
    QString sourceSrtPath;
    QString sourceVttPath;
    QString translatedSrtPath;
    QString translatedVttPath;
    QString subtitleSourcePath;
    QString subtitleSourceKind;
    QString subtitleSourceError;
    QString subtitleSourceLanguage;
    QString sourcePriorityRank;
    QString sourceSelectionReason;
    QString sourceRejectReason;
    QString providerId;
    QString transcriptionModel;
    QString translationModel;
    bool usedSubtitleSource = false;
    bool usedAudioAsr = false;
    bool subtitleSourceTimelineUsable = false;
    int subtitleSourceCandidateCount = 0;
    bool onlineSearchEnabled = false;
    QString onlineSourceKind;
    double onlineMatchConfidence = 0.0;
    bool usedOnlineSubtitle = false;
    QString onlineError;
    bool ocrEnabled = false;
    QString ocrProvider;
    int ocrSamples = 0;
    int ocrAcceptedCueCount = 0;
    int ocrRejectedCueCount = 0;
    QString ocrError;
    bool fusionEnabled = false;
    QString fusionPath;
    int fusionEnhancedCueCount = 0;
    int fusionQuickFallbackCueCount = 0;
    bool lowConfidenceRepairEnabled = false;
    QString lowConfidenceRepairPath;
    int lowConfidenceRepairCueCount = 0;
    QStringList terminologyHints;
    int sourceCueCount = 0;
    int translatedCueCount = 0;
    int failedTranslationBatchCount = 0;
    int skippedAudioChunkCount = 0;
    QJsonArray skippedAudioChunks;
    double mediaDurationSeconds = 0.0;
    double processedEndSeconds = 0.0;
    double sourceCoverageEndSeconds = 0.0;
    double translatedCoverageEndSeconds = 0.0;
    qint64 durationMs = 0;
    QVector<GeneratedSubtitleCue> cues;
    QJsonObject toJson() const;
};

struct SubtitleRefinementRequest
{
    QString mediaPath;
    QString outputDirectory;
    QString targetLanguage = QStringLiteral("zh-Hans");
    QString sourceLanguageHint = QStringLiteral("auto");
    QString providerId;
    QString model;
    QVector<GeneratedSubtitleCue> sourceCues;
    QVector<GeneratedSubtitleCue> quickCues;
    bool mockWithoutApi = false;
    std::shared_ptr<std::atomic_bool> cancelRequested;
};

struct SubtitleRefinementResult
{
    bool success = false;
    QString errorMessage;
    QString mediaPath;
    QString refinedSrtPath;
    QString refinedVttPath;
    QString providerId;
    QString model;
    int inputCueCount = 0;
    int refinedCueCount = 0;
    int changedCueCount = 0;
    QJsonObject validation;
    qint64 durationMs = 0;
    QVector<GeneratedSubtitleCue> cues;
    QJsonObject toJson() const;
};

class SubtitleGenerationService final
{
public:
    SubtitleGenerationService(
        IAIProviderManager* providerManager,
        ISettingsService* userSettings);

    SubtitleGenerationResult generate(const SubtitleGenerationRequest& request) const;
    SubtitleRefinementResult refine(const SubtitleRefinementRequest& request) const;

    static QString defaultSourceSrtPath(const QString& mediaPath, const QString& outputDirectory = {});
    static QString defaultTranslatedSrtPath(
        const QString& mediaPath,
        const QString& targetLanguage,
        const QString& outputDirectory = {});
    static QString defaultTranslatedVttPath(
        const QString& mediaPath,
        const QString& targetLanguage,
        const QString& outputDirectory = {});
    static QString defaultRefinedTranslatedSrtPath(
        const QString& mediaPath,
        const QString& targetLanguage,
        const QString& outputDirectory = {});
    static QString defaultRefinedTranslatedVttPath(
        const QString& mediaPath,
        const QString& targetLanguage,
        const QString& outputDirectory = {});
    static QString defaultOnlineSubtitleCachePath(const QString& mediaPath, const QString& outputDirectory = {});
    static QString defaultOcrSubtitleCachePath(const QString& mediaPath, const QString& outputDirectory = {});
    static QString defaultEnhancedTranslatedVttPath(
        const QString& mediaPath,
        const QString& targetLanguage,
        const QString& outputDirectory = {});
    static QString defaultLowConfidenceRepairVttPath(
        const QString& mediaPath,
        const QString& targetLanguage,
        const QString& outputDirectory = {});
    static QJsonObject mediaIdentity(const QString& mediaPath, double durationSeconds = 0.0);
    static QString mediaFingerprint(const QString& mediaPath, double durationSeconds = 0.0);
    static QString mediaIdentitySidecarPath(const QString& cachePath);
    static bool writeMediaIdentitySidecar(
        const QString& cachePath,
        const QString& mediaPath,
        double durationSeconds = 0.0,
        QString* errorMessage = nullptr);
    static bool cacheMatchesMediaIdentity(
        const QString& cachePath,
        const QString& mediaPath,
        double durationSeconds = 0.0,
        QString* mismatchReason = nullptr,
        QJsonObject* recordedIdentity = nullptr);
    static QVector<GeneratedSubtitleCue> readSubtitleFile(const QString& subtitlePath);

private:
    QString _providerId(const SubtitleGenerationRequest& request) const;
    QString _providerId(const SubtitleRefinementRequest& request) const;
    QString _transcriptionModel(const SubtitleGenerationRequest& request) const;
    QString _translationModel(const SubtitleGenerationRequest& request) const;
    QString _refinementModel(const SubtitleRefinementRequest& request) const;
    bool _useApiTranscription(const SubtitleGenerationRequest& request, const QString& providerId, const QString& model) const;
    QVector<GeneratedSubtitleCue> _transcribeChunkWithMimo(
        const QString& audioPath,
        double offsetSeconds,
        double fallbackDurationSeconds,
        const SubtitleGenerationRequest& request,
        const QString& model,
        QString* errorMessage) const;
    QString _targetLanguageName(const QString& code) const;
    bool _extractAudioChunk(
        const QString& mediaPath,
        double startSeconds,
        double durationSeconds,
        const QString& outputPath,
        QString* errorMessage) const;
    QVector<GeneratedSubtitleCue> _transcribeChunk(
        const QString& audioPath,
        double offsetSeconds,
        double fallbackDurationSeconds,
        const SubtitleGenerationRequest& request,
        const QString& providerId,
        const QString& model,
        bool preferLocalTranscription,
        bool* disableApiTranscription,
        QString* errorMessage) const;
    bool _translateCues(
        QVector<GeneratedSubtitleCue>* cues,
        const SubtitleGenerationRequest& request,
        const QString& providerId,
        const QString& model,
        QString* errorMessage,
        int* failedBatchCount = nullptr) const;
    bool _chatSubtitleTranslationApi(
        const QString& systemPrompt,
        const QString& userPrompt,
        const QString& model,
        QString* rawText,
        QString* errorMessage) const;
    bool _refineCuesWithContext(
        QVector<GeneratedSubtitleCue>* cues,
        const SubtitleRefinementRequest& request,
        const QString& providerId,
        const QString& model,
        QString* errorMessage) const;
    QJsonObject _validateRefinedCues(
        const QVector<GeneratedSubtitleCue>& quickCues,
        const QVector<GeneratedSubtitleCue>& refinedCues) const;
    bool _writeSrt(const QString& path, const QVector<GeneratedSubtitleCue>& cues, bool translated) const;
    bool _writeVtt(const QString& path, const QVector<GeneratedSubtitleCue>& cues, bool translated) const;

    IAIProviderManager* _providerManager = nullptr;
    ISettingsService* _userSettings = nullptr;
};

} // namespace cgplay
