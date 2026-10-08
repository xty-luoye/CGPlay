#include "TranslationEnhancementSupport.h"

#include "common/jobs/JobSystem.h"
#include "TranslationFusionController.h"
#include "TranslationPlaybackStrategy.h"
#include "TranslationTextPolicy.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QStandardPaths>
#include <QStringConverter>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QEventLoop>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cgplay::translation_enhancement_support {

QString onlineVttTime(double seconds)
{
    const qint64 totalMs = std::max<qint64>(0, qRound64(seconds * 1000.0));
    const qint64 ms = totalMs % 1000;
    const qint64 totalSeconds = totalMs / 1000;
    const qint64 s = totalSeconds % 60;
    const qint64 totalMinutes = totalSeconds / 60;
    const qint64 m = totalMinutes % 60;
    const qint64 h = totalMinutes / 60;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(ms, 3, 10, QLatin1Char('0'));
}

QString defaultOnlineHelperPath()
{
    const QStringList candidates{
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("tools/cgplay/ai/online_subtitle_search.py")),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../../../tools/cgplay/ai/online_subtitle_search.py")),
        QDir::current().filePath(QStringLiteral("tools/cgplay/ai/online_subtitle_search.py"))
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return candidates.last();
}

QString defaultOcrHelperPath()
{
    const QStringList candidates{
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("tools/cgplay/ai/subtitle_ocr_extract.py")),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../../../tools/cgplay/ai/subtitle_ocr_extract.py")),
        QDir::current().filePath(QStringLiteral("tools/cgplay/ai/subtitle_ocr_extract.py"))
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return candidates.last();
}

QString defaultCodexExecutablePath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates{
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("CGPLAY_CODEX_EXECUTABLE")),
        QDir(appDir).filePath(QStringLiteral("runtime/codex/codex.exe")),
        QDir(appDir).filePath(QStringLiteral("../../runtime/codex/codex.exe")),
        QDir(QProcessEnvironment::systemEnvironment().value(QStringLiteral("LOCALAPPDATA")))
            .filePath(QStringLiteral("Programs/CGPlay/runtime/codex/codex.exe")),
        QStandardPaths::findExecutable(QStringLiteral("codex.exe"))
    };
    for (const QString& candidate : candidates) {
        const QFileInfo info(candidate.trimmed());
        if (info.isAbsolute() && info.isFile()) return info.absoluteFilePath();
    }
    return {};
}

QString subtitleCodexHomePath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("subtitle-codex-home"));
}

QString defaultLongAsrHelperPath()
{
    const QStringList candidates{
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("tools/cgplay/ai/long_window_asr_worker.py")),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../../../tools/cgplay/ai/long_window_asr_worker.py")),
        QDir::current().filePath(QStringLiteral("tools/cgplay/ai/long_window_asr_worker.py"))
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return candidates.last();
}

QString defaultLongAsrTranslateHelperPath()
{
    const QStringList candidates{
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("tools/cgplay/ai/local_text_translate.py")),
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../../../tools/cgplay/ai/local_text_translate.py")),
        QDir::current().filePath(QStringLiteral("tools/cgplay/ai/local_text_translate.py"))
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return candidates.last();
}

QString ocrIntakeJsonPath(const QString& ocrSourcePath)
{
    QString path = ocrSourcePath;
    if (path.endsWith(QStringLiteral(".ocr.source.vtt"), Qt::CaseInsensitive)) {
        path.chop(QStringLiteral(".ocr.source.vtt").size());
        path += QStringLiteral(".ocr.intake.json");
    } else {
        path += QStringLiteral(".intake.json");
    }
    return path;
}

QJsonObject readJsonObjectFile(const QString& path)
{
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

ProcessOutcome waitForWorkerProcess(
    QProcess* process,
    int timeoutMs,
    const std::shared_ptr<std::atomic_bool>& cancelRequested,
    bool* canceled)
{
    if (canceled) *canceled = false;
    if (!process) return {};

    JobContext context(timeoutMs);
    std::mutex monitorMutex;
    std::condition_variable monitorWake;
    bool finished = false;
    bool monitorUnavailable = false;
    std::thread cancelMonitor;
    if (cancelRequested) {
        try {
            cancelMonitor = std::thread([&]() {
                std::unique_lock<std::mutex> lock(monitorMutex);
                while (!finished) {
                    if (cancelRequested->load()) {
                        context.cancel();
                        return;
                    }
                    monitorWake.wait_for(lock, std::chrono::milliseconds(25), [&]() {
                        return finished;
                    });
                }
            });
        } catch (const std::system_error&) {
            monitorUnavailable = true;
            context.cancel();
        }
    }

    ProcessOutcome outcome = context.waitForProcess(*process, 25, timeoutMs);
    {
        std::lock_guard<std::mutex> lock(monitorMutex);
        finished = true;
    }
    monitorWake.notify_one();
    if (cancelMonitor.joinable()) {
        cancelMonitor.join();
    }
    if (monitorUnavailable) {
        outcome.state = JobState::TimedOut;
    }
    if (canceled) {
        *canceled = outcome.state == JobState::Canceled;
    }
    return outcome;
}

bool lowerWorkerPriorityForPlayback(QProcess* process, bool playbackAlreadyRunning)
{
    if (!process || !playbackAlreadyRunning || process->processId() <= 0) return false;
#ifdef Q_OS_WIN
    HANDLE handle = OpenProcess(PROCESS_SET_INFORMATION, FALSE, static_cast<DWORD>(process->processId()));
    if (!handle) return false;
    const bool changed = SetPriorityClass(handle, BELOW_NORMAL_PRIORITY_CLASS) != FALSE;
    CloseHandle(handle);
    return changed;
#else
    return false;
#endif
}

QString longAsrJsonPath(const QString& sourcePath)
{
    QString path = sourcePath;
    if (path.endsWith(QStringLiteral(".longasr.source.vtt"), Qt::CaseInsensitive)) {
        path.chop(QStringLiteral(".longasr.source.vtt").size());
        path += QStringLiteral(".longasr.report.json");
    } else {
        path += QStringLiteral(".report.json");
    }
    return path;
}

QString onlineSearchJsonPath(const QString& onlineSourcePath)
{
    QString path = onlineSourcePath;
    if (path.endsWith(QStringLiteral(".online.source.vtt"), Qt::CaseInsensitive)) {
        path.chop(QStringLiteral(".online.source.vtt").size());
        path += QStringLiteral(".online.search.json");
    } else {
        path += QStringLiteral(".online.search.json");
    }
    return path;
}

QString onlineReferenceProviderKind(const TranslationEnhancementScheduleRequest& request)
{
    if (request.localReferenceProviderAvailable &&
        QFileInfo::exists(request.localReferenceSubtitlePath)) {
        return QStringLiteral("local-reference-provider");
    }
    if (request.workbenchCanSearchOnlineSubtitles) {
        return QStringLiteral("workbench-online-reference-provider");
    }
    if (request.onlineConfigured &&
        (request.onlineMockMode ||
         !request.onlineApiKey.trimmed().isEmpty() ||
         !request.onlineBaseUrl.trimmed().isEmpty())) {
        return QStringLiteral("explicit-online-search-provider");
    }
    return QStringLiteral("not-configured");
}

QString onlineReferenceFallbackReason(const TranslationEnhancementScheduleRequest& request)
{
    const QString kind = onlineReferenceProviderKind(request);
    if (kind == QStringLiteral("local-reference-provider")) {
        return QStringLiteral("local-reference-available");
    }
    if (kind == QStringLiteral("workbench-online-reference-provider")) {
        return QStringLiteral("workbench-online-reference-provider-available");
    }
    if (kind == QStringLiteral("explicit-online-search-provider")) {
        if (!request.onlineMockMode && request.onlineApiKey.trimmed().isEmpty()) {
            return QStringLiteral("online-search-api-key-missing");
        }
        return QStringLiteral("explicit-online-search-provider-available");
    }
    if (request.workbenchProviderAvailable && !request.workbenchCanSearchOnlineSubtitles) {
        return QStringLiteral("workbench-online-search-api-not-configured");
    }
    return QStringLiteral("online-search-provider-not-configured");
}

QString translateSubtitleTextWithHelper(
    const QString& helperPath,
    const QString& text,
    const QString& sourceLanguage,
    const QString& targetLanguage,
    QString* errorMessage)
{
    if (text.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("empty-translation-input");
        }
        return {};
    }
    if (!QFileInfo::exists(helperPath)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("translation-helper-missing");
        }
        return {};
    }
    QProcess process;
    QProcessEnvironment processEnv = QProcessEnvironment::systemEnvironment();
    processEnv.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    process.setProcessEnvironment(processEnv);
    const QString python = QProcessEnvironment::systemEnvironment().value(QStringLiteral("SUBTITLE_ONLINE_PYTHON"), QStringLiteral("python"));
    process.start(python, { helperPath, text, sourceLanguage, targetLanguage });
    if (!process.waitForStarted(5000)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("translation-helper-start-failed");
        }
        return {};
    }
    JobContext job(90000);
    const ProcessOutcome processOutcome = job.waitForProcess(process, 25, 90000);
    if (processOutcome.state == JobState::TimedOut) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("translation-helper-timeout");
        }
        return {};
    }
    const QString stdoutText = QString::fromUtf8(processOutcome.standardOutput).trimmed();
    const QJsonDocument doc = QJsonDocument::fromJson(stdoutText.toUtf8());
    const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject{};
    const QString translatedText = obj.value(QStringLiteral("translatedText")).toString().trimmed();
    if (!processOutcome.succeeded() || translatedText.isEmpty()) {
        if (errorMessage) {
            *errorMessage = obj.value(QStringLiteral("error")).toString(QStringLiteral("translation-helper-failed"));
        }
        return {};
    }
    return translatedText;
}

QString normalizeOpenAIChatEndpoint(QString baseUrl)
{
    baseUrl = baseUrl.trimmed();
    while (baseUrl.endsWith(QLatin1Char('/'))) {
        baseUrl.chop(1);
    }
    if (baseUrl.endsWith(QStringLiteral("/chat/completions"), Qt::CaseInsensitive)) {
        return baseUrl;
    }
    if (baseUrl.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive)) {
        return baseUrl + QStringLiteral("/chat/completions");
    }
    return baseUrl + QStringLiteral("/v1/chat/completions");
}

QString firstChatCompletionText(const QJsonObject& object)
{
    const QJsonArray choices = object.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return object.value(QStringLiteral("output_text")).toString(
            object.value(QStringLiteral("text")).toString());
    }
    const QJsonObject message = choices.first().toObject().value(QStringLiteral("message")).toObject();
    const QJsonValue content = message.value(QStringLiteral("content"));
    if (content.isArray()) {
        QStringList parts;
        for (const QJsonValue& part : content.toArray()) {
            if (!part.isObject()) {
                continue;
            }
            parts.append(part.toObject().value(QStringLiteral("text")).toString());
        }
        return parts.join(QStringLiteral("\n")).trimmed();
    }
    return content.toString().trimmed();
}

QString translateSubtitleTextWithOpenAICompatible(
    const QString& systemPrompt,
    const QString& userPrompt,
    const QString& baseUrl,
    const QString& apiKey,
    const QString& model,
    QString* errorMessage)
{
    const QString endpoint = normalizeOpenAIChatEndpoint(baseUrl);
    if (endpoint.isEmpty() || apiKey.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("translation-provider-unavailable");
        }
        return {};
    }
    QJsonObject payload{
        { QStringLiteral("model"), model.trimmed() },
        { QStringLiteral("messages"), QJsonArray{
            QJsonObject{{ QStringLiteral("role"), QStringLiteral("system") }, { QStringLiteral("content"), systemPrompt }},
            QJsonObject{{ QStringLiteral("role"), QStringLiteral("user") }, { QStringLiteral("content"), userPrompt }}
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
    timer.start(90000);
    loop.exec();
    if (timer.isActive()) {
        timer.stop();
    } else {
        reply->abort();
        reply->deleteLater();
        if (errorMessage) {
            *errorMessage = QStringLiteral("translation-provider-timeout");
        }
        return {};
    }
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBytes = reply->readAll();
    const QString networkError = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
    reply->deleteLater();
    if (statusCode < 200 || statusCode >= 300 || !networkError.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("translation-provider-http-error:%1").arg(statusCode);
        }
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(responseBytes);
    const QString text = firstChatCompletionText(doc.isObject() ? doc.object() : QJsonObject{});
    if (text.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("translation-provider-empty-text");
        }
        return {};
    }
    return text.trimmed();
}

bool writeJsonFile(const QString& path, const QJsonObject& payload)
{
    if (path.trimmed().isEmpty()) {
        return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(payload).toJson(QJsonDocument::Indented));
    return true;
}

bool isFinalDisplayableText(const QString& text)
{
    return translation_text::isFinalDisplayableText(text);
}

QString repairSchedulerMojibake(const QString& text);


QString activeEnhancementSource(
    bool manualFusionSucceeded,
    const QString& fusionPrimarySource,
    bool repairSucceeded,
    bool longAsrSucceeded,
    bool ocrSucceeded,
    bool onlineTranslationSucceeded)
{
    if (manualFusionSucceeded) {
        return fusionPrimarySource;
    }
    if (repairSucceeded) {
        return QStringLiteral("repair-cache");
    }
    if (longAsrSucceeded) {
        return QStringLiteral("long-asr-corrected");
    }
    if (ocrSucceeded) {
        return QStringLiteral("hard-sub-ocr");
    }
    if (onlineTranslationSucceeded) {
        return QStringLiteral("online-subtitle");
    }
    return QStringLiteral("quick-fallback");
}

QString enhancementFallbackReason(
    bool visibleEnhancementSucceeded,
    bool manualAllowed,
    bool executeWorkers,
    bool anyEnhancementSucceeded,
    bool highQualityMode)
{
    if (visibleEnhancementSucceeded) {
        return {};
    }
    if (!manualAllowed) {
        return highQualityMode
            ? QStringLiteral("manual-trigger-required-or-playback-running")
            : QStringLiteral("disabled-in-quick-playback");
    }
    if (!executeWorkers) {
        return QStringLiteral("diagnostics-only-not-executed");
    }
    return anyEnhancementSucceeded
        ? QStringLiteral("enhancement-cache-produced-but-not-displayable-fallback-quick")
        : QStringLiteral("no-high-quality-enhancement-produced-fallback-quick");
}


VisualFallbackReport visualFallbackReport(
    bool visualCueAtTime,
    bool visualTrackAuthoritativeAtTime,
    bool visualTrackAuthoritative,
    bool workbenchCanReadImageText,
    bool ocrConfigured,
    bool ocrAutoEnabledForHighQualityCurrentMedia,
    const QString& ocrDegradedReason,
    const QString& finalDisplayedText)
{
    const QString noVisualCueFallbackReason =
        visualTrackAuthoritativeAtTime
            ? QStringLiteral("visual-no-text-suppress-quick-asr")
            : (visualTrackAuthoritative
                  ? QStringLiteral("outside-visual-coverage-quick-baseline-allowed")
                  : (workbenchCanReadImageText
                        ? QStringLiteral("visual-no-current-cue-quick-baseline-allowed")
                        : (ocrConfigured || ocrAutoEnabledForHighQualityCurrentMedia
                              ? (ocrDegradedReason.trimmed().isEmpty()
                                    ? QStringLiteral("local-ocr-no-displayable-text")
                                    : ocrDegradedReason)
                              : QStringLiteral("workbench-vision-not-configured"))));
    const QString visualFallbackReason = visualCueAtTime
        ? (finalDisplayedText.isEmpty()
              ? QStringLiteral("visual-cue-text-missing-suppress-quick-asr")
              : QStringLiteral("visual-subtitle-text-detected"))
        : noVisualCueFallbackReason;
    return { noVisualCueFallbackReason, visualFallbackReason };
}

QString visualPrecheckChosenReason(
    const QString& chosenReason,
    const QString& visualFallbackReason)
{
    return chosenReason.isEmpty() ? visualFallbackReason : chosenReason;
}

QString reportedVisualCueSourceKind(
    bool visualSourceCueUsableAtTime,
    bool visualTranslatedCueUsableAtTime,
    bool visualEnhancedCueAtTime)
{
    if (visualSourceCueUsableAtTime) {
        return QStringLiteral("visual-subtitle");
    }
    if (visualTranslatedCueUsableAtTime) {
        return QStringLiteral("hard-sub-ocr");
    }
    return visualEnhancedCueAtTime ? QStringLiteral("visual-subtitle") : QString();
}

QString reportedCurrentFrameSourceKind(
    bool visualCueAtTime,
    bool currentFrameUsesWorkbenchVision,
    const QString& workerSourceKind,
    const QString& visualCueSourceKind,
    bool visualTrackAuthoritativeAtTime)
{
    if (visualCueAtTime) {
        if (currentFrameUsesWorkbenchVision) {
            return QStringLiteral("workbench-vision");
        }
        return workerSourceKind.isEmpty() || workerSourceKind == QStringLiteral("unknown")
            ? visualCueSourceKind
            : workerSourceKind;
    }
    return visualTrackAuthoritativeAtTime
        ? QStringLiteral("visual-no-text")
        : QStringLiteral("quick-baseline-allowed-outside-visual-coverage");
}

QString activeHighQualitySource(
    int enhancedDisplayableCueCount,
    double enhancedCoverageEndSeconds,
    const QString& manualFusionPrimarySource,
    int onlineDisplayableCueCount,
    double onlineCoverageEndSeconds,
    int ocrTranslatedDisplayableCueCount,
    double ocrTranslatedCoverageEndSeconds,
    int longAsrDisplayableCueCount,
    double longAsrCoverageEndSeconds)
{
    QString source = QStringLiteral("quick-fallback");
    double bestCoverageEndSeconds = -1.0;
    const auto consider = [&](int cueCount, double coverageEndSeconds, const QString& candidate) {
        if (cueCount > 0 && coverageEndSeconds > bestCoverageEndSeconds + 0.001) {
            bestCoverageEndSeconds = coverageEndSeconds;
            source = candidate;
        }
    };
    consider(
        enhancedDisplayableCueCount,
        enhancedCoverageEndSeconds,
        manualFusionPrimarySource.isEmpty() ? QStringLiteral("enhanced") : manualFusionPrimarySource);
    consider(onlineDisplayableCueCount, onlineCoverageEndSeconds, QStringLiteral("online-subtitle"));
    consider(ocrTranslatedDisplayableCueCount, ocrTranslatedCoverageEndSeconds, QStringLiteral("hard-sub-ocr"));
    consider(longAsrDisplayableCueCount, longAsrCoverageEndSeconds, QStringLiteral("long-asr-corrected"));
    return source;
}

FinalDisplaySelection selectCurrentFrameFinalDisplay(
    bool visualCueAtTime,
    const QString& visualSourceDisplayNormalized,
    const QString& visualTranslatedDisplayNormalized,
    const QString& visualEnhancedDisplayNormalized,
    bool quickCueAtTime,
    const QString& quickDisplayedText,
    bool visualTrackAuthoritativeAtTime)
{
    const QString quickDisplayableText = repairSchedulerMojibake(quickDisplayedText).trimmed();
    if (visualCueAtTime) {
        if (isFinalDisplayableText(visualSourceDisplayNormalized)) {
            return { visualSourceDisplayNormalized, QStringLiteral("literal-cjk-visual-cue") };
        }
        if (isFinalDisplayableText(visualTranslatedDisplayNormalized)) {
            return { visualTranslatedDisplayNormalized, QStringLiteral("translated-visual-cue") };
        }
        if (isFinalDisplayableText(visualEnhancedDisplayNormalized)) {
            return { visualEnhancedDisplayNormalized, QStringLiteral("translated-enhanced-visual-cue") };
        }
        if (quickCueAtTime && isFinalDisplayableText(quickDisplayableText)) {
            return { quickDisplayableText, QStringLiteral("quick-chinese-baseline-until-visual-translation-ready") };
        }
        return { {}, QStringLiteral("visual-cue-source-needs-translation") };
    }

    if (!visualTrackAuthoritativeAtTime && quickCueAtTime && isFinalDisplayableText(quickDisplayableText)) {
        return { quickDisplayableText, QStringLiteral("quick-baseline-outside-visual-coverage") };
    }

    return {};
}

double displayableCoverageEndSeconds(const QVector<GeneratedSubtitleCue>& cues)
{
    double coverageEndSeconds = 0.0;
    for (const GeneratedSubtitleCue& cue : cues) {
        if (isFinalDisplayableText(cue.translatedText)) {
            coverageEndSeconds = std::max(coverageEndSeconds, cue.endSeconds);
        }
    }
    return coverageEndSeconds;
}

double displayableCoverageStartSeconds(const QVector<GeneratedSubtitleCue>& cues)
{
    double coverageStartSeconds = -1.0;
    for (const GeneratedSubtitleCue& cue : cues) {
        if (isFinalDisplayableText(cue.translatedText)) {
            coverageStartSeconds = coverageStartSeconds < 0.0
                ? cue.startSeconds
                : std::min(coverageStartSeconds, cue.startSeconds);
        }
    }
    return coverageStartSeconds;
}

double continuousFinalCoverageThroughSeconds(const QVector<GeneratedSubtitleCue>& cues, double currentSeconds)
{
    QVector<GeneratedSubtitleCue> sorted = cues;
    std::sort(sorted.begin(), sorted.end(), [](const GeneratedSubtitleCue& a, const GeneratedSubtitleCue& b) {
        if (!qFuzzyCompare(a.startSeconds, b.startSeconds)) {
            return a.startSeconds < b.startSeconds;
        }
        return a.endSeconds < b.endSeconds;
    });
    double through = currentSeconds;
    for (const GeneratedSubtitleCue& cue : sorted) {
        if (cue.endSeconds < currentSeconds) {
            continue;
        }
        if (!isFinalDisplayableText(cue.translatedText)) {
            break;
        }
        through = std::max(through, cue.endSeconds);
    }
    return through;
}

bool coverageMeetsTarget(
    const QJsonObject& state,
    double currentSeconds,
    double targetCoverageSeconds)
{
    if (targetCoverageSeconds <= 0.0) {
        return true;
    }
    const double targetEndSeconds = currentSeconds + targetCoverageSeconds;
    return state.value(QStringLiteral("translatedDisplayableCoverageEndSeconds")).toDouble() + 0.5 >=
        targetEndSeconds;
}

QString semanticRepairText(const QString& quickText, bool* changed)
{
    QString text = quickText.trimmed();
    auto replaceTerm = [&](const QString& before, const QString& after) {
        if (text.contains(before)) {
            text.replace(before, after);
            if (changed) {
                *changed = true;
            }
        }
    };
    replaceTerm(QStringLiteral("\u795e\u79d8\u9b54\u4eba\u6697\u5f71"),
                QStringLiteral("\u795e\u79d8\u9b54\u5251\u58eb\u6697\u5f71"));
    replaceTerm(QStringLiteral("\u91ce\u65b9\u6c0f"), QStringLiteral("\u6211\u65b9"));
    replaceTerm(QStringLiteral("\u673a\u5173"), QStringLiteral("\u88c5\u7f6e"));
    replaceTerm(QStringLiteral("\u5f3a\u8005\u7684\u6218\u6597\u65b9\u5f0f"),
                QStringLiteral("\u5f3a\u8005\u7684\u6218\u6597\u65b9\u6cd5"));
    return text;
}

int schedulerMojibakeScore(const QString& text)
{
    int score = text.count(QChar(0xFFFD)) * 5;
    QChar previous;
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (u >= 0xE000 && u <= 0xF8FF) {
            score += 6;
        } else if ((u >= 0xFF10 && u <= 0xFF19) ||
                   (u >= 0xFF21 && u <= 0xFF3A) ||
                   (u >= 0xFF41 && u <= 0xFF5A)) {
            score += 4;
        } else if (u == 0x20AC) {
            score += 4;
        } else if (u == 0x003F) {
            const ushort p = previous.unicode();
            if ((p >= 0x4E00 && p <= 0x9FFF) || (p >= 0xE000 && p <= 0xF8FF)) {
                score += 2;
            }
        }
        switch (u) {
        case 0x50A6:
        case 0x59DD:
        case 0x6902:
        case 0x69F8:
        case 0x9286:
        case 0x934B:
        case 0x9360:
        case 0x9365:
        case 0x93B4:
        case 0x93C4:
        case 0x93C8:
        case 0x93C9:
        case 0x9470:
        case 0x9473:
        case 0x95AD:
        case 0x95AB:
            score += 3;
            break;
        default:
            break;
        }
        previous = ch;
    }
    static const QStringList markers = {
        QStringLiteral("銇"),
        QStringLiteral("銈"),
        QStringLiteral("偄"),
        QStringLiteral("儯"),
        QStringLiteral("銆"),
        QStringLiteral("€"),
        QStringLiteral("灏"),
        QStringLiteral("辫"),
        QStringLiteral("繖"),
        QStringLiteral("枫"),
        QStringLiteral("鍑"),
        QStringLiteral("鐜"),
        QStringLiteral("涓"),
        QStringLiteral("仮"),
        QStringLiteral("浠"),
        QStringLiteral("璧"),
        QStringLiteral("瀛"),
        QStringLiteral("鐢"),
        QStringLiteral("垚"),
        QStringLiteral("娴"),
        QStringLiteral("嬭"),
        QStringLiteral("瘯"),
        QStringLiteral("浣犳"),
        QStringLiteral("槸璇"),
        QStringLiteral("缃楀"),
        QStringLiteral("鍚楋"),
        QStringLiteral("鐨勮"),
        QStringLiteral("鍦哄"),
        QStringLiteral("鍙"),
        QStringLiteral("绋嬬"),
        QStringLiteral("搴嗙"),
        QStringLiteral("€?")
    };
    for (const QString& marker : markers) {
        score += text.count(marker) * 3;
    }
    return score;
}

QString repairSchedulerMojibake(const QString& text)
{
    if (text.trimmed().isEmpty() || schedulerMojibakeScore(text) < 3) {
        return text;
    }
    const QString localRepaired = QString::fromUtf8(text.toLocal8Bit()).trimmed();
    if (!localRepaired.isEmpty() &&
        schedulerMojibakeScore(localRepaired) + 2 < schedulerMojibakeScore(text)) {
        return localRepaired;
    }
    const auto encoding = QStringConverter::encodingForName("GB18030");
    if (!encoding.has_value()) {
        return text;
    }
    QStringEncoder encoder(*encoding);
    const QString repaired = QString::fromUtf8(encoder(text)).trimmed();
    if (repaired.isEmpty()) {
        return text;
    }
    QString cleaned = repaired;
    cleaned.replace(QStringLiteral("\uFFFD?"), QStringLiteral("\u3002"));
    cleaned.replace(QStringLiteral("\uFFFD"), QString());
    if (cleaned.endsWith(QLatin1Char('?')) && schedulerMojibakeScore(text) >= 3) {
        cleaned.chop(1);
        cleaned += QStringLiteral("\u3002");
    }
    return schedulerMojibakeScore(cleaned) + 2 < schedulerMojibakeScore(text) ? cleaned : text;
}

bool isCjkUnifiedIdeograph(QChar ch)
{
    const ushort code = ch.unicode();
    return code >= 0x4e00 && code <= 0x9fff;
}

QString stripLeadingIsolatedHanOcrNoise(QString text)
{
    text = text.trimmed();
    const int firstSpace = text.indexOf(QLatin1Char(' '));
    if (firstSpace != 1 || text.size() < 4 || !isCjkUnifiedIdeograph(text.at(0))) {
        return text;
    }

    const QString rest = text.mid(firstSpace + 1).trimmed();
    if (rest.isEmpty() || !isCjkUnifiedIdeograph(rest.at(0))) {
        return text;
    }

    int restHan = 0;
    for (const QChar ch : rest) {
        if (isCjkUnifiedIdeograph(ch)) {
            ++restHan;
        }
    }

    const QString allowedPrefixes = QStringLiteral(
        "\u554a\u55ef\u5443\u54e6\u5582\u662f\u4e0d\u6211\u4f60\u4ed6\u5979\u5b83"
        "\u8fd9\u90a3\u597d\u6765\u53bb\u522b\u770b\u542c\u8bf4\u8d70\u4f4f\u5feb");
    if (restHan >= 2 && !allowedPrefixes.contains(text.at(0))) {
        return rest;
    }
    return text;
}

QString normalizeOcrChineseForZhHans(QString text)
{
    text = repairSchedulerMojibake(text);
    const std::pair<QString, QString> replacements[] = {
        { QStringLiteral("\u767c\u5e03"), QStringLiteral("\u53d1\u5e03") },
        { QStringLiteral("\u767c"), QStringLiteral("\u53d1") },
        { QStringLiteral("\u56b4"), QStringLiteral("\u4e25") },
        { QStringLiteral("\u95a3\u5f71"), QStringLiteral("\u6697\u5f71") },
        { QStringLiteral("\u95c7\u5f71"), QStringLiteral("\u6697\u5f71") },
        { QStringLiteral("\u95d4\u5f71"), QStringLiteral("\u6697\u5f71") },
        { QStringLiteral("\u7a4d\u6975"), QStringLiteral("\u79ef\u6781") },
        { QStringLiteral("\u884c\u52d5"), QStringLiteral("\u884c\u52a8") },
        { QStringLiteral("\u6211\u5011"), QStringLiteral("\u6211\u4eec") },
        { QStringLiteral("\u4ed6\u5011"), QStringLiteral("\u4ed6\u4eec") },
        { QStringLiteral("\u4f60\u5011"), QStringLiteral("\u4f60\u4eec") },
        { QStringLiteral("\u5011"), QStringLiteral("\u4eec") },
        { QStringLiteral("\u59b3\u5011"), QStringLiteral("\u4f60\u4eec") },
        { QStringLiteral("\u59b3"), QStringLiteral("\u4f60") },
        { QStringLiteral("\u5f8c"), QStringLiteral("\u540e") },
        { QStringLiteral("\u9019"), QStringLiteral("\u8fd9") },
        { QStringLiteral("\u6703"), QStringLiteral("\u4f1a") },
        { QStringLiteral("\u904a"), QStringLiteral("\u6e38") },
        { QStringLiteral("\u7d50"), QStringLiteral("\u7ed3") },
        { QStringLiteral("\u6216\u8a31"), QStringLiteral("\u6216\u8bb8") },
        { QStringLiteral("\u653e\u68c4"), QStringLiteral("\u653e\u5f03") },
        { QStringLiteral("\u689d"), QStringLiteral("\u6761") },
        { QStringLiteral("\u55aa"), QStringLiteral("\u4e27") },
        { QStringLiteral("\u8f29"), QStringLiteral("\u8f88") },
        { QStringLiteral("\u9084"), QStringLiteral("\u8fd8") },
        { QStringLiteral("\u9078\u64c7"), QStringLiteral("\u9009\u62e9") },
        { QStringLiteral("\u982d"), QStringLiteral("\u5934") },
        { QStringLiteral("\u8b93"), QStringLiteral("\u8ba9") },
        { QStringLiteral("\u77e5\u66c9"), QStringLiteral("\u77e5\u6653") },
        { QStringLiteral("\u5c0d"), QStringLiteral("\u5bf9") },
        { QStringLiteral("\u89aa"), QStringLiteral("\u4eb2") },
        { QStringLiteral("\u8072"), QStringLiteral("\u58f0") },
        { QStringLiteral("\u8056"), QStringLiteral("\u5723") },
        { QStringLiteral("\u8ab0"), QStringLiteral("\u8c01") },
        { QStringLiteral("\u6b64\u6a23\u8c8c"), QStringLiteral("\u6b64\u6837\u8c8c") },
        { QStringLiteral("\u6a23"), QStringLiteral("\u6837") },
        { QStringLiteral("\u8c8c"), QStringLiteral("\u8c8c") },
        { QStringLiteral("\u7ad9\u4f4f"), QStringLiteral("\u7ad9\u4f4f") },
        { QStringLiteral("\u5225"), QStringLiteral("\u522b") },
        { QStringLiteral("\u59a8\u7919"), QStringLiteral("\u59a8\u788d") },
        { QStringLiteral("\u5ead\u5712"), QStringLiteral("\u5ead\u56ed") },
        { QStringLiteral("\u8aaa"), QStringLiteral("\u8bf4") },
        { QStringLiteral("\u543e\u7b49\u4e43"), QStringLiteral("\u543e\u7b49\u4e43") },
        { QStringLiteral("\u5f97\u8d95\u5feb"), QStringLiteral("\u5f97\u8d76\u5feb") },
        { QStringLiteral("\u8655\u7406"), QStringLiteral("\u5904\u7406") },
        { QStringLiteral("\u96dc\u4e8b"), QStringLiteral("\u6742\u4e8b") },
        { QStringLiteral("\u7121\u6cd5"), QStringLiteral("\u65e0\u6cd5") },
        { QStringLiteral("\u90fd\u5e02"), QStringLiteral("\u90fd\u5e02") },
        { QStringLiteral("\u8abf\u67e5"), QStringLiteral("\u8c03\u67e5") },
        { QStringLiteral("\u81e8\u6642"), QStringLiteral("\u4e34\u65f6") },
        { QStringLiteral("\u6821\u820d"), QStringLiteral("\u6821\u820d") },
        { QStringLiteral("\u91cd\u5efa"), QStringLiteral("\u91cd\u5efa") },
        { QStringLiteral("\u5de5\u7a0b"), QStringLiteral("\u5de5\u7a0b") },
        { QStringLiteral("\u4e09\u8d8a"), QStringLiteral("\u4e09\u8d8a") },
        { QStringLiteral("\u8cc7\u91d1"), QStringLiteral("\u8d44\u91d1") },
        { QStringLiteral("\u5236\u670d"), QStringLiteral("\u5236\u670d") },
        { QStringLiteral("\u4e00\u8d77"), QStringLiteral("\u4e00\u8d77") },
        { QStringLiteral("\u66f4\u65b0"), QStringLiteral("\u66f4\u65b0") },
        { QStringLiteral("\u807d\u8aaa"), QStringLiteral("\u542c\u8bf4") },
        { QStringLiteral("\u5df2\u7d93"), QStringLiteral("\u5df2\u7ecf") },
        { QStringLiteral("\u5df2\u7d93\u9806\u5229"), QStringLiteral("\u5df2\u7ecf\u987a\u5229") },
        { QStringLiteral("\u4f86"), QStringLiteral("\u6765") },
        { QStringLiteral("\u70ba"), QStringLiteral("\u4e3a") },
        { QStringLiteral("\u9ebc"), QStringLiteral("\u4e48") },
        { QStringLiteral("\u6232"), QStringLiteral("\u620f") },
        { QStringLiteral("\u5834"), QStringLiteral("\u573a") },
        { QStringLiteral("\u6b63\u8981"), QStringLiteral("\u6b63\u8981") },
        { QStringLiteral("\u55da"), QStringLiteral("\u545c") },
        { QStringLiteral("\u54c7"), QStringLiteral("\u54c7") },
        { QStringLiteral("\u5b8c\u5168"), QStringLiteral("\u5b8c\u5168") },
        { QStringLiteral("\u4e00\u5207"), QStringLiteral("\u4e00\u5207") },
        { QStringLiteral("\u90fd\u662f"), QStringLiteral("\u90fd\u662f") },
        { QStringLiteral("\u8cd3"), QStringLiteral("\u5bbe") },
        { QStringLiteral("\u907f\u96e3"), QStringLiteral("\u907f\u96be") },
        { QStringLiteral("\u5b8c\u7562"), QStringLiteral("\u5b8c\u6bd5") },
        { QStringLiteral("\u9806\u5229"), QStringLiteral("\u987a\u5229") },
        { QStringLiteral("\u8cbf\u7136"), QStringLiteral("\u8d38\u7136") },
        { QStringLiteral("\u8e0f\u5165"), QStringLiteral("\u8e0f\u5165") },
        { QStringLiteral("\u9ed1\u6697"), QStringLiteral("\u9ed1\u6697") },
        { QStringLiteral("\u53d7\u5230\u6ce2\u53ca"), QStringLiteral("\u53d7\u5230\u6ce2\u53ca") },
        { QStringLiteral("\u642d\u8457"), QStringLiteral("\u642d\u7740") },
        { QStringLiteral("\u99ac\u8eca"), QStringLiteral("\u9a6c\u8f66") },
        { QStringLiteral("\u90ca\u5916"), QStringLiteral("\u90ca\u5916") },
        { QStringLiteral("\u5b78\u671f"), QStringLiteral("\u5b66\u671f") },
        { QStringLiteral("\u738b\u90fd"), QStringLiteral("\u738b\u90fd") },
        { QStringLiteral("\u6642\u5019"), QStringLiteral("\u65f6\u5019") }
    };
    for (const auto& replacement : replacements) {
        text.replace(replacement.first, replacement.second);
    }
    return stripLeadingIsolatedHanOcrNoise(text);
}

QString detectSubtitleLanguageForDiagnostics(const QString& text)
{
    int latin = 0;
    int cjk = 0;
    int kana = 0;
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')) {
            ++latin;
        } else if ((u >= 0x4E00 && u <= 0x9FFF) || (u >= 0x3400 && u <= 0x4DBF)) {
            ++cjk;
        } else if ((u >= 0x3040 && u <= 0x30FF) || (u >= 0x31F0 && u <= 0x31FF)) {
            ++kana;
        }
    }
    if (latin >= std::max(3, cjk + kana)) {
        return QStringLiteral("en");
    }
    if (kana > 0 && kana >= cjk) {
        return QStringLiteral("ja");
    }
    if (cjk > 0) {
        return QStringLiteral("zh");
    }
    return QStringLiteral("unknown");
}

QJsonObject cacheState(const QString& path, const QString& quickPath, double currentSeconds)
{
    const QFileInfo info(path);
    const QFileInfo quickInfo(quickPath);
    QJsonObject mediaIdentity;
    const QString mediaIdentityPath = SubtitleGenerationService::mediaIdentitySidecarPath(path);
    QFile mediaIdentityFile(mediaIdentityPath);
    if (mediaIdentityFile.exists() &&
        mediaIdentityFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        mediaIdentity = QJsonDocument::fromJson(mediaIdentityFile.readAll()).object();
    }
    const QVector<GeneratedSubtitleCue> cues =
        info.exists() ? SubtitleGenerationService::readSubtitleFile(path) : QVector<GeneratedSubtitleCue>{};
    bool stale = false;
    if (info.exists() && quickInfo.exists()) {
        stale = info.lastModified().msecsTo(quickInfo.lastModified()) > 2000;
    }
    const double coverageStartSeconds = displayableCoverageStartSeconds(cues);
    const double coverageEndSeconds = displayableCoverageEndSeconds(cues);
    const double actualFinalCoverageThroughSeconds =
        continuousFinalCoverageThroughSeconds(cues, currentSeconds);
    const double progressOverreportedSeconds =
        std::max(0.0, coverageEndSeconds - actualFinalCoverageThroughSeconds);
    return QJsonObject{
        { QStringLiteral("path"), path },
        { QStringLiteral("exists"), info.exists() },
        { QStringLiteral("cueCount"), cues.size() },
        { QStringLiteral("displayableCueCount"), std::count_if(cues.cbegin(), cues.cend(), [](const GeneratedSubtitleCue& cue) {
              return isFinalDisplayableText(cue.translatedText);
          }) },
        { QStringLiteral("translatedDisplayableCoverageStartSeconds"), coverageStartSeconds },
        { QStringLiteral("translatedDisplayableCoverageEndSeconds"), coverageEndSeconds },
        { QStringLiteral("actualFinalCoverageThroughSeconds"), actualFinalCoverageThroughSeconds },
        { QStringLiteral("progressOverreportedSeconds"), progressOverreportedSeconds },
        { QStringLiteral("progressTruthPass"), progressOverreportedSeconds <= 0.25 },
        { QStringLiteral("staleAgainstQuick"), stale },
        { QStringLiteral("cacheMediaIdentityPath"), mediaIdentityPath },
        { QStringLiteral("cacheMediaIdentityVerified"), !mediaIdentity.isEmpty() },
        { QStringLiteral("mediaFingerprint"), mediaIdentity.value(QStringLiteral("mediaFingerprint")).toString() },
        { QStringLiteral("cacheMediaIdentity"), mediaIdentity },
        { QStringLiteral("lastModifiedUtc"), info.exists() ? info.lastModified().toUTC().toString(Qt::ISODateWithMs) : QString() }
    };
}

void writeCacheMediaIdentityIfExists(const QStringList& paths, const QString& mediaPath)
{
    for (const QString& path : paths) {
        if (path.trimmed().isEmpty() || !QFileInfo::exists(path)) {
            continue;
        }
        QString error;
        if (!SubtitleGenerationService::writeMediaIdentitySidecar(path, mediaPath, 0.0, &error)) {
            qWarning() << "[TranslationEnhancement] Failed to write cache media identity"
                       << path << error;
        }
    }
}

QString highQualityNeedReason(
    const TranslationEnhancementScheduleRequest& request,
    const QJsonObject& quickState,
    const QJsonObject& enhancedState)
{
    if (request.mediaPath.trimmed().isEmpty()) {
        return QStringLiteral("no-readable-source");
    }
    if (!quickState.value(QStringLiteral("exists")).toBool() ||
        quickState.value(QStringLiteral("displayableCueCount")).toInt() <= 0) {
        return QStringLiteral("quick-baseline-missing");
    }
    if (!enhancedState.value(QStringLiteral("exists")).toBool()) {
        return QStringLiteral("enhanced-missing");
    }
    if (enhancedState.value(QStringLiteral("staleAgainstQuick")).toBool()) {
        return QStringLiteral("enhanced-stale");
    }
    if (enhancedState.value(QStringLiteral("displayableCueCount")).toInt() <= 0) {
        return QStringLiteral("enhanced-empty");
    }
    if (!coverageMeetsTarget(
            enhancedState,
            request.currentSeconds,
            request.highQualityTargetCoverageSeconds)) {
        return QStringLiteral("enhanced-below-target-coverage");
    }
    return QStringLiteral("enhanced-cache-ready");
}

QJsonObject videoClassEntry(
    const QString& id,
    const QString& state,
    const QString& reason,
    const QString& activeSource,
    bool measured)
{
    return QJsonObject{
        { QStringLiteral("videoClass"), id },
        { QStringLiteral("state"), state },
        { QStringLiteral("measured"), measured },
        { QStringLiteral("proven80Percent"), false },
        { QStringLiteral("activeSource"), activeSource },
        { QStringLiteral("reason"), reason }
    };
}

} // namespace cgplay::translation_enhancement_support
