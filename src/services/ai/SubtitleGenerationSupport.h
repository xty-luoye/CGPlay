#pragma once

#include "SubtitleGenerationService.h"
#include "media/MediaProbe.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace cgplay {

class ISettingsService;

namespace subtitle_generation_support {

struct SubtitleChunkTask
{
    int order = 0;
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    QString audioPath;
    QVector<GeneratedSubtitleCue> cues;
    QString errorMessage;
    QString skippedReason;
    int extractionRetryCount = 0;
};

struct SubtitleTranslationTask
{
    int batchStart = 0;
    int batchEnd = 0;
    QJsonArray items;
    QVector<QPair<int, QString>> translatedItems;
    QString errorMessage;
};

QString cleanLanguageCode(QString code);
int configuredInt(ISettingsService* settings, const QString& settingsKey, const QString& envKey,
    int fallback, int minimum, int maximum);
bool configuredBool(ISettingsService* settings, const QString& settingsKey, const QString& envKey, bool fallback);
QString subtitleSuffix(const QString& targetLanguage);
QString baseOutputPath(const QString& mediaPath, const QString& outputDirectory);
QString fileContentSha256(const QString& path);
void writeMediaIdentitySidecars(const QStringList& cachePaths, const QString& mediaPath, double durationSeconds);
QStringList candidateSubtitlePaths(const QString& mediaPath);
QString subtitleSourceKindForPath(const QString& path);
bool subtitleTimelineUsable(const QVector<GeneratedSubtitleCue>& cues);
QString detectSubtitleTextLanguage(const QVector<GeneratedSubtitleCue>& cues);
QStringList defaultTerminologyHints();
QString onlineSubtitleApiKey();
QString onlineSubtitleBaseUrl(ISettingsService* settings);
QString srtTime(double seconds);
QString vttTime(double seconds);
double parseSubtitleTime(QString value);
int subtitleMojibakeScore(const QString& text);
QString repairSubtitleMojibake(const QString& text);
bool sourceHintExpectsChinese(const QString& sourceLanguageHint);
bool textContainsHan(const QString& text);
QString normalizeChineseSubtitleLiteralForZhHans(QString text);
QString normalizeJapaneseAsrSourceText(QString text, const QString& sourceLanguageHint);
bool subtitleTranslationFailureCanUsePlaceholder(const QString& error);
bool subtitleTranslationFailureCanFallbackToProvider(const QString& error);
bool workspaceRefinementFailureCanUseSubtitleApiFallback(const QString& error);
bool textContainsJapaneseKana(const QString& text);
QJsonObject analyzeSubtitleSourceQuality(const QString& sourceText, const QString& sourceLanguageHint,
    double durationSeconds);
void annotateSubtitleSourceQuality(GeneratedSubtitleCue* cue, const QString& sourceLanguageHint);
QString subtitleTranslationQualityReason(const GeneratedSubtitleCue& cue);
QString normalizeTranslatedSubtitleLine(QString text);
QString enforceSafetyNegationPolarity(const QString& sourceText, const QString& translatedText);
int applySafetyNegationPolarity(QVector<GeneratedSubtitleCue>* cues);
int applySafetyNegationPolarityFromSourceCues(QVector<GeneratedSubtitleCue>* translatedCues,
    const QVector<GeneratedSubtitleCue>& sourceCues);
void dedupeNearDuplicateSubtitleCues(QVector<GeneratedSubtitleCue>* cues);
QString cueTextForWrite(const GeneratedSubtitleCue& cue, bool translated);
QString stripJsonMarkdownFence(QString text);
QJsonArray extractSegments(const QJsonObject& object);
double segmentNumber(const QJsonObject& object, const QString& key, double fallback);
QString segmentText(const QJsonObject& object);
QString tempAudioChunkPath(const QString& mediaPath, double startSeconds);
void fillTranslationTaskWithSourcePlaceholders(SubtitleTranslationTask* task);
QString defaultAnimeTerminologyPrompt();
QString normalizeAnimeTerminology(QString text);
QVector<GeneratedSubtitleCue> readAssSubtitleFile(const QString& subtitlePath);
QString resolveSubtitleSourcePath(const SubtitleGenerationRequest& request, const MediaInfo& mediaInfo,
    QString* errorMessage);

} // namespace subtitle_generation_support
} // namespace cgplay
