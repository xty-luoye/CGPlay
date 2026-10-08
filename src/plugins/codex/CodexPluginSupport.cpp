#include "CodexPlugin.h"
#include "CodexPluginSupport.h"

#include "common/jobs/JobSystem.h"
#include "common/core/ServiceLocator.h"
#include "ai/api/IAICredentialStore.h"
#include "ai/api/IAIProviderDetector.h"
#include "settings/api/ISettingsService.h"
#include "playback/api/IPlaybackService.h"
#include "annotation/api/IAnnotationService.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFile>
#include <QFutureWatcher>
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QImageReader>
#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QMessageBox>
#include <QEventLoop>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QHttpMultiPart>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QThread>
#include <QWidget>
#include <QUrl>
#include <QUrlQuery>

#include <utility>
#include <QUuid>
#include <QVersionNumber>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#endif

#include <QtConcurrent>

#include <algorithm>

namespace cgplay {
namespace codex_plugin_detail {

JobOutcome runSynchronousJob(
    const QString& id,
    QObject* owner,
    int timeoutMs,
    JobRunner::Worker worker)
{
    JobOutcome outcome = JobOutcome::canceled(QStringLiteral("Job owner was destroyed."));
    bool completed = false;
    QEventLoop loop;
    QPointer<JobHandle> handle(JobRunner::start(id, owner, timeoutMs, std::move(worker)));
    QObject::connect(handle, &JobHandle::finished, &loop, [&](const JobOutcome& value) {
        outcome = value;
        completed = true;
        loop.quit();
    });
    if (owner) {
        QObject::connect(owner, &QObject::destroyed, &loop, [&] {
            if (handle) handle->cancel();
            loop.quit();
        });
    }
    if (!completed) loop.exec();
    if (handle) handle->deleteLater();
    return outcome;
}

JobOutcome runProcessJob(
    QObject* owner,
    const QString& id,
    const QString& program,
    const QStringList& arguments,
    const QString& workingDirectory,
    int timeoutMs,
    const QByteArray& standardInput)
{
    return runSynchronousJob(id, owner, timeoutMs,
        [program, arguments, workingDirectory, standardInput](JobContext& context) {
            QProcess process;
            if (!workingDirectory.isEmpty()) process.setWorkingDirectory(workingDirectory);
            process.setProcessChannelMode(QProcess::SeparateChannels);
            bool observedStarted = false;
            QObject::connect(&process, &QProcess::started, &process, [&observedStarted] {
                observedStarted = true;
            }, Qt::DirectConnection);
            process.start(program, arguments);

            QElapsedTimer startElapsed;
            startElapsed.start();
            bool started = observedStarted || process.state() == QProcess::Running;
            while (!started && process.state() == QProcess::Starting &&
                   startElapsed.elapsed() < 5000 && !context.shouldStop()) {
                started = process.waitForStarted(25) || observedStarted;
            }
            if (!started && process.state() != QProcess::Running) {
                QVariantMap details;
                details.insert(QStringLiteral("stderr"), process.readAllStandardError());
                details.insert(QStringLiteral("processError"), process.errorString());
                JobOutcome failed = context.hasTimedOut()
                    ? JobOutcome::timedOut(QStringLiteral("Process timed out while starting."))
                    : (context.isCancellationRequested()
                        ? JobOutcome::canceled(QStringLiteral("Process start was canceled."))
                        : JobOutcome::failure(QStringLiteral("process_start_failed"), process.errorString()));
                failed.value = details;
                return failed;
            }
            if (!standardInput.isEmpty()) {
                process.write(standardInput);
                process.closeWriteChannel();
            }

            context.reportProgress(5, QStringLiteral("Process started"));
            const ProcessOutcome processResult = context.waitForProcess(process, 25);
            QVariantMap details;
            details.insert(QStringLiteral("exitCode"), processResult.exitCode);
            details.insert(QStringLiteral("stdout"), processResult.standardOutput);
            details.insert(QStringLiteral("stderr"), processResult.standardError);
            JobOutcome outcome;
            outcome.state = processResult.state;
            outcome.value = details;
            if (processResult.state == JobState::TimedOut) {
                outcome.errorCode = QStringLiteral("timeout");
                outcome.errorMessage = QStringLiteral("Process timed out.");
            } else if (processResult.state == JobState::Canceled) {
                outcome.errorCode = QStringLiteral("canceled");
                outcome.errorMessage = QStringLiteral("Process was canceled.");
            } else if (!processResult.succeeded()) {
                outcome.errorCode = QStringLiteral("process_failed");
                outcome.errorMessage = QStringLiteral("Process exited with code %1.").arg(processResult.exitCode);
            } else {
                context.reportProgress(100, QStringLiteral("Process completed"));
            }
            return outcome;
        });
}

NetworkJobResult waitForNetworkJob(
    QNetworkReply* reply,
    JobContext& context,
    int operationTimeoutMs)
{
    NetworkJobResult result;
    if (!reply) {
        result.errorString = QStringLiteral("Network request was not created.");
        return result;
    }

    QElapsedTimer operationElapsed;
    operationElapsed.start();
    QEventLoop loop;
    QTimer watchdog;
    watchdog.setInterval(25);
    QObject::connect(&watchdog, &QTimer::timeout, reply, [&] {
        const bool operationTimedOut = operationTimeoutMs > 0 &&
            operationElapsed.elapsed() >= operationTimeoutMs;
        if ((context.shouldStop() || operationTimedOut) && !reply->isFinished()) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    watchdog.start();
    if (!reply->isFinished()) loop.exec();
    watchdog.stop();

    const bool operationTimedOut = operationTimeoutMs > 0 &&
        operationElapsed.elapsed() >= operationTimeoutMs;
    result.state = context.isCancellationRequested()
        ? JobState::Canceled
        : ((context.hasTimedOut() || operationTimedOut)
            ? JobState::TimedOut
            : (reply->error() == QNetworkReply::NoError ? JobState::Succeeded : JobState::Failed));
    result.body = reply->readAll();
    result.networkError = reply->error();
    result.errorString = reply->errorString();
    result.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
    result.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    return result;
}

bool waitForJobDelay(JobContext& context, int delayMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    QEventLoop loop;
    QTimer timer;
    timer.setInterval(25);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if (context.shouldStop() || elapsed.elapsed() >= delayMs) loop.quit();
    });
    timer.start();
    loop.exec();
    return !context.shouldStop();
}

QString jobError(const JobOutcome& outcome, const QString& fallback)
{
    if (!outcome.errorMessage.trimmed().isEmpty()) return outcome.errorMessage;
    const QVariantMap details = outcome.value.toMap();
    const QString standardError = QString::fromLocal8Bit(details.value(QStringLiteral("stderr")).toByteArray()).trimmed();
    const QString processError = details.value(QStringLiteral("processError")).toString().trimmed();
    if (!standardError.isEmpty()) return standardError;
    if (!processError.isEmpty()) return processError;
    return fallback;
}

void appendMediaAssetRecord(const QString& kind, const QString& path, const QJsonObject& extra)
{
    if (path.trimmed().isEmpty()) return;
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (directory.isEmpty()) return;
    QDir().mkpath(directory);
    QJsonObject record{{QStringLiteral("kind"), kind}, {QStringLiteral("path"), path}, {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) record.insert(it.key(), it.value());
    QFile file(QDir(directory).filePath(QStringLiteral("codex_media_assets.jsonl")));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        file.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
        file.write("\n");
    }
}

QJsonObject loadLocalThreadHistory(const QString& threadId)
{
    if (threadId.trimmed().isEmpty()) return {};
    const QString home = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                             .filePath(QStringLiteral("codex-home"));
    QDirIterator files(home, {QStringLiteral("*.jsonl")}, QDir::Files, QDirIterator::Subdirectories);
    QHash<QString, QList<QStringList>> submittedImagesByPrompt;
    QHash<QString, int> consumedImagesByPrompt;
    const QString assetPath = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                  .filePath(QStringLiteral("codex_media_assets.jsonl"));
    QFile assets(assetPath);
    if (assets.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!assets.atEnd()) {
            QJsonParseError assetError;
            const QJsonDocument assetDocument = QJsonDocument::fromJson(assets.readLine(), &assetError);
            if (assetError.error != QJsonParseError::NoError || !assetDocument.isObject()) continue;
            const QJsonObject asset = assetDocument.object();
            if (asset.value(QStringLiteral("kind")).toString() != QStringLiteral("chat-image-input") ||
                asset.value(QStringLiteral("threadId")).toString() != threadId) continue;
            QStringList paths;
            for (const QJsonValue& value : asset.value(QStringLiteral("paths")).toArray()) {
                if (QFileInfo(value.toString()).isFile()) paths.push_back(value.toString());
            }
            const QString source = asset.value(QStringLiteral("source")).toString();
            const QString generatedImagesRoot = QDir::fromNativeSeparators(
                QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                    .filePath(QStringLiteral("generated_images")));
            const QString firstImagePath = paths.isEmpty()
                ? QString()
                : QDir::fromNativeSeparators(QFileInfo(paths.first()).absoluteFilePath());
            if (source.isEmpty() && !paths.isEmpty() &&
                firstImagePath.startsWith(generatedImagesRoot + QLatin1Char('/'), Qt::CaseInsensitive)) {
                continue;
            }
            if (!paths.isEmpty()) submittedImagesByPrompt[asset.value(QStringLiteral("prompt")).toString()].push_back(paths);
        }
    }
    while (files.hasNext()) {
        const QString path = files.next();
        if (!QFileInfo(path).fileName().contains(threadId, Qt::CaseInsensitive)) continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

        QJsonArray turns;
        QJsonArray items;
        QSet<QString> imagePaths;
        QSet<QString> filePaths;
        const auto appendImagesFromText = [&items, &imagePaths](const QString& text, const QString& id) {
            static const QRegularExpression imagePathPattern(
                QStringLiteral("([A-Za-z]:[\\\\/][^\\r\\n\\\"<>]+?\\.(?:png|jpe?g|webp))"),
                QRegularExpression::CaseInsensitiveOption);
            QRegularExpressionMatchIterator matches = imagePathPattern.globalMatch(text);
            while (matches.hasNext()) {
                const QString savedPath = QDir::fromNativeSeparators(matches.next().captured(1));
                if (imagePaths.contains(savedPath) || !QFileInfo::exists(savedPath)) continue;
                imagePaths.insert(savedPath);
                items.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("imageGeneration")},
                                           {QStringLiteral("id"), id},
                                           {QStringLiteral("status"), QStringLiteral("completed")},
                                           {QStringLiteral("savedPath"), savedPath}});
            }
        };
        const auto appendFilesFromText = [&items, &filePaths](const QString& text, const QString& id) {
            static const QRegularExpression filePathPattern(
                QStringLiteral("([A-Za-z]:[\\/][^\\r\\n`\"<>|]+?\\.[A-Za-z0-9]{1,12})"));
            QRegularExpressionMatchIterator matches = filePathPattern.globalMatch(text);
            while (matches.hasNext()) {
                const QString path = QDir::fromNativeSeparators(matches.next().captured(1).trimmed());
                const QFileInfo file(path);
                if (!file.isFile() || filePaths.contains(file.absoluteFilePath()) ||
                    QImageReader::supportedImageFormats().contains(file.suffix().toLower().toLatin1())) continue;
                filePaths.insert(file.absoluteFilePath());
                items.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("fileArtifact")},
                                            {QStringLiteral("id"), id},
                                            {QStringLiteral("path"), file.absoluteFilePath()}});
            }
        };
        while (!file.atEnd()) {
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(file.readLine(), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) continue;
            const QJsonObject record = document.object();
            if (record.value(QStringLiteral("type")).toString() == QStringLiteral("event_msg")) {
                const QJsonObject payload = record.value(QStringLiteral("payload")).toObject();
                const QString eventType = payload.value(QStringLiteral("type")).toString();
                QString text;
                QString itemType;
                QJsonArray localImages;
                if (eventType == QStringLiteral("user_message")) {
                    text = payload.value(QStringLiteral("message")).toString();
                    const int policy = text.indexOf(QStringLiteral("\n[CGPlay host policy"));
                    if (policy >= 0) text.truncate(policy);
                    text.remove(QRegularExpression(QStringLiteral("\\s*<image\\s+[^>]*>\\s*</image>"),
                                                    QRegularExpression::CaseInsensitiveOption));
                    text.remove(QRegularExpression(QStringLiteral("^\\s*file:///[^\\r\\n]+\\s*$"),
                                                    QRegularExpression::CaseInsensitiveOption));
                    for (const QJsonValue& value : payload.value(QStringLiteral("local_images")).toArray()) {
                        const QString imagePath = QDir::fromNativeSeparators(value.toString());
                        if (QFileInfo(imagePath).isFile()) localImages.push_back(imagePath);
                    }
                    const QString promptKey = text.trimmed();
                    const QList<QStringList> savedSubmissions = submittedImagesByPrompt.value(promptKey);
                    const int submissionIndex = consumedImagesByPrompt.value(promptKey);
                    if (submissionIndex < savedSubmissions.size()) {
                        localImages = QJsonArray::fromStringList(savedSubmissions.at(submissionIndex));
                        consumedImagesByPrompt.insert(promptKey, submissionIndex + 1);
                    } else if (promptKey != QStringLiteral("Inspect these images.")) {
                        // Codex repeats context local_images on later turns. Without a
                        // matching host submission record they are not new attachments.
                        localImages = {};
                    }
                    itemType = QStringLiteral("userMessage");
                } else if (eventType == QStringLiteral("agent_message")) {
                    text = payload.value(QStringLiteral("message")).toString();
                    itemType = QStringLiteral("agentMessage");
                }
                if (!text.trimmed().isEmpty() || !localImages.isEmpty()) {
                    if (itemType == QStringLiteral("userMessage") && !items.isEmpty()) {
                        turns.push_back(QJsonObject{{QStringLiteral("items"), items}});
                        items = {};
                    }
                    QJsonObject item{{QStringLiteral("type"), itemType},
                                     {QStringLiteral("text"), text.trimmed()}};
                    if (!localImages.isEmpty()) item.insert(QStringLiteral("localImages"), localImages);
                    items.push_back(item);
                    if (itemType == QStringLiteral("agentMessage")) {
                        appendFilesFromText(text, QStringLiteral("history-file"));
                        turns.push_back(QJsonObject{{QStringLiteral("items"), items}});
                        items = {};
                    }
                }
                continue;
            }
            if (record.value(QStringLiteral("type")).toString() != QStringLiteral("response_item")) continue;
            const QJsonObject payload = record.value(QStringLiteral("payload")).toObject();
            const QString payloadType = payload.value(QStringLiteral("type")).toString();
            if (payloadType == QStringLiteral("function_call_output") || payloadType == QStringLiteral("custom_tool_call_output")) {
                const QJsonValue output = payload.value(QStringLiteral("output"));
                QString outputText;
                if (output.isString()) outputText = output.toString();
                else if (output.isArray()) outputText = QString::fromUtf8(QJsonDocument(output.toArray()).toJson(QJsonDocument::Compact));
                else if (output.isObject()) outputText = QString::fromUtf8(QJsonDocument(output.toObject()).toJson(QJsonDocument::Compact));
                appendImagesFromText(outputText, payload.value(QStringLiteral("call_id")).toString());
            }
        }
        if (!items.isEmpty()) turns.push_back(QJsonObject{{QStringLiteral("items"), items}});
        if (!turns.isEmpty()) return QJsonObject{{QStringLiteral("id"), threadId}, {QStringLiteral("turns"), turns}};
    }
    return {};
}

QStringList searchLocalThreadHistory(const QString& query)
{
    const QString needle = query.trimmed();
    if (needle.isEmpty()) return {};
    const QString home = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex-home"));
    QDirIterator files(home, {QStringLiteral("*.jsonl")}, QDir::Files, QDirIterator::Subdirectories);
    QStringList matches;
    while (files.hasNext()) {
        const QString path = files.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        int line = 0;
        while (!file.atEnd()) {
            ++line;
            const QString text = QString::fromUtf8(file.readLine());
            if (!text.contains(needle, Qt::CaseInsensitive)) continue;
            const QString excerpt = text.simplified().left(360);
            matches << QStringLiteral("%1:%2  %3").arg(QFileInfo(path).baseName()).arg(line).arg(excerpt);
            if (matches.size() >= 100) return matches;
        }
    }
    return matches;
}

QString tomlString(QString value)
{
    value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    value.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    value.replace(QStringLiteral("\r"), QStringLiteral("\\r"));
    value.replace(QStringLiteral("\n"), QStringLiteral("\\n"));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

bool hasSafeIdentifier(const QString& value)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z][A-Za-z0-9_-]*$"));
    return expression.match(value).hasMatch();
}

bool copyDirectoryTree(const QString& sourcePath, const QString& targetPath, QString* error)
{
    const QDir source(sourcePath);
    if (!source.exists() || !QDir().mkpath(targetPath)) { if (error) *error = QStringLiteral("Cannot create target directory."); return false; }
    QDirIterator iterator(sourcePath, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString sourceItem = iterator.next();
        const QString relative = source.relativeFilePath(sourceItem);
        const QString targetItem = QDir(targetPath).filePath(relative);
        if (QFileInfo(sourceItem).isDir()) { if (!QDir().mkpath(targetItem)) { if (error) *error = QStringLiteral("Cannot create %1").arg(targetItem); return false; } }
        else { QDir().mkpath(QFileInfo(targetItem).absolutePath()); QFile::remove(targetItem); if (!QFile::copy(sourceItem, targetItem)) { if (error) *error = QStringLiteral("Cannot copy %1").arg(relative); return false; } }
    }
    return true;
}

bool isSensitiveCredentialQueryKey(const QString& value)
{
    const QString key = value.trimmed().toLower();
    return key == QStringLiteral("key") ||
        key == QStringLiteral("api_key") ||
        key == QStringLiteral("api-key") ||
        key == QStringLiteral("apikey") ||
        key == QStringLiteral("x-api-key") ||
        key == QStringLiteral("x_api_key") ||
        key == QStringLiteral("x-goog-api-key") ||
        key == QStringLiteral("x_goog_api_key") ||
        key == QStringLiteral("subscription-key") ||
        key == QStringLiteral("subscription_key") ||
        key == QStringLiteral("access_token") ||
        key == QStringLiteral("access-token") ||
        key == QStringLiteral("token") ||
        key == QStringLiteral("authorization") ||
        key == QStringLiteral("auth");
}

bool hasSensitiveCredentialQuery(const QUrl& url)
{
    const QUrlQuery query(url);
    for (const auto& item : query.queryItems(QUrl::FullyDecoded)) {
        if (isSensitiveCredentialQueryKey(item.first)) return true;
    }
    return false;
}

QByteArray loadImageCredentialFallback()
{
#ifdef Q_OS_WIN
    QByteArray encoded;
    const QStringList roots{
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)};
    const QRegularExpression match(QStringLiteral("(?:^|\\n)cgplay\\.ai\\.image-generation=([^\\r\\n]+)"));
    for (const QString& root : roots) {
        QFile file(QDir(root).filePath(QStringLiteral("ai_credentials.ini")));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const auto found = match.match(QString::fromUtf8(file.readAll()));
        if (!found.hasMatch()) continue;
        encoded = QByteArray::fromBase64(found.captured(1).trimmed().toLatin1());
        if (!encoded.isEmpty()) break;
    }
    QByteArray entropyBytes("CGPlay.AI.Credential.v1");
    DATA_BLOB input{static_cast<DWORD>(encoded.size()), reinterpret_cast<BYTE*>(const_cast<char*>(encoded.constData()))};
    DATA_BLOB entropy{static_cast<DWORD>(entropyBytes.size()), reinterpret_cast<BYTE*>(entropyBytes.data())};
    DATA_BLOB output{};
    if (encoded.isEmpty() || !CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return {};
    QByteArray result(reinterpret_cast<const char*>(output.pbData), static_cast<int>(output.cbData));
    LocalFree(output.pbData);
    return result;
#else
    return {};
#endif
}

bool isAzureProviderUrl(const QUrl& url)
{
    const QString host = url.host().toLower();
    const QString path = url.path().toLower();
    return host.contains(QStringLiteral("openai.azure.")) ||
        host.contains(QStringLiteral("cognitiveservices.azure.")) ||
        host.contains(QStringLiteral("aoai.azure.")) ||
        host.contains(QStringLiteral("azure-api.")) ||
        host.contains(QStringLiteral("azurefd.")) ||
        path.contains(QStringLiteral("/openai/deployments/"));
}

QString responsesBaseUrl(QString endpoint)
{
    QUrl url(endpoint.trimmed());
    if (!url.isValid()) return {};
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    if (path.endsWith(QStringLiteral("/responses"), Qt::CaseInsensitive)) {
        path.chop(QStringLiteral("/responses").size());
    }
    url.setPath(path);
    url.setFragment(QString());
    return url.toString(QUrl::FullyEncoded);
}

QString tomlInlineStringTable(const QList<QPair<QString, QString>>& items)
{
    QStringList fields;
    for (const auto& item : items) {
        fields.push_back(QStringLiteral("%1 = %2").arg(tomlString(item.first), tomlString(item.second)));
    }
    return QStringLiteral("{ %1 }").arg(fields.join(QStringLiteral(", ")));
}

QString displayProtocolState(const QString& state)
{
    if (state == QStringLiteral("disabled")) return QObject::tr("已停用");
    if (state == QStringLiteral("initializing")) return QObject::tr("正在初始化");
    if (state == QStringLiteral("startingThread")) return QObject::tr("正在创建会话");
    if (state == QStringLiteral("ready")) return QObject::tr("就绪");
    if (state == QStringLiteral("turnInProgress")) return QObject::tr("正在处理");
    if (state == QStringLiteral("stopping")) return QObject::tr("正在停止");
    if (state == QStringLiteral("failed")) return QObject::tr("失败");
    return state;
}

QString displayTurnStatus(const QString& status)
{
    if (status == QStringLiteral("completed")) return QObject::tr("已完成");
    if (status == QStringLiteral("failed")) return QObject::tr("失败");
    if (status == QStringLiteral("interrupted")) return QObject::tr("已中断");
    if (status == QStringLiteral("inProgress")) return QObject::tr("进行中");
    return status;
}

QJsonObject toolTextResult(bool success, const QJsonObject& payload)
{
    return {
        {QStringLiteral("success"), success},
        {QStringLiteral("contentItems"), QJsonArray{QJsonObject{
            {QStringLiteral("type"), QStringLiteral("inputText")},
            {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))}
        }}}
    };
}

QJsonObject toolError(const QString& code, const QString& message)
{
    return toolTextResult(false, QJsonObject{
        {QStringLiteral("ok"), false},
        {QStringLiteral("error"), QJsonObject{
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), message}
        }}
    });
}

QString firstNonEmptySetting(ISettingsService* settings, std::initializer_list<const char*> keys)
{
    if (!settings) return {};
    for (const char* key : keys) {
        const QString value = settings->value(QString::fromLatin1(key)).toString().trimmed();
        if (!value.isEmpty()) return value;
    }
    return {};
}

QString workbenchMediaEndpoint(ISettingsService* settings, const QString& kind)
{
    const QString base = firstNonEmptySetting(settings, {"ai/connection/baseUrl", "ai/providers/openai/baseUrl"});
    QUrl url(base);
    if (!url.isValid() || url.isRelative() || url.scheme() != QStringLiteral("https") || url.host().isEmpty()) return {};
    QString path = url.path();
    if (!path.endsWith(QStringLiteral("/v1"))) path = path.endsWith(QLatin1Char('/')) ? path + QStringLiteral("v1") : path + QStringLiteral("/v1");
    const QString suffix = kind == QStringLiteral("video") ? QStringLiteral("/videos")
        : (kind == QStringLiteral("audio") ? QStringLiteral("/audio/generations") : QStringLiteral("/images/edits"));
    url.setPath(path + suffix);
    url.setQuery(QString());
    url.setFragment(QString());
    return url.toString(QUrl::FullyEncoded);
}

QString workbenchMediaModel(ISettingsService* settings)
{
    return firstNonEmptySetting(settings, {"ai/workspace/model", "ai/connection/model", "ai/connection/recommendedModel"});
}

void insertExecutable(QJsonObject* executables, const QString& name, const QString& path)
{
    if (!executables || executables->contains(name)) return;
    const QFileInfo info(path);
    if (info.isFile()) executables->insert(name, info.absoluteFilePath());
}

QList<QFileInfo> versionedDirectories(const QString& root, const QStringList& filters)
{
    QList<QFileInfo> directories = QDir(root).entryInfoList(filters, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    std::sort(directories.begin(), directories.end(), [](const QFileInfo& lhs, const QFileInfo& rhs) {
        const QRegularExpression versionPattern(QStringLiteral("(\\d+(?:\\.\\d+)+)"));
        const QVersionNumber left = QVersionNumber::fromString(versionPattern.match(lhs.fileName()).captured(1));
        const QVersionNumber right = QVersionNumber::fromString(versionPattern.match(rhs.fileName()).captured(1));
        return QVersionNumber::compare(left, right) > 0;
    });
    return directories;
}

QJsonObject discoverSoftwareExecutables()
{
    QJsonObject found;
    const QStringList names{
        QStringLiteral("hython"), QStringLiteral("hbatch"), QStringLiteral("houdini"),
        QStringLiteral("blender"), QStringLiteral("mayapy"), QStringLiteral("maya"),
        QStringLiteral("nuke"), QStringLiteral("nukex")};
    for (const QString& executable : names) {
        insertExecutable(&found, executable, QStandardPaths::findExecutable(executable));
    }

#ifdef Q_OS_WIN
    QStringList programRoots{
        qEnvironmentVariable("ProgramW6432"),
        qEnvironmentVariable("ProgramFiles"),
        qEnvironmentVariable("ProgramFiles(x86)"),
        QStringLiteral("C:/Program Files"),
        QStringLiteral("C:/Program Files (x86)")};
    programRoots.removeAll(QString());
    programRoots.removeDuplicates();
    for (const QString& programRoot : programRoots) {
        const QString sideFxRoot = QDir(programRoot).filePath(QStringLiteral("Side Effects Software"));
        for (const QFileInfo& directory : versionedDirectories(sideFxRoot, {QStringLiteral("Houdini *")})) {
            insertExecutable(&found, QStringLiteral("hython"), QDir(directory.absoluteFilePath()).filePath(QStringLiteral("bin/hython.exe")));
            insertExecutable(&found, QStringLiteral("hbatch"), QDir(directory.absoluteFilePath()).filePath(QStringLiteral("bin/hbatch.exe")));
            insertExecutable(&found, QStringLiteral("houdini"), QDir(directory.absoluteFilePath()).filePath(QStringLiteral("bin/houdini.exe")));
        }

        const QString blenderRoot = QDir(programRoot).filePath(QStringLiteral("Blender Foundation"));
        for (const QFileInfo& directory : versionedDirectories(blenderRoot, {QStringLiteral("Blender *")})) {
            insertExecutable(&found, QStringLiteral("blender"), QDir(directory.absoluteFilePath()).filePath(QStringLiteral("blender.exe")));
        }

        const QString autodeskRoot = QDir(programRoot).filePath(QStringLiteral("Autodesk"));
        for (const QFileInfo& directory : versionedDirectories(autodeskRoot, {QStringLiteral("Maya*")})) {
            insertExecutable(&found, QStringLiteral("mayapy"), QDir(directory.absoluteFilePath()).filePath(QStringLiteral("bin/mayapy.exe")));
            insertExecutable(&found, QStringLiteral("maya"), QDir(directory.absoluteFilePath()).filePath(QStringLiteral("bin/maya.exe")));
        }

        for (const QFileInfo& directory : versionedDirectories(programRoot, {QStringLiteral("Nuke*")})) {
            const QFileInfoList candidates = QDir(directory.absoluteFilePath()).entryInfoList(
                {QStringLiteral("Nuke*.exe")}, QDir::Files, QDir::Name | QDir::Reversed);
            static const QRegularExpression mainNukePattern(
                QStringLiteral("^Nuke\\d+(?:\\.\\d+)?\\.exe$"),
                QRegularExpression::CaseInsensitiveOption);
            for (const QFileInfo& candidate : candidates) {
                if (!mainNukePattern.match(candidate.fileName()).hasMatch()) continue;
                insertExecutable(&found, QStringLiteral("nuke"), candidate.absoluteFilePath());
                insertExecutable(&found, QStringLiteral("nukex"), candidate.absoluteFilePath());
                break;
            }
        }
    }
#endif
    return found;
}

QString resolveSoftwareExecutable(const QString& requested)
{
    const QFileInfo direct(requested);
    if (direct.isAbsolute() && direct.isFile()) return direct.absoluteFilePath();
    const QString key = QFileInfo(requested).completeBaseName().toLower();
    return discoverSoftwareExecutables().value(key).toString();
}

QString artifactPathFromToolResult(const QString& tool, const QJsonObject& result)
{
    static const QSet<QString> artifactTools{
        QStringLiteral("workspace_register_artifact"), QStringLiteral("workspace_archive"),
        QStringLiteral("artifact_copy"), QStringLiteral("export_media"),
        QStringLiteral("review_pack"), QStringLiteral("generate_video"),
        QStringLiteral("generate_audio")};
    const QString name = tool.section(QLatin1Char('.'), -1);
    if (!artifactTools.contains(name) || !result.value(QStringLiteral("success")).toBool()) return {};
    const QJsonArray contentItems = result.value(QStringLiteral("contentItems")).toArray();
    for (const QJsonValue& value : contentItems) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("type")).toString() != QStringLiteral("inputText")) continue;
        const QJsonDocument document = QJsonDocument::fromJson(item.value(QStringLiteral("text")).toString().toUtf8());
        const QJsonObject payload = document.object();
        for (const QString& key : {QStringLiteral("savedPath"), QStringLiteral("path"), QStringLiteral("output")}) {
            const QFileInfo file(payload.value(key).toString());
            if (file.isFile()) return file.absoluteFilePath();
        }
    }
    return {};
}

QString dynamicToolSignature(const QJsonArray& tools)
{
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(tools).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}

QByteArray loadAiWorkspaceSecret(
    IAICredentialStore* store,
    const QString& providerId,
    const QString& providerName,
    const QString& detectedProtocol,
    const QString& baseUrl)
{
    if (!store) return {};
    QStringList ids{QStringLiteral("ai/defaultApiKey")};
    const QString category = QStringLiteral("%1 %2 %3 %4")
        .arg(providerId, providerName, detectedProtocol, baseUrl)
        .toLower();
    if (category.contains(QStringLiteral("anthropic")) || category.contains(QStringLiteral("claude"))) {
        ids.push_back(QStringLiteral("anthropic/apiKey"));
    } else if (category.contains(QStringLiteral("gemini")) ||
               category.contains(QStringLiteral("googleapis.com")) ||
               category.contains(QStringLiteral("google.ai"))) {
        ids.push_back(QStringLiteral("gemini/apiKey"));
    } else if (category.contains(QStringLiteral("azure")) ||
               category.contains(QStringLiteral("/openai/deployments/"))) {
        ids.push_back(QStringLiteral("azure/openaiApiKey"));
    } else if (category.contains(QStringLiteral("qwen")) ||
               category.contains(QStringLiteral("dashscope")) ||
               category.contains(QStringLiteral("aliyuncs.com"))) {
        ids.push_back(QStringLiteral("qwen/apiKey"));
    } else {
        ids.push_back(QStringLiteral("openai/apiKey"));
    }
    for (const QString& id : ids) {
        QString ignoredError;
        const QByteArray secret = store->loadSecret(id, &ignoredError).trimmed();
        if (!secret.isEmpty()) return secret;
    }
    return {};
}

void appendUniqueModel(QStringList* models, const QString& model)
{
    if (!models) return;
    const QString trimmed = model.trimmed();
    if (trimmed.isEmpty()) return;
    for (const QString& existing : *models) {
        if (existing.compare(trimmed, Qt::CaseInsensitive) == 0) return;
    }
    models->push_back(trimmed);
}

QString normalizedModelKey(const QString& model)
{
    return model.trimmed().toLower();
}

bool isRunnableCodexExecutable(const QString& path)
{
    const QFileInfo executable(path);
    if (!executable.isAbsolute() || !executable.exists() || !executable.isFile()) return false;

    QProcess probe;
    probe.setProgram(executable.absoluteFilePath());
    probe.setArguments({QStringLiteral("--version")});
    probe.start();
    if (!probe.waitForStarted(1500)) return false;
    JobContext context(3000);
    const ProcessOutcome result = context.waitForProcess(probe, 25, 3000);
    const QByteArray output = result.standardOutput + result.standardError;
    return result.succeeded() && output.contains("codex-cli");
}

QString discoverCodexExecutable(const QString& configuredOverride)
{
    if (!configuredOverride.trimmed().isEmpty()) {
        const QString candidate = QFileInfo(configuredOverride.trimmed()).absoluteFilePath();
        return isRunnableCodexExecutable(candidate) ? candidate : QString();
    }
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates{
        QDir(appDir).filePath(QStringLiteral("runtime/codex/codex.exe")),
        QStandardPaths::findExecutable(QStringLiteral("codex.exe")),
        QStandardPaths::findExecutable(QStringLiteral("codex"))
    };

    QString localAppData = QProcessEnvironment::systemEnvironment()
        .value(QStringLiteral("LOCALAPPDATA"))
        .trimmed();
    if (localAppData.isEmpty()) {
        localAppData = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    }
    QDir binRoot(QDir(localAppData).filePath(QStringLiteral("OpenAI/Codex/bin")));
    const QFileInfoList versionDirectories = binRoot.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Time);
    for (const QFileInfo& versionDirectory : versionDirectories) {
        candidates.push_back(QDir(versionDirectory.absoluteFilePath()).filePath(QStringLiteral("codex.exe")));
    }

    const QString appDirCandidate = QDir(appDir).filePath(QStringLiteral("codex.exe"));
    candidates.push_back(appDirCandidate);
    for (const QString& candidate : candidates) {
        if (isRunnableCodexExecutable(candidate)) return QFileInfo(candidate).absoluteFilePath();
    }
    return {};
}

QString findProjectRootFrom(QString startPath)
{
    QDir directory(startPath);
    for (int depth = 0; depth < 10 && directory.exists(); ++depth) {
        if (QFileInfo::exists(directory.filePath(QStringLiteral("CMakeLists.txt"))) &&
            QDir(directory.filePath(QStringLiteral("src"))).exists()) {
            return QFileInfo(directory.absolutePath()).canonicalFilePath();
        }
        if (!directory.cdUp()) break;
    }
    return {};
}

QString discoverProjectRoot()
{
    for (const QString& start : {QDir::currentPath(), QCoreApplication::applicationDirPath()}) {
        const QString root = findProjectRootFrom(start);
        if (!root.isEmpty()) return root;
    }
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!documents.isEmpty()) {
        const QString workspace = QDir(documents).filePath(QStringLiteral("CGPlay Workspace"));
        if (QDir().mkpath(workspace)) return QFileInfo(workspace).absoluteFilePath();
    }
    return QFileInfo(QDir::currentPath()).absoluteFilePath();
}

QString approvalPolicyForMode(const QString& mode)
{
    return mode == QStringLiteral("full") ? QStringLiteral("never") : QStringLiteral("on-request");
}

QString sandboxForMode(const QString& mode)
{
    return mode == QStringLiteral("full")
        ? QStringLiteral("danger-full-access")
        : QStringLiteral("workspace-write");
}

QByteArray redactSensitiveText(QByteArray text)
{
    QString value = QString::fromUtf8(text);
    static const QRegularExpression authorization(
        QStringLiteral("(?i)(authorization\\s*[:=]\\s*(?:bearer\\s+)?)\\S+"));
    static const QRegularExpression apiKey(
        QStringLiteral("(?i)((?:api[_-]?key|x-api-key|x-goog-api-key)\\s*[:=]\\s*)\\S+"));
    static const QRegularExpression sensitiveQuery(
        QStringLiteral("(?i)([?&](?:key|api_key|apikey|access_token|token|authorization|auth)=)[^&#\\s]+"));
    static const QRegularExpression secretToken(
        QStringLiteral("(?i)\\b(?:sk|xai|glm|key)-[A-Za-z0-9_\\-]{8,}\\b"));
    value.replace(authorization, QStringLiteral("\\1[REDACTED]"));
    value.replace(apiKey, QStringLiteral("\\1[REDACTED]"));
    value.replace(sensitiveQuery, QStringLiteral("\\1[REDACTED]"));
    value.replace(secretToken, QStringLiteral("[REDACTED]"));
    return value.toUtf8();
}

QString describeThreadItem(const QJsonObject& item)
{
    const QString type = item.value(QStringLiteral("type")).toString().trimmed();
    if (type == QStringLiteral("commandExecution")) {
        QString command = item.value(QStringLiteral("command")).toString().simplified();
        if (command.size() > 72) command = command.left(69) + QStringLiteral("...");
        return command.isEmpty() ? QObject::tr("执行命令") : QObject::tr("执行命令：%1").arg(command);
    }
    if (type == QStringLiteral("fileChange")) {
        const QString path = item.value(QStringLiteral("path")).toString().trimmed();
        return path.isEmpty() ? QObject::tr("更新项目文件") : QObject::tr("更新文件：%1").arg(path);
    }
    if (type == QStringLiteral("mcpToolCall")) {
        const QString tool = item.value(QStringLiteral("tool")).toString().trimmed();
        return tool.isEmpty() ? QObject::tr("调用工具") : QObject::tr("调用工具：%1").arg(tool);
    }
    if (type == QStringLiteral("collabAgentToolCall")) {
        const QString tool = item.value(QStringLiteral("tool")).toString();
        QStringList receivers;
        for (const QJsonValue& value : item.value(QStringLiteral("receiverThreadIds")).toArray()) receivers.push_back(value.toString().left(8));
        return QObject::tr("协作代理：%1%2").arg(tool.isEmpty() ? QObject::tr("操作") : tool,
            receivers.isEmpty() ? QString() : QStringLiteral(" -> %1").arg(receivers.join(QStringLiteral(", "))));
    }
    if (type == QStringLiteral("subAgentActivity")) {
        return QObject::tr("子代理 %1：%2")
            .arg(item.value(QStringLiteral("agentThreadId")).toString().left(8),
                 item.value(QStringLiteral("activity")).toString());
    }
    if (type == QStringLiteral("webSearch")) return QObject::tr("检索网络信息");
    if (type == QStringLiteral("reasoning")) return QObject::tr("分析任务");
    if (type == QStringLiteral("plan")) return QObject::tr("更新任务计划");
    return type.isEmpty() ? QObject::tr("处理任务步骤") : QObject::tr("任务步骤：%1").arg(type);
}

} // namespace codex_plugin_detail
} // namespace cgplay
