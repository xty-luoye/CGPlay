#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>

namespace cgplay {

// Resolve existing ancestors too, so a not-yet-created destination below a
// directory link is compared against the directory it will actually use.
inline QString resolvedCopyPath(const QString& path)
{
    QFileInfo entry(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    QStringList tail;
    while (!entry.exists() && !entry.isRoot()) {
        tail.prepend(entry.fileName());
        entry.setFile(entry.absolutePath());
    }
    QString resolved = entry.canonicalFilePath();
    if (resolved.isEmpty()) resolved = entry.absoluteFilePath();
    for (const auto& part : tail) resolved = QDir(resolved).filePath(part);
    return QDir::cleanPath(resolved);
}

inline Qt::CaseSensitivity copyPathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

inline bool copyFileAtomically(const QString& source, const QString& destination,
                               QString* error = nullptr)
{
    QFile input(source);
    if (!QFileInfo(source).isFile() || !input.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Cannot read source file: %1").arg(source);
        return false;
    }
    if (resolvedCopyPath(source).compare(resolvedCopyPath(destination),
                                        copyPathCaseSensitivity()) == 0) return true;
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        if (error) *error = QStringLiteral("Cannot create destination directory.");
        return false;
    }
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error) *error = output.errorString();
        return false;
    }
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (true) {
        const qint64 count = input.read(buffer.data(), buffer.size());
        if (count < 0 || (count > 0 && output.write(buffer.constData(), count) != count)) {
            if (error) *error = count < 0 ? input.errorString() : output.errorString();
            output.cancelWriting();
            return false;
        }
        if (count == 0) break;
    }
    if (!output.commit()) {
        if (error) *error = output.errorString();
        return false;
    }
    return true;
}

} // namespace cgplay
