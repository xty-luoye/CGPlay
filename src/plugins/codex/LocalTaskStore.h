#pragma once

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace cgplay::localTaskStore {

// Close the read handle before any QSaveFile replaces the queue on Windows.
inline bool read(const QString& path, QJsonArray* tasks, QString* error)
{
    QFile input(path);
    QJsonArray snapshot;
    if (!input.exists()) { *tasks = snapshot; return true; }
    if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = input.errorString();
        return false;
    }
    while (!input.atEnd()) {
        const QByteArray line = input.readLine();
        if (line.trimmed().isEmpty()) continue;
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            if (error) *error = QStringLiteral("Invalid local task queue record: %1").arg(path);
            return false;
        }
        snapshot.append(document.object());
    }
    if (input.error() != QFileDevice::NoError) {
        if (error) *error = input.errorString();
        return false;
    }
    input.close();
    *tasks = snapshot;
    return true;
}

inline bool write(const QString& path, const QJsonArray& tasks, QString* error)
{
    QByteArray bytes;
    for (const QJsonValue& task : tasks) {
        bytes += QJsonDocument(task.toObject()).toJson(QJsonDocument::Compact);
        bytes += '\n';
    }
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
        if (error) *error = output.errorString();
        return false;
    }
    return true;
}

} // namespace cgplay::localTaskStore
