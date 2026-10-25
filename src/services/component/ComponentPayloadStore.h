#pragma once

// Immutable component versions. This helper never renames or overwrites the
// running application, a previously selected payload, or the installed state.
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QUuid>
#include <functional>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cgplay::component_payload_store {

using Cancel = std::function<bool()>;
using Progress = std::function<void(int)>;

inline bool fail(QString* error, const QString& text)
{
    if (error) *error = text;
    return false;
}

inline bool safeRelative(const QString& value)
{
    if (value.isEmpty() || value.contains(QLatin1Char('\\')) ||
        value.contains(QLatin1Char(':')) || QDir::isAbsolutePath(value)) return false;
    const QString clean = QDir::cleanPath(value);
    return clean == value && clean != QStringLiteral(".") &&
        clean != QStringLiteral("..") && !clean.startsWith(QStringLiteral("../"));
}

inline bool linked(const QString& path)
{
    const QFileInfo file(path);
    if (file.isSymLink()) return true;
#ifdef Q_OS_WIN
    const DWORD flags = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    if (flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    return false;
}

inline bool containedDirectory(const QString& root, const QString& candidate)
{
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    const QString canonical = QFileInfo(candidate).canonicalFilePath();
    if (canonicalRoot.isEmpty() || canonical.isEmpty()) return false;
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    if (canonical.compare(canonicalRoot, sensitivity) != 0 &&
        !canonical.startsWith(canonicalRoot + QLatin1Char('/'), sensitivity)) return false;
    for (QFileInfo part(candidate); !part.absoluteFilePath().isEmpty();) {
        if (linked(part.absoluteFilePath())) return false;
        if (part.absoluteFilePath().compare(QFileInfo(root).absoluteFilePath(), sensitivity) == 0) return true;
        const QFileInfo parent(part.absolutePath());
        if (parent.absoluteFilePath() == part.absoluteFilePath()) return false;
        part = parent;
    }
    return false;
}

inline QString resolvePublishedRoot(const QString& componentsRoot, const QString& id,
                                    const QString& relative)
{
    const QString prefix = QStringLiteral("payloads/") + id + QLatin1Char('/');
    if (!safeRelative(relative) || !relative.startsWith(prefix) ||
        relative.mid(prefix.size()).contains(QLatin1Char('/'))) return {};
    const QString root = QDir(componentsRoot).filePath(relative);
    return containedDirectory(componentsRoot, root) && QFileInfo(root).isDir() ? root : QString{};
}

inline bool requiredFilesPresent(const QString& root, const QStringList& required,
                                 QString* error)
{
    if (required.isEmpty()) return fail(error, QStringLiteral("Component requiredFiles is empty"));
    for (const QString& relative : required) {
        if (!safeRelative(relative)) return fail(error, QStringLiteral("Unsafe required file: %1").arg(relative));
        const QFileInfo file(QDir(root).filePath(relative));
        if (!file.isFile() || file.size() <= 0 || linked(file.absoluteFilePath()) ||
            !containedDirectory(root, file.absolutePath())) {
            return fail(error, QStringLiteral("Incomplete component payload: %1").arg(relative));
        }
    }
    return true;
}

inline bool digestFile(const QString& path, QByteArray* digest, const Cancel& canceled,
                       QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, file.errorString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (canceled && canceled()) return fail(error, QStringLiteral("Component installation canceled"));
        const QByteArray bytes = file.read(1024 * 1024);
        if (bytes.isEmpty() && file.error() != QFileDevice::NoError) return fail(error, file.errorString());
        hash.addData(bytes);
    }
    *digest = hash.result();
    return true;
}

inline bool prepare(const QString& extractedRoot, const QString& componentsRoot,
                    const QString& id, const QString& version, const QStringList& required,
                    QJsonObject* result, QString* error,
                    const Cancel& canceled = {}, const Progress& progress = {})
{
    if (id == QStringLiteral("core")) return fail(error, QStringLiteral("Core updates require the full installer after exit"));
    if (!safeRelative(id) || id.contains(QLatin1Char('/')) || !result)
        return fail(error, QStringLiteral("Unsafe component identity"));
    if (canceled && canceled()) return fail(error, QStringLiteral("Component installation canceled"));
    if (!containedDirectory(extractedRoot, extractedRoot)) return fail(error, QStringLiteral("Unsafe extraction root"));
    QString payloadRoot = extractedRoot;
    QString validationError;
    if (!requiredFilesPresent(payloadRoot, required, &validationError)) {
        const auto dirs = QDir(extractedRoot).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        // A root with files is not silently discarded merely because it also
        // has one cgplay/ child. Strip a wrapper only when requiredFiles prove it.
        if (dirs.size() != 1 || linked(dirs.front().absoluteFilePath()) ||
            !requiredFilesPresent(dirs.front().absoluteFilePath(), required, &validationError))
            return fail(error, validationError);
        payloadRoot = dirs.front().absoluteFilePath();
    }
    QDirIterator entries(payloadRoot, QDir::AllEntries | QDir::Hidden | QDir::System |
                          QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    QStringList files;
    while (entries.hasNext()) {
        entries.next();
        const QFileInfo file = entries.fileInfo();
        if (linked(file.absoluteFilePath())) return fail(error, QStringLiteral("Linked payload entry is forbidden"));
        if (file.isFile()) files.push_back(file.absoluteFilePath());
        else if (!file.isDir()) return fail(error, QStringLiteral("Unsupported payload entry"));
    }
    files.sort();
    QDir().mkpath(componentsRoot);
    if (linked(componentsRoot)) return fail(error, QStringLiteral("Linked components root is forbidden"));
    QTemporaryDir staging(QDir(componentsRoot).filePath(QStringLiteral(".component-stage-XXXXXX")));
    if (!staging.isValid()) return fail(error, QStringLiteral("Cannot create component staging directory"));
    const QString staged = QDir(staging.path()).filePath(QStringLiteral("payload"));
    if (!QDir().mkpath(staged)) return fail(error, QStringLiteral("Cannot create staged payload"));
    QJsonArray inventory;
    for (int index = 0; index < files.size(); ++index) {
        if (canceled && canceled()) return fail(error, QStringLiteral("Component installation canceled"));
        const QString relative = QDir(payloadRoot).relativeFilePath(files[index]);
        if (!safeRelative(relative)) return fail(error, QStringLiteral("Unsafe payload path"));
        const QString target = QDir(staged).filePath(relative);
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) return fail(error, QStringLiteral("Cannot create payload directory"));
        QFile source(files[index]), destination(target);
        if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return fail(error, QStringLiteral("Cannot stage component file: %1").arg(relative));
        QCryptographicHash sourceHash(QCryptographicHash::Sha256);
        while (!source.atEnd()) {
            if (canceled && canceled()) return fail(error, QStringLiteral("Component installation canceled"));
            const QByteArray bytes = source.read(1024 * 1024);
            if (bytes.isEmpty() && source.error() != QFileDevice::NoError) return fail(error, source.errorString());
            if (destination.write(bytes) != bytes.size()) return fail(error, destination.errorString());
            sourceHash.addData(bytes);
        }
        if (!destination.flush()) return fail(error, destination.errorString());
        destination.close();
        QByteArray copiedHash;
        if (!digestFile(target, &copiedHash, canceled, error) || copiedHash != sourceHash.result())
            return fail(error, QStringLiteral("Staged file digest mismatch: %1").arg(relative));
        inventory.append(QJsonObject{{QStringLiteral("path"), relative},
            {QStringLiteral("sha256"), QString::fromLatin1(copiedHash.toHex())},
            {QStringLiteral("size"), static_cast<double>(QFileInfo(target).size())}});
        if (progress) progress((index + 1) * 100 / qMax(1, files.size()));
    }
    if (!requiredFilesPresent(staged, required, error)) return false;
    if (canceled && canceled()) return fail(error, QStringLiteral("Component installation canceled"));
    const QString relative = QStringLiteral("payloads/%1/%2").arg(id,
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString published = QDir(componentsRoot).filePath(relative);
    if (!QDir().mkpath(QFileInfo(published).absolutePath()) ||
        !containedDirectory(componentsRoot, QFileInfo(published).absolutePath()) ||
        QFileInfo::exists(published) || !QDir().rename(staged, published))
        return fail(error, QStringLiteral("Cannot publish immutable component payload"));
    *result = QJsonObject{{QStringLiteral("payloadRoot"), relative},
        {QStringLiteral("absoluteRoot"), published}, {QStringLiteral("version"), version},
        {QStringLiteral("files"), inventory}};
    // The old selected root remains intact even if the state commit fails or
    // the process exits here. An unselected immutable directory is harmless;
    // later background GC may remove it after proving that no job uses it.
    return true;
}

inline bool writeState(const QString& path, const QJsonObject& state, QString* error)
{
    const QByteArray bytes = QJsonDocument(state).toJson(QJsonDocument::Indented);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(error, QStringLiteral("Component state commit failed: %1").arg(file.errorString()));
    return true;
}

inline bool runRuntimeProbe(const QString& executable, const QStringList& arguments,
                            const Cancel& canceled, QString* error, QByteArray* standardOutput = nullptr)
{
    if (executable.isEmpty() || !QFileInfo(executable).isFile()) return fail(error, QStringLiteral("Missing validation executable"));
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("PYTHONHOME"));
    environment.remove(QStringLiteral("PYTHONPATH"));
    process.setProcessEnvironment(environment);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(executable, arguments, QIODevice::ReadOnly);
    QElapsedTimer elapsed;
    elapsed.start();
    QByteArray output;
    QByteArray diagnostics;
    while (process.state() != QProcess::NotRunning) {
        if ((canceled && canceled()) || elapsed.elapsed() > 60000) {
            process.kill();
            process.waitForFinished(1000);
            return fail(error, QStringLiteral("Component runtime validation canceled or timed out"));
        }
        process.waitForFinished(25);
        output = (output + process.readAllStandardOutput()).right(16384);
        diagnostics = (diagnostics + process.readAllStandardError()).right(16384);
    }
    output = (output + process.readAllStandardOutput()).right(16384);
    diagnostics = (diagnostics + process.readAllStandardError()).right(16384);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
        process.error() == QProcess::FailedToStart)
        return fail(error, QStringLiteral("Component runtime validation failed: %1 %2")
            .arg(process.errorString(), QString::fromUtf8(output + diagnostics)));
    if (standardOutput) *standardOutput = output;
    return true;
}

inline bool validateRuntime(const QString& id, const QString& publishedRoot,
                            const QString& selectedPython, const Cancel& canceled, QString* error)
{
    if (id == QStringLiteral("recording-runtime")) {
        QByteArray output;
        if (!runRuntimeProbe(QDir(publishedRoot).filePath(QStringLiteral("CGPlayRecorder.exe")),
                {QStringLiteral("--check-runtime")}, canceled, error, &output)) return false;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(output, &parseError);
        const QJsonObject probe = document.object();
        const QByteArray manifestDigest = probe.value(QStringLiteral("manifestSHA256")).toString().toLatin1();
        bool validDigest = manifestDigest.size() == 64;
        for (const char ch : manifestDigest)
            validDigest = validDigest && ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
        if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
            probe.value(QStringLiteral("schema")).toInt() != 1 || !probe.value(QStringLiteral("ok")).toBool() ||
            probe.value(QStringLiteral("obsVersion")).toString() != QStringLiteral("33.0.0") ||
            probe.value(QStringLiteral("obsApi")).toInt() != 553648128 ||
            probe.value(QStringLiteral("sourceCommit")).toString() != QStringLiteral("72e86655715d9a25d69ce446aa4dd44ba58e10d2") ||
            probe.value(QStringLiteral("capturePatch")).toInt() != 1 ||
            probe.value(QStringLiteral("wasapiPatch")).toInt() != 1 || !validDigest)
            return fail(error, QStringLiteral("Recorder runtime identity or capture-module validation failed"));
        return true;
    }
    if (id == QStringLiteral("python-runtime")) {
        return runRuntimeProbe(QDir(publishedRoot).filePath(QStringLiteral("python.exe")),
            {QStringLiteral("-I"), QStringLiteral("-B"), QStringLiteral("-c"),
             QStringLiteral("import encodings, av, numpy, OpenImageIO, PyOpenColorIO, PIL, requests, certifi; print('runtime-ok')")},
             canceled, error);
    }
    if (id == QStringLiteral("ffmpeg")) {
        for (const QString& executable : {QStringLiteral("ffmpeg.exe"), QStringLiteral("ffprobe.exe")})
            if (!runRuntimeProbe(QDir(publishedRoot).filePath(executable), {QStringLiteral("-version")}, canceled, error)) return false;
    }
    if (id == QStringLiteral("tools")) {
        return runRuntimeProbe(selectedPython,
            {QStringLiteral("-I"), QStringLiteral("-B"), QStringLiteral("-c"),
             QStringLiteral("import ast,pathlib,sys; files=list(pathlib.Path(sys.argv[1]).rglob('*.py')); assert files; [ast.parse(p.read_text(encoding='utf-8-sig'),filename=str(p)) for p in files]; print('tools-ok')"),
             publishedRoot}, canceled, error);
    }
    return true;
}

} // namespace cgplay::component_payload_store
