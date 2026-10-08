#include "SubtitleGenerationService.h"
#include "SubtitleGenerationSupport.h"

#include "common/jobs/JobSystem.h"
#include "common/core/ServiceLocator.h"
#include "ai/api/IAIProviderManager.h"
#include "ai/api/IAICredentialStore.h"
#include "ai/SubtitleAsrApiClient.h"
#include "media/MediaProbe.h"
#include "settings/api/ISettingsService.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>

namespace cgplay {

namespace {

constexpr int kChunkSeconds = 4;
constexpr double kMinimumAudioChunkSeconds = 0.25;
constexpr qint64 kMaximumAsrAudioBytes = 48LL * 1024LL * 1024LL;
constexpr int kFfmpegTimeoutMs = 120000;
constexpr int kMimoRequestTimeoutMs = 20000;
constexpr int kSubtitleTranslationRequestTimeoutMs = 45000;
constexpr int kMaxTranslateBatch = 6;
constexpr int kMaxSubtitleTranslationAttempts = 2;
constexpr int kDefaultAsrConcurrency = 8;
constexpr int kDefaultTranslationConcurrency = 8;
constexpr qint64 kForwardPartialCoalesceMs = 650;
constexpr auto kMimoDefaultBaseUrl = "https://api.xiaomimimo.com";
constexpr auto kMimoDefaultModel = "mimo-v2.5-asr";
constexpr auto kSubtitleAsrCredentialId = "subtitles/asrApiKey";
constexpr auto kMimoCredentialId = "mimo/apiKey";
constexpr auto kQwenCredentialId = "qwen/apiKey";
constexpr auto kSubtitleTranslationCredentialId = "subtitles/translationApiKey";
constexpr auto kGenericCredentialId = "ai/defaultApiKey";
constexpr auto kOpenAICredentialId = "openai/apiKey";
constexpr auto kQwenDefaultCompatibleBaseUrl = "https://dashscope.aliyuncs.com/compatible-mode/v1";
constexpr auto kQwenDefaultTranslationModel = "qwen-plus";
constexpr auto kQwenDefaultAsrModel = "qwen3-asr-flash";


using namespace subtitle_generation_support;

QStringList pythonCandidates()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates{
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("CGPLAY_PYTHON_EXE")).trimmed(),
        QDir(appDir).filePath(QStringLiteral("runtime/python/python.exe")),
        QDir(appDir).filePath(QStringLiteral("python/python.exe")),
        QDir(appDir).filePath(QStringLiteral("../runtime/python/python.exe")),
        QStandardPaths::findExecutable(QStringLiteral("python")),
        QStandardPaths::findExecutable(QStringLiteral("python3"))
    };
    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(), [](const QString& path) {
            return path.trimmed().isEmpty() || !QFileInfo::exists(path);
        }),
        candidates.end());
    candidates.removeDuplicates();
    return candidates;
}

QString localTranscribeScriptPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates{
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("CGPLAY_LOCAL_TRANSCRIBE_SCRIPT")).trimmed(),
        QDir(appDir).filePath(QStringLiteral("tools/cgplay/ai/local_transcribe.py")),
        QDir(appDir).filePath(QStringLiteral("../tools/cgplay/ai/local_transcribe.py")),
        QDir(appDir).filePath(QStringLiteral("../../tools/cgplay/ai/local_transcribe.py")),
        QDir(appDir).filePath(QStringLiteral("../../../tools/cgplay/ai/local_transcribe.py"))
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return {};
}

QJsonObject runLocalTranscribeScript(
    const QString& audioPath,
    const QString& sourceLanguageHint,
    const QString& modelName,
    int timeoutMs,
    QString* errorMessage)
{
    const QString scriptPath = localTranscribeScriptPath();
    const QStringList pythons = pythonCandidates();
    if (scriptPath.isEmpty() || pythons.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("API transcription failed and local transcriber is unavailable");
        }
        return {};
    }

    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    process.setProcessEnvironment(env);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(
        pythons.first(),
        {
            scriptPath,
            audioPath,
            sourceLanguageHint.trimmed(),
            modelName.trimmed().isEmpty() ? QStringLiteral("large-v3-turbo-ct2") : modelName.trimmed()
        });
    const int processTimeoutMs = std::max(60000, timeoutMs);
    JobContext job(processTimeoutMs);
    const ProcessOutcome processOutcome = job.waitForProcess(process, 25, processTimeoutMs);
    if (processOutcome.state == JobState::TimedOut) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Local transcription timed out");
        }
        return {};
    }

    const QByteArray& stdoutBytes = processOutcome.standardOutput;
    const QByteArray& stderrBytes = processOutcome.standardError;
    const QJsonDocument document = QJsonDocument::fromJson(stdoutBytes);
    const QJsonObject object = document.object();
    if (!processOutcome.succeeded() ||
        !object.value(QStringLiteral("success")).toBool(false)) {
        if (errorMessage) {
            const QString scriptError = object.value(QStringLiteral("error")).toString().trimmed();
            const QString processError = QString::fromUtf8(stderrBytes).trimmed().left(600);
            *errorMessage = scriptError.isEmpty()
                ? (processError.isEmpty() ? process.errorString() : processError)
                : scriptError;
        }
        return object;
    }
    return object;
}

QVector<GeneratedSubtitleCue> cuesFromLocalTranscribeJson(
    const QJsonObject& local,
    double offsetSeconds,
    double fallbackDurationSeconds)
{
    QVector<GeneratedSubtitleCue> cues;
    const QJsonArray segments = local.value(QStringLiteral("segments")).toArray();
    for (const QJsonValue& value : segments) {
        const QJsonObject object = value.toObject();
        const QString text = object.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty()) {
            continue;
        }
        GeneratedSubtitleCue cue;
        cue.startSeconds = offsetSeconds + object.value(QStringLiteral("start")).toDouble(0.0);
        cue.endSeconds = offsetSeconds + object.value(QStringLiteral("end")).toDouble(fallbackDurationSeconds);
        if (cue.endSeconds <= cue.startSeconds) {
            cue.endSeconds = cue.startSeconds + 2.5;
        }
        cue.sourceText = normalizeJapaneseAsrSourceText(text, QStringLiteral("auto"));
        cues.push_back(cue);
    }
    if (cues.isEmpty()) {
        const QString text = local.value(QStringLiteral("text")).toString().trimmed();
        if (!text.isEmpty()) {
            GeneratedSubtitleCue cue;
            cue.startSeconds = offsetSeconds;
            cue.endSeconds = offsetSeconds + std::max(1.0, fallbackDurationSeconds);
            cue.sourceText = text;
            cues.push_back(cue);
        }
    }
    return cues;
}

QJsonObject cueJson(const GeneratedSubtitleCue& cue)
{
    return QJsonObject{
        { QStringLiteral("index"), cue.index },
        { QStringLiteral("startSeconds"), cue.startSeconds },
        { QStringLiteral("endSeconds"), cue.endSeconds },
        { QStringLiteral("sourceText"), cue.sourceText },
        { QStringLiteral("translatedText"), cue.translatedText },
        { QStringLiteral("lowConfidence"), cue.lowConfidence },
        { QStringLiteral("qualityReason"), cue.qualityReason },
        { QStringLiteral("qualityMetrics"), cue.qualityMetrics }
    };
}

double subtitleCueMaxEndSeconds(const QVector<GeneratedSubtitleCue>& cues)
{
    double maxEnd = 0.0;
    for (const auto& cue : cues) {
        maxEnd = std::max(maxEnd, cue.endSeconds);
    }
    return maxEnd;
}

double translatedSubtitleCueMaxEndSeconds(const QVector<GeneratedSubtitleCue>& cues)
{
    double maxEnd = 0.0;
    for (const auto& cue : cues) {
        if (!cueTextForWrite(cue, true).isEmpty()) {
            maxEnd = std::max(maxEnd, cue.endSeconds);
        }
    }
    return maxEnd;
}

bool transcriptionErrorLooksLikeNoSpeech(const QString& errorMessage)
{
    const QString normalized = errorMessage.trimmed().toLower();
    return normalized.contains(QStringLiteral("empty text")) ||
        normalized.contains(QStringLiteral("returned empty text")) ||
        normalized.contains(QStringLiteral("no_speech")) ||
        normalized.contains(QStringLiteral("no speech"));
}

bool transcriptionErrorLooksLikeEmptyAudioChunk(const QString& errorMessage)
{
    const QString normalized = errorMessage.trimmed().toLower();
    return normalized.contains(QStringLiteral("audio-chunk-empty-skipped")) ||
        normalized.contains(QStringLiteral("invalid chunk size: 0 bytes")) ||
        (normalized.contains(QStringLiteral("audio extraction produced invalid chunk size")) &&
         normalized.contains(QStringLiteral("0 bytes")));
}

bool testShouldForceEmptyAudioChunk(double startSeconds)
{
    const QString raw =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("CGPLAY_TEST_FORCE_EMPTY_AUDIO_CHUNK_START")).trimmed();
    if (raw.isEmpty()) {
        return false;
    }
    bool ok = false;
    const double forcedStart = raw.toDouble(&ok);
    return ok && std::abs(forcedStart - startSeconds) < 0.05;
}

bool transcriptionErrorLooksLikeRateLimit(const QString& errorMessage)
{
    const QString normalized = errorMessage.trimmed().toLower();
    return normalized.contains(QStringLiteral("429")) ||
        normalized.contains(QStringLiteral("timed out")) ||
        normalized.contains(QStringLiteral("timeout")) ||
        normalized.contains(QStringLiteral("quota/rate limit")) ||
        normalized.contains(QStringLiteral("resource_exhausted")) ||
        normalized.contains(QStringLiteral("quota exceeded")) ||
        normalized.contains(QStringLiteral("rate limit"));
}

bool transcriptionErrorLooksLikeUnsupportedEndpoint(const QString& errorMessage)
{
    const QString normalized = errorMessage.trimmed().toLower();
    return normalized.contains(QStringLiteral("404")) ||
        normalized.contains(QStringLiteral("not found")) ||
        normalized.contains(QStringLiteral("audio/transcriptions")) ||
        normalized.contains(QStringLiteral("audio transcription"));
}

bool modelLooksLikeAudioTranscriptionModel(const QString& model)
{
    const QString normalized = model.trimmed().toLower();
    return normalized.contains(QStringLiteral("transcribe")) ||
        normalized.contains(QStringLiteral("whisper")) ||
        normalized.contains(QStringLiteral("mimo")) ||
        normalized.contains(QStringLiteral("asr"));
}

bool providerLooksLikeTextOnlyProvider(const QString& providerId)
{
    const QString normalized = providerId.trimmed().toLower();
    return normalized.contains(QStringLiteral("deepseek")) ||
        normalized.contains(QStringLiteral("anthropic")) ||
        normalized.contains(QStringLiteral("claude")) ||
        normalized.contains(QStringLiteral("gemini")) ||
        normalized.contains(QStringLiteral("ollama"));
}

bool modelLooksLikeMimoModel(const QString& model)
{
    const QString normalized = model.trimmed().toLower();
    return normalized.contains(QStringLiteral("mimo"));
}

bool modelLooksUnsuitableForSubtitleTranslation(const QString& model)
{
    const QString normalized = model.trimmed().toLower();
    return normalized.isEmpty() ||
        normalized == QStringLiteral("auto") ||
        normalized.contains(QStringLiteral("mimo")) ||
        normalized.contains(QStringLiteral("transcribe")) ||
        normalized.contains(QStringLiteral("whisper")) ||
        normalized.contains(QStringLiteral("asr"));
}

QString trimTrailingSlashes(QString value)
{
    value = value.trimmed();
    while (value.endsWith(QLatin1Char('/'))) {
        value.chop(1);
    }
    return value;
}

QString normalizeMimoChatEndpoint(QString baseUrl)
{
    baseUrl = trimTrailingSlashes(baseUrl);
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromLatin1(kMimoDefaultBaseUrl);
    }
    if (baseUrl.endsWith(QStringLiteral("/chat/completions"), Qt::CaseInsensitive)) {
        return baseUrl;
    }
    if (baseUrl.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive)) {
        return baseUrl + QStringLiteral("/chat/completions");
    }
    return baseUrl + QStringLiteral("/v1/chat/completions");
}

QString normalizeOpenAIChatEndpoint(QString baseUrl)
{
    baseUrl = trimTrailingSlashes(baseUrl);
    if (baseUrl.isEmpty()) {
        return {};
    }
    if (baseUrl.endsWith(QStringLiteral("/chat/completions"), Qt::CaseInsensitive)) {
        return baseUrl;
    }
    if (baseUrl.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive)) {
        return baseUrl + QStringLiteral("/chat/completions");
    }
    return baseUrl + QStringLiteral("/v1/chat/completions");
}

QString audioFormatFromPath(const QString& audioPath)
{
    const QString suffix = QFileInfo(audioPath).suffix().trimmed().toLower();
    if (suffix == QStringLiteral("mp3")) {
        return QStringLiteral("mp3");
    }
    return QStringLiteral("wav");
}

QString jsonContentText(const QJsonValue& value)
{
    if (value.isString()) {
        return value.toString().trimmed();
    }
    if (value.isArray()) {
        QStringList parts;
        for (const QJsonValue& item : value.toArray()) {
            const QJsonObject object = item.toObject();
            const QString text = object.value(QStringLiteral("text")).toString().trimmed();
            if (!text.isEmpty()) {
                parts.push_back(text);
            }
        }
        return parts.join(QLatin1Char('\n')).trimmed();
    }
    return {};
}

QString firstChatCompletionText(const QJsonObject& object)
{
    const QJsonArray choices = object.value(QStringLiteral("choices")).toArray();
    if (!choices.isEmpty()) {
        const QJsonObject message = choices.first().toObject().value(QStringLiteral("message")).toObject();
        const QString content = jsonContentText(message.value(QStringLiteral("content")));
        if (!content.isEmpty()) {
            return content;
        }
    }
    return object.value(QStringLiteral("text")).toString().trimmed();
}

} // namespace

QJsonObject SubtitleGenerationResult::toJson() const
{
    QJsonArray cueArray;
    for (const auto& cue : cues) {
        cueArray.append(cueJson(cue));
    }
    QJsonArray terminologyArray;
    for (const QString& term : terminologyHints) {
        terminologyArray.append(term);
    }
    return QJsonObject{
        { QStringLiteral("success"), success },
        { QStringLiteral("errorMessage"), errorMessage },
        { QStringLiteral("mediaPath"), mediaPath },
        { QStringLiteral("mediaFingerprint"), SubtitleGenerationService::mediaFingerprint(mediaPath) },
        { QStringLiteral("mediaIdentity"), SubtitleGenerationService::mediaIdentity(mediaPath, mediaDurationSeconds) },
        { QStringLiteral("audioDirectory"), audioDirectory },
        { QStringLiteral("sourceSrtPath"), sourceSrtPath },
        { QStringLiteral("sourceVttPath"), sourceVttPath },
        { QStringLiteral("translatedSrtPath"), translatedSrtPath },
        { QStringLiteral("translatedVttPath"), translatedVttPath },
        { QStringLiteral("subtitleSourcePath"), subtitleSourcePath },
        { QStringLiteral("subtitleSourceKind"), subtitleSourceKind },
        { QStringLiteral("subtitleSourceError"), subtitleSourceError },
        { QStringLiteral("subtitleSourceLanguage"), subtitleSourceLanguage },
        { QStringLiteral("sourcePriorityRank"), sourcePriorityRank },
        { QStringLiteral("sourceSelectionReason"), sourceSelectionReason },
        { QStringLiteral("sourceRejectReason"), sourceRejectReason },
        { QStringLiteral("providerId"), providerId },
        { QStringLiteral("transcriptionModel"), transcriptionModel },
        { QStringLiteral("translationModel"), translationModel },
        { QStringLiteral("usedSubtitleSource"), usedSubtitleSource },
        { QStringLiteral("usedAudioAsr"), usedAudioAsr },
        { QStringLiteral("subtitleSourceTimelineUsable"), subtitleSourceTimelineUsable },
        { QStringLiteral("subtitleSourceCandidateCount"), subtitleSourceCandidateCount },
        { QStringLiteral("onlineSearchEnabled"), onlineSearchEnabled },
        { QStringLiteral("onlineSourceKind"), onlineSourceKind },
        { QStringLiteral("onlineMatchConfidence"), onlineMatchConfidence },
        { QStringLiteral("usedOnlineSubtitle"), usedOnlineSubtitle },
        { QStringLiteral("onlineError"), onlineError },
        { QStringLiteral("ocrEnabled"), ocrEnabled },
        { QStringLiteral("ocrProvider"), ocrProvider },
        { QStringLiteral("ocrSamples"), ocrSamples },
        { QStringLiteral("ocrAcceptedCueCount"), ocrAcceptedCueCount },
        { QStringLiteral("ocrRejectedCueCount"), ocrRejectedCueCount },
        { QStringLiteral("ocrError"), ocrError },
        { QStringLiteral("fusionEnabled"), fusionEnabled },
        { QStringLiteral("fusionPath"), fusionPath },
        { QStringLiteral("fusionEnhancedCueCount"), fusionEnhancedCueCount },
        { QStringLiteral("fusionQuickFallbackCueCount"), fusionQuickFallbackCueCount },
        { QStringLiteral("lowConfidenceRepairEnabled"), lowConfidenceRepairEnabled },
        { QStringLiteral("lowConfidenceRepairPath"), lowConfidenceRepairPath },
        { QStringLiteral("lowConfidenceRepairCueCount"), lowConfidenceRepairCueCount },
        { QStringLiteral("terminologyHints"), terminologyArray },
        { QStringLiteral("sourceCueCount"), sourceCueCount },
        { QStringLiteral("translatedCueCount"), translatedCueCount },
        { QStringLiteral("failedTranslationBatchCount"), failedTranslationBatchCount },
        { QStringLiteral("skippedAudioChunkCount"), skippedAudioChunkCount },
        { QStringLiteral("skippedAudioChunks"), skippedAudioChunks },
        { QStringLiteral("mediaDurationSeconds"), mediaDurationSeconds },
        { QStringLiteral("processedEndSeconds"), processedEndSeconds },
        { QStringLiteral("sourceCoverageEndSeconds"), sourceCoverageEndSeconds },
        { QStringLiteral("translatedCoverageEndSeconds"), translatedCoverageEndSeconds },
        { QStringLiteral("durationMs"), static_cast<double>(durationMs) },
        { QStringLiteral("cues"), cueArray }
    };
}

QJsonObject SubtitleRefinementResult::toJson() const
{
    QJsonArray cueArray;
    for (const auto& cue : cues) {
        cueArray.append(cueJson(cue));
    }
    return QJsonObject{
        { QStringLiteral("success"), success },
        { QStringLiteral("errorMessage"), errorMessage },
        { QStringLiteral("mediaPath"), mediaPath },
        { QStringLiteral("mediaFingerprint"), SubtitleGenerationService::mediaFingerprint(mediaPath) },
        { QStringLiteral("mediaIdentity"), SubtitleGenerationService::mediaIdentity(mediaPath) },
        { QStringLiteral("refinedSrtPath"), refinedSrtPath },
        { QStringLiteral("refinedVttPath"), refinedVttPath },
        { QStringLiteral("providerId"), providerId },
        { QStringLiteral("model"), model },
        { QStringLiteral("inputCueCount"), inputCueCount },
        { QStringLiteral("refinedCueCount"), refinedCueCount },
        { QStringLiteral("changedCueCount"), changedCueCount },
        { QStringLiteral("validation"), validation },
        { QStringLiteral("durationMs"), static_cast<double>(durationMs) },
        { QStringLiteral("cues"), cueArray }
    };
}

SubtitleGenerationService::SubtitleGenerationService(
    IAIProviderManager* providerManager,
    ISettingsService* userSettings)
    : _providerManager(providerManager)
    , _userSettings(userSettings)
{
}

QString SubtitleGenerationService::defaultSourceSrtPath(const QString& mediaPath, const QString& outputDirectory)
{
    return baseOutputPath(mediaPath, outputDirectory) + QStringLiteral(".source.srt");
}

QString SubtitleGenerationService::defaultTranslatedSrtPath(
    const QString& mediaPath,
    const QString& targetLanguage,
    const QString& outputDirectory)
{
    return baseOutputPath(mediaPath, outputDirectory) + QStringLiteral(".%1.srt").arg(subtitleSuffix(targetLanguage));
}

QString SubtitleGenerationService::defaultTranslatedVttPath(
    const QString& mediaPath,
    const QString& targetLanguage,
    const QString& outputDirectory)
{
    return baseOutputPath(mediaPath, outputDirectory) + QStringLiteral(".%1.vtt").arg(subtitleSuffix(targetLanguage));
}

QString SubtitleGenerationService::defaultRefinedTranslatedSrtPath(
    const QString& mediaPath,
    const QString& targetLanguage,
    const QString& outputDirectory)
{
    return baseOutputPath(mediaPath, outputDirectory) + QStringLiteral(".refined.%1.srt").arg(subtitleSuffix(targetLanguage));
}

QString SubtitleGenerationService::defaultRefinedTranslatedVttPath(
    const QString& mediaPath,
    const QString& targetLanguage,
    const QString& outputDirectory)
{
    return baseOutputPath(mediaPath, outputDirectory) + QStringLiteral(".refined.%1.vtt").arg(subtitleSuffix(targetLanguage));
}

QString SubtitleGenerationService::defaultOnlineSubtitleCachePath(const QString& mediaPath, const QString& outputDirectory)
{
    return baseOutputPath(mediaPath, outputDirectory) + QStringLiteral(".online.source.vtt");
}

QString SubtitleGenerationService::defaultOcrSubtitleCachePath(const QString& mediaPath, const QString& outputDirectory)
{
    return baseOutputPath(mediaPath, outputDirectory) + QStringLiteral(".ocr.source.vtt");
}

QString SubtitleGenerationService::defaultEnhancedTranslatedVttPath(
    const QString& mediaPath,
    const QString& targetLanguage,
    const QString& outputDirectory)
{
    return baseOutputPath(mediaPath, outputDirectory) + QStringLiteral(".enhanced.%1.vtt").arg(subtitleSuffix(targetLanguage));
}

QString SubtitleGenerationService::defaultLowConfidenceRepairVttPath(
    const QString& mediaPath,
    const QString& targetLanguage,
    const QString& outputDirectory)
{
    return baseOutputPath(mediaPath, outputDirectory) + QStringLiteral(".repair.%1.vtt").arg(subtitleSuffix(targetLanguage));
}

QJsonObject SubtitleGenerationService::mediaIdentity(const QString& mediaPath, double durationSeconds)
{
    const QFileInfo info(mediaPath);
    const QString absolutePath = info.absoluteFilePath();
    const qint64 fileSize = info.exists() ? info.size() : -1;
    const qint64 mtimeMs = info.exists() ? info.lastModified().toUTC().toMSecsSinceEpoch() : -1;
    const qint64 durationMs = durationSeconds > 0.0 ? qRound64(durationSeconds * 1000.0) : -1;
    const QString fingerprintInput = QStringLiteral("%1|%2|%3")
        .arg(absolutePath.toLower(), QString::number(fileSize), QString::number(mtimeMs));
    const QString fingerprint =
        QString::fromLatin1(QCryptographicHash::hash(fingerprintInput.toUtf8(), QCryptographicHash::Sha256).toHex());
    return QJsonObject{
        { QStringLiteral("mediaPath"), absolutePath },
        { QStringLiteral("fileSize"), fileSize },
        { QStringLiteral("mtimeMs"), mtimeMs },
        { QStringLiteral("durationMs"), durationMs },
        { QStringLiteral("mediaFingerprint"), fingerprint }
    };
}

QString SubtitleGenerationService::mediaFingerprint(const QString& mediaPath, double durationSeconds)
{
    return mediaIdentity(mediaPath, durationSeconds).value(QStringLiteral("mediaFingerprint")).toString();
}

QString SubtitleGenerationService::mediaIdentitySidecarPath(const QString& cachePath)
{
    const QString trimmed = cachePath.trimmed();
    return trimmed.isEmpty() ? QString() : QFileInfo(trimmed).absoluteFilePath() + QStringLiteral(".media.json");
}

bool SubtitleGenerationService::writeMediaIdentitySidecar(
    const QString& cachePath,
    const QString& mediaPath,
    double durationSeconds,
    QString* errorMessage)
{
    if (cachePath.trimmed().isEmpty() || mediaPath.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("cache-or-media-path-empty");
        }
        return false;
    }
    QJsonObject payload = mediaIdentity(mediaPath, durationSeconds);
    payload.insert(QStringLiteral("cachePath"), QFileInfo(cachePath).absoluteFilePath());
    payload.insert(QStringLiteral("schema"), QStringLiteral("cgplay-media-identity-v1"));
    payload.insert(QStringLiteral("cacheContentSha256"), fileContentSha256(cachePath));
    payload.insert(QStringLiteral("cacheContentSize"), QFileInfo(cachePath).size());
    payload.insert(QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    const QString sidecarPath = mediaIdentitySidecarPath(cachePath);
    QDir().mkpath(QFileInfo(sidecarPath).absolutePath());
    QFile file(sidecarPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }
    file.write(QJsonDocument(payload).toJson(QJsonDocument::Indented));
    return true;
}

bool SubtitleGenerationService::cacheMatchesMediaIdentity(
    const QString& cachePath,
    const QString& mediaPath,
    double durationSeconds,
    QString* mismatchReason,
    QJsonObject* recordedIdentity)
{
    const QString sidecarPath = mediaIdentitySidecarPath(cachePath);
    QFile file(sidecarPath);
    if (!file.exists()) {
        if (mismatchReason) {
            *mismatchReason = QStringLiteral("cache-media-metadata-missing");
        }
        return false;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (mismatchReason) {
            *mismatchReason = QStringLiteral("cache-media-metadata-unreadable:%1").arg(file.errorString());
        }
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    const QJsonObject recorded = doc.object();
    if (recordedIdentity) {
        *recordedIdentity = recorded;
    }
    const QJsonObject current = mediaIdentity(mediaPath, durationSeconds);
    const QString recordedFingerprint = recorded.value(QStringLiteral("mediaFingerprint")).toString();
    const QString currentFingerprint = current.value(QStringLiteral("mediaFingerprint")).toString();
    if (recordedFingerprint.isEmpty() || recordedFingerprint != currentFingerprint) {
        if (mismatchReason) {
            *mismatchReason = QStringLiteral("cache-media-mismatch");
        }
        return false;
    }
    const qint64 recordedDurationMs = recorded.value(QStringLiteral("durationMs")).toVariant().toLongLong();
    const qint64 currentDurationMs = current.value(QStringLiteral("durationMs")).toVariant().toLongLong();
    if (recordedDurationMs >= 0 && currentDurationMs >= 0 &&
        std::abs(static_cast<double>(recordedDurationMs - currentDurationMs)) > 1000.0) {
        if (mismatchReason) {
            *mismatchReason = QStringLiteral("cache-media-duration-mismatch");
        }
        return false;
    }
    const QString recordedContentSha = recorded.value(QStringLiteral("cacheContentSha256")).toString();
    if (!recordedContentSha.isEmpty()) {
        const QString currentContentSha = fileContentSha256(cachePath);
        if (currentContentSha.isEmpty() || currentContentSha != recordedContentSha) {
            if (mismatchReason) {
                *mismatchReason = QStringLiteral("cache-content-hash-mismatch");
            }
            return false;
        }
    } else if (recorded.value(QStringLiteral("source")).toString() ==
               QStringLiteral("headless-acceptance-cache-sync")) {
        if (mismatchReason) {
            *mismatchReason = QStringLiteral("cache-content-hash-missing-for-synced-cache");
        }
        return false;
    }
    return true;
}

QVector<GeneratedSubtitleCue> SubtitleGenerationService::readSubtitleFile(const QString& subtitlePath)
{
    const QString suffix = QFileInfo(subtitlePath).suffix().trimmed().toLower();
    if (suffix == QStringLiteral("ass") || suffix == QStringLiteral("ssa")) {
        return readAssSubtitleFile(subtitlePath);
    }

    QFile file(subtitlePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    const QString content = QString::fromUtf8(file.readAll()).replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    const QStringList blocks = content.split(QRegularExpression(QStringLiteral("\n\\s*\n")), Qt::SkipEmptyParts);
    QVector<GeneratedSubtitleCue> cues;
    QRegularExpression timingRe(QStringLiteral("(\\d{2}:\\d{2}:\\d{2}[,.]\\d{3})\\s*-->\\s*(\\d{2}:\\d{2}:\\d{2}[,.]\\d{3})"));
    for (const QString& rawBlock : blocks) {
        QStringList lines = rawBlock.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (lines.isEmpty() || lines.first().trimmed() == QStringLiteral("WEBVTT")) {
            continue;
        }
        int timingIndex = -1;
        QRegularExpressionMatch timingMatch;
        for (int i = 0; i < lines.size(); ++i) {
            timingMatch = timingRe.match(lines.at(i));
            if (timingMatch.hasMatch()) {
                timingIndex = i;
                break;
            }
        }
        if (timingIndex < 0) {
            continue;
        }
        GeneratedSubtitleCue cue;
        cue.index = cues.size() + 1;
        cue.startSeconds = parseSubtitleTime(timingMatch.captured(1));
        cue.endSeconds = parseSubtitleTime(timingMatch.captured(2));
        cue.translatedText = lines.mid(timingIndex + 1).join(QLatin1Char('\n')).trimmed();
        cue.sourceText = cue.translatedText;
        if (!cue.translatedText.isEmpty()) {
            cues.push_back(cue);
        }
    }
    return cues;
}

SubtitleGenerationResult SubtitleGenerationService::generate(const SubtitleGenerationRequest& request) const
{
    QElapsedTimer timer;
    timer.start();

    SubtitleGenerationResult result;
    result.mediaPath = QFileInfo(request.mediaPath).absoluteFilePath();
    result.providerId = _providerId(request);
    result.transcriptionModel = _transcriptionModel(request);
    result.translationModel = _translationModel(request);

    const auto fail = [&result, &timer](const QString& message) {
        result.success = false;
        result.errorMessage = message;
        result.durationMs = timer.elapsed();
        return result;
    };

    if (result.mediaPath.isEmpty() || !QFileInfo::exists(result.mediaPath)) {
        return fail(QStringLiteral("Media file not found"));
    }
    const auto isCanceled = [&request]() {
        return request.cancelRequested && request.cancelRequested->load();
    };
    if (isCanceled()) {
        return fail(QStringLiteral("Subtitle generation canceled"));
    }

    const MediaInfo mediaInfo = MediaProbe::probe(result.mediaPath);
    result.mediaDurationSeconds = mediaInfo.durationSeconds > 0.0
        ? mediaInfo.durationSeconds
        : std::max(1, mediaInfo.effectiveFrameCount()) / std::max(1.0, mediaInfo.fps);
    result.terminologyHints = defaultTerminologyHints();
    result.onlineSearchEnabled = configuredBool(
        _userSettings,
        QStringLiteral("ai/subtitles/online/enabled"),
        QStringLiteral("SUBTITLE_ONLINE_ENABLED"),
        false);
    result.onlineSourceKind = QStringLiteral("opensubtitles-rest");
    if (result.onlineSearchEnabled) {
        const QString apiKey = onlineSubtitleApiKey();
        result.onlineError = apiKey.isEmpty()
            ? QStringLiteral("online subtitle API key is not configured")
            : QStringLiteral("online subtitle search is background/manual only to protect quick subtitle speed");
        Q_UNUSED(onlineSubtitleBaseUrl(_userSettings));
    }
    result.ocrEnabled = configuredBool(
        _userSettings,
        QStringLiteral("ai/subtitles/ocr/enabled"),
        QStringLiteral("SUBTITLE_OCR_ENABLED"),
        false);
    result.ocrProvider = _userSettings
        ? _userSettings->value(QStringLiteral("ai/subtitles/ocr/provider"), QStringLiteral("local_subtitle_ocr")).toString().trimmed()
        : QStringLiteral("local_subtitle_ocr");
    if (result.ocrEnabled) {
        result.ocrError = QStringLiteral("OCR extraction is background/manual only to protect quick subtitle speed");
    }
    result.fusionPath = defaultEnhancedTranslatedVttPath(result.mediaPath, request.targetLanguage, request.outputDirectory);
    result.fusionEnabled = false;
    if (QFileInfo::exists(result.fusionPath)) {
        const QVector<GeneratedSubtitleCue> fusionCues = readSubtitleFile(result.fusionPath);
        result.fusionEnhancedCueCount = fusionCues.size();
    }
    result.lowConfidenceRepairEnabled = configuredBool(
        _userSettings,
        QStringLiteral("ai/subtitles/repair/enabled"),
        QStringLiteral("SUBTITLE_REPAIR_ENABLED"),
        false);
    result.lowConfidenceRepairPath = defaultLowConfidenceRepairVttPath(
        result.mediaPath,
        request.targetLanguage,
        request.outputDirectory);
    if (QFileInfo::exists(result.lowConfidenceRepairPath)) {
        result.lowConfidenceRepairCueCount = readSubtitleFile(result.lowConfidenceRepairPath).size();
    }

    QStringList sourceCandidates = mediaInfo.externalSubtitlePaths;
    sourceCandidates.append(candidateSubtitlePaths(mediaInfo.path));
    sourceCandidates.removeDuplicates();
    result.subtitleSourceCandidateCount = sourceCandidates.size();
    QString subtitleSourceError;
    const QString subtitleSourcePath = resolveSubtitleSourcePath(request, mediaInfo, &subtitleSourceError);
    result.subtitleSourceError = subtitleSourceError;
    result.subtitleSourcePath = subtitleSourcePath;
    result.subtitleSourceKind = subtitleSourceKindForPath(subtitleSourcePath);
    result.usedSubtitleSource = !subtitleSourcePath.isEmpty();
    result.sourcePriorityRank = result.usedSubtitleSource
        ? QStringLiteral("1-local-embedded-external-subtitle")
        : QStringLiteral("3-audio-asr-fallback");
    result.sourceSelectionReason = result.usedSubtitleSource
        ? QStringLiteral("local-embedded-external-subtitle-source-selected-before-asr")
        : QStringLiteral("no-usable-local-embedded-external-subtitle-source");

    result.sourceSrtPath = defaultSourceSrtPath(result.mediaPath, request.outputDirectory);
    result.sourceVttPath = baseOutputPath(result.mediaPath, request.outputDirectory) + QStringLiteral(".source.vtt");
    result.translatedSrtPath = defaultTranslatedSrtPath(result.mediaPath, request.targetLanguage, request.outputDirectory);
    result.translatedVttPath = defaultTranslatedVttPath(result.mediaPath, request.targetLanguage, request.outputDirectory);
    QDir().mkpath(QFileInfo(result.sourceSrtPath).absolutePath());
    if (request.forceRegenerate) {
        QFile::remove(result.sourceSrtPath);
        QFile::remove(result.sourceVttPath);
        QFile::remove(result.translatedSrtPath);
        QFile::remove(result.translatedVttPath);
    }

    if (!request.forceRegenerate && QFileInfo::exists(result.translatedVttPath)) {
        result.cues = readSubtitleFile(result.translatedVttPath);
        QVector<GeneratedSubtitleCue> sourceCues = readSubtitleFile(result.sourceVttPath);
        if (sourceCues.isEmpty()) {
            sourceCues = readSubtitleFile(result.sourceSrtPath);
        }
        result.sourcePriorityRank = result.usedSubtitleSource
            ? QStringLiteral("1-local-embedded-external-subtitle")
            : QStringLiteral("3-audio-asr-fallback");
        result.sourceSelectionReason = result.usedSubtitleSource
            ? QStringLiteral("cached-local-embedded-external-subtitle-source")
            : QStringLiteral("cached-quick-baseline");
        const int repairedFromSource =
            applySafetyNegationPolarityFromSourceCues(&result.cues, sourceCues);
        if (repairedFromSource > 0) {
            _writeSrt(result.translatedSrtPath, result.cues, true);
            _writeVtt(result.translatedVttPath, result.cues, true);
            writeMediaIdentitySidecars(
                { result.translatedSrtPath, result.translatedVttPath },
                result.mediaPath,
                result.mediaDurationSeconds);
        }
        result.sourceCueCount = result.cues.size();
        result.translatedCueCount = result.cues.size();
        result.processedEndSeconds = subtitleCueMaxEndSeconds(result.cues);
        result.sourceCoverageEndSeconds = subtitleCueMaxEndSeconds(result.cues);
        result.translatedCoverageEndSeconds = translatedSubtitleCueMaxEndSeconds(result.cues);
        result.success = !result.cues.isEmpty();
        result.durationMs = timer.elapsed();
        if (!result.success) {
            result.errorMessage = QStringLiteral("Existing subtitle file could not be read");
        }
        return result;
    }

    if (!subtitleSourcePath.isEmpty()) {
        result.cues = readSubtitleFile(subtitleSourcePath);
        if (!result.cues.isEmpty()) {
            result.subtitleSourceLanguage = detectSubtitleTextLanguage(result.cues);
            result.subtitleSourceTimelineUsable = subtitleTimelineUsable(result.cues);
            if (!result.subtitleSourceTimelineUsable) {
                result.subtitleSourceError = QStringLiteral("subtitle source timeline is not usable; falling back to ASR");
                result.usedSubtitleSource = false;
                result.subtitleSourcePath.clear();
                result.subtitleSourceKind.clear();
                result.sourcePriorityRank = QStringLiteral("3-audio-asr-fallback");
                result.sourceRejectReason = QStringLiteral("local-embedded-external-subtitle-timeline-unusable");
                result.cues.clear();
            }
        }
        if (!result.cues.isEmpty()) {
            for (auto& cue : result.cues) {
                if (cue.sourceText.trimmed().isEmpty()) {
                    cue.sourceText = cue.translatedText.trimmed();
                }
                cue.translatedText.clear();
                if (result.subtitleSourceLanguage.startsWith(QStringLiteral("zh"), Qt::CaseInsensitive) &&
                    textContainsHan(cue.sourceText) &&
                    !textContainsJapaneseKana(cue.sourceText)) {
                    cue.translatedText = normalizeChineseSubtitleLiteralForZhHans(cue.sourceText);
                }
                annotateSubtitleSourceQuality(&cue, request.sourceLanguageHint);
            }
            result.sourceCueCount = result.cues.size();
            result.translatedCueCount = result.cues.size();
            int failedTranslationBatchCount = 0;
            if (_translateCues(
                    &result.cues,
                    request,
                    result.providerId,
                    result.translationModel,
                    &result.errorMessage,
                    &failedTranslationBatchCount)) {
                result.failedTranslationBatchCount = failedTranslationBatchCount;
                result.translatedCueCount = 0;
                for (const auto& cue : result.cues) {
                    if (!cueTextForWrite(cue, true).isEmpty()) {
                        ++result.translatedCueCount;
                    }
                }
                result.sourceCoverageEndSeconds = subtitleCueMaxEndSeconds(result.cues);
                result.translatedCoverageEndSeconds = translatedSubtitleCueMaxEndSeconds(result.cues);
                if (result.translatedCueCount == 0) {
                    result.subtitleSourceError = result.errorMessage.isEmpty()
                        ? QStringLiteral("Subtitle source translation produced no usable Chinese text; falling back to ASR")
                        : QStringLiteral("Subtitle source translation failed; falling back to ASR: %1").arg(result.errorMessage);
                    result.errorMessage.clear();
                    result.usedSubtitleSource = false;
                    result.subtitleSourcePath.clear();
                    result.subtitleSourceKind.clear();
                    result.sourcePriorityRank = QStringLiteral("3-audio-asr-fallback");
                    result.sourceRejectReason = QStringLiteral("local-embedded-external-subtitle-translation-empty");
                    result.cues.clear();
                } else {
                    if (!_writeSrt(result.sourceSrtPath, result.cues, false) ||
                        !_writeVtt(result.sourceVttPath, result.cues, false) ||
                        !_writeSrt(result.translatedSrtPath, result.cues, true) ||
                        !_writeVtt(result.translatedVttPath, result.cues, true)) {
                        return fail(QStringLiteral("Failed to write subtitle files"));
                    }
                    writeMediaIdentitySidecars(
                        { result.sourceSrtPath, result.sourceVttPath, result.translatedSrtPath, result.translatedVttPath },
                        result.mediaPath,
                        result.mediaDurationSeconds);
                    result.success = true;
                    result.durationMs = timer.elapsed();
                    result.processedEndSeconds = subtitleCueMaxEndSeconds(result.cues);
                    result.sourceCoverageEndSeconds = subtitleCueMaxEndSeconds(result.cues);
                    result.translatedCoverageEndSeconds = translatedSubtitleCueMaxEndSeconds(result.cues);
                    return result;
                }
            }
            result.subtitleSourceError = result.errorMessage.isEmpty()
                ? QStringLiteral("Subtitle translation failed; falling back to ASR")
                : QStringLiteral("Subtitle translation failed; falling back to ASR: %1").arg(result.errorMessage);
            result.errorMessage.clear();
            result.usedSubtitleSource = false;
            result.subtitleSourcePath.clear();
            result.subtitleSourceKind.clear();
            result.sourcePriorityRank = QStringLiteral("3-audio-asr-fallback");
            result.sourceRejectReason = QStringLiteral("local-embedded-external-subtitle-translation-failed");
            result.cues.clear();
        } else {
            result.subtitleSourceError = result.subtitleSourceError.trimmed().isEmpty()
                ? QStringLiteral("subtitle source could not be read; falling back to ASR")
                : result.subtitleSourceError;
            result.usedSubtitleSource = false;
            result.subtitleSourcePath.clear();
            result.subtitleSourceKind.clear();
            result.sourcePriorityRank = QStringLiteral("3-audio-asr-fallback");
            result.sourceRejectReason = QStringLiteral("local-embedded-external-subtitle-unreadable");
        }
    }

    QString firstRateLimitError;
    result.sourceSelectionReason = result.sourceSelectionReason.trimmed().isEmpty()
        ? QStringLiteral("audio-asr-fallback-after-no-usable-local-embedded-external-source")
        : result.sourceSelectionReason;
    result.usedAudioAsr = !request.mockWithoutApi;
    if (request.mockWithoutApi) {
        GeneratedSubtitleCue cue;
        cue.index = 1;
        const double mediaDuration = std::max(1.0, result.mediaDurationSeconds);
        const bool hasMockCueStart = request.mockCueStartSeconds >= 0.0;
        cue.startSeconds = hasMockCueStart
            ? std::clamp(request.mockCueStartSeconds, 0.0, std::max(0.0, mediaDuration - 1.0))
            : 0.0;
        cue.endSeconds = hasMockCueStart ? std::min(cue.startSeconds + 5.0, mediaDuration) : mediaDuration;
        if (cue.endSeconds <= cue.startSeconds) {
            cue.endSeconds = std::min(cue.startSeconds + 1.0, mediaDuration);
        }
        cue.sourceText = QStringLiteral("CGPlay subtitle generation smoke test");
        cue.translatedText = QStringLiteral("CGPlay 字幕生成测试");
        annotateSubtitleSourceQuality(&cue, request.sourceLanguageHint);
        result.cues = { cue };
        result.processedEndSeconds = cue.endSeconds;
    } else {
        if (!_providerManager) {
            return fail(QStringLiteral("AI provider manager is not available"));
        }
        if (MediaProbe::locateFfmpeg().isEmpty()) {
            return fail(QStringLiteral("ffmpeg not found"));
        }

        const double totalDuration = std::max(1.0, result.mediaDurationSeconds);
        const double processStart = std::clamp(request.startSeconds, 0.0, std::max(0.0, totalDuration - 1.0));
        const double requestedDuration = request.maxDurationSeconds > 0.0
            ? std::min(request.maxDurationSeconds, totalDuration - processStart)
            : totalDuration;
        const double processDuration = std::max(1.0, requestedDuration);
        const double processEnd = std::min(totalDuration, processStart + processDuration);
        QVector<GeneratedSubtitleCue> cues;
        QVector<SubtitleChunkTask> chunks;
        double processedThroughSeconds = processStart;
        bool stoppedEarlyAfterRateLimit = false;
        int chunkOrder = 0;
        const auto normalizeGeneratedCues = [](QVector<GeneratedSubtitleCue>* target) {
            if (!target) {
                return;
            }
            std::sort(target->begin(), target->end(), [](const GeneratedSubtitleCue& a, const GeneratedSubtitleCue& b) {
                if (a.startSeconds == b.startSeconds) {
                    return a.endSeconds < b.endSeconds;
                }
                return a.startSeconds < b.startSeconds;
            });
            for (int i = 0; i < target->size(); ++i) {
                (*target)[i].index = i + 1;
                if ((*target)[i].endSeconds <= (*target)[i].startSeconds) {
                    (*target)[i].endSeconds = (*target)[i].startSeconds + 2.5;
                }
            }
            dedupeNearDuplicateSubtitleCues(target);
            for (int i = 0; i < target->size(); ++i) {
                (*target)[i].index = i + 1;
            }
        };
        const auto commitPartialWindow = [&](QVector<GeneratedSubtitleCue>* partialCues, double processedThrough) {
            if (!partialCues || partialCues->isEmpty()) {
                return;
            }
            normalizeGeneratedCues(partialCues);
            if (partialCues->isEmpty()) {
                return;
            }
            if (!_writeSrt(result.sourceSrtPath, *partialCues, false) ||
                !_writeVtt(result.sourceVttPath, *partialCues, false)) {
                qWarning() << "[SubtitleGeneration] Failed to write partial source subtitles";
                return;
            }
            writeMediaIdentitySidecars(
                { result.sourceSrtPath, result.sourceVttPath },
                result.mediaPath,
                result.mediaDurationSeconds);
            if (request.translate) {
                QString partialTranslateError;
                int partialFailedBatchCount = 0;
                if (!_translateCues(
                        partialCues,
                        request,
                        result.providerId,
                        result.translationModel,
                        &partialTranslateError,
                        &partialFailedBatchCount)) {
                    qWarning() << "[SubtitleGeneration] Failed to translate partial subtitles"
                               << partialTranslateError;
                    return;
                }
                if (translatedSubtitleCueMaxEndSeconds(*partialCues) <= 0.0) {
                    qWarning() << "[SubtitleGeneration] Partial subtitle translation produced no displayable Chinese"
                               << "failedBatches=" << partialFailedBatchCount
                               << partialTranslateError;
                    return;
                }
            }
            if (!_writeSrt(result.translatedSrtPath, *partialCues, true) ||
                !_writeVtt(result.translatedVttPath, *partialCues, true)) {
                qWarning() << "[SubtitleGeneration] Failed to write partial translated subtitles";
                return;
            }
            writeMediaIdentitySidecars(
                { result.translatedSrtPath, result.translatedVttPath },
                result.mediaPath,
                result.mediaDurationSeconds);
            qInfo() << "[SubtitleGeneration] Partial subtitle window committed"
                    << "cues=" << partialCues->size()
                    << "processedThrough=" << processedThrough
                    << "translatedCoverageEnd=" << translatedSubtitleCueMaxEndSeconds(*partialCues)
                    << "sourceCoverageEnd=" << subtitleCueMaxEndSeconds(*partialCues);
        };
        for (double start = processStart; start + kMinimumAudioChunkSeconds < processEnd; start += kChunkSeconds) {
            const double remaining = processEnd - start;
            if (remaining < kMinimumAudioChunkSeconds) {
                break;
            }
            const double duration = std::min<double>(kChunkSeconds, remaining);
            SubtitleChunkTask chunk;
            chunk.order = chunkOrder++;
            chunk.startSeconds = start;
            chunk.durationSeconds = duration;
            chunk.audioPath = tempAudioChunkPath(result.mediaPath, start);
            chunks.push_back(chunk);
        }
        bool prioritizeForwardChunks = false;
        if (!chunks.isEmpty()) {
            if (request.priorityStartSeconds > processStart + 0.01 &&
                request.priorityStartSeconds < processEnd - 0.01) {
                prioritizeForwardChunks = true;
                const double priorityStartSeconds = request.priorityStartSeconds;
                std::stable_sort(chunks.begin(), chunks.end(), [priorityStartSeconds](const SubtitleChunkTask& a, const SubtitleChunkTask& b) {
                    const bool aPriority = a.startSeconds + a.durationSeconds > priorityStartSeconds;
                    const bool bPriority = b.startSeconds + b.durationSeconds > priorityStartSeconds;
                    if (aPriority != bPriority) {
                        return aPriority;
                    }
                    return a.startSeconds < b.startSeconds;
                });
                for (int i = 0; i < chunks.size(); ++i) {
                    chunks[i].order = i;
                }
                qInfo() << "[SubtitleGeneration] Prioritizing forward subtitle chunks"
                        << "priorityStart=" << priorityStartSeconds
                        << "windowStart=" << processStart
                        << "windowEnd=" << processEnd;
            }
            result.audioDirectory = QFileInfo(chunks.front().audioPath).absolutePath();
        }

        int asrConcurrency = configuredInt(
            _userSettings,
            QStringLiteral("ai/subtitles/asr/concurrency"),
            QStringLiteral("SUBTITLE_ASR_CONCURRENCY"),
            kDefaultAsrConcurrency,
            1,
            16);
        if (QProcessEnvironment::systemEnvironment().value(QStringLiteral("SUBTITLE_ASR_CONCURRENCY")).trimmed().isEmpty() &&
            asrConcurrency == 4) {
            asrConcurrency = kDefaultAsrConcurrency;
        }
        qInfo() << "[SubtitleGeneration] ASR window"
                << "start=" << processStart
                << "duration=" << processDuration
                << "chunks=" << chunks.size()
                << "concurrency=" << asrConcurrency;

        const int chunkCount = static_cast<int>(chunks.size());
        for (int batchStart = 0; batchStart < chunkCount; batchStart += asrConcurrency) {
            if (isCanceled()) {
                return fail(QStringLiteral("Subtitle generation canceled"));
            }
            const int batchEnd = std::min(batchStart + asrConcurrency, chunkCount);
            std::vector<std::future<SubtitleChunkTask>> futures;
            futures.reserve(static_cast<size_t>(batchEnd - batchStart));
            for (int i = batchStart; i < batchEnd; ++i) {
                SubtitleChunkTask chunk = chunks.at(i);
                futures.push_back(std::async(std::launch::async, [this, chunk, &request, &result, isCanceled]() mutable {
                    if (isCanceled()) {
                        chunk.errorMessage = QStringLiteral("Subtitle generation canceled");
                        return chunk;
                    }
                    QString error;
                    if (!_extractAudioChunk(
                            result.mediaPath,
                            chunk.startSeconds,
                            chunk.durationSeconds,
                            chunk.audioPath,
                            &error)) {
                        if (transcriptionErrorLooksLikeEmptyAudioChunk(error)) {
                            chunk.extractionRetryCount = 1;
                            QThread::msleep(120 + (chunk.order % 3) * 40);
                            QString retryError;
                            if (_extractAudioChunk(
                                    result.mediaPath,
                                    chunk.startSeconds,
                                    chunk.durationSeconds,
                                    chunk.audioPath,
                                    &retryError)) {
                                error.clear();
                            } else {
                                error = retryError.trimmed().isEmpty() ? error : retryError;
                            }
                        }
                        if (!error.trimmed().isEmpty()) {
                            chunk.errorMessage = transcriptionErrorLooksLikeEmptyAudioChunk(error)
                                ? QStringLiteral("audio-chunk-empty-skipped: %1").arg(error)
                                : error;
                            if (transcriptionErrorLooksLikeEmptyAudioChunk(error)) {
                                chunk.skippedReason = QStringLiteral("audio-chunk-empty-skipped");
                            }
                        }
                        QFile::remove(chunk.audioPath);
                        if (!chunk.errorMessage.isEmpty()) {
                            return chunk;
                        }
                    }

                    for (int attempt = 0; attempt < 2; ++attempt) {
                        error.clear();
                        chunk.cues = _transcribeChunkWithMimo(
                            chunk.audioPath,
                            chunk.startSeconds,
                            chunk.durationSeconds,
                            request,
                            result.transcriptionModel,
                            &error);
                        if (error.isEmpty() || !transcriptionErrorLooksLikeRateLimit(error)) {
                            break;
                        }
                        QThread::msleep(1200 + attempt * 2200 + (chunk.order % 3) * 350);
                    }
                    QFile::remove(chunk.audioPath);
                    if (!error.isEmpty()) {
                        chunk.errorMessage = error;
                    }
                    return chunk;
                }));
            }

            bool stopAfterPartialRateLimit = false;
            QString batchRateLimitError;
            QString fatalChunkError;
            std::vector<bool> futureConsumed(futures.size(), false);
            int remainingFutures = static_cast<int>(futures.size());
            bool partialDirty = false;
            int partialDirtyCueCount = 0;
            QElapsedTimer partialDirtyTimer;
            auto consumeChunk = [&](SubtitleChunkTask chunk) {
                if (!chunk.errorMessage.isEmpty()) {
                    if (transcriptionErrorLooksLikeNoSpeech(chunk.errorMessage) ||
                        transcriptionErrorLooksLikeEmptyAudioChunk(chunk.errorMessage)) {
                        const QString skippedReason = transcriptionErrorLooksLikeEmptyAudioChunk(chunk.errorMessage)
                            ? QStringLiteral("audio-chunk-empty-skipped")
                            : QStringLiteral("no-speech-skipped");
                        qInfo() << "[SubtitleGeneration] Skipping subtitle chunk"
                                << skippedReason
                                << chunk.startSeconds
                                << chunk.errorMessage;
                        result.skippedAudioChunkCount += 1;
                        result.skippedAudioChunks.append(QJsonObject{
                            { QStringLiteral("reason"), skippedReason },
                            { QStringLiteral("startSeconds"), chunk.startSeconds },
                            { QStringLiteral("durationSeconds"), chunk.durationSeconds },
                            { QStringLiteral("retryCount"), chunk.extractionRetryCount },
                            { QStringLiteral("message"), chunk.errorMessage }
                        });
                        processedThroughSeconds = std::max(
                            processedThroughSeconds,
                            chunk.startSeconds + chunk.durationSeconds);
                        return;
                    }
                    if (transcriptionErrorLooksLikeRateLimit(chunk.errorMessage)) {
                        if (firstRateLimitError.isEmpty()) {
                            firstRateLimitError = chunk.errorMessage;
                        }
                        batchRateLimitError = QStringLiteral("ASR rate limit at %1s: %2")
                            .arg(chunk.startSeconds, 0, 'f', 1)
                            .arg(chunk.errorMessage);
                        stopAfterPartialRateLimit = true;
                        return;
                    }
                    fatalChunkError = QStringLiteral("Subtitle chunk %1s failed: %2")
                        .arg(chunk.startSeconds, 0, 'f', 1)
                        .arg(chunk.errorMessage);
                    return;
                }
                processedThroughSeconds = std::max(
                    processedThroughSeconds,
                    chunk.startSeconds + chunk.durationSeconds);
                if (!partialDirty) {
                    partialDirtyTimer.restart();
                }
                cues += chunk.cues;
                partialDirty = true;
                partialDirtyCueCount += chunk.cues.size();
            };
            auto commitIfDirty = [&]() {
                if (!partialDirty) {
                    return;
                }
                for (auto& cue : cues) {
                    annotateSubtitleSourceQuality(&cue, request.sourceLanguageHint);
                }
                commitPartialWindow(&cues, processedThroughSeconds);
                partialDirty = false;
                partialDirtyCueCount = 0;
            };
            if (!prioritizeForwardChunks) {
                for (auto& future : futures) {
                    consumeChunk(future.get());
                    if (!fatalChunkError.isEmpty()) {
                        break;
                    }
                }
                commitIfDirty();
            } else {
                while (remainingFutures > 0 && fatalChunkError.isEmpty()) {
                    bool consumedAny = false;
                    for (int i = 0; i < static_cast<int>(futures.size()); ++i) {
                        if (futureConsumed.at(i)) {
                            continue;
                        }
                        if (futures.at(i).wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                            continue;
                        }
                        futureConsumed[i] = true;
                        --remainingFutures;
                        consumedAny = true;
                        consumeChunk(futures[i].get());
                        if (!fatalChunkError.isEmpty()) {
                            break;
                        }
                    }
                    const bool partialBatchReady =
                        partialDirty &&
                        (remainingFutures <= 0 ||
                         partialDirtyCueCount >= kMaxTranslateBatch ||
                         partialDirtyTimer.elapsed() >= kForwardPartialCoalesceMs);
                    if (partialBatchReady) {
                        commitIfDirty();
                    }
                    if (remainingFutures <= 0 || !fatalChunkError.isEmpty()) {
                        break;
                    }
                    if (!consumedAny) {
                        QThread::msleep(50);
                    }
                }
            }
            if (!fatalChunkError.isEmpty()) {
                return fail(fatalChunkError);
            }
            if (stopAfterPartialRateLimit) {
                if (cues.isEmpty()) {
                    return fail(batchRateLimitError);
                }
                stoppedEarlyAfterRateLimit = true;
                qWarning() << "[SubtitleGeneration] Keeping partial subtitle window after ASR rate limit:"
                           << batchRateLimitError
                           << "partialCues=" << cues.size();
                break;
            }
        }
        result.processedEndSeconds = stoppedEarlyAfterRateLimit
            ? processedThroughSeconds
            : processEnd;

        normalizeGeneratedCues(&cues);
        for (auto& cue : cues) {
            annotateSubtitleSourceQuality(&cue, request.sourceLanguageHint);
        }
        result.cues = cues;

        if (request.translate) {
            QString error;
            int failedTranslationBatchCount = 0;
            if (!_translateCues(
                    &result.cues,
                    request,
                    result.providerId,
                    result.translationModel,
                    &error,
                    &failedTranslationBatchCount)) {
                return fail(error);
            }
            result.failedTranslationBatchCount = failedTranslationBatchCount;
        }
    }

    result.sourceCueCount = result.cues.size();
    int translatedCount = 0;
    for (const auto& cue : result.cues) {
        if (!cueTextForWrite(cue, true).isEmpty()) {
            ++translatedCount;
        }
    }
    result.translatedCueCount = translatedCount;
    result.sourceCoverageEndSeconds = subtitleCueMaxEndSeconds(result.cues);
    result.translatedCoverageEndSeconds = translatedSubtitleCueMaxEndSeconds(result.cues);

    if (result.cues.isEmpty()) {
        if (!firstRateLimitError.isEmpty()) {
            return fail(QStringLiteral("ASR rate limit: %1").arg(firstRateLimitError));
        }
        return fail(QStringLiteral("No subtitle cues were generated"));
    }
    if (request.translate && result.translatedCueCount == 0) {
        return fail(result.failedTranslationBatchCount > 0
            ? QStringLiteral("Subtitle translation failed for all batches")
            : QStringLiteral("Subtitle translation produced no usable Chinese text"));
    }
    if (!_writeSrt(result.sourceSrtPath, result.cues, false) ||
        !_writeVtt(result.sourceVttPath, result.cues, false) ||
        !_writeSrt(result.translatedSrtPath, result.cues, true) ||
        !_writeVtt(result.translatedVttPath, result.cues, true)) {
        return fail(QStringLiteral("Failed to write subtitle files"));
    }
    writeMediaIdentitySidecars(
        { result.sourceSrtPath, result.sourceVttPath, result.translatedSrtPath, result.translatedVttPath },
        result.mediaPath,
        result.mediaDurationSeconds);

    result.success = true;
    result.durationMs = timer.elapsed();
    return result;
}

SubtitleRefinementResult SubtitleGenerationService::refine(const SubtitleRefinementRequest& request) const
{
    QElapsedTimer timer;
    timer.start();

    SubtitleRefinementResult result;
    result.mediaPath = QFileInfo(request.mediaPath).absoluteFilePath();
    result.providerId = _providerId(request);
    result.model = _refinementModel(request);
    result.refinedSrtPath = defaultRefinedTranslatedSrtPath(
        request.mediaPath,
        request.targetLanguage,
        request.outputDirectory);
    result.refinedVttPath = defaultRefinedTranslatedVttPath(
        request.mediaPath,
        request.targetLanguage,
        request.outputDirectory);
    result.inputCueCount = request.quickCues.size();

    auto fail = [&](const QString& message) {
        result.success = false;
        result.errorMessage = message;
        result.durationMs = timer.elapsed();
        return result;
    };

    if (request.quickCues.isEmpty()) {
        return fail(QStringLiteral("No quick subtitles available for refinement"));
    }
    if (request.cancelRequested && request.cancelRequested->load()) {
        return fail(QStringLiteral("Subtitle refinement canceled"));
    }

    QVector<GeneratedSubtitleCue> refined = request.quickCues;
    for (int i = 0; i < refined.size(); ++i) {
        refined[i].index = i + 1;
        refined[i].sourceText = repairSubtitleMojibake(
            i < request.sourceCues.size() && !request.sourceCues.at(i).sourceText.trimmed().isEmpty()
                ? request.sourceCues.at(i).sourceText
                : refined[i].sourceText);
        refined[i].translatedText = normalizeTranslatedSubtitleLine(
            normalizeAnimeTerminology(refined[i].translatedText));
    }

    QString error;
    if (request.mockWithoutApi) {
        for (auto& cue : refined) {
            if (cue.translatedText.trimmed().isEmpty()) {
                cue.translatedText = cue.sourceText.trimmed();
            }
        }
    } else if (!_refineCuesWithContext(&refined, request, result.providerId, result.model, &error)) {
        return fail(error.isEmpty() ? QStringLiteral("Subtitle refinement failed") : error);
    }

    result.validation = _validateRefinedCues(request.quickCues, refined);
    if (!result.validation.value(QStringLiteral("passed")).toBool(false)) {
        return fail(result.validation.value(QStringLiteral("reason")).toString(QStringLiteral("Refined subtitles did not pass validation")));
    }

    result.refinedCueCount = refined.size();
    for (int i = 0; i < refined.size() && i < request.quickCues.size(); ++i) {
        if (normalizeTranslatedSubtitleLine(refined.at(i).translatedText) !=
            normalizeTranslatedSubtitleLine(request.quickCues.at(i).translatedText)) {
            result.changedCueCount += 1;
        }
    }

    if (!_writeSrt(result.refinedSrtPath, refined, true) ||
        !_writeVtt(result.refinedVttPath, refined, true)) {
        return fail(QStringLiteral("Failed to write refined subtitle files"));
    }
    writeMediaIdentitySidecars(
        { result.refinedSrtPath, result.refinedVttPath },
        result.mediaPath,
        0.0);

    result.cues = refined;
    result.success = true;
    result.durationMs = timer.elapsed();
    return result;
}

QString SubtitleGenerationService::_providerId(const SubtitleGenerationRequest& request) const
{
    if (!request.providerId.trimmed().isEmpty()) {
        return request.providerId.trimmed();
    }
    if (_userSettings) {
        const QString subtitleConfigured =
            _userSettings->value(QStringLiteral("ai/subtitles/asr/providerId")).toString().trimmed();
        if (!subtitleConfigured.isEmpty() && subtitleConfigured.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
            return subtitleConfigured;
        }
        const QString configured = _userSettings->value(QStringLiteral("ai/workspace/providerId")).toString().trimmed();
        if (!configured.isEmpty()) {
            return configured;
        }
    }
    return _providerManager ? _providerManager->defaultProviderId() : QStringLiteral("openai");
}

QString SubtitleGenerationService::_providerId(const SubtitleRefinementRequest& request) const
{
    if (!request.providerId.trimmed().isEmpty()) {
        return request.providerId.trimmed();
    }
    if (_userSettings) {
        const QString workspace = _userSettings->value(QStringLiteral("ai/workspace/providerId")).toString().trimmed();
        if (!workspace.isEmpty() && workspace.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
            return workspace;
        }
    }
    return QString();
}

QString SubtitleGenerationService::_transcriptionModel(const SubtitleGenerationRequest& request) const
{
    const QString requestModel = request.transcriptionModel.trimmed();
    if (!requestModel.isEmpty() && requestModel.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
        return requestModel;
    }
    if (_userSettings) {
        const QString configured =
            _userSettings->value(QStringLiteral("ai/subtitles/transcriptionModel")).toString().trimmed();
        if (!configured.isEmpty() && configured.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
            return configured;
        }
    }
    if (_userSettings) {
        const QString protocol =
            _userSettings->value(QStringLiteral("ai/subtitles/asr/protocol")).toString().trimmed();
        if (protocol.compare(QStringLiteral("qwen"), Qt::CaseInsensitive) == 0 ||
            protocol.compare(QStringLiteral("qwen3_asr"), Qt::CaseInsensitive) == 0 ||
            protocol.compare(QStringLiteral("qwen_dashscope_asr"), Qt::CaseInsensitive) == 0) {
            return QString::fromLatin1(kQwenDefaultAsrModel);
        }
        if (protocol.compare(QStringLiteral("responses_audio"), Qt::CaseInsensitive) == 0) {
            return QStringLiteral("gpt-5.4");
        }
    }
    return QStringLiteral("mimo-v2.5-asr");
}

QString SubtitleGenerationService::_translationModel(const SubtitleGenerationRequest& request) const
{
    const QString requestModel = request.translationModel.trimmed();
    if (!requestModel.isEmpty() && requestModel.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
        return requestModel;
    }
    if (_userSettings) {
        const QString translationProvider =
            _userSettings->value(QStringLiteral("ai/subtitles/translation/provider")).toString().trimmed();
        const QString subtitleModel =
            _userSettings->value(QStringLiteral("ai/subtitles/translationModel")).toString().trimmed();
        if (translationProvider.compare(QStringLiteral("qwen"), Qt::CaseInsensitive) == 0) {
            if (!modelLooksUnsuitableForSubtitleTranslation(subtitleModel) &&
                subtitleModel.startsWith(QStringLiteral("qwen"), Qt::CaseInsensitive)) {
                return subtitleModel;
            }
            return QString::fromLatin1(kQwenDefaultTranslationModel);
        }
        if (!modelLooksUnsuitableForSubtitleTranslation(subtitleModel) &&
            subtitleModel.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
            return subtitleModel;
        }
        const QString recommended = _userSettings->value(QStringLiteral("ai/connection/recommendedModel")).toString().trimmed();
        if (!modelLooksUnsuitableForSubtitleTranslation(recommended) &&
            recommended.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
            return recommended;
        }
        const QStringList available =
            _userSettings->value(QStringLiteral("ai/connection/availableModels")).toStringList();
        for (const QString& candidate : available) {
            if (!modelLooksUnsuitableForSubtitleTranslation(candidate) &&
                candidate.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
                return candidate.trimmed();
            }
        }
    }
    return QStringLiteral("gpt-5.5");
}

QString SubtitleGenerationService::_refinementModel(const SubtitleRefinementRequest& request) const
{
    const QString requestModel = request.model.trimmed();
    if (!requestModel.isEmpty() && requestModel.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
        return requestModel;
    }
    if (_userSettings) {
        const QString workspaceModel =
            _userSettings->value(QStringLiteral("ai/workspace/model")).toString().trimmed();
        if (!modelLooksUnsuitableForSubtitleTranslation(workspaceModel) &&
            workspaceModel.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
            return workspaceModel;
        }
    }
    return QString();
}

bool SubtitleGenerationService::_useApiTranscription(
    const SubtitleGenerationRequest& request,
    const QString& providerId,
    const QString& model) const
{
    if (providerLooksLikeTextOnlyProvider(providerId)) {
        return false;
    }
    if (request.useApiTranscription) {
        return true;
    }
    if (_userSettings) {
        return _userSettings->value(QStringLiteral("ai/subtitles/useApiTranscription"), false).toBool();
    }
    return false;
}

QVector<GeneratedSubtitleCue> SubtitleGenerationService::_transcribeChunkWithMimo(
    const QString& audioPath,
    double offsetSeconds,
    double fallbackDurationSeconds,
    const SubtitleGenerationRequest& request,
    const QString& model,
    QString* errorMessage) const
{
    QString apiKey;
    QString baseUrl;
    const QString protocolValue = _userSettings
        ? _userSettings->value(QStringLiteral("ai/subtitles/asr/protocol")).toString().trimmed()
        : QString();
    const bool qwenAsrProtocol =
        protocolValue.compare(QStringLiteral("qwen"), Qt::CaseInsensitive) == 0 ||
        protocolValue.compare(QStringLiteral("qwen3_asr"), Qt::CaseInsensitive) == 0 ||
        protocolValue.compare(QStringLiteral("qwen_dashscope_asr"), Qt::CaseInsensitive) == 0;
    if (_userSettings) {
        baseUrl = _userSettings->value(QStringLiteral("ai/subtitles/asr/baseUrl")).toString().trimmed();
        if (baseUrl.isEmpty()) {
            baseUrl = _userSettings->value(QStringLiteral("ai/subtitles/mimo/baseUrl")).toString().trimmed();
        }
        if (baseUrl.isEmpty()) {
            baseUrl = _userSettings->value(QStringLiteral("ai/subtitles/qwen/asrBaseUrl")).toString().trimmed();
        }
    }
    if (apiKey.isEmpty()) {
        if (auto* store = ServiceLocator::getService<IAICredentialStore>()) {
            QString error;
            const QStringList ids = qwenAsrProtocol
                ? QStringList{
                    QString::fromLatin1(kQwenCredentialId),
                    QString::fromLatin1(kSubtitleAsrCredentialId),
                    QString::fromLatin1(kGenericCredentialId)
                }
                : QStringList{
                    QString::fromLatin1(kSubtitleAsrCredentialId),
                    QString::fromLatin1(kMimoCredentialId),
                    QString::fromLatin1(kGenericCredentialId),
                    QString::fromLatin1(kOpenAICredentialId)
                };
            for (const QString& id : ids) {
                const QByteArray secret = store->loadSecret(id, &error).trimmed();
                if (!secret.isEmpty()) {
                    apiKey = QString::fromUtf8(secret).trimmed();
                    break;
                }
            }
        }
    }
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (apiKey.isEmpty()) {
        apiKey = env.value(QStringLiteral("SUBTITLE_ASR_API_KEY")).trimmed();
    }
    if (apiKey.isEmpty()) {
        apiKey = env.value(QStringLiteral("ASR_API_KEY")).trimmed();
    }
    if (apiKey.isEmpty() && !qwenAsrProtocol) {
        apiKey = env.value(QStringLiteral("GROQ_API_KEY")).trimmed();
    }
    if (apiKey.isEmpty() && !qwenAsrProtocol) {
        apiKey = env.value(QStringLiteral("OPENAI_API_KEY")).trimmed();
    }
    if (apiKey.isEmpty() && !qwenAsrProtocol) {
        apiKey = env.value(QStringLiteral("GEMINI_API_KEY")).trimmed();
    }
    if (apiKey.isEmpty() && !qwenAsrProtocol) {
        apiKey = env.value(QStringLiteral("MIMO_API_KEY")).trimmed();
    }
    if (apiKey.isEmpty() && !qwenAsrProtocol) {
        apiKey = env.value(QStringLiteral("XIAOMI_MIMO_API_KEY")).trimmed();
    }
    if (apiKey.isEmpty()) {
        apiKey = env.value(QStringLiteral("QWEN_API_KEY")).trimmed();
    }
    if (apiKey.isEmpty()) {
        apiKey = env.value(QStringLiteral("DASHSCOPE_API_KEY")).trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = env.value(QStringLiteral("SUBTITLE_ASR_BASE_URL")).trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = env.value(QStringLiteral("ASR_BASE_URL")).trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = env.value(QStringLiteral("GROQ_BASE_URL")).trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = env.value(QStringLiteral("GEMINI_BASE_URL")).trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = env.value(QStringLiteral("MIMO_BASE_URL")).trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = env.value(QStringLiteral("XIAOMI_MIMO_BASE_URL")).trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = env.value(QStringLiteral("QWEN_ASR_BASE_URL")).trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = env.value(QStringLiteral("DASHSCOPE_ASR_BASE_URL")).trimmed();
    }
    if (apiKey.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("ASR API key is missing");
        }
        return {};
    }

    SubtitleAsrApiConfig config;
    config.baseUrl = baseUrl;
    config.apiKey = apiKey;
    config.model = qwenAsrProtocol &&
            (model.trimmed().isEmpty() ||
             model.compare(QStringLiteral("qwen3-asr"), Qt::CaseInsensitive) == 0 ||
             model.contains(QStringLiteral("mimo"), Qt::CaseInsensitive))
        ? QString::fromLatin1(kQwenDefaultAsrModel)
        : (model.trimmed().isEmpty() ? QStringLiteral("mimo-v2.5-asr") : model.trimmed());
    config.sourceLanguageHint = request.sourceLanguageHint;
    config.timeoutMs = kMimoRequestTimeoutMs;

    if (protocolValue.compare(QStringLiteral("gemini"), Qt::CaseInsensitive) == 0) {
        config.protocol = SubtitleAsrProtocol::GeminiGenerateContent;
    } else if (protocolValue.compare(QStringLiteral("qwen"), Qt::CaseInsensitive) == 0 ||
               protocolValue.compare(QStringLiteral("qwen3_asr"), Qt::CaseInsensitive) == 0 ||
               protocolValue.compare(QStringLiteral("qwen_dashscope_asr"), Qt::CaseInsensitive) == 0) {
        config.protocol = SubtitleAsrProtocol::QwenDashScopeAsr;
    } else if (protocolValue.compare(QStringLiteral("openai"), Qt::CaseInsensitive) == 0) {
        config.protocol = SubtitleAsrProtocol::OpenAITranscriptions;
    } else if (protocolValue.compare(QStringLiteral("responses_audio"), Qt::CaseInsensitive) == 0) {
        config.protocol = SubtitleAsrProtocol::OpenAIResponsesAudio;
    } else {
        config.protocol = SubtitleAsrProtocol::MimoChat;
    }

    const SubtitleAsrApiResult apiResult = transcribeSubtitleAudioWithProtocol(config, audioPath);
    if (!apiResult.success) {
        if (errorMessage) {
            *errorMessage = apiResult.errorMessage;
        }
        return {};
    }

    const QJsonObject object = apiResult.rawJson;

    QVector<GeneratedSubtitleCue> cues;
    const QJsonArray segments = extractSegments(object);
    for (const QJsonValue& value : segments) {
        const QJsonObject segment = value.toObject();
        const QString text = segmentText(segment);
        if (text.isEmpty()) {
            continue;
        }
        GeneratedSubtitleCue cue;
        cue.startSeconds = offsetSeconds + segmentNumber(segment, QStringLiteral("start"), 0.0);
        cue.endSeconds = offsetSeconds + segmentNumber(segment, QStringLiteral("end"), fallbackDurationSeconds);
        cue.sourceText = text;
        cues.push_back(cue);
    }
    if (!cues.isEmpty()) {
        return cues;
    }

    const QString text = apiResult.text.trimmed().isEmpty()
        ? firstChatCompletionText(object)
        : apiResult.text.trimmed();
    if (text.isEmpty()) {
        return {};
    }

    GeneratedSubtitleCue cue;
    cue.startSeconds = offsetSeconds;
    cue.endSeconds = offsetSeconds + std::max(1.0, fallbackDurationSeconds);
    cue.sourceText = normalizeJapaneseAsrSourceText(text, request.sourceLanguageHint);
    return { cue };
}

QString SubtitleGenerationService::_targetLanguageName(const QString& code) const
{
    const QString normalized = cleanLanguageCode(code).toLower();
    if (normalized == QStringLiteral("en")) {
        return QStringLiteral("English");
    }
    if (normalized == QStringLiteral("ja")) {
        return QStringLiteral("Japanese");
    }
    if (normalized == QStringLiteral("ko")) {
        return QStringLiteral("Korean");
    }
    if (normalized == QStringLiteral("zh-hant")) {
        return QStringLiteral("Traditional Chinese");
    }
    return QStringLiteral("Simplified Chinese");
}

bool SubtitleGenerationService::_extractAudioChunk(
    const QString& mediaPath,
    double startSeconds,
    double durationSeconds,
    const QString& outputPath,
    QString* errorMessage) const
{
    const QString ffmpeg = MediaProbe::locateFfmpeg();
    if (ffmpeg.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("ffmpeg not found");
        }
        return false;
    }

    QFile::remove(outputPath);
    if (testShouldForceEmptyAudioChunk(startSeconds)) {
        QFile forced(outputPath);
        forced.open(QIODevice::WriteOnly);
        forced.close();
        if (errorMessage) {
            *errorMessage = QStringLiteral("ffmpeg audio extraction produced invalid chunk size: 0 bytes for %1s")
                .arg(durationSeconds, 0, 'f', 3);
        }
        return false;
    }
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(ffmpeg, {
        QStringLiteral("-y"),
        QStringLiteral("-ss"), QString::number(startSeconds, 'f', 3),
        QStringLiteral("-t"), QString::number(durationSeconds, 'f', 3),
        QStringLiteral("-i"), mediaPath,
        QStringLiteral("-vn"),
        QStringLiteral("-ac"), QStringLiteral("1"),
        QStringLiteral("-ar"), QStringLiteral("16000"),
        QStringLiteral("-f"), QStringLiteral("wav"),
        outputPath
    });
    JobContext job(kFfmpegTimeoutMs);
    const ProcessOutcome processOutcome = job.waitForProcess(process, 25, kFfmpegTimeoutMs);
    if (processOutcome.state == JobState::TimedOut) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("ffmpeg audio extraction timed out");
        }
        return false;
    }
    if (!processOutcome.succeeded() || !QFileInfo::exists(outputPath)) {
        if (errorMessage) {
            const QString processError = QString::fromUtf8(processOutcome.standardOutput).trimmed().left(600);
            *errorMessage = QStringLiteral("ffmpeg audio extraction failed: %1")
                .arg(processError.isEmpty() ? process.errorString() : processError);
        }
        return false;
    }
    const qint64 outputBytes = QFileInfo(outputPath).size();
    if (outputBytes <= 0 || outputBytes > kMaximumAsrAudioBytes) {
        QFile::remove(outputPath);
        if (errorMessage) {
            *errorMessage = QStringLiteral("ffmpeg audio extraction produced invalid chunk size: %1 bytes for %2s")
                .arg(outputBytes)
                .arg(durationSeconds, 0, 'f', 3);
        }
        return false;
    }
    return true;
}

QVector<GeneratedSubtitleCue> SubtitleGenerationService::_transcribeChunk(
    const QString& audioPath,
    double offsetSeconds,
    double fallbackDurationSeconds,
    const SubtitleGenerationRequest& request,
    const QString& providerId,
    const QString& model,
    bool preferLocalTranscription,
    bool* disableApiTranscription,
    QString* errorMessage) const
{
    if (preferLocalTranscription) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Local transcription path disabled");
        }
        return {};
    }

    AIAudioTranscriptionRequest audioRequest;
    audioRequest.providerId = providerId;
    audioRequest.model = model;
    audioRequest.audioFilePath = audioPath;
    audioRequest.mimeType = QStringLiteral("audio/wav");
    if (request.sourceLanguageHint.trimmed().toLower() != QStringLiteral("auto")) {
        audioRequest.languageHint = request.sourceLanguageHint.trimmed();
    }
    audioRequest.prompt = request.sourceLanguageHint.trimmed().compare(QStringLiteral("ja"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("ASR only. Transcribe the spoken audio verbatim as original Japanese dialogue in kana/kanji. Do not translate into Chinese or English, do not romanize, do not summarize, and do not infer plot events beyond the audio. Do not output English fillers such as Yeah, Okay, Like my power, or My power unless the character clearly speaks English. Do not output Simplified Chinese drift inside the transcript. Preserve names, honorifics, pauses, and short utterances. Return only the transcript text.")
        : QStringLiteral("ASR only. Transcribe dialogue accurately in the original spoken language. Do not translate, summarize, or explain. Preserve names, honorifics, pauses, and short utterances. Return only the transcript text.");
    audioRequest.options.insert(QStringLiteral("response_format"), QStringLiteral("verbose_json"));
    audioRequest.options.insert(QStringLiteral("timestamp_granularities"), QJsonArray{ QStringLiteral("segment") });

    const AIAudioTranscriptionResult transcription = _providerManager->transcribe(audioRequest);
    if (!transcription.success) {
        if (errorMessage) {
            *errorMessage = transcription.errorMessage.trimmed();
        }
        return {};
    }

    QVector<GeneratedSubtitleCue> cues;
    const QJsonArray segments = extractSegments(transcription.rawJson);
    for (const QJsonValue& value : segments) {
        const QJsonObject object = value.toObject();
        const QString text = segmentText(object);
        if (text.isEmpty()) {
            continue;
        }
        GeneratedSubtitleCue cue;
        cue.startSeconds = offsetSeconds + segmentNumber(object, QStringLiteral("start"), 0.0);
        cue.endSeconds = offsetSeconds + segmentNumber(object, QStringLiteral("end"), fallbackDurationSeconds);
        cue.sourceText = normalizeJapaneseAsrSourceText(text, request.sourceLanguageHint);
        cues.push_back(cue);
    }

    if (cues.isEmpty() && !transcription.text.trimmed().isEmpty()) {
        GeneratedSubtitleCue cue;
        cue.startSeconds = offsetSeconds;
        cue.endSeconds = offsetSeconds + std::max(1.0, fallbackDurationSeconds);
        cue.sourceText = normalizeJapaneseAsrSourceText(transcription.text, request.sourceLanguageHint);
        cues.push_back(cue);
    }
    return cues;
}

bool SubtitleGenerationService::_translateCues(
    QVector<GeneratedSubtitleCue>* cues,
    const SubtitleGenerationRequest& request,
    const QString& providerId,
    const QString& model,
    QString* errorMessage,
    int* failedBatchCount) const
{
    if (!cues || cues->isEmpty()) {
        return true;
    }
    if (failedBatchCount) {
        *failedBatchCount = 0;
    }

    for (GeneratedSubtitleCue& cue : *cues) {
        cue.sourceText = normalizeJapaneseAsrSourceText(cue.sourceText, request.sourceLanguageHint);
        annotateSubtitleSourceQuality(&cue, request.sourceLanguageHint);
        if (sourceHintExpectsChinese(request.sourceLanguageHint) &&
            textContainsHan(cue.sourceText) &&
            !textContainsJapaneseKana(cue.sourceText)) {
            cue.translatedText = normalizeChineseSubtitleLiteralForZhHans(cue.sourceText);
        }
    }
    if (request.mockWithoutApi) {
        for (GeneratedSubtitleCue& cue : *cues) {
            if (cue.translatedText.trimmed().isEmpty()) {
                cue.translatedText = cue.sourceText.trimmed().isEmpty()
                    ? QStringLiteral("CGPlay subtitle generation smoke test")
                    : cue.sourceText.trimmed();
            }
        }
        return true;
    }

    QVector<SubtitleTranslationTask> tasks;
    const int cueCount = static_cast<int>(cues->size());
    for (int start = 0; start < cueCount; start += kMaxTranslateBatch) {
        SubtitleTranslationTask task;
        task.batchStart = start;
        task.batchEnd = std::min(start + kMaxTranslateBatch, cueCount);
        for (int i = task.batchStart; i < task.batchEnd; ++i) {
            const GeneratedSubtitleCue& cue = cues->at(i);
            if (!cueTextForWrite(cue, true).isEmpty()) {
                continue;
            }
            task.items.append(QJsonObject{
                { QStringLiteral("i"), i },
                { QStringLiteral("t"), cue.sourceText },
                { QStringLiteral("sourceLanguageHint"), request.sourceLanguageHint },
                { QStringLiteral("lowConfidence"), cue.lowConfidence },
                { QStringLiteral("qualityReason"), cue.qualityReason }
            });
        }
        if (!task.items.isEmpty()) {
            tasks.push_back(task);
        }
    }
    if (tasks.isEmpty()) {
        return true;
    }

    const int translationConcurrency = configuredInt(
        _userSettings,
        QStringLiteral("ai/subtitles/translation/concurrency"),
        QStringLiteral("SUBTITLE_TRANSLATION_CONCURRENCY"),
        kDefaultTranslationConcurrency,
        1,
        16);
    const int effectiveTranslationConcurrency =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("SUBTITLE_TRANSLATION_CONCURRENCY")).trimmed().isEmpty() &&
            translationConcurrency == 4
        ? kDefaultTranslationConcurrency
        : translationConcurrency;

    const QString targetLanguageName = _targetLanguageName(request.targetLanguage);
    const QString systemPrompt = QStringLiteral(
        "You translate subtitles for anime, films, and dialogue into faithful Simplified Chinese. "
        "Return only valid JSON: an array of objects with keys i and text. "
        "Keep the same i values. Translate only the current cue text. Preserve subject, action, polarity, tone, names, and short utterance length. "
        "Do not polish, dramatize, expand, summarize, complete missing words, infer plot details, merge nearby dialogue, or add context. "
        "Context may only disambiguate a word; it must never change the visible/current cue text. "
        "If the source cue is already Chinese or Traditional Chinese, only normalize to Simplified Chinese, punctuation, and spacing; do not rewrite it. "
        "For short cues such as acknowledgements, yes/no, commands, Wow, Yes, or one-word Japanese cues, return an equally short faithful Chinese cue. "
        "Do not leave non-Chinese source text untranslated, do not add explanations, and do not alter timing. "
        "%1\n"
        "Preserve established character and group names consistently. "
        "Prefer concise lines, but never omit or add meaning.")
        .arg(defaultAnimeTerminologyPrompt());

    const int taskCount = static_cast<int>(tasks.size());
    for (int batchStart = 0; batchStart < taskCount; batchStart += effectiveTranslationConcurrency) {
        const int batchEnd = std::min(batchStart + effectiveTranslationConcurrency, taskCount);
        std::vector<std::future<SubtitleTranslationTask>> futures;
        futures.reserve(static_cast<size_t>(batchEnd - batchStart));
        for (int i = batchStart; i < batchEnd; ++i) {
            SubtitleTranslationTask task = tasks.at(i);
            futures.push_back(std::async(std::launch::async, [this, task, request, providerId, model, targetLanguageName, systemPrompt]() mutable {
                const QString userPrompt = QStringLiteral(
                    "Target language: %1\n"
                    "Translate each item independently and faithfully. The surrounding order is only for ambiguity resolution and must not add words to any cue.\n%2")
                    .arg(targetLanguageName, QString::fromUtf8(QJsonDocument(task.items).toJson(QJsonDocument::Compact)));

                QString lastSoftError;
                for (int attempt = 0; attempt < kMaxSubtitleTranslationAttempts; ++attempt) {
                    QString rawText;
                    QString directError;
                    const bool usedDirectTranslationApi =
                        _chatSubtitleTranslationApi(systemPrompt, userPrompt, model, &rawText, &directError);
                    if (!usedDirectTranslationApi) {
                        if (!directError.isEmpty()) {
                            if (subtitleTranslationFailureCanFallbackToProvider(directError) && _providerManager) {
                                lastSoftError = directError;
                            } else {
                                if (subtitleTranslationFailureCanUsePlaceholder(directError)) {
                                    lastSoftError = directError;
                                    continue;
                                }
                                task.errorMessage = directError;
                                return task;
                            }
                        }

                        AIRequest aiRequest;
                        aiRequest.providerId = providerId;
                        aiRequest.model = model;
                        aiRequest.systemPrompt = systemPrompt;
                        aiRequest.userPrompt = userPrompt;
                        aiRequest.options.insert(QStringLiteral("maxOutputTokens"), 1800);
                        aiRequest.options.insert(QStringLiteral("disableThinking"), true);

                        const AIResponse response = _providerManager->chatSync(aiRequest);
                        if (!response.success) {
                            if (subtitleTranslationFailureCanUsePlaceholder(response.errorMessage)) {
                                lastSoftError = response.errorMessage;
                                continue;
                            }
                            task.errorMessage = response.errorMessage;
                            return task;
                        }
                        rawText = response.rawText;
                    }

                    const QJsonDocument json = QJsonDocument::fromJson(stripJsonMarkdownFence(rawText).toUtf8());
                    const QJsonArray translated = json.array();
                    if (translated.isEmpty()) {
                        lastSoftError = QStringLiteral("Translation response was not a JSON array");
                        continue;
                    }

                    task.translatedItems.clear();
                    for (const QJsonValue& value : translated) {
                        const QJsonObject object = value.toObject();
                        const int cueIndex = object.value(QStringLiteral("i")).toInt(-1);
                        const QString text = normalizeTranslatedSubtitleLine(
                            normalizeAnimeTerminology(object.value(QStringLiteral("text")).toString().trimmed()));
                        if (cueIndex >= task.batchStart && cueIndex < task.batchEnd && !text.isEmpty()) {
                            if (textContainsJapaneseKana(text)) {
                                lastSoftError = QStringLiteral("Translation response still contains Japanese kana");
                                continue;
                            }
                            task.translatedItems.push_back(qMakePair(cueIndex, text));
                        }
                    }
                    if (!task.translatedItems.isEmpty()) {
                        return task;
                    }
                }
                qWarning() << "[SubtitleGeneration] Translation batch used source placeholders after retries"
                           << task.batchStart + 1
                           << task.batchEnd
                           << lastSoftError;
                task.errorMessage = lastSoftError.isEmpty()
                    ? QStringLiteral("Subtitle translation returned no usable Chinese text")
                    : lastSoftError;
                return task;
            }));
        }

        for (auto& future : futures) {
            const SubtitleTranslationTask task = future.get();
            if (!task.errorMessage.isEmpty()) {
                if (failedBatchCount) {
                    *failedBatchCount += 1;
                }
                if (errorMessage && errorMessage->trimmed().isEmpty()) {
                    *errorMessage = task.errorMessage;
                }
                qWarning() << "[SubtitleGeneration] Skipping failed subtitle translation batch"
                           << task.batchStart + 1
                           << task.batchEnd
                           << task.errorMessage;
                continue;
            }
            for (const auto& item : task.translatedItems) {
                const int cueIndex = item.first;
                if (cueIndex >= 0 && cueIndex < cues->size()) {
                    (*cues)[cueIndex].translatedText =
                        enforceSafetyNegationPolarity((*cues)[cueIndex].sourceText, item.second);
                }
            }
        }
    }
    applySafetyNegationPolarity(cues);
    return true;
}

bool SubtitleGenerationService::_chatSubtitleTranslationApi(
    const QString& systemPrompt,
    const QString& userPrompt,
    const QString& model,
    QString* rawText,
    QString* errorMessage) const
{
    QString baseUrl;
    QString apiKey;
    QString providerKind = QStringLiteral("openai-compatible");
    if (_userSettings) {
        providerKind = _userSettings->value(
            QStringLiteral("ai/subtitles/translation/provider"),
            QStringLiteral("openai-compatible")).toString().trimmed();
        if (providerKind.isEmpty()) {
            providerKind = QStringLiteral("openai-compatible");
        }
        baseUrl = _userSettings->value(QStringLiteral("ai/subtitles/translation/baseUrl")).toString().trimmed();
        if (providerKind.compare(QStringLiteral("qwen"), Qt::CaseInsensitive) == 0) {
            if (baseUrl.isEmpty()) {
                baseUrl = _userSettings->value(QStringLiteral("ai/subtitles/qwen/baseUrl")).toString().trimmed();
            }
            if (baseUrl.isEmpty()) {
                baseUrl = QString::fromLatin1(kQwenDefaultCompatibleBaseUrl);
            }
        }
        if (baseUrl.isEmpty() &&
            providerKind.compare(QStringLiteral("mimo"), Qt::CaseInsensitive) == 0) {
            baseUrl = _userSettings->value(QStringLiteral("ai/subtitles/mimo/baseUrl")).toString().trimmed();
        }
        if (baseUrl.isEmpty() &&
            providerKind.compare(QStringLiteral("openai-compatible"), Qt::CaseInsensitive) == 0) {
            baseUrl = _userSettings->value(QStringLiteral("ai/connection/baseUrl")).toString().trimmed();
        }
        if (baseUrl.isEmpty() &&
            providerKind.compare(QStringLiteral("openai-compatible"), Qt::CaseInsensitive) == 0) {
            baseUrl = _userSettings->value(QStringLiteral("ai/providers/openai/baseUrl")).toString().trimmed();
        }
    }
    if (apiKey.isEmpty()) {
        if (auto* store = ServiceLocator::getService<IAICredentialStore>()) {
            QString error;
            for (const QString& id : {
                     QString::fromLatin1(kSubtitleTranslationCredentialId),
                     QString::fromLatin1(kQwenCredentialId),
                     QString::fromLatin1(kMimoCredentialId),
                     QString::fromLatin1(kGenericCredentialId),
                     QString::fromLatin1(kOpenAICredentialId)
                 }) {
                const QByteArray secret = store->loadSecret(id, &error).trimmed();
                if (!secret.isEmpty()) {
                    apiKey = QString::fromUtf8(secret).trimmed();
                    break;
                }
            }
        }
    }
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (baseUrl.isEmpty()) {
        baseUrl = env.value(QStringLiteral("SUBTITLE_TRANSLATION_BASE_URL")).trimmed();
    }
    if (apiKey.isEmpty()) {
        apiKey = env.value(QStringLiteral("SUBTITLE_TRANSLATION_API_KEY")).trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = env.value(QStringLiteral("MIMO_BASE_URL")).trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = env.value(QStringLiteral("XIAOMI_MIMO_BASE_URL")).trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = env.value(QStringLiteral("QWEN_BASE_URL")).trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = env.value(QStringLiteral("DASHSCOPE_BASE_URL")).trimmed();
    }
    if (apiKey.isEmpty()) {
        apiKey = env.value(QStringLiteral("MIMO_API_KEY")).trimmed();
    }
    if (apiKey.isEmpty()) {
        apiKey = env.value(QStringLiteral("XIAOMI_MIMO_API_KEY")).trimmed();
    }
    if (apiKey.isEmpty()) {
        apiKey = env.value(QStringLiteral("QWEN_API_KEY")).trimmed();
    }
    if (apiKey.isEmpty()) {
        apiKey = env.value(QStringLiteral("DASHSCOPE_API_KEY")).trimmed();
    }
    if (baseUrl.isEmpty() &&
        providerKind.compare(QStringLiteral("qwen"), Qt::CaseInsensitive) == 0) {
        baseUrl = QString::fromLatin1(kQwenDefaultCompatibleBaseUrl);
    }
    if (baseUrl.isEmpty()) {
        return false;
    }
    if (apiKey.isEmpty()) {
        if (errorMessage) {
            *errorMessage = providerKind.compare(QStringLiteral("qwen"), Qt::CaseInsensitive) == 0
                ? QStringLiteral("Subtitle translation API key is missing for provider=qwen")
                : QStringLiteral("Subtitle translation API key is missing");
        }
        return false;
    }
    QString effectiveModel = model.trimmed();
    if (effectiveModel.isEmpty() &&
        providerKind.compare(QStringLiteral("qwen"), Qt::CaseInsensitive) == 0) {
        effectiveModel = QString::fromLatin1(kQwenDefaultTranslationModel);
    }
    if (modelLooksUnsuitableForSubtitleTranslation(effectiveModel)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Subtitle translation model is missing or not suitable for translation");
        }
        return false;
    }

    const QString endpoint = normalizeOpenAIChatEndpoint(baseUrl);
    QJsonObject payload{
        { QStringLiteral("model"), effectiveModel },
        { QStringLiteral("messages"), QJsonArray{
            QJsonObject{
                { QStringLiteral("role"), QStringLiteral("system") },
                { QStringLiteral("content"), systemPrompt }
            },
            QJsonObject{
                { QStringLiteral("role"), QStringLiteral("user") },
                { QStringLiteral("content"), userPrompt }
            }
        } },
        { QStringLiteral("temperature"), 0.2 },
        { QStringLiteral("max_tokens"), 1800 }
    };

    QNetworkRequest request{ QUrl(endpoint) };
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());

    QNetworkAccessManager manager;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QNetworkReply* reply = manager.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(kSubtitleTranslationRequestTimeoutMs);
    loop.exec();

    if (timer.isActive()) {
        timer.stop();
    } else {
        reply->abort();
        reply->deleteLater();
        if (errorMessage) {
            *errorMessage = QStringLiteral("Subtitle translation API timed out: provider=%1 model=%2 endpoint=%3")
                .arg(providerKind, effectiveModel, endpoint);
        }
        return false;
    }

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBytes = reply->readAll();
    const QString networkError = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
    reply->deleteLater();

    const QJsonDocument document = QJsonDocument::fromJson(responseBytes);
    const QJsonObject object = document.object();
    if (statusCode < 200 || statusCode >= 300 || !networkError.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Subtitle translation API failed: provider=%1 model=%2 endpoint=%3 HTTP %4: %5")
                .arg(providerKind, effectiveModel, endpoint)
                .arg(statusCode)
                .arg(networkError.isEmpty() ? QString::fromUtf8(responseBytes).left(500) : networkError);
        }
        return false;
    }

    const QString text = firstChatCompletionText(object);
    if (text.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Subtitle translation API returned empty text");
        }
        return false;
    }
    if (rawText) {
        *rawText = text;
    }
    qInfo() << "[SubtitleGeneration] Subtitle translation API request succeeded"
            << "provider=" << providerKind
            << "model=" << effectiveModel
            << "endpoint=" << endpoint;
    return true;
}

bool SubtitleGenerationService::_refineCuesWithContext(
    QVector<GeneratedSubtitleCue>* cues,
    const SubtitleRefinementRequest& request,
    const QString& providerId,
    const QString& model,
    QString* errorMessage) const
{
    if (!cues || cues->isEmpty()) {
        return true;
    }
    const bool hasWorkspaceRoute =
        !providerId.trimmed().isEmpty() &&
        !modelLooksUnsuitableForSubtitleTranslation(model);
    SubtitleGenerationRequest subtitleFallbackRequest;
    subtitleFallbackRequest.targetLanguage = request.targetLanguage;
    const QString subtitleFallbackModel = _translationModel(subtitleFallbackRequest);

    constexpr int kRefineBatchSize = 16;
    constexpr int kRefineContextSize = 5;
    const QString targetLanguageName = _targetLanguageName(request.targetLanguage);
    int successfulBatches = 0;
    int skippedBatches = 0;
    const QString systemPrompt = QStringLiteral(
        "You are a subtitle refinement editor for Japanese anime and films. "
        "Improve existing Simplified Chinese subtitles using the source text and neighboring context. "
        "Keep exactly the same cue ids and do not change timing or cue count. "
        "Fix mistranslations, ASR mistakes, missing Chinese translations, Japanese leftovers, names, titles, tone, and terminology. "
        "Cues with lowConfidence=true likely have ASR language drift, English filler substitutions, Chinese/English mixed into Japanese source, or bad segmentation; use neighboring context to repair them conservatively. "
        "If translationQualityReason is present, prioritize that cue; if a cue looks reliable, keep the quick subtitle unless there is a clear contextual correction. "
        "Keep lines concise and natural for on-screen subtitles. "
        "Return only valid JSON: an array of objects with keys i and text. "
        "Do not add explanations. Do not invent plot details. "
        "%1")
        .arg(defaultAnimeTerminologyPrompt());

    const int cueCount = static_cast<int>(cues->size());
    for (int batchStart = 0; batchStart < cueCount; batchStart += kRefineBatchSize) {
        if (request.cancelRequested && request.cancelRequested->load()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Subtitle refinement canceled");
            }
            return false;
        }

        const int batchEnd = std::min(batchStart + kRefineBatchSize, cueCount);
        const int contextStart = std::max(0, batchStart - kRefineContextSize);
        const int contextEnd = std::min(cueCount, batchEnd + kRefineContextSize);

        QJsonArray contextItems;
        for (int i = contextStart; i < contextEnd; ++i) {
            const GeneratedSubtitleCue& cue = cues->at(i);
            const QString source = i < request.sourceCues.size()
                ? request.sourceCues.at(i).sourceText.trimmed()
                : cue.sourceText.trimmed();
            const QJsonObject quality = cue.qualityMetrics.isEmpty()
                ? analyzeSubtitleSourceQuality(
                    source,
                    request.sourceLanguageHint,
                    std::max(0.0, cue.endSeconds - cue.startSeconds))
                : cue.qualityMetrics;
            const bool lowConfidence = cue.lowConfidence ||
                quality.value(QStringLiteral("lowConfidence")).toBool(false);
            const QString translationQualityReason = subtitleTranslationQualityReason(cue);
            QStringList qualityReasons;
            if (lowConfidence) {
                const QString sourceReason = cue.qualityReason.trimmed().isEmpty()
                    ? quality.value(QStringLiteral("reason")).toString().trimmed()
                    : cue.qualityReason.trimmed();
                if (!sourceReason.isEmpty()) {
                    qualityReasons.push_back(sourceReason);
                }
            }
            if (!translationQualityReason.isEmpty()) {
                qualityReasons.push_back(translationQualityReason);
            }
            contextItems.append(QJsonObject{
                { QStringLiteral("i"), i },
                { QStringLiteral("refine"), i >= batchStart && i < batchEnd },
                { QStringLiteral("start"), cue.startSeconds },
                { QStringLiteral("end"), cue.endSeconds },
                { QStringLiteral("source"), repairSubtitleMojibake(source) },
                { QStringLiteral("quick"), normalizeTranslatedSubtitleLine(cue.translatedText) },
                { QStringLiteral("lowConfidence"), lowConfidence || !translationQualityReason.isEmpty() },
                { QStringLiteral("qualityReason"), qualityReasons.join(QLatin1Char('|')) },
                { QStringLiteral("translationQualityReason"), translationQualityReason },
                { QStringLiteral("quality"), quality }
            });
        }

        const QString userPrompt = QStringLiteral(
            "Target language: %1\n"
            "Only rewrite items where refine=true. Keep each i unchanged. "
            "Use refine=false items only as context.\n"
            "%2")
            .arg(targetLanguageName, QString::fromUtf8(QJsonDocument(contextItems).toJson(QJsonDocument::Compact)));

        QString rawText;
        QString workspaceError;
        if (hasWorkspaceRoute && _providerManager) {
            AIRequest aiRequest;
            aiRequest.providerId = providerId;
            aiRequest.model = model;
            aiRequest.systemPrompt = systemPrompt;
            aiRequest.userPrompt = userPrompt;
            aiRequest.options.insert(QStringLiteral("maxOutputTokens"), 3000);
            aiRequest.options.insert(QStringLiteral("disableThinking"), true);

            const AIResponse response = _providerManager->chatSync(aiRequest);
            if (response.success) {
                rawText = response.rawText;
            } else {
                workspaceError = response.errorMessage.trimmed();
            }
        } else {
            workspaceError = QStringLiteral("Subtitle refinement workspace API is not configured");
        }

        if (rawText.trimmed().isEmpty()) {
            if (!workspaceRefinementFailureCanUseSubtitleApiFallback(workspaceError)) {
                if (errorMessage) {
                    *errorMessage = workspaceError.isEmpty()
                        ? QStringLiteral("Subtitle refinement returned empty text from workspace API")
                        : workspaceError;
                }
                skippedBatches += 1;
                qWarning() << "[SubtitleRefinement] Skipping refinement batch; workspace returned no usable text"
                           << batchStart + 1
                           << batchEnd
                           << workspaceError;
                continue;
            }

            QString directError;
            if (!_chatSubtitleTranslationApi(systemPrompt, userPrompt, subtitleFallbackModel, &rawText, &directError)) {
                if (errorMessage) {
                    *errorMessage = directError.isEmpty() ? workspaceError : directError;
                }
                skippedBatches += 1;
                qWarning() << "[SubtitleRefinement] Skipping refinement batch; fallback API failed"
                           << batchStart + 1
                           << batchEnd
                           << directError;
                continue;
            }
        }

        QJsonDocument document = QJsonDocument::fromJson(stripJsonMarkdownFence(rawText).toUtf8());
        QJsonArray refinedItems = document.array();
        if (refinedItems.isEmpty() && document.isObject()) {
            refinedItems = document.object().value(QStringLiteral("items")).toArray();
        }
        if (refinedItems.isEmpty()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Subtitle refinement response was not a JSON array");
            }
            skippedBatches += 1;
            qWarning() << "[SubtitleRefinement] Skipping refinement batch; response was not JSON"
                       << batchStart + 1
                       << batchEnd;
            continue;
        }

        int applied = 0;
        for (const QJsonValue& value : refinedItems) {
            const QJsonObject object = value.toObject();
            const int cueIndex = object.value(QStringLiteral("i")).toInt(-1);
            if (cueIndex < batchStart || cueIndex >= batchEnd || cueIndex >= cues->size()) {
                continue;
            }
            QString text = object.value(QStringLiteral("text")).toString().trimmed();
            text = normalizeTranslatedSubtitleLine(normalizeAnimeTerminology(text));
            text = enforceSafetyNegationPolarity(cues->at(cueIndex).sourceText, text);
            if (text.isEmpty() || textContainsJapaneseKana(text)) {
                continue;
            }
            (*cues)[cueIndex].translatedText = text;
            applied += 1;
        }
        if (applied <= 0) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Subtitle refinement did not return all requested cue ids");
            }
            skippedBatches += 1;
            qWarning() << "[SubtitleRefinement] Skipping empty refinement batch"
                       << batchStart + 1
                       << batchEnd
                       << "applied=" << applied;
            continue;
        }
        if (applied != batchEnd - batchStart) {
            skippedBatches += 1;
            successfulBatches += 1;
            qWarning() << "[SubtitleRefinement] Keeping partial refinement batch; quick subtitles remain for missing cues"
                       << batchStart + 1
                       << batchEnd
                       << "applied=" << applied;
            continue;
        }
        successfulBatches += 1;
    }

    if (successfulBatches == 0 && skippedBatches > 0) {
        return false;
    }
    return true;
}

QJsonObject SubtitleGenerationService::_validateRefinedCues(
    const QVector<GeneratedSubtitleCue>& quickCues,
    const QVector<GeneratedSubtitleCue>& refinedCues) const
{
    QJsonObject validation{
        { QStringLiteral("passed"), false },
        { QStringLiteral("quickCueCount"), quickCues.size() },
        { QStringLiteral("refinedCueCount"), refinedCues.size() },
        { QStringLiteral("emptyTranslations"), 0 },
        { QStringLiteral("japaneseKanaTranslations"), 0 },
        { QStringLiteral("mojibakeTranslations"), 0 },
        { QStringLiteral("timingMismatches"), 0 },
        { QStringLiteral("lengthOutliers"), 0 }
    };

    if (quickCues.size() != refinedCues.size()) {
        validation.insert(QStringLiteral("reason"), QStringLiteral("cue-count-mismatch"));
        return validation;
    }

    int emptyCount = 0;
    int kanaCount = 0;
    int mojibakeCount = 0;
    int timingMismatchCount = 0;
    int lengthOutlierCount = 0;
    int changedCount = 0;

    for (int i = 0; i < quickCues.size(); ++i) {
        const GeneratedSubtitleCue& quick = quickCues.at(i);
        const GeneratedSubtitleCue& refined = refinedCues.at(i);
        if (std::abs(quick.startSeconds - refined.startSeconds) > 0.02 ||
            std::abs(quick.endSeconds - refined.endSeconds) > 0.02) {
            timingMismatchCount += 1;
        }
        const QString text = normalizeTranslatedSubtitleLine(refined.translatedText);
        if (text.isEmpty()) {
            emptyCount += 1;
        }
        if (textContainsJapaneseKana(text)) {
            kanaCount += 1;
        }
        if (subtitleMojibakeScore(text) >= 4) {
            mojibakeCount += 1;
        }

        const QString quickText = normalizeTranslatedSubtitleLine(quick.translatedText);
        const int sourceLength = std::max(
            static_cast<int>(quick.sourceText.trimmed().size()),
            static_cast<int>(quickText.size()));
        if (sourceLength > 0 && static_cast<int>(text.size()) > std::max(80, sourceLength * 4 + 20)) {
            lengthOutlierCount += 1;
        }
        if (sourceLength >= 4 && text.size() <= 1) {
            lengthOutlierCount += 1;
        }
        if (text != quickText) {
            changedCount += 1;
        }
    }

    validation.insert(QStringLiteral("emptyTranslations"), emptyCount);
    validation.insert(QStringLiteral("japaneseKanaTranslations"), kanaCount);
    validation.insert(QStringLiteral("mojibakeTranslations"), mojibakeCount);
    validation.insert(QStringLiteral("timingMismatches"), timingMismatchCount);
    validation.insert(QStringLiteral("lengthOutliers"), lengthOutlierCount);
    validation.insert(QStringLiteral("changedCueCount"), changedCount);

    if (timingMismatchCount > 0) {
        validation.insert(QStringLiteral("reason"), QStringLiteral("timing-mismatch"));
        return validation;
    }
    if (emptyCount > 0) {
        validation.insert(QStringLiteral("reason"), QStringLiteral("empty-translation"));
        return validation;
    }
    if (kanaCount > 0) {
        validation.insert(QStringLiteral("reason"), QStringLiteral("japanese-kana-leftover"));
        return validation;
    }
    if (mojibakeCount > 0) {
        validation.insert(QStringLiteral("reason"), QStringLiteral("mojibake-leftover"));
        return validation;
    }
    if (lengthOutlierCount > std::max(1, static_cast<int>(refinedCues.size()) / 8)) {
        validation.insert(QStringLiteral("reason"), QStringLiteral("length-outliers"));
        return validation;
    }

    validation.insert(QStringLiteral("passed"), true);
    validation.insert(QStringLiteral("reason"), QStringLiteral("ok"));
    return validation;
}

bool SubtitleGenerationService::_writeSrt(
    const QString& path,
    const QVector<GeneratedSubtitleCue>& cues,
    bool translated) const
{
    QFile file(path);
    QDir().mkpath(QFileInfo(file).absolutePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    int index = 1;
    for (const auto& cue : cues) {
        const QString text = cueTextForWrite(cue, translated);
        if (text.isEmpty()) {
            continue;
        }
        stream << index++ << "\n";
        stream << srtTime(cue.startSeconds) << " --> " << srtTime(cue.endSeconds) << "\n";
        stream << text << "\n\n";
    }
    return true;
}

bool SubtitleGenerationService::_writeVtt(
    const QString& path,
    const QVector<GeneratedSubtitleCue>& cues,
    bool translated) const
{
    QFile file(path);
    QDir().mkpath(QFileInfo(file).absolutePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "WEBVTT\n\n";
    for (const auto& cue : cues) {
        const QString text = cueTextForWrite(cue, translated);
        if (text.isEmpty()) {
            continue;
        }
        stream << vttTime(cue.startSeconds) << " --> " << vttTime(cue.endSeconds) << "\n";
        stream << text << "\n\n";
    }
    return true;
}

} // namespace cgplay
