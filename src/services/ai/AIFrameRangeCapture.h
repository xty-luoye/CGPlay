#pragma once
// Value-only range decoding, invoked inside the provider worker before any API request.
#include "ai/api/AIProviderTypes.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QTemporaryDir>
#include <atomic>
#include <memory>

namespace cgplay {
inline bool prepareDeferredAIFrames(AIRequest* request,
    const std::shared_ptr<std::atomic_bool>& canceled, QString* error)
{
    const auto fail = [error](const QString& message) { if (error) *error = message; return false; };
    const auto stopped = [&] { return canceled && canceled->load(); };
    if (stopped()) return fail(QStringLiteral("Canceled"));
    const auto plan = request->options.value(QStringLiteral("rangeCaptureJob")).toObject();
    if (plan.isEmpty()) return true;
    if (request->context.scope != AIRequestScope::CurrentShot && request->context.scope != AIRequestScope::CurrentSequence)
        return fail(QStringLiteral("Deferred frame capture requires a range scope"));
    const auto samples = plan.value(QStringLiteral("samples")).toArray();
    if (samples.isEmpty() || samples.size() > 32) return fail(QStringLiteral("Invalid deferred sample count"));
    QTemporaryDir temporary;
    if (!temporary.isValid()) return fail(QStringLiteral("Cannot create AI frame capture directory"));
    const QString jobPath = temporary.filePath(QStringLiteral("job.json"));
    QSaveFile job(jobPath);
    const QByteArray payload = QJsonDocument(plan).toJson(QJsonDocument::Compact);
    if (!job.open(QIODevice::WriteOnly) || job.write(payload) != payload.size() || !job.commit())
        return fail(QStringLiteral("Cannot write AI frame capture job"));
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    environment.insert(QStringLiteral("PYTHONPATH"), plan.value(QStringLiteral("toolsRoot")).toString());
    const QString home = plan.value(QStringLiteral("pythonHome")).toString();
    if (!home.isEmpty()) environment.insert(QStringLiteral("PYTHONHOME"), home);
    process.setProcessEnvironment(environment);
    process.start(plan.value(QStringLiteral("python")).toString(),
        {plan.value(QStringLiteral("helper")).toString(), QStringLiteral("--job"), jobPath,
         QStringLiteral("--output"), temporary.path()}, QIODevice::ReadOnly);
    if (!process.waitForStarted(5000)) return fail(QStringLiteral("Cannot start AI range decoder: %1").arg(process.errorString()));
    QElapsedTimer timer; timer.start();
    while (!process.waitForFinished(25)) {
        if (stopped() || timer.elapsed() > 90000) {
            process.kill(); process.waitForFinished(5000);
            return fail(stopped() ? QStringLiteral("Canceled") : QStringLiteral("AI range decoder timed out"));
        }
    }
    if (stopped()) return fail(QStringLiteral("Canceled"));
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return fail(QStringLiteral("AI range decoder failed: %1").arg(QString::fromUtf8(process.readAllStandardError()).right(1200)));
    const auto report = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    if (!report.value(QStringLiteral("success")).toBool() ||
        report.value(QStringLiteral("frameCount")).toInt() != samples.size())
        return fail(QStringLiteral("AI range decoder returned incomplete frames"));
    QVector<AIMediaFrameReference> frames;
    qint64 bytes = 0;
    for (int i = 0; i < samples.size(); ++i) {
        if (stopped()) return fail(QStringLiteral("Canceled"));
        QFile file(temporary.filePath(QStringLiteral("frame_%1.png").arg(i)));
        if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
            return fail(QStringLiteral("Missing or oversized AI sample"));
        AIMediaFrameReference frame;
        frame.mediaPath = request->context.mediaPath;
        frame.frame = samples[i].toObject().value(QStringLiteral("frame")).toInt();
        frame.encodedBytes = file.readAll();
        bytes += frame.encodedBytes.size();
        QImage image; image.loadFromData(frame.encodedBytes, "PNG");
        if (image.isNull() || bytes > 64 * 1024 * 1024) return fail(QStringLiteral("Invalid or oversized AI frame set"));
        frame.size = image.size(); frame.mimeType = QStringLiteral("image/png");
        frame.metadata = {{QStringLiteral("captureMode"), QStringLiteral("decoded-range")},
            {QStringLiteral("frameResolved"), frame.frame},
            {QStringLiteral("requestedTimeSeconds"), frame.frame / request->context.fps}};
        frames.append(std::move(frame));
    }
    request->frames = std::move(frames);
    return true;
}
}
