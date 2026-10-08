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
using namespace codex_plugin_detail;

QJsonObject CodexPlugin::executeDynamicTool(const QJsonObject& params)
{
    const auto response = [](bool success, QJsonObject object) {
        object.insert(QStringLiteral("ok"), success);
        return QJsonObject{{QStringLiteral("success"), success}, {QStringLiteral("contentItems"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("inputText")}, {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact))}}}}};
    };
    const auto imageResponse = [](QJsonObject object, const QString& path) {
        QFile imageFile(path);
        if (!imageFile.open(QIODevice::ReadOnly)) {
            object.insert(QStringLiteral("ok"), false);
            object.insert(QStringLiteral("error"), QStringLiteral("Captured image could not be read."));
            return QJsonObject{{QStringLiteral("success"), false}, {QStringLiteral("contentItems"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("inputText")}, {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact))}}}}};
        }
        const QString dataUrl = QStringLiteral("data:image/png;base64,%1").arg(QString::fromLatin1(imageFile.readAll().toBase64()));
        object.insert(QStringLiteral("ok"), true);
        return QJsonObject{{QStringLiteral("success"), true}, {QStringLiteral("contentItems"), QJsonArray{
            QJsonObject{{QStringLiteral("type"), QStringLiteral("inputText")}, {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact))}},
            QJsonObject{{QStringLiteral("type"), QStringLiteral("inputImage")}, {QStringLiteral("imageUrl"), dataUrl}}
        }}};
    };
    const QString name = params.value(QStringLiteral("tool")).toString().section(QLatin1Char('.'), -1);
    const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();
    const QString workspaceRoot = _session.config().workingDirectory.isEmpty() ? QDir::currentPath() : QDir(_session.config().workingDirectory).absolutePath();
    const auto resolveWorkspacePath = [&workspaceRoot](const QString& requested) -> QString {
        const QString clean = requested.trimmed();
        if (clean.isEmpty()) return {};
        const QString root = QDir::fromNativeSeparators(QDir(workspaceRoot).absolutePath());
        const QFileInfo requestedInfo(clean);
        const QString candidate = QDir::fromNativeSeparators(requestedInfo.isAbsolute()
            ? requestedInfo.absoluteFilePath()
            : QFileInfo(QDir(root).filePath(QDir::cleanPath(clean))).absoluteFilePath());
        if (candidate == root || candidate.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive)) return candidate;
        return {};
    };
    if (name == QStringLiteral("workspace_read_file")) {
        const QString path = resolveWorkspacePath(arguments.value(QStringLiteral("path")).toString());
        QFile file(path);
        if (path.isEmpty() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("File is outside workspace or cannot be read.")}});
        const QByteArray data = file.read(2 * 1024 * 1024);
        return response(true, QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("content"), QString::fromUtf8(data)}, {QStringLiteral("truncated"), !file.atEnd()}});
    }
    if (name == QStringLiteral("workspace_write_file")) {
        const QString path = resolveWorkspacePath(arguments.value(QStringLiteral("path")).toString());
        if (path.isEmpty()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("File path is outside workspace.")}});
        QSaveFile file(path);
        if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(arguments.value(QStringLiteral("content")).toString().toUtf8()) < 0 || !file.commit()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("File could not be written.")}});
        appendMediaAssetRecord(QStringLiteral("workspace-file"), path);
        return response(true, QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("bytes"), QFileInfo(path).size()}});
    }
    if (name == QStringLiteral("workspace_list")) {
        const QString requested = arguments.value(QStringLiteral("path")).toString();
        const QString path = requested.trimmed().isEmpty() ? workspaceRoot : resolveWorkspacePath(requested);
        if (path.isEmpty() || !QDir(path).exists()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Directory is outside workspace or does not exist.")}});
        QJsonArray entries;
        for (const QFileInfo& info : QDir(path).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries, QDir::Name)) entries.push_back(QJsonObject{{QStringLiteral("name"), info.fileName()}, {QStringLiteral("directory"), info.isDir()}, {QStringLiteral("size"), info.isFile() ? info.size() : 0}});
        return response(true, QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("entries"), entries}});
    }
    if (name == QStringLiteral("workspace_run_process")) {
        const QString program = arguments.value(QStringLiteral("program")).toString().trimmed();
        const QString cwd = arguments.value(QStringLiteral("cwd")).toString().trimmed().isEmpty() ? workspaceRoot : resolveWorkspacePath(arguments.value(QStringLiteral("cwd")).toString());
        if (program.isEmpty() || cwd.isEmpty() || !QDir(cwd).exists()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Invalid program or working directory.")}});
        QStringList args; for (const QJsonValue& value : arguments.value(QStringLiteral("arguments")).toArray()) args.push_back(value.toString());
        const int timeout = qBound(100, arguments.value(QStringLiteral("timeoutMs")).toInt(120000), 600000);
        const JobOutcome processJob = runProcessJob(this, QStringLiteral("codex.workspace-process"), program, args, cwd, timeout);
        const QVariantMap processDetails = processJob.value.toMap();
        const QByteArray stdoutBytes = processDetails.value(QStringLiteral("stdout")).toByteArray();
        const QByteArray stderrBytes = processDetails.value(QStringLiteral("stderr")).toByteArray();
        QJsonObject payload{{QStringLiteral("program"), program}, {QStringLiteral("exitCode"), processDetails.value(QStringLiteral("exitCode"), -1).toInt()}, {QStringLiteral("stdout"), QString::fromLocal8Bit(stdoutBytes.left(2 * 1024 * 1024))}, {QStringLiteral("stderr"), QString::fromLocal8Bit(stderrBytes.left(2 * 1024 * 1024))}, {QStringLiteral("outputTruncated"), stdoutBytes.size() > 2 * 1024 * 1024 || stderrBytes.size() > 2 * 1024 * 1024}};
        if (!processJob.succeeded()) payload.insert(QStringLiteral("error"), jobError(processJob, QStringLiteral("Process failed.")));
        return response(processJob.succeeded(), payload);
    }
    if (name == QStringLiteral("workspace_run_process_async")) {
        if (_asyncToolJobs.size() >= 4) return response(false, {{QStringLiteral("error"), QStringLiteral("Too many asynchronous local tasks are already running.")}, {QStringLiteral("limit"), 4}});
        const QString program = arguments.value(QStringLiteral("program")).toString().trimmed();
        const QString cwd = arguments.value(QStringLiteral("cwd")).toString().trimmed().isEmpty() ? workspaceRoot : resolveWorkspacePath(arguments.value(QStringLiteral("cwd")).toString());
        if (program.isEmpty() || cwd.isEmpty() || !QDir(cwd).exists()) return response(false, {{QStringLiteral("error"), QStringLiteral("Invalid program or working directory.")}});
        QStringList args; for (const QJsonValue& value : arguments.value(QStringLiteral("arguments")).toArray()) args.push_back(value.toString());
        const int timeout = qBound(100, arguments.value(QStringLiteral("timeoutMs")).toInt(120000), 600000);
        const QString taskId = QUuid::createUuid().toString(QUuid::WithoutBraces); const QString statePath = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_async_tasks.jsonl"));
        const QJsonObject queued{{QStringLiteral("taskId"), taskId}, {QStringLiteral("status"), QStringLiteral("running")}, {QStringLiteral("program"), program}, {QStringLiteral("startedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
        { QFile file(statePath); if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) { file.write(QJsonDocument(queued).toJson(QJsonDocument::Compact)); file.write("\n"); } }
        JobHandle* job = JobRunner::start(taskId, this, timeout, [program, args, cwd](JobContext& context) {
            QProcess process; process.setWorkingDirectory(cwd); process.setProcessChannelMode(QProcess::SeparateChannels); bool observedStarted = false; QObject::connect(&process, &QProcess::started, &process, [&observedStarted] { observedStarted = true; }, Qt::DirectConnection); process.start(program, args);
            QElapsedTimer startElapsed; startElapsed.start(); bool started = observedStarted || process.state() == QProcess::Running;
            while (!started && process.state() == QProcess::Starting && startElapsed.elapsed() < 5000 && !context.shouldStop()) started = process.waitForStarted(25) || observedStarted;
            if (!started && process.state() != QProcess::Running) return JobOutcome::failure(QStringLiteral("process_start_failed"), process.errorString());
            const ProcessOutcome processResult = context.waitForProcess(process, 25);
            const QByteArray out = processResult.standardOutput; const QByteArray err = processResult.standardError;
            const QJsonObject value{{QStringLiteral("status"), processResult.succeeded() ? QStringLiteral("completed") : (processResult.state == JobState::Canceled ? QStringLiteral("canceled") : QStringLiteral("failed"))}, {QStringLiteral("exitCode"), processResult.exitCode}, {QStringLiteral("stdout"), QString::fromLocal8Bit(out.left(2 * 1024 * 1024))}, {QStringLiteral("stderr"), QString::fromLocal8Bit(err.left(2 * 1024 * 1024))}, {QStringLiteral("outputTruncated"), out.size() > 2 * 1024 * 1024 || err.size() > 2 * 1024 * 1024}};
            JobOutcome outcome;
            outcome.state = processResult.state;
            outcome.value = value;
            if (processResult.state == JobState::TimedOut) { outcome.errorCode = QStringLiteral("timeout"); outcome.errorMessage = QStringLiteral("Process timed out."); }
            else if (processResult.state == JobState::Canceled) { outcome.errorCode = QStringLiteral("canceled"); outcome.errorMessage = QStringLiteral("Process was canceled."); }
            else if (!processResult.succeeded()) { outcome.errorCode = QStringLiteral("process_failed"); outcome.errorMessage = QStringLiteral("Process exited with code %1.").arg(processResult.exitCode); }
            return outcome;
        });
        _asyncToolJobs.insert(taskId, job);
        QPointer<CodexPlugin> guard(this);
        connect(job, &JobHandle::finished, this, [guard, job, taskId, statePath](const JobOutcome& outcome) {
            QJsonObject result = outcome.value.toJsonObject();
            if (result.isEmpty()) result.insert(QStringLiteral("status"), outcome.state == JobState::Canceled ? QStringLiteral("canceled") : QStringLiteral("failed"));
            if (!outcome.errorMessage.isEmpty()) result.insert(QStringLiteral("error"), outcome.errorMessage);
            result.insert(QStringLiteral("taskId"), taskId); result.insert(QStringLiteral("finishedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            QFile file(statePath); QJsonArray rows; if (file.open(QIODevice::ReadOnly | QIODevice::Text)) { while (!file.atEnd()) { QJsonParseError error; const QJsonDocument doc = QJsonDocument::fromJson(file.readLine(), &error); if (error.error == QJsonParseError::NoError && doc.isObject()) rows.push_back(doc.object()); } }
            rows.push_back(result); QSaveFile output(statePath); if (output.open(QIODevice::WriteOnly)) { for (const QJsonValue& row : rows) { output.write(QJsonDocument(row.toObject()).toJson(QJsonDocument::Compact)); output.write("\n"); } output.commit(); }
            if (guard) guard->_asyncToolJobs.remove(taskId); job->deleteLater();
        });
        return response(true, QJsonObject{{QStringLiteral("taskId"), taskId}, {QStringLiteral("status"), QStringLiteral("running")}});
    }
    if (name == QStringLiteral("local_task_status")) {
        const QString taskId = arguments.value(QStringLiteral("taskId")).toString().trimmed(); const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_async_tasks.jsonl")); QFile file(path); if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return response(false, {{QStringLiteral("error"), QStringLiteral("Async task store is unavailable.")}});
        QJsonObject latest; while (!file.atEnd()) { QJsonParseError error; const QJsonDocument doc = QJsonDocument::fromJson(file.readLine(), &error); if (error.error == QJsonParseError::NoError && doc.object().value(QStringLiteral("taskId")).toString() == taskId) latest = doc.object(); }
        return latest.isEmpty() ? response(false, {{QStringLiteral("error"), QStringLiteral("Async task not found.")}}) : response(true, latest);
    }
    if (name == QStringLiteral("workspace_register_artifact")) {
        const QString path = resolveWorkspacePath(arguments.value(QStringLiteral("path")).toString());
        if (path.isEmpty() || !QFileInfo(path).isFile()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Artifact is outside workspace or does not exist.")}});
        appendMediaAssetRecord(QStringLiteral("workspace-artifact"), path, QJsonObject{{QStringLiteral("size"), QFileInfo(path).size()}});
        return response(true, QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("size"), QFileInfo(path).size()}});
    }
    if (name == QStringLiteral("workspace_search")) {
        const QString query = arguments.value(QStringLiteral("query")).toString();
        const QString base = arguments.value(QStringLiteral("path")).toString().trimmed().isEmpty() ? workspaceRoot : resolveWorkspacePath(arguments.value(QStringLiteral("path")).toString());
        if (query.isEmpty() || base.isEmpty()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Query or path is invalid.")}});
        QJsonArray matches; QDirIterator iterator(base, QDir::Files, QDirIterator::Subdirectories);
        while (iterator.hasNext() && matches.size() < 200) {
            const QString filePath = iterator.next(); QFileInfo info(filePath); if (info.size() > 4 * 1024 * 1024) continue; QFile file(filePath);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            int lineNo = 0; while (!file.atEnd() && matches.size() < 200) { ++lineNo; const QString line = QString::fromUtf8(file.readLine()); if (line.contains(query, Qt::CaseInsensitive)) matches.push_back(QJsonObject{{QStringLiteral("path"), QDir(workspaceRoot).relativeFilePath(filePath)}, {QStringLiteral("line"), lineNo}, {QStringLiteral("text"), line.trimmed().left(500)}}); }
        }
        return response(true, QJsonObject{{QStringLiteral("query"), query}, {QStringLiteral("matches"), matches}, {QStringLiteral("truncated"), matches.size() >= 200}});
    }
    if (name == QStringLiteral("workspace_archive")) {
        const QString source = resolveWorkspacePath(arguments.value(QStringLiteral("path")).toString());
        const QString output = resolveWorkspacePath(arguments.value(QStringLiteral("output")).toString());
        if (source.isEmpty() || output.isEmpty() || !QFileInfo(source).exists()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Archive source/output is outside workspace or missing.")}});
        QString safeSource = source; safeSource.replace("'", "''"); QString safeOutput = output; safeOutput.replace("'", "''");
        const JobOutcome archiveJob = runProcessJob(this, QStringLiteral("codex.workspace-archive"), QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), QStringLiteral("Compress-Archive -Path '%1' -DestinationPath '%2' -Force").arg(safeSource, safeOutput)}, workspaceRoot, 120000);
        if (!archiveJob.succeeded() || !QFileInfo(output).isFile()) return response(false, QJsonObject{{QStringLiteral("error"), jobError(archiveJob, QStringLiteral("Workspace archive was not created."))}});
        appendMediaAssetRecord(QStringLiteral("workspace-archive"), output); return response(true, QJsonObject{{QStringLiteral("path"), output}, {QStringLiteral("size"), QFileInfo(output).size()}});
    }
    if (name == QStringLiteral("media_metadata")) {
        auto* playback = reinterpret_cast<IPlaybackService*>(QCoreApplication::instance()->property("cgplay.codex.playbackService").toULongLong());
        if (!playback || !playback->isValid()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("No media is open.")}});
        const QString path = playback->currentPath(); const double fps = playback->fps(); const int count = playback->totalFrames();
        return response(true, QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("fps"), fps}, {QStringLiteral("frameCount"), count}, {QStringLiteral("durationSeconds"), fps > 0.0 ? count / fps : 0.0}});
    }
    if (name == QStringLiteral("export_media")) {
        auto* playback = reinterpret_cast<IPlaybackService*>(QCoreApplication::instance()->property("cgplay.codex.playbackService").toULongLong());
        if (!playback || !playback->isValid()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("No media is open.")}});
        const QString output = resolveWorkspacePath(arguments.value(QStringLiteral("output")).toString()); if (output.isEmpty()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Output must be inside workspace.")}});
        const int count = playback->totalFrames();
        int start = arguments.value(QStringLiteral("startFrame")).toInt(playback->hasInPoint() ? playback->inPoint() : 0); int end = arguments.value(QStringLiteral("endFrame")).toInt(playback->hasOutPoint() ? playback->outPoint() : qMax(0, count - 1)); if (count > 0) { start = qBound(0, start, count - 1); end = qBound(0, end, count - 1); }
        if (start > end) std::swap(start, end); QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg")); if (ffmpeg.isEmpty()) ffmpeg = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("ffmpeg.exe")); if (ffmpeg.isEmpty()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("ffmpeg is unavailable.")}}); const double fps = playback->fps() > 0.0 ? playback->fps() : 24.0; const double begin = start / fps; const double duration = qMax(0.01, (end - start + 1) / fps); const JobOutcome exportJob = runProcessJob(this, QStringLiteral("codex.export-media"), ffmpeg, {QStringLiteral("-y"), QStringLiteral("-ss"), QString::number(begin, 'f', 3), QStringLiteral("-i"), playback->currentPath(), QStringLiteral("-t"), QString::number(duration, 'f', 3), QStringLiteral("-c"), QStringLiteral("copy"), output}, {}, 120000); if (!exportJob.succeeded() || !QFileInfo(output).isFile()) return response(false, QJsonObject{{QStringLiteral("error"), jobError(exportJob, QStringLiteral("Media export failed."))}}); appendMediaAssetRecord(QStringLiteral("exported-media"), output, QJsonObject{{QStringLiteral("startFrame"), start}, {QStringLiteral("endFrame"), end}}); return response(true, QJsonObject{{QStringLiteral("path"), output}, {QStringLiteral("startFrame"), start}, {QStringLiteral("endFrame"), end}});
    }
    if (name == QStringLiteral("task_plan")) {
        const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_task_plan.json"));
        QSaveFile file(path); QJsonObject plan{{QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {QStringLiteral("steps"), arguments.value(QStringLiteral("steps"))}, {QStringLiteral("status"), QStringLiteral("planned")}};
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(plan).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Task plan could not be saved.")}});
        return response(true, QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("status"), QStringLiteral("planned")}});
    }
    if (name == QStringLiteral("task_update")) {
        const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_task_plan.json")); QFile file(path); QJsonObject plan; if (file.open(QIODevice::ReadOnly)) plan = QJsonDocument::fromJson(file.readAll()).object(); plan.insert(QStringLiteral("status"), arguments.value(QStringLiteral("status"))); if (arguments.contains(QStringLiteral("step"))) plan.insert(QStringLiteral("currentStep"), arguments.value(QStringLiteral("step"))); if (arguments.contains(QStringLiteral("detail"))) plan.insert(QStringLiteral("detail"), arguments.value(QStringLiteral("detail"))); plan.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)); QSaveFile out(path); if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(plan).toJson(QJsonDocument::Indented)) < 0 || !out.commit()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Task plan update failed.")}}); return response(true, plan);
    }
    if (name == QStringLiteral("artifact_copy")) {
        const QString source = resolveWorkspacePath(arguments.value(QStringLiteral("source")).toString()); const QString destination = resolveWorkspacePath(arguments.value(QStringLiteral("destination")).toString()); if (source.isEmpty() || destination.isEmpty() || !QFileInfo(source).isFile()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Artifact path invalid.")}}); QDir().mkpath(QFileInfo(destination).absolutePath()); QFile::remove(destination); if (!QFile::copy(source, destination)) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Artifact copy failed.")}}); appendMediaAssetRecord(QStringLiteral("artifact-copy"), destination); return response(true, QJsonObject{{QStringLiteral("path"), destination}, {QStringLiteral("size"), QFileInfo(destination).size()}});
    }
    if (name == QStringLiteral("software_detect")) {
        return response(true, QJsonObject{{QStringLiteral("executables"), discoverSoftwareExecutables()}});
    }
    if (name == QStringLiteral("software_run")) {
        const QString requested = arguments.value(QStringLiteral("program")).toString().trimmed();
        const QString program = resolveSoftwareExecutable(requested);
        if (program.isEmpty()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Requested software executable was not found in PATH or standard installation directories.")}, {QStringLiteral("program"), requested}});
        QJsonObject forwarded = arguments;
        forwarded.insert(QStringLiteral("program"), program);
        forwarded.insert(QStringLiteral("cwd"), arguments.value(QStringLiteral("cwd")).toString().trimmed().isEmpty() ? QStringLiteral(".") : arguments.value(QStringLiteral("cwd")));
        QJsonObject nested{{QStringLiteral("tool"), QStringLiteral("workspace_run_process")}, {QStringLiteral("arguments"), forwarded}};
        return executeDynamicTool(nested);
    }
    if (name == QStringLiteral("review_pack")) {
        auto* playback = reinterpret_cast<IPlaybackService*>(QCoreApplication::instance()->property("cgplay.codex.playbackService").toULongLong()); if (!playback || !playback->isValid()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("No media is open.")}}); const QString output = resolveWorkspacePath(arguments.value(QStringLiteral("output")).toString()); if (output.isEmpty()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Output is outside workspace.")}}); const QString staging = QDir(QFileInfo(output).absolutePath()).filePath(QStringLiteral("review_pack_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))); QDir().mkpath(staging); QFile::copy(playback->currentPath(), QDir(staging).filePath(QFileInfo(playback->currentPath()).fileName())); QFile meta(QDir(staging).filePath(QStringLiteral("media.json"))); if (meta.open(QIODevice::WriteOnly)) meta.write(QJsonDocument(QJsonObject{{QStringLiteral("path"), playback->currentPath()}, {QStringLiteral("fps"), playback->fps()}, {QStringLiteral("currentFrame"), playback->currentFrame()}, {QStringLiteral("totalFrames"), playback->totalFrames()}}).toJson(QJsonDocument::Indented)); QString safeStage = staging; safeStage.replace("'", "''"); QString safeOutput = output; safeOutput.replace("'", "''"); const JobOutcome reviewPackJob = runProcessJob(this, QStringLiteral("codex.review-pack"), QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), QStringLiteral("Compress-Archive -Path '%1\\*' -DestinationPath '%2' -Force").arg(safeStage, safeOutput)}, {}, 120000); if (!reviewPackJob.succeeded()) { QDir(staging).removeRecursively(); return response(false, QJsonObject{{QStringLiteral("error"), jobError(reviewPackJob, QStringLiteral("Review pack creation failed."))}}); } QDir(staging).removeRecursively(); appendMediaAssetRecord(QStringLiteral("review-pack"), output); return response(true, QJsonObject{{QStringLiteral("path"), output}});
    }
    if (name == QStringLiteral("media_audio_review") || name == QStringLiteral("media_hdr_info")) {
        auto* playback = reinterpret_cast<IPlaybackService*>(QCoreApplication::instance()->property("cgplay.codex.playbackService").toULongLong()); if (!playback || !playback->isValid()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("No media is open.")}}); QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe")); if (ffprobe.isEmpty()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("ffprobe is unavailable.")}}); const JobOutcome probeJob = runProcessJob(this, QStringLiteral("codex.media-probe"), ffprobe, {QStringLiteral("-v"), QStringLiteral("quiet"), QStringLiteral("-print_format"), QStringLiteral("json"), QStringLiteral("-show_streams"), QStringLiteral("-show_format"), playback->currentPath()}, {}, 30000); if (!probeJob.succeeded()) return response(false, QJsonObject{{QStringLiteral("error"), jobError(probeJob, QStringLiteral("ffprobe failed."))}}); const QJsonDocument doc = QJsonDocument::fromJson(probeJob.value.toMap().value(QStringLiteral("stdout")).toByteArray()); return response(!doc.isNull(), QJsonObject{{QStringLiteral("analysis"), doc.object()}, {QStringLiteral("kind"), name == QStringLiteral("media_audio_review") ? QStringLiteral("audio") : QStringLiteral("hdr-color")}});
    }
    if (name == QStringLiteral("browser_navigate") || name == QStringLiteral("browser_click") || name == QStringLiteral("browser_type") || name == QStringLiteral("browser_extract")) {
        if (!_workspace) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Codex browser workspace is not open.")}});
        QString error;
        if (name == QStringLiteral("browser_navigate")) {
            const bool ok = _workspace->browserNavigate(arguments.value(QStringLiteral("url")).toString(), &error);
            return response(ok, ok ? QJsonObject{{QStringLiteral("url"), arguments.value(QStringLiteral("url")).toString()}} : QJsonObject{{QStringLiteral("error"), error}});
        }
        if (name == QStringLiteral("browser_click")) {
            const bool ok = _workspace->browserClick(arguments.value(QStringLiteral("selector")).toString(), &error);
            return response(ok, ok ? QJsonObject{{QStringLiteral("selector"), arguments.value(QStringLiteral("selector")).toString()}, {QStringLiteral("action"), QStringLiteral("clicked")}} : QJsonObject{{QStringLiteral("error"), error}});
        }
        if (name == QStringLiteral("browser_type")) {
            const bool ok = _workspace->browserType(arguments.value(QStringLiteral("selector")).toString(), arguments.value(QStringLiteral("text")).toString(), &error);
            return response(ok, ok ? QJsonObject{{QStringLiteral("selector"), arguments.value(QStringLiteral("selector")).toString()}, {QStringLiteral("action"), QStringLiteral("typed")}} : QJsonObject{{QStringLiteral("error"), error}});
        }
        const QString text = _workspace->browserExtract(arguments.value(QStringLiteral("selector")).toString(), &error);
        return response(!text.isEmpty() || error.isEmpty(), error.isEmpty() ? QJsonObject{{QStringLiteral("text"), text}} : QJsonObject{{QStringLiteral("error"), error}});
    }
    if (name == QStringLiteral("schedule_local_task")) {
        const QString objective = arguments.value(QStringLiteral("objective")).toString().trimmed();
        const QDateTime runAt = QDateTime::fromString(arguments.value(QStringLiteral("runAt")).toString().trimmed(), Qt::ISODate);
        if (objective.isEmpty() || !runAt.isValid()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("objective and ISO runAt are required.")}});
        const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_local_scheduled_tasks.jsonl"));
        QFile file(path); if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Cannot persist scheduled task.")}});
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces); const QJsonObject task{{QStringLiteral("id"), id}, {QStringLiteral("objective"), objective}, {QStringLiteral("runAt"), runAt.toUTC().toString(Qt::ISODate)}, {QStringLiteral("status"), QStringLiteral("queued")}};
        file.write(QJsonDocument(task).toJson(QJsonDocument::Compact)); file.write("\n");
        return response(true, task);
    }
    if (name == QStringLiteral("workspace_fuzzy_search")) {
        const QString query = arguments.value(QStringLiteral("query")).toString().trimmed(); const QString base = arguments.value(QStringLiteral("path")).toString().trimmed().isEmpty() ? workspaceRoot : resolveWorkspacePath(arguments.value(QStringLiteral("path")).toString());
        if (query.isEmpty() || base.isEmpty()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Query or path is invalid.")}});
        QJsonArray matches; QDirIterator iterator(base, QDir::Files, QDirIterator::Subdirectories); const QString needle = query.toLower();
        while (iterator.hasNext() && matches.size() < 100) { const QString path = iterator.next(); const QFileInfo info(path); if (info.fileName().toLower().contains(needle)) matches.push_back(QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("kind"), QStringLiteral("filename")}}); else if (info.size() < 4 * 1024 * 1024) { QFile file(path); if (file.open(QIODevice::ReadOnly | QIODevice::Text) && QString::fromUtf8(file.readAll()).contains(query, Qt::CaseInsensitive)) matches.push_back(QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("kind"), QStringLiteral("content")}}); } }
        return response(true, QJsonObject{{QStringLiteral("query"), query}, {QStringLiteral("matches"), matches}});
    }
    if (name == QStringLiteral("terminal_snapshot")) {
        const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_terminal.jsonl")); QFile file(path); if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return response(true, QJsonObject{{QStringLiteral("entries"), QJsonArray{}}}); return response(true, QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("text"), QString::fromUtf8(file.readAll()).right(24000)}});
    }
    if (name == QStringLiteral("local_capability")) {
        const QString action = arguments.value(QStringLiteral("action")).toString().trimmed();
        const QString value = arguments.value(QStringLiteral("value")).toString();
        const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_local"));
        QDir().mkpath(dir);
        auto saveJson = [&](const QString& fileName, const QJsonObject& patch) {
            const QString path = QDir(dir).filePath(fileName); QJsonObject current; QFile input(path);
            if (input.open(QIODevice::ReadOnly)) current = QJsonDocument::fromJson(input.readAll()).object();
            for (auto it = patch.constBegin(); it != patch.constEnd(); ++it) current.insert(it.key(), it.value());
            current.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            QSaveFile output(path);
            if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(current).toJson(QJsonDocument::Indented)) < 0 || !output.commit()) return QJsonObject{{QStringLiteral("error"), QStringLiteral("Cannot save local capability state.")}};
            return current;
        };
        if (action == QStringLiteral("get_local_state")) {
            QJsonObject state; for (const QString& fileName : {QStringLiteral("preferences.json"), QStringLiteral("extensions.json"), QStringLiteral("hooks.json"), QStringLiteral("sandbox.json"), QStringLiteral("account.json"), QStringLiteral("git_records.json")}) { QFile input(QDir(dir).filePath(fileName)); if (input.open(QIODevice::ReadOnly)) state.insert(fileName, QJsonDocument::fromJson(input.readAll()).object()); }
            return response(true, state);
        }
        if (action == QStringLiteral("plugin_share") || action == QStringLiteral("marketplace_cache")) return response(true, saveJson(QStringLiteral("extensions.json"), {{action, value}, {QStringLiteral("source"), arguments.value(QStringLiteral("source")).toString()}, {QStringLiteral("target"), arguments.value(QStringLiteral("target")).toString()}}));
        if (action == QStringLiteral("hook_write")) return response(true, saveJson(QStringLiteral("hooks.json"), {{QStringLiteral("hook"), value}, {QStringLiteral("content"), arguments.value(QStringLiteral("content")).toString()}, {QStringLiteral("enabled"), true}}));
        if (action == QStringLiteral("sandbox_status")) return response(true, saveJson(QStringLiteral("sandbox.json"), {{QStringLiteral("status"), value.isEmpty() ? QStringLiteral("local-ready") : value}, {QStringLiteral("installPath"), QDir(dir).filePath(QStringLiteral("sandbox"))}}));
        if (action == QStringLiteral("guardian_restore")) return response(true, saveJson(QStringLiteral("guardian.json"), {{QStringLiteral("lastDecision"), QStringLiteral("restore-requested")}, {QStringLiteral("target"), value}}));
        if (action == QStringLiteral("file_watch")) {
            const QString target = resolveWorkspacePath(arguments.value(QStringLiteral("target")).toString());
            if (target.isEmpty()) return response(false, {{QStringLiteral("error"), QStringLiteral("Watch target is outside workspace.")}});
            if (!_localFileWatcher || !QFileInfo(target).exists() || !_localFileWatcher->addPath(target)) return response(false, {{QStringLiteral("error"), QStringLiteral("File watcher could not add the target.")}});
            return response(true, saveJson(QStringLiteral("file_watches.json"), {{target, QStringLiteral("enabled")}}));
        }
        if (action == QStringLiteral("file_metadata")) {
            const QString target = resolveWorkspacePath(arguments.value(QStringLiteral("target")).toString()); QFileInfo info(target);
            if (!info.exists()) return response(false, {{QStringLiteral("error"), QStringLiteral("File does not exist.")}});
            return response(true, QJsonObject{{QStringLiteral("path"), target}, {QStringLiteral("size"), info.size()}, {QStringLiteral("modified"), info.lastModified().toUTC().toString(Qt::ISODate)}, {QStringLiteral("directory"), info.isDir()}});
        }
        if (action == QStringLiteral("agent_import")) {
            const QString source = arguments.value(QStringLiteral("source")).toString().trimmed(); QFileInfo info(source);
            if (!info.isFile()) return response(false, {{QStringLiteral("error"), QStringLiteral("Agent config source does not exist.")}});
            const QString target = QDir(dir).filePath(QStringLiteral("imported_agents/%1").arg(info.fileName())); QDir().mkpath(QFileInfo(target).absolutePath());
            if (!QFile::copy(source, target)) return response(false, {{QStringLiteral("error"), QStringLiteral("Agent config import failed.")}});
            return response(true, QJsonObject{{QStringLiteral("source"), source}, {QStringLiteral("target"), target}});
        }
        if (action == QStringLiteral("terminal_resize")) return response(true, saveJson(QStringLiteral("terminal.json"), {{QStringLiteral("columns"), arguments.value(QStringLiteral("value")).toInt(120)}, {QStringLiteral("rows"), arguments.value(QStringLiteral("target")).toInt(40)}}));
        if (action == QStringLiteral("browser_cdp_record")) return response(true, saveJson(QStringLiteral("interaction_records.jsonl"), {{action, value}, {QStringLiteral("target"), arguments.value(QStringLiteral("target")).toString()}, {QStringLiteral("recordedOnly"), true}}));
        if (action == QStringLiteral("set_fast_mode")) return response(true, saveJson(QStringLiteral("preferences.json"), {{QStringLiteral("fastMode"), value == QStringLiteral("true")}}));
        if (action == QStringLiteral("set_personality")) return response(true, saveJson(QStringLiteral("preferences.json"), {{QStringLiteral("personality"), value}}));
        if (action == QStringLiteral("thread_rename")) return response(true, saveJson(QStringLiteral("thread_%1.json").arg(_threadId), {{QStringLiteral("name"), value}}));
        if (action == QStringLiteral("thread_metadata")) return response(true, saveJson(QStringLiteral("thread_%1.json").arg(_threadId), {{QStringLiteral("metadata"), value}}));
        if (action == QStringLiteral("account_local")) return response(true, saveJson(QStringLiteral("account.json"), {{QStringLiteral("status"), value}}));
        if (action == QStringLiteral("mention_resolve")) return response(true, QJsonObject{{QStringLiteral("mention"), value}, {QStringLiteral("resolvedPath"), resolveWorkspacePath(value)}});
        if (action == QStringLiteral("git_commit")) {
            const JobOutcome addJob = runProcessJob(this, QStringLiteral("codex.git-add"), QStringLiteral("git"), {QStringLiteral("add"), QStringLiteral("-A")}, workspaceRoot, 30000);
            if (!addJob.succeeded()) return response(false, {{QStringLiteral("error"), jobError(addJob, QStringLiteral("git add failed."))}});
            const QString message = arguments.value(QStringLiteral("message")).toString().trimmed().isEmpty() ? QStringLiteral("CGPlay local commit") : arguments.value(QStringLiteral("message")).toString().trimmed();
            const JobOutcome commitJob = runProcessJob(this, QStringLiteral("codex.git-commit"), QStringLiteral("git"), {QStringLiteral("commit"), QStringLiteral("-m"), message}, workspaceRoot, 30000);
            const QVariantMap details = commitJob.value.toMap();
            return response(commitJob.succeeded(), {{QStringLiteral("output"), QString::fromLocal8Bit(details.value(QStringLiteral("stdout")).toByteArray() + details.value(QStringLiteral("stderr")).toByteArray())}, {QStringLiteral("error"), commitJob.succeeded() ? QString() : jobError(commitJob, QStringLiteral("git commit failed."))}});
        }
        if (action == QStringLiteral("git_push_record") || action == QStringLiteral("git_pr_record")) return response(true, saveJson(QStringLiteral("git_records.json"), {{action, value}, {QStringLiteral("message"), arguments.value(QStringLiteral("message")).toString()}}));
        if (action == QStringLiteral("desktop_action")) return response(false, {{QStringLiteral("error"), QStringLiteral("Desktop action requires explicit UI approval; unattended OS control is disabled.")}});
        return response(false, {{QStringLiteral("error"), QStringLiteral("Unknown local capability action.")}});
    }
    if (name == QStringLiteral("generate_image")) return executeImageGeneration(arguments);
    if (name == QStringLiteral("search_images")) return executeImageSearch(arguments);
    if (name == QStringLiteral("generate_video")) return executeMediaGeneration(QStringLiteral("video"), arguments);
    if (name == QStringLiteral("generate_audio")) return executeMediaGeneration(QStringLiteral("audio"), arguments);
    if (name == QStringLiteral("edit_image")) return executeMediaGeneration(QStringLiteral("imageEditing"), arguments);
    if (name == QStringLiteral("capture_current_frame") || name == QStringLiteral("capture_selection_context")) {
        auto* viewport = reinterpret_cast<QWidget*>(QCoreApplication::instance()->property("cgplay.codex.activeViewport").toULongLong());
        if (!viewport || !viewport->isVisible()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("No active video viewport is available.")}});
        const QPixmap frame = viewport->grab();
        if (frame.isNull()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Cannot capture the current video frame.")}});
        const QString outputDir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_frames"));
        if (!QDir().mkpath(outputDir)) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Cannot create frame capture directory.")}});
        const QString path = QDir(outputDir).filePath(QStringLiteral("frame_%1.png").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!frame.save(path, "PNG")) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Cannot save the captured frame.")}});
        QJsonObject context{{QStringLiteral("savedPath"), path}};
        if (name == QStringLiteral("capture_selection_context")) {
            auto* playback = reinterpret_cast<IPlaybackService*>(QCoreApplication::instance()->property("cgplay.codex.playbackService").toULongLong());
            if (!playback || !playback->isValid() || !playback->hasInPoint() || !playback->hasOutPoint()) {
                return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Set both In and Out points before sending selection context.")}});
            }
            context.insert(QStringLiteral("inFrame"), playback->inPoint());
            context.insert(QStringLiteral("outFrame"), playback->outPoint());
            context.insert(QStringLiteral("fps"), playback->fps());
            context.insert(QStringLiteral("currentFrame"), playback->currentFrame());
            const QString mediaPath = playback->currentPath();
            const double fps = playback->fps();
            const int inFrame = playback->inPoint();
            const int outFrame = playback->outPoint();
            QString ffmpeg = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("ffmpeg.exe"));
            if (!QFileInfo(ffmpeg).isFile()) ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
            if (mediaPath.isEmpty() || fps <= 0.0 || ffmpeg.isEmpty()) {
                return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Selection media or bundled ffmpeg is unavailable.")}});
            }
            const QString selectionDir = QDir(outputDir).filePath(QStringLiteral("selection_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
            if (!QDir().mkpath(selectionDir)) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Cannot create selection capture directory.")}});
            const QList<int> frames{inFrame, inFrame + (outFrame - inFrame) / 2, outFrame};
            QJsonArray sampledPaths;
            for (int index = 0; index < frames.size(); ++index) {
                const QString samplePath = QDir(selectionDir).filePath(QStringLiteral("sample_%1.png").arg(index + 1));
                const JobOutcome frameJob = runProcessJob(this, QStringLiteral("codex.selection-frame"), ffmpeg, {QStringLiteral("-y"), QStringLiteral("-ss"), QString::number(frames.at(index) / fps, 'f', 3), QStringLiteral("-i"), mediaPath, QStringLiteral("-frames:v"), QStringLiteral("1"), samplePath}, {}, 30000);
                if (!frameJob.succeeded() || !QFileInfo(samplePath).isFile()) {
                    return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("ffmpeg could not extract selection frame %1.").arg(index + 1)}});
                }
                sampledPaths.push_back(samplePath);
            }
            const QString audioPath = QDir(selectionDir).filePath(QStringLiteral("selection.wav"));
            const double startSeconds = inFrame / fps;
            const double durationSeconds = qMax(0.01, (outFrame - inFrame) / fps);
            const JobOutcome audioJob = runProcessJob(this, QStringLiteral("codex.selection-audio"), ffmpeg, {QStringLiteral("-y"), QStringLiteral("-ss"), QString::number(startSeconds, 'f', 3), QStringLiteral("-t"), QString::number(durationSeconds, 'f', 3), QStringLiteral("-i"), mediaPath, QStringLiteral("-vn"), QStringLiteral("-ac"), QStringLiteral("1"), QStringLiteral("-ar"), QStringLiteral("16000"), audioPath}, {}, 60000);
            if (audioJob.succeeded() && QFileInfo(audioPath).isFile()) context.insert(QStringLiteral("audioPath"), audioPath);
            context.insert(QStringLiteral("sampledPaths"), sampledPaths);
            for (const QJsonValue& sample : sampledPaths) appendMediaAssetRecord(QStringLiteral("selection-frame"), sample.toString(), context);
            if (context.contains(QStringLiteral("audioPath"))) appendMediaAssetRecord(QStringLiteral("selection-audio"), context.value(QStringLiteral("audioPath")).toString(), context);
        }
        return imageResponse(context, path);
    }
    auto* playback = reinterpret_cast<IPlaybackService*>(
        QCoreApplication::instance()->property("cgplay.codex.playbackService").toULongLong());
    if (!playback) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("CGPlay playback runtime is unavailable.")}});
    const auto status = [playback] {
        const bool media = playback->isValid();
        const int frame = playback->currentFrame();
        const int total = playback->totalFrames();
        const double fps = playback->fps();
        const QString transport = playback->playbackState() == 1 ? QStringLiteral("playing") : (playback->playbackState() == 2 ? QStringLiteral("reverse") : QStringLiteral("stopped"));
        QJsonObject result;
        result.insert(QStringLiteral("hasMedia"), media);
        result.insert(QStringLiteral("mediaPath"), media ? playback->currentPath() : QString());
        result.insert(QStringLiteral("transport"), media ? transport : QStringLiteral("no_media"));
        result.insert(QStringLiteral("currentFrame"), media ? QJsonValue(frame) : QJsonValue(QJsonValue::Null));
        result.insert(QStringLiteral("totalFrames"), media ? QJsonValue(total) : QJsonValue(QJsonValue::Null));
        result.insert(QStringLiteral("fps"), media && fps > 0.0 ? QJsonValue(fps) : QJsonValue(QJsonValue::Null));
        result.insert(QStringLiteral("currentSeconds"), media && fps > 0.0 ? QJsonValue(frame / fps) : QJsonValue(QJsonValue::Null));
        result.insert(QStringLiteral("durationSeconds"), media && fps > 0.0 ? QJsonValue(total / fps) : QJsonValue(QJsonValue::Null));
        result.insert(QStringLiteral("volume"), QJsonValue(static_cast<double>(playback->getVolume())));
        result.insert(QStringLiteral("muted"), QJsonValue::fromVariant(QVariant(playback->isMuted())));
        result.insert(QStringLiteral("selection"), QJsonObject{
            {QStringLiteral("hasIn"), media && playback->hasInPoint()},
            {QStringLiteral("inFrame"), media && playback->hasInPoint() ? QJsonValue(playback->inPoint()) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("hasOut"), media && playback->hasOutPoint()},
            {QStringLiteral("outFrame"), media && playback->hasOutPoint() ? QJsonValue(playback->outPoint()) : QJsonValue(QJsonValue::Null)}
        });
        return result;
    };
    if (name == QStringLiteral("annotation_status")) {
        auto* annotations = ServiceLocator::getService<IAnnotationService>();
        if (!annotations) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("CGPlay annotation service is unavailable.")}});
        QJsonArray values;
        for (const AnnotationItem& annotation : annotations->annotations()) {
            values.push_back(QJsonObject{{QStringLiteral("id"), annotation.id}, {QStringLiteral("frame"), annotation.frame}, {QStringLiteral("type"), annotationTypeString(annotation.type)}, {QStringLiteral("comment"), annotation.latestCommentText()}, {QStringLiteral("status"), reviewStatusString(annotation.status)}, {QStringLiteral("assignee"), annotation.assignee}});
        }
        return response(true, QJsonObject{{QStringLiteral("count"), annotations->count()}, {QStringLiteral("selectedId"), annotations->selectedAnnotationId()}, {QStringLiteral("annotations"), values}});
    }
    if (name == QStringLiteral("annotation_control")) {
        auto* annotations = ServiceLocator::getService<IAnnotationService>();
        if (!annotations) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("CGPlay annotation service is unavailable.")}});
        const QString action = arguments.value(QStringLiteral("action")).toString();
        const QString annotationId = arguments.value(QStringLiteral("annotationId")).toString().trimmed();
        const QString text = arguments.value(QStringLiteral("text")).toString().trimmed();
        if (action == QStringLiteral("create_note")) {
            if (text.isEmpty()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("text is required.")}});
            const QString id = annotations->createNote(text);
            return response(!id.isEmpty(), QJsonObject{{QStringLiteral("id"), id}});
        }
        if (annotationId.isEmpty()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("annotationId is required.")}});
        if (action == QStringLiteral("select")) { annotations->selectAnnotation(annotationId); return response(annotations->selectedAnnotationId() == annotationId, QJsonObject{{QStringLiteral("selectedId"), annotations->selectedAnnotationId()}}); }
        if (action == QStringLiteral("delete")) return response(annotations->removeAnnotation(annotationId), QJsonObject{{QStringLiteral("deletedId"), annotationId}});
        if (action == QStringLiteral("update_comment")) { if (text.isEmpty()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("text is required.")}}); return response(annotations->updateAnnotationComment(annotationId, text), QJsonObject{{QStringLiteral("updatedId"), annotationId}}); }
        return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Unsupported annotation action.")}});
    }
    if (name == QStringLiteral("player_status")) return response(true, status());
    if (name != QStringLiteral("player_control")) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Unknown CGPlay host tool.")}});
    const QString action = arguments.value(QStringLiteral("action")).toString();
    const QSet<QString> allowed{QStringLiteral("action"), QStringLiteral("frame"), QStringLiteral("loopMode"), QStringLiteral("path"), QStringLiteral("volume"), QStringLiteral("muted"), QStringLiteral("offsetSeconds"), QStringLiteral("channel")};
    for (auto it = arguments.constBegin(); it != arguments.constEnd(); ++it) {
        if (!allowed.contains(it.key())) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Unknown player_control argument: %1.").arg(it.key())}});
    }
    if (action.isEmpty()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("action is required.")}});
    if (!playback->isValid() && action != QStringLiteral("set_volume") && action != QStringLiteral("set_mute")) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("No media is open.")}});
    if (action == QStringLiteral("play")) playback->play();
    else if (action == QStringLiteral("pause")) playback->pause();
    else if (action == QStringLiteral("stop")) playback->stop();
    else if (action == QStringLiteral("next_frame")) playback->nextFrame();
    else if (action == QStringLiteral("previous_frame")) playback->prevFrame();
    else if (action == QStringLiteral("seek")) { if (!arguments.value(QStringLiteral("frame")).isDouble()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("frame is required and must be an integer.")}}); const int frame = arguments.value(QStringLiteral("frame")).toInt(-1); if (frame < 0 || frame >= playback->totalFrames()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("frame must be inside the current media.")}}); playback->seekToFrame(frame); }
    else if (action == QStringLiteral("goto_start")) playback->gotoStart();
    else if (action == QStringLiteral("goto_end")) playback->gotoEnd();
    else if (action == QStringLiteral("set_loop")) playback->setLoop(arguments.value(QStringLiteral("loopMode")).toInt());
    else if (action == QStringLiteral("set_in")) playback->setInPoint(arguments.value(QStringLiteral("frame")).toInt(playback->currentFrame()));
    else if (action == QStringLiteral("set_out")) playback->setOutPoint(arguments.value(QStringLiteral("frame")).toInt(playback->currentFrame()));
    else if (action == QStringLiteral("clear_in_out")) playback->clearInOutPoints();
    else if (action == QStringLiteral("set_compare")) { const QString path = arguments.value(QStringLiteral("path")).toString(); if (!QFileInfo(path).isFile()) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("path must be an existing comparison media file.")}}); playback->setCompareFile(path); }
    else if (action == QStringLiteral("clear_compare")) playback->clearCompare();
    else if (action == QStringLiteral("set_volume")) { const double volume = arguments.value(QStringLiteral("volume")).toDouble(-1.0); if (volume < 0.0 || volume > 1.0) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("volume must be between 0 and 1.")}}); playback->setVolume(static_cast<float>(volume)); }
    else if (action == QStringLiteral("set_mute")) { if (!arguments.contains(QStringLiteral("muted"))) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("muted is required.")}}); playback->setMute(arguments.value(QStringLiteral("muted")).toBool()); }
    else if (action == QStringLiteral("set_audio_offset")) playback->setAudioOffset(arguments.value(QStringLiteral("offsetSeconds")).toDouble());
    else if (action == QStringLiteral("set_channel_mute")) { if (!arguments.contains(QStringLiteral("channel")) || !arguments.contains(QStringLiteral("muted"))) return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("channel and muted are required.")}}); playback->setChannelMute(arguments.value(QStringLiteral("channel")).toInt(), arguments.value(QStringLiteral("muted")).toBool()); }
    else return response(false, QJsonObject{{QStringLiteral("error"), QStringLiteral("Unsupported player control action.")}});
    QJsonObject result = status(); result.insert(QStringLiteral("action"), action); return response(true, result);
}

bool CodexPlugin::imageApiConfigured() const
{
    auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"));
    if (!settings) return false;
    const QUrl endpoint(settings->value(QStringLiteral("ai/imageGeneration/endpoint")).toString());
    return endpoint.isValid() && !endpoint.isRelative() && endpoint.scheme() == QStringLiteral("https") &&
        !endpoint.host().isEmpty() && !endpoint.userInfo().isEmpty() == false &&
        !settings->value(QStringLiteral("ai/imageGeneration/model")).toString().trimmed().isEmpty();
}

bool CodexPlugin::mediaApiConfigured(const QString& kind) const
{
    auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"));
    auto* credentials = ServiceLocator::getService<IAICredentialStore>();
    if (!settings || !credentials) return false;
    const QString prefix = kind == QStringLiteral("imageEditing") ? QStringLiteral("imageEditing") : kind + QStringLiteral("Generation");
    const QString configuredEndpoint = settings->value(QStringLiteral("ai/%1/endpoint").arg(prefix)).toString();
    const QUrl endpoint(configuredEndpoint.isEmpty() ? workbenchMediaEndpoint(settings, kind) : configuredEndpoint);
    const QString model = settings->value(QStringLiteral("ai/%1/model").arg(prefix)).toString().trimmed();
    const QString credential = kind == QStringLiteral("video") ? QStringLiteral("cgplay.ai.video-generation") : (kind == QStringLiteral("audio") ? QStringLiteral("cgplay.ai.audio-generation") : QStringLiteral("cgplay.ai.image-editing"));
    // API credentials are stored separately with DPAPI. A normal HTTPS endpoint
    // must not embed user info, otherwise ordinary configured providers never
    // become available as Codex tools.
    const QString resolvedModel = model.isEmpty() ? workbenchMediaModel(settings) : model;
    Q_UNUSED(credentials);
    Q_UNUSED(credential);
    // Register inherited media tools from the Workbench endpoint/model. The
    // credential is resolved only on execution, so an old dedicated-key check
    // cannot hide a valid Workbench capability from a newly started thread.
    return endpoint.isValid() && !endpoint.isRelative() && endpoint.scheme() == QStringLiteral("https") &&
        !endpoint.host().isEmpty() && endpoint.userInfo().isEmpty() && !resolvedModel.isEmpty();
}

QJsonObject CodexPlugin::executeMediaGeneration(const QString& kind, const QJsonObject& arguments)
{
    const auto result = [](bool success, const QJsonObject& payload) { return QJsonObject{{QStringLiteral("success"), success}, {QStringLiteral("contentItems"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("inputText")}, {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))}}}}}; };
    auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"));
    auto* credentials = ServiceLocator::getService<IAICredentialStore>();
    const QString prompt = arguments.value(QStringLiteral("prompt")).toString().trimmed();
    const QString prefix = kind == QStringLiteral("imageEditing") ? QStringLiteral("imageEditing") : kind + QStringLiteral("Generation");
    const QString credential = kind == QStringLiteral("video") ? QStringLiteral("cgplay.ai.video-generation") : (kind == QStringLiteral("audio") ? QStringLiteral("cgplay.ai.audio-generation") : QStringLiteral("cgplay.ai.image-editing"));
    if (!settings || !credentials || prompt.isEmpty() || !mediaApiConfigured(kind)) return result(false, {{QStringLiteral("error"), QStringLiteral("Media API is not configured.")}});
    QByteArray key = credentials->loadSecret(credential).trimmed();
    const QString configuredEndpoint = settings->value(QStringLiteral("ai/%1/endpoint").arg(prefix)).toString();
    const QUrl endpoint(configuredEndpoint.isEmpty() ? workbenchMediaEndpoint(settings, kind) : configuredEndpoint);
    QString model = settings->value(QStringLiteral("ai/%1/model").arg(prefix)).toString().trimmed();
    if (model.isEmpty()) model = workbenchMediaModel(settings);
    if (key.isEmpty()) key = loadAiWorkspaceSecret(credentials,
        settings->value(QStringLiteral("ai/workspace/providerId")).toString(),
        settings->value(QStringLiteral("ai/connection/providerName")).toString(),
        settings->value(QStringLiteral("ai/connection/detectedProtocol")).toString(),
        settings->value(QStringLiteral("ai/connection/baseUrl")).toString());
    if (key.isEmpty()) return result(false, {{QStringLiteral("error"), QStringLiteral("Workbench API key is unavailable.")}});
    JobContext mediaJob(15 * 60 * 1000);
    _activeNetworkJobs.insert(&mediaJob);
    const QPointer<CodexPlugin> mediaJobOwner(this);
    const auto mediaJobGuard = qScopeGuard([mediaJobOwner, &mediaJob] { if (mediaJobOwner) mediaJobOwner->_activeNetworkJobs.remove(&mediaJob); });
    if (kind == QStringLiteral("imageEditing")) {
        const QString sourcePath = _workspace ? _workspace->property("codexLatestGeneratedImage").toString() : QString();
        const QFileInfo source(sourcePath);
        if (!source.isFile()) { key.fill('\0'); return result(false, {{QStringLiteral("error"), QStringLiteral("Generate or attach a reference image before editing.")}}); }
        QFile imageFile(source.absoluteFilePath());
        if (!imageFile.open(QIODevice::ReadOnly)) { key.fill('\0'); return result(false, {{QStringLiteral("error"), QStringLiteral("Cannot read the reference image.")}}); }
        auto* multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        const auto addText = [multipart](const QByteArray& name, const QByteArray& value) { QHttpPart part; part.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(QStringLiteral("form-data; name=\"%1\"").arg(QString::fromLatin1(name)))); part.setBody(value); multipart->append(part); };
        addText("model", model.toUtf8()); addText("prompt", prompt.toUtf8()); addText("response_format", "b64_json");
        QHttpPart imagePart; imagePart.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("image/png")); imagePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(QStringLiteral("form-data; name=\"image\"; filename=\"reference.png\""))); imagePart.setBodyDevice(&imageFile); imageFile.setParent(multipart); multipart->append(imagePart);
        QNetworkAccessManager network; QNetworkRequest request(endpoint); request.setRawHeader("Authorization", QByteArray("Bearer ") + key); key.fill('\0'); QNetworkReply* reply = network.post(request, multipart); multipart->setParent(reply);
        const NetworkJobResult editRequest = waitForNetworkJob(reply, mediaJob, 180000);
        const QByteArray response = editRequest.body; QJsonParseError parseError; const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
        if (!editRequest.succeeded() || parseError.error != QJsonParseError::NoError) return result(false, {{QStringLiteral("error"), editRequest.state == JobState::TimedOut ? QStringLiteral("Image-edit request timed out.") : editRequest.errorString.left(300)}});
        const QJsonArray data = document.object().value(QStringLiteral("data")).toArray(); const QJsonObject first = data.isEmpty() ? QJsonObject() : data.first().toObject(); const QByteArray imageBytes = QByteArray::fromBase64(first.value(QStringLiteral("b64_json")).toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (imageBytes.isEmpty()) return result(false, {{QStringLiteral("error"), QStringLiteral("Image-edit API returned no valid b64_json image.")}});
        const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("generated_images")); QDir().mkpath(dir); const QString path = QDir(dir).filePath(QStringLiteral("cgplay_edit_%1.png").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))); QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly) || output.write(imageBytes) != imageBytes.size() || !output.commit()) return result(false, {{QStringLiteral("error"), QStringLiteral("Cannot save edited image.")}});
        const QJsonObject item{{QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("type"), QStringLiteral("imageGeneration")}, {QStringLiteral("status"), QStringLiteral("completed")}, {QStringLiteral("revisedPrompt"), first.value(QStringLiteral("revised_prompt")).toString(prompt)}, {QStringLiteral("savedPath"), path}};
        appendMediaAssetRecord(QStringLiteral("image-edit"), path, item);
        if (_workspace) _workspace->updateImageGeneration(item, true);
        return result(true, {{QStringLiteral("savedPath"), path}});
    }
    QNetworkAccessManager network;
    QNetworkRequest request(endpoint);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + key);
    key.fill('\0');
    QNetworkReply* reply = nullptr;
    if (kind == QStringLiteral("video")) {
        auto* multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        const auto addText = [multipart](const QByteArray& name, const QByteArray& value) {
            QHttpPart part;
            part.setHeader(QNetworkRequest::ContentDispositionHeader,
                QVariant(QStringLiteral("form-data; name=\"%1\"").arg(QString::fromLatin1(name))));
            part.setBody(value);
            multipart->append(part);
        };
        addText("model", model.toUtf8());
        addText("prompt", prompt.toUtf8());
        // OpenAI-compatible video APIs commonly accept this optional field for
        // image-to-video. Text-to-video providers simply ignore its absence.
        const QString sourcePath = _workspace ? _workspace->property("codexLatestGeneratedImage").toString() : QString();
        QFile* imageFile = new QFile(sourcePath, multipart);
        if (QFileInfo(sourcePath).isFile() && imageFile->open(QIODevice::ReadOnly)) {
            QHttpPart imagePart;
            imagePart.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("image/png"));
            imagePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                QVariant(QStringLiteral("form-data; name=\"input_reference\"; filename=\"reference.png\"")));
            imagePart.setBodyDevice(imageFile);
            multipart->append(imagePart);
        } else {
            imageFile->deleteLater();
        }
        reply = network.post(request, multipart);
        multipart->setParent(reply);
    } else {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        reply = network.post(request, QJsonDocument(QJsonObject{{QStringLiteral("model"), model}, {kind == QStringLiteral("audio") ? QStringLiteral("input") : QStringLiteral("prompt"), prompt}}).toJson(QJsonDocument::Compact));
    }
    const NetworkJobResult generationRequest = waitForNetworkJob(reply, mediaJob, 180000);
    const QByteArray response = generationRequest.body; const QString errorText = generationRequest.errorString; const QByteArray contentType = generationRequest.contentType; const int httpStatus = generationRequest.httpStatus;
    if (!generationRequest.succeeded()) {
        if (generationRequest.state == JobState::TimedOut) {
            return result(false, {{QStringLiteral("error"), QStringLiteral("Media generation request timed out.")}});
        }
        QJsonParseError errorParse;
        const QJsonDocument errorDocument = QJsonDocument::fromJson(response, &errorParse);
        QString providerMessage;
        if (errorParse.error == QJsonParseError::NoError) {
            providerMessage = errorDocument.object().value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
            if (providerMessage.isEmpty()) providerMessage = errorDocument.object().value(QStringLiteral("message")).toString();
        }
        if (providerMessage.isEmpty()) providerMessage = QString::fromUtf8(response).trimmed().left(500);
        return result(false, {{QStringLiteral("error"), QStringLiteral("Video API HTTP %1: %2").arg(httpStatus).arg(providerMessage.isEmpty() ? errorText.left(300) : providerMessage)}});
    }
    QJsonParseError parseError; const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    QString url;
    QString taskId;
    if (parseError.error == QJsonParseError::NoError) {
        const QJsonObject root = document.object(); const QJsonArray data = root.value(QStringLiteral("data")).toArray(); const QJsonObject first = data.isEmpty() ? QJsonObject() : data.first().toObject();
        url = first.value(QStringLiteral("url")).toString(); if (url.isEmpty()) url = root.value(QStringLiteral("url")).toString();
        taskId = root.value(QStringLiteral("task_id")).toString(); if (taskId.isEmpty()) taskId = root.value(QStringLiteral("id")).toString(); if (taskId.isEmpty()) taskId = first.value(QStringLiteral("id")).toString();
    }
    if (url.isEmpty() && !taskId.isEmpty()) {
        const QString statusTemplate = settings->value(QStringLiteral("ai/%1/statusEndpoint").arg(prefix)).toString();
        if (!statusTemplate.isEmpty()) {
            const QUrl statusUrl(statusTemplate.arg(taskId));
            QByteArray pollKey = credentials->loadSecret(credential).trimmed();
            if (pollKey.isEmpty()) pollKey = loadAiWorkspaceSecret(credentials,
                settings->value(QStringLiteral("ai/workspace/providerId")).toString(),
                settings->value(QStringLiteral("ai/connection/providerName")).toString(),
                settings->value(QStringLiteral("ai/connection/detectedProtocol")).toString(),
                settings->value(QStringLiteral("ai/connection/baseUrl")).toString());
            QElapsedTimer pollElapsed;
            pollElapsed.start();
            for (int attempt = 0; attempt < 120 && url.isEmpty() && pollElapsed.elapsed() < 10 * 60 * 1000 && !mediaJob.shouldStop(); ++attempt) {
                QNetworkRequest statusRequest(statusUrl); statusRequest.setRawHeader("Authorization", QByteArray("Bearer ") + pollKey);
                const NetworkJobResult statusResult = waitForNetworkJob(network.get(statusRequest), mediaJob, 15000);
                const QByteArray statusBytes = statusResult.body;
                if (!statusResult.succeeded()) { pollKey.fill('\0'); return result(false, {{QStringLiteral("error"), statusResult.state == JobState::TimedOut ? QStringLiteral("Media task status query timed out.") : QStringLiteral("Media task status query failed.")}, {QStringLiteral("taskId"), taskId}}); }
                QJsonParseError statusParseError; const QJsonDocument statusDocument = QJsonDocument::fromJson(statusBytes, &statusParseError);
                if (statusParseError.error != QJsonParseError::NoError || !statusDocument.isObject()) { pollKey.fill('\0'); return result(false, {{QStringLiteral("error"), QStringLiteral("Media task status returned invalid JSON.")}, {QStringLiteral("taskId"), taskId}}); }
                const QJsonObject status = statusDocument.object();
                const QJsonObject output = status.value(QStringLiteral("output")).toObject(); const QJsonArray statusData = status.value(QStringLiteral("data")).toArray(); const QJsonObject statusFirst = statusData.isEmpty() ? QJsonObject() : statusData.first().toObject();
                url = status.value(QStringLiteral("url")).toString(); if (url.isEmpty()) url = output.value(QStringLiteral("url")).toString(); if (url.isEmpty()) url = statusFirst.value(QStringLiteral("url")).toString();
                const QString state = status.value(QStringLiteral("status")).toString().toLower();
                if (state == QStringLiteral("failed") || state == QStringLiteral("error") || state == QStringLiteral("cancelled")) { pollKey.fill('\0'); return result(false, {{QStringLiteral("error"), status.value(QStringLiteral("error")).toString(QStringLiteral("Media task failed."))}, {QStringLiteral("taskId"), taskId}}); }
                if (url.isEmpty() && !waitForJobDelay(mediaJob, 5000)) break;
            }
            pollKey.fill('\0');
            if (url.isEmpty()) return result(false, {{QStringLiteral("error"), QStringLiteral("Media task did not finish within 10 minutes.")}, {QStringLiteral("taskId"), taskId}});
        }
    }
    if (!url.isEmpty()) {
        const NetworkJobResult downloadResult = waitForNetworkJob(network.get(QNetworkRequest(QUrl(url))), mediaJob, 180000);
        const QByteArray bytes = downloadResult.body;
        if (!downloadResult.succeeded() || bytes.isEmpty()) return result(false, {{QStringLiteral("error"), downloadResult.state == JobState::TimedOut ? QStringLiteral("Artifact download timed out.") : QStringLiteral("Provider returned an artifact URL, but download failed.")}, {QStringLiteral("url"), url}});
        const QString extension = kind == QStringLiteral("audio") ? QStringLiteral("mp3") : QStringLiteral("mp4"); const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("generated_media")); QDir().mkpath(dir); const QString path = QDir(dir).filePath(QStringLiteral("cgplay_%1.%2").arg(QUuid::createUuid().toString(QUuid::WithoutBraces), extension)); QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return result(false, {{QStringLiteral("error"), QStringLiteral("Cannot save downloaded media.")}});
        appendMediaAssetRecord(kind, path);
        if (auto* playback = reinterpret_cast<IPlaybackService*>(QCoreApplication::instance()->property("cgplay.codex.playbackService").toULongLong())) playback->openFile(path);
        return result(true, {{QStringLiteral("savedPath"), path}, {QStringLiteral("url"), url}});
    }
    if (contentType.startsWith("audio/") || contentType.startsWith("video/")) { const QString extension = kind == QStringLiteral("audio") ? QStringLiteral("mp3") : QStringLiteral("mp4"); const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("generated_media")); QDir().mkpath(dir); const QString path = QDir(dir).filePath(QStringLiteral("cgplay_%1.%2").arg(QUuid::createUuid().toString(QUuid::WithoutBraces), extension)); QSaveFile file(path); if (!file.open(QIODevice::WriteOnly) || file.write(response) != response.size() || !file.commit()) return result(false, {{QStringLiteral("error"), QStringLiteral("Cannot save generated media.")}}); appendMediaAssetRecord(kind, path); if (auto* playback = reinterpret_cast<IPlaybackService*>(QCoreApplication::instance()->property("cgplay.codex.playbackService").toULongLong())) playback->openFile(path); return result(true, {{QStringLiteral("savedPath"), path}}); }
    return result(false, {{QStringLiteral("error"), QStringLiteral("Provider returned an unsupported media response; configure a provider that returns a direct artifact URL or media bytes.")}});
}

QJsonObject CodexPlugin::executeImageGeneration(const QJsonObject& arguments)
{
    const auto result = [](bool success, const QJsonObject& payload) {
        return QJsonObject{{QStringLiteral("success"), success}, {QStringLiteral("contentItems"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("inputText")}, {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))}}}}};
    };
    auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"));
    auto* credentials = ServiceLocator::getService<IAICredentialStore>();
    const QString prompt = arguments.value(QStringLiteral("prompt")).toString().trimmed();
    const QString size = arguments.value(QStringLiteral("size")).toString(QStringLiteral("1024x1024"));
    if (!settings || !credentials || prompt.isEmpty() || prompt.size() > 4000) return result(false, {{QStringLiteral("error"), QStringLiteral("Invalid image request.")}});
    const QUrl endpoint(settings->value(QStringLiteral("ai/imageGeneration/endpoint")).toString());
    const QString model = settings->value(QStringLiteral("ai/imageGeneration/model")).toString().trimmed();
    QString credentialError;
    QByteArray key = credentials->loadSecret(QStringLiteral("cgplay.ai.image-generation"), &credentialError).trimmed();
    if (key.isEmpty()) key = loadImageCredentialFallback().trimmed();
    if (!endpoint.isValid() || endpoint.scheme() != QStringLiteral("https") || endpoint.host().isEmpty() || !endpoint.userInfo().isEmpty() || model.isEmpty() || key.isEmpty()) {
        key.fill('\0');
        return result(false, {{QStringLiteral("error"), QStringLiteral("Image API is not configured.")}});
    }

    QNetworkAccessManager network;
    QNetworkRequest request(endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + key);
    key.fill('\0');
    const QJsonObject body{{QStringLiteral("model"), model}, {QStringLiteral("prompt"), prompt}, {QStringLiteral("size"), size}, {QStringLiteral("n"), 1}, {QStringLiteral("response_format"), QStringLiteral("b64_json")}};
    JobContext imageJob(120000);
    _activeNetworkJobs.insert(&imageJob);
    const QPointer<CodexPlugin> imageJobOwner(this);
    const auto imageJobGuard = qScopeGuard([imageJobOwner, &imageJob] { if (imageJobOwner) imageJobOwner->_activeNetworkJobs.remove(&imageJob); });
    const NetworkJobResult requestResult = waitForNetworkJob(
        network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact)),
        imageJob,
        120000);
    const QByteArray response = requestResult.body;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    if (!requestResult.succeeded() || response.size() > 25 * 1024 * 1024) {
        const QString apiError = document.object().value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString().trimmed();
        const QString failure = requestResult.state == JobState::TimedOut
            ? QStringLiteral("Image generation timed out.")
            : (apiError.isEmpty() ? requestResult.errorString : apiError);
        return result(false, {{QStringLiteral("error"), failure.left(300)}});
    }
    const QJsonArray data = document.object().value(QStringLiteral("data")).toArray();
    const QJsonObject first = data.isEmpty() ? QJsonObject() : data.at(0).toObject();
    QByteArray imageBytes = QByteArray::fromBase64(first.value(QStringLiteral("b64_json")).toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (parseError.error != QJsonParseError::NoError || imageBytes.isEmpty() || imageBytes.size() > 20 * 1024 * 1024) {
        return result(false, {{QStringLiteral("error"), QStringLiteral("Image API returned no valid b64_json image.")}});
    }
    QBuffer imageBuffer(&imageBytes);
    imageBuffer.open(QIODevice::ReadOnly);
    QImageReader imageReader(&imageBuffer);
    imageReader.setDecideFormatFromContent(true);
    if (!imageReader.canRead()) return result(false, {{QStringLiteral("error"), QStringLiteral("Image API returned undecodable image data.")}});
    const QString outputDir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("generated_images"));
    if (!QDir().mkpath(outputDir)) return result(false, {{QStringLiteral("error"), QStringLiteral("Cannot create generated image directory.")}});
    const QString path = QDir(outputDir).filePath(QStringLiteral("cgplay_%1.png").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(imageBytes) != imageBytes.size() || !file.commit()) return result(false, {{QStringLiteral("error"), QStringLiteral("Cannot save generated image.")}});
    const QJsonObject item{{QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("type"), QStringLiteral("imageGeneration")}, {QStringLiteral("status"), QStringLiteral("completed")}, {QStringLiteral("revisedPrompt"), first.value(QStringLiteral("revised_prompt")).toString(prompt)}, {QStringLiteral("savedPath"), path}};
    appendMediaAssetRecord(QStringLiteral("image"), path, item);
    if (_workspace) {
        _workspace->setImageGenerationCapability(true, QString());
        _workspace->updateImageGeneration(item, true);
    }
    return result(true, {{QStringLiteral("savedPath"), path}, {QStringLiteral("message"), QStringLiteral("Image generated and shown in CGPlay.")}});
}

QJsonObject CodexPlugin::executeImageSearch(const QJsonObject& arguments)
{
    const auto result = [](bool success, const QJsonObject& payload, const QJsonArray& images = {}) {
        QJsonArray items{QJsonObject{{QStringLiteral("type"), QStringLiteral("inputText")}, {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))}}};
        for (const auto& value : images) items.push_back(value);
        return QJsonObject{{QStringLiteral("success"), success}, {QStringLiteral("contentItems"), items}};
    };
    const QString query = arguments.value(QStringLiteral("query")).toString().trimmed();
    const int limit = std::clamp(arguments.value(QStringLiteral("limit")).toInt(3), 1, 5);
    if (query.isEmpty() || query.size() > 240) return result(false, {{QStringLiteral("error"), QStringLiteral("Image search query is invalid.")}});
    QUrl url(QStringLiteral("https://commons.wikimedia.org/w/api.php"));
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("action"), QStringLiteral("query"));
    params.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    params.addQueryItem(QStringLiteral("origin"), QStringLiteral("*"));
    params.addQueryItem(QStringLiteral("generator"), QStringLiteral("search"));
    params.addQueryItem(QStringLiteral("gsrsearch"), query);
    params.addQueryItem(QStringLiteral("gsrnamespace"), QStringLiteral("6"));
    params.addQueryItem(QStringLiteral("gsrlimit"), QString::number(limit));
    params.addQueryItem(QStringLiteral("prop"), QStringLiteral("imageinfo"));
    params.addQueryItem(QStringLiteral("iiprop"), QStringLiteral("url|mime"));
    params.addQueryItem(QStringLiteral("iiurlwidth"), QStringLiteral("768"));
    url.setQuery(params);
    QNetworkAccessManager network;
    JobContext searchJob(20 * 1000 + limit * 20 * 1000);
    _activeNetworkJobs.insert(&searchJob);
    const QPointer<CodexPlugin> searchJobOwner(this);
    const auto searchJobGuard = qScopeGuard([searchJobOwner, &searchJob] { if (searchJobOwner) searchJobOwner->_activeNetworkJobs.remove(&searchJob); });
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "CGPlay/1.0 (image reference search)");
    const NetworkJobResult searchResult = waitForNetworkJob(network.get(request), searchJob, 20000);
    const QByteArray body = searchResult.body;
    if (!searchResult.succeeded()) return result(false, {{QStringLiteral("error"), searchResult.state == JobState::TimedOut ? QStringLiteral("Image search timed out.") : QStringLiteral("Image search failed: %1").arg(searchResult.errorString)}});
    QJsonParseError parseError; const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) return result(false, {{QStringLiteral("error"), QStringLiteral("Image search returned invalid JSON.")}});
    const QJsonObject pages = doc.object().value(QStringLiteral("query")).toObject().value(QStringLiteral("pages")).toObject();
    QJsonArray attachments; QJsonArray results;
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("image_search")); QDir().mkpath(dir);
    for (auto it = pages.constBegin(); it != pages.constEnd(); ++it) {
        const QJsonObject page = it.value().toObject(); const QJsonArray imageInfo = page.value(QStringLiteral("imageinfo")).toArray(); const QJsonObject info = imageInfo.isEmpty() ? QJsonObject() : imageInfo.first().toObject();
        const QString thumb = info.value(QStringLiteral("thumburl")).toString(); const QString source = info.value(QStringLiteral("descriptionurl")).toString();
        if (thumb.isEmpty()) continue;
        const NetworkJobResult imageResult = waitForNetworkJob(network.get(QNetworkRequest(QUrl(thumb))), searchJob, 20000);
        const QByteArray bytes = imageResult.body;
        if (!imageResult.succeeded() || bytes.isEmpty()) continue;
        const QString path = QDir(dir).filePath(QStringLiteral("ref_%1.jpg").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))); QSaveFile file(path); if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) continue;
        results.push_back(QJsonObject{{QStringLiteral("title"), page.value(QStringLiteral("title")).toString()}, {QStringLiteral("savedPath"), path}, {QStringLiteral("sourceUrl"), source}, {QStringLiteral("thumbnailUrl"), thumb}});
        QFile imageFile(path); if (!imageFile.open(QIODevice::ReadOnly)) continue; attachments.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("inputImage")}, {QStringLiteral("imageUrl"), QStringLiteral("data:image/jpeg;base64,%1").arg(QString::fromLatin1(imageFile.readAll().toBase64()))}});
    }
    return result(true, {{QStringLiteral("query"), query}, {QStringLiteral("count"), results.size()}, {QStringLiteral("results"), results}}, attachments);
}

void CodexPlugin::sendDynamicToolResponse(const QJsonValue& id, const QJsonObject& result)
{
    QString error;
    if (!sendJsonRpc(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("result"), result}}, &error)) reportProtocolError(error);
}


} // namespace cgplay
