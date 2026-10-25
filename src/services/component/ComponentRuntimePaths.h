#pragma once

#include "ComponentManager.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStringList>

namespace cgplay {

// Capture once on the requesting thread, then pass by value into the job.
// Previously selected immutable payloads are retained for running processes.
struct ComponentRuntimePaths
{
    bool captured = false;
    QString pythonExecutable;
    QString pythonHome;
    QString toolsRoot;
    QString ffmpegExecutable;
    QString ffprobeExecutable;
    QString recordingRoot;
    QString recordingExecutable;
    QString recordingGeneration;
    QHash<QString, QString> pythonOverrides;
    QString localTranscribeOverride;

    QString script(const QString& relative) const
    {
        if (toolsRoot.isEmpty() || relative.isEmpty() || QDir::isAbsolutePath(relative) ||
            relative.contains(QLatin1Char(':')) || relative.contains(QLatin1Char('\\')) ||
            QDir::cleanPath(relative) != relative || relative.startsWith(QStringLiteral("../"))) return {};
        const QString path = QDir(toolsRoot).filePath(relative);
        return QFileInfo(path).isFile() ? QFileInfo(path).absoluteFilePath() : QString{};
    }

    QString pythonFor(const QString& overrideKey = {}) const
    {
        if (overrideKey == QStringLiteral("SUBTITLE_LONG_ASR_PYTHON") &&
            !pythonOverrides.contains(overrideKey) && pythonOverrides.contains(QStringLiteral("SUBTITLE_OCR_PYTHON")))
            return pythonOverrides.value(QStringLiteral("SUBTITLE_OCR_PYTHON"));
        return pythonOverrides.value(overrideKey, pythonExecutable);
    }

    QProcessEnvironment pythonEnvironment(const QString& overrideKey = {}) const
    {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
        if (pythonFor(overrideKey) == pythonExecutable && !pythonHome.isEmpty())
            environment.insert(QStringLiteral("PYTHONHOME"), pythonHome);
        if (!toolsRoot.isEmpty()) {
            const QString old = environment.value(QStringLiteral("PYTHONPATH"));
            environment.insert(QStringLiteral("PYTHONPATH"), toolsRoot + (old.isEmpty() ? QString{} : QDir::listSeparator() + old));
        }
        return environment;
    }
};

inline ComponentRuntimePaths captureComponentRuntimePaths()
{
    const QJsonObject roots = ComponentManager::instance().runtimeRootsSnapshot();
    ComponentRuntimePaths result;
    result.captured = true;
    const QString appDir = QCoreApplication::applicationDirPath();
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString& key : {QStringLiteral("CGPLAY_PYTHON_EXE"), QStringLiteral("SUBTITLE_ONLINE_PYTHON"),
                              QStringLiteral("SUBTITLE_OCR_PYTHON"), QStringLiteral("SUBTITLE_LONG_ASR_PYTHON")}) {
        const QString value = environment.value(key).trimmed();
        if (!value.isEmpty()) result.pythonOverrides.insert(key, value);
    }
    result.localTranscribeOverride = environment.value(QStringLiteral("CGPLAY_LOCAL_TRANSCRIBE_SCRIPT")).trimmed();
    const auto root = [&](const QString& id) { return roots.value(id).toObject().value(QStringLiteral("root")).toString(); };
    const auto managed = [&](const QString& id) { return roots.value(id).toObject().value(QStringLiteral("managed")).toBool(); };
    const auto existing = [](const QStringList& paths) {
        for (const QString& path : paths) if (!path.isEmpty() && QFileInfo(path).isFile()) return QFileInfo(path).absoluteFilePath();
        return QString{};
    };
    result.recordingRoot = root(QStringLiteral("recording-runtime"));
    if (!result.recordingRoot.isEmpty()) {
        result.recordingExecutable = existing({QDir(result.recordingRoot).filePath(QStringLiteral("CGPlayRecorder.exe"))});
        result.recordingGeneration = result.recordingRoot + QLatin1Char('|') +
            roots.value(QStringLiteral("recording-runtime")).toObject().value(QStringLiteral("version")).toString();
    }
    QStringList pythons;
    const QString pythonRoot = root(QStringLiteral("python-runtime"));
    if (!pythonRoot.isEmpty()) pythons << QDir(pythonRoot).filePath(QStringLiteral("python.exe"));
    if (!managed(QStringLiteral("python-runtime"))) {
        pythons << QDir(appDir).filePath(QStringLiteral("runtime/python/python.exe"))
                << QDir(appDir).filePath(QStringLiteral("python/python.exe"))
                << QDir(appDir).filePath(QStringLiteral("../runtime/python/python.exe"))
                << QStandardPaths::findExecutable(QStringLiteral("python"))
                << QStandardPaths::findExecutable(QStringLiteral("python3"));
    }
    result.pythonExecutable = existing(pythons);
    if (!result.pythonExecutable.isEmpty() && !pythonRoot.isEmpty() &&
        result.pythonExecutable == QFileInfo(QDir(pythonRoot).filePath(QStringLiteral("python.exe"))).absoluteFilePath())
        result.pythonHome = pythonRoot;
    const QString tools = root(QStringLiteral("tools"));
    if (!tools.isEmpty() && QFileInfo(tools).isDir()) result.toolsRoot = tools;
    if (result.toolsRoot.isEmpty() && !managed(QStringLiteral("tools"))) {
        for (const QString& relative : {QStringLiteral("tools"), QStringLiteral("../tools"),
                                      QStringLiteral("../../tools"), QStringLiteral("../../../tools")}) {
            const QString path = QDir(appDir).filePath(relative);
            if (QFileInfo(QDir(path).filePath(QStringLiteral("cgplay/export/export_engine.py"))).isFile()) {
                result.toolsRoot = QFileInfo(path).absoluteFilePath();
                break;
            }
        }
    }
    const QString ffmpegRoot = root(QStringLiteral("ffmpeg"));
    for (const QString& tool : {QStringLiteral("ffmpeg.exe"), QStringLiteral("ffprobe.exe")}) {
        QStringList candidates;
        if (!ffmpegRoot.isEmpty()) candidates << QDir(ffmpegRoot).filePath(tool);
        if (!managed(QStringLiteral("ffmpeg"))) candidates << QDir(appDir).filePath(tool) << QStandardPaths::findExecutable(tool);
        const QString selected = existing(candidates);
        if (tool == QStringLiteral("ffmpeg.exe")) result.ffmpegExecutable = selected;
        else result.ffprobeExecutable = selected;
    }
    return result;
}

} // namespace cgplay
