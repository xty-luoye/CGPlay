#include "ComponentManager.h"
#include "DownloadService.h"
#include "common/jobs/JobSystem.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>

namespace cgplay {

namespace {

QString normalizeComponentId(const QString& id)
{
    return id.trimmed().toLower();
}

QString asSlashPath(const QString& path)
{
    return QDir::fromNativeSeparators(path);
}

QString preferredArchiveName(const QString& url, const QString& fallback)
{
    const QString clean = QUrl(url).fileName();
    if (!clean.isEmpty()) {
        return clean;
    }
    return fallback;
}

QStringList jsonStringList(const QJsonObject& object, const QString& key)
{
    QStringList values;
    const QJsonArray array = object.value(key).toArray();
    for (const auto& value : array) {
        const QString text = value.toString().trimmed();
        if (!text.isEmpty() && !values.contains(text)) {
            values.push_back(text);
        }
    }
    return values;
}

void writeComponentLog(const QString& message)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    QFile file(QDir(dir).filePath(QStringLiteral("component-download.log")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    const QString line = QStringLiteral("[%1] %2\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), message);
    file.write(line.toUtf8());
}

} // namespace

bool ComponentSpec::isValid() const
{
    return !id.isEmpty() && !version.isEmpty();
}

ComponentManager& ComponentManager::instance()
{
    static ComponentManager manager;
    return manager;
}

ComponentManager::ComponentManager(QObject* parent)
    : QObject(parent)
{
    _loadBundledManifest();
    _loadCachedManifest();
    _loadState();
}

void ComponentManager::setManifestUrl(const QString& url)
{
    _manifestUrl = url.trimmed();
}

QString ComponentManager::manifestUrl() const
{
    return _manifestUrl;
}

bool ComponentManager::refreshRemoteManifest(QString* error)
{
    const QStringList urls = _resolveManifestUrls();
    if (urls.isEmpty()) {
        return false;
    }

    QStringList errors;
    for (const QString& url : urls) {
        writeComponentLog(QStringLiteral("refresh manifest: %1").arg(url));
        QByteArray json;
        QString downloadError;
        if (!DownloadService::instance().downloadBytesFromUrls({url}, &json, nullptr, &downloadError)) {
            writeComponentLog(QStringLiteral("manifest failed: %1").arg(downloadError));
            errors.push_back(downloadError);
            continue;
        }

        QString parseError;
        if (!_loadManifestDocument(json, &parseError)) {
            writeComponentLog(QStringLiteral("manifest parse failed: %1").arg(parseError));
            errors.push_back(parseError);
            continue;
        }

        QFile cacheFile(manifestCachePath());
        QDir().mkpath(QFileInfo(cacheFile).absolutePath());
        if (cacheFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            cacheFile.write(json);
            cacheFile.close();
        }
        writeComponentLog(QStringLiteral("manifest refreshed ok: %1").arg(url));
        return true;
    }

    if (error) {
        *error = errors.join(QStringLiteral("\n\n"));
    }
    return false;
}

bool ComponentManager::hasComponent(const QString& id) const
{
    return _componentSpec(id).isValid();
}

QStringList ComponentManager::componentIds() const
{
    QStringList ids;
    const QJsonObject components = _manifestRoot.value(QStringLiteral("components")).toObject();
    for (auto it = components.constBegin(); it != components.constEnd(); ++it) {
        ids.push_back(it.key());
    }
    ids.sort();
    return ids;
}

QStringList ComponentManager::missingRequiredComponents() const
{
    QStringList missing;
    const QJsonObject components = _manifestRoot.value(QStringLiteral("components")).toObject();
    for (auto it = components.constBegin(); it != components.constEnd(); ++it) {
        const ComponentSpec spec = _componentSpec(it.key());
        if (!spec.isValid() || spec.optional || spec.id == QStringLiteral("core")) {
            continue;
        }
        if (!_isInstalled(spec)) {
            missing.push_back(spec.id);
        }
    }
    missing.sort();
    return missing;
}

QString ComponentManager::remoteAppVersion() const
{
    return _manifestRoot.value(QStringLiteral("appVersion")).toString().trimmed();
}

qint64 ComponentManager::remoteInstallerSize(const QString& mode) const
{
    const QJsonObject installers = _manifestRoot.value(QStringLiteral("installers")).toObject();
    const QJsonObject installer = installers.value(mode.trimmed().toLower()).toObject();
    return static_cast<qint64>(installer.value(QStringLiteral("size")).toDouble(0));
}

bool ComponentManager::isAppUpdateAvailable(const QString& currentVersion) const
{
    const QString remote = remoteAppVersion();
    if (remote.isEmpty()) {
        return false;
    }

    const QVersionNumber remoteVersion = QVersionNumber::fromString(remote);
    const QVersionNumber localVersion = QVersionNumber::fromString(currentVersion.trimmed());
    if (!remoteVersion.isNull() && !localVersion.isNull()) {
        return QVersionNumber::compare(remoteVersion, localVersion) > 0;
    }

    return remote != currentVersion.trimmed();
}

bool ComponentManager::downloadFromUrls(
    const QStringList& urls,
    QByteArray* out,
    QWidget* parentWidget,
    QString* error)
{
    return DownloadService::instance().downloadBytesFromUrls(urls, out, parentWidget, error);
}

bool ComponentManager::downloadFileFromUrls(
    const QStringList& urls,
    const QString& destinationPath,
    QWidget* parentWidget,
    QString* error)
{
    return DownloadService::instance().downloadFileFromUrls(urls, destinationPath, parentWidget, error);
}

bool ComponentManager::ensureComponent(
    const QString& id,
    QWidget* parentWidget,
    bool allowDownloadPrompt,
    QString* error)
{
    const ComponentSpec spec = _componentSpec(id);
    if (!spec.isValid()) {
        if (error) {
            *error = QStringLiteral("Unknown component: %1").arg(id);
        }
        return false;
    }

    if (_isInstalled(spec)) {
        return true;
    }

    if (spec.urls.isEmpty()) {
        if (error) {
            *error = QObject::tr("组件 %1 未安装，且当前 manifest 没有可下载地址。").arg(spec.id);
        }
        return false;
    }

    if (allowDownloadPrompt) {
        const auto reply = QMessageBox::question(
            parentWidget,
            QObject::tr("需要下载组件"),
            QObject::tr("当前功能需要组件“%1”。\n\n是否现在自动下载并安装？")
                .arg(spec.id),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::Yes);
        if (reply != QMessageBox::Yes) {
            if (error) {
                *error = QObject::tr("用户取消下载组件：%1").arg(spec.id);
            }
            return false;
        }
    }

    return _downloadAndInstall(spec, parentWidget, error);
}

QString ComponentManager::componentExecutablePath(const QString& id, const QString& fileName) const
{
    const ComponentSpec spec = _componentSpec(id);
    if (!spec.isValid()) {
        return {};
    }

    const QString root = componentRootPath(id);
    if (root.isEmpty()) {
        return {};
    }

    const QString direct = QDir(root).filePath(fileName);
    if (QFileInfo::exists(direct)) {
        return direct;
    }

    for (const QString& relativePath : spec.requiredFiles) {
        if (QFileInfo(relativePath).fileName().compare(fileName, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString candidate = QDir(root).filePath(relativePath);
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

QString ComponentManager::componentRootPath(const QString& id) const
{
    const QString normalized = normalizeComponentId(id);
    if (normalized.isEmpty()) {
        return {};
    }

    const ComponentSpec spec = _componentSpec(normalized);
    if (!spec.isValid()) {
        return {};
    }

    if (spec.installSubdir.isEmpty() || spec.installSubdir == QStringLiteral(".")) {
        return appRootPath();
    }

    return QDir(appRootPath()).filePath(spec.installSubdir);
}

QString ComponentManager::componentVersion(const QString& id) const
{
    const QJsonObject installed = _installedState
        .value(QStringLiteral("components"))
        .toObject()
        .value(normalizeComponentId(id))
        .toObject();
    return installed.value(QStringLiteral("version")).toString();
}

QJsonObject ComponentManager::manifestDocument() const
{
    return _manifestRoot;
}

QString ComponentManager::appRootPath()
{
    return QCoreApplication::applicationDirPath();
}

QString ComponentManager::componentsRootPath()
{
    const QString path = QDir(appRootPath()).filePath(QStringLiteral("components"));
    QDir().mkpath(path);
    return path;
}

QString ComponentManager::stateFilePath()
{
    return QDir(componentsRootPath()).filePath(QStringLiteral("installed-components.json"));
}

QString ComponentManager::manifestCachePath()
{
    return QDir(componentsRootPath()).filePath(QStringLiteral("manifest-cache.json"));
}

bool ComponentManager::_loadBundledManifest()
{
    QFile file(QStringLiteral(":/cgplay/component_manifest.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    return _loadManifestDocument(file.readAll(), nullptr);
}

bool ComponentManager::_loadCachedManifest()
{
    QFile file(manifestCachePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    return _loadManifestDocument(file.readAll(), nullptr);
}

bool ComponentManager::_loadManifestDocument(const QByteArray& json, QString* error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) {
            *error = QStringLiteral("Manifest parse failed: %1").arg(parseError.errorString());
        }
        return false;
    }

    _manifestRoot = doc.object();
    if (_manifestUrl.isEmpty()) {
        _manifestUrl = _manifestRoot.value(QStringLiteral("manifestUrl")).toString().trimmed();
    }
    return true;
}

bool ComponentManager::_loadState()
{
    QFile file(stateFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        _installedState = QJsonObject{
            {QStringLiteral("components"), QJsonObject{}}
        };
        return true;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        _installedState = QJsonObject{
            {QStringLiteral("components"), QJsonObject{}}
        };
        return false;
    }

    _installedState = doc.object();
    if (!_installedState.contains(QStringLiteral("components"))) {
        _installedState.insert(QStringLiteral("components"), QJsonObject{});
    }
    return true;
}

bool ComponentManager::_saveState() const
{
    QFile file(stateFilePath());
    QDir().mkpath(QFileInfo(file).absolutePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(_installedState).toJson(QJsonDocument::Indented));
    return true;
}

ComponentSpec ComponentManager::_componentSpec(const QString& id) const
{
    const QString normalized = normalizeComponentId(id);
    const QJsonObject components = _manifestRoot.value(QStringLiteral("components")).toObject();
    const QJsonObject object = components.value(normalized).toObject();

    ComponentSpec spec;
    spec.id = normalized;
    spec.version = object.value(QStringLiteral("version")).toString();
    spec.url = object.value(QStringLiteral("url")).toString();
    spec.urls = jsonStringList(object, QStringLiteral("urls"));
    const QString legacyUrl = spec.url.trimmed();
    if (!legacyUrl.isEmpty() && !spec.urls.contains(legacyUrl)) {
        spec.urls.push_front(legacyUrl);
    }
    spec.sha256 = object.value(QStringLiteral("sha256")).toString().trimmed().toLower();
    spec.archiveName = object.value(QStringLiteral("archiveName")).toString();
    spec.installSubdir = asSlashPath(object.value(QStringLiteral("installSubdir")).toString().trimmed());
    spec.optional = object.value(QStringLiteral("optional")).toBool(false);

    const QJsonArray files = object.value(QStringLiteral("requiredFiles")).toArray();
    for (const auto& value : files) {
        const QString file = asSlashPath(value.toString().trimmed());
        if (!file.isEmpty()) {
            spec.requiredFiles.push_back(file);
        }
    }

    if (spec.archiveName.isEmpty()) {
        const QString firstUrl = spec.urls.isEmpty() ? spec.url : spec.urls.first();
        spec.archiveName = preferredArchiveName(firstUrl, spec.id + QStringLiteral(".zip"));
    }

    return spec;
}

bool ComponentManager::_isInstalled(const ComponentSpec& spec) const
{
    if (!spec.isValid()) {
        return false;
    }

    const QJsonObject components = _installedState.value(QStringLiteral("components")).toObject();
    const QJsonObject installed = components.value(spec.id).toObject();
    const QString root = componentRootPath(spec.id);
    if (root.isEmpty()) {
        return false;
    }

    bool filesExist = false;
    if (spec.requiredFiles.isEmpty()) {
        filesExist = QDir(root).exists();
    } else {
        filesExist = true;
        for (const QString& relative : spec.requiredFiles) {
            const QString absolute = QDir(root).filePath(relative);
            if (!QFileInfo::exists(absolute)) {
                filesExist = false;
                break;
            }
        }
    }

    if (!filesExist) {
        return false;
    }

    const QString installedVersion = installed.value(QStringLiteral("version")).toString().trimmed();
    if (installedVersion.isEmpty()) {
        return true;
    }

    if (installedVersion == spec.version) {
        return true;
    }

    return false;
}

bool ComponentManager::_downloadAndInstall(const ComponentSpec& spec, QWidget* parentWidget, QString* error)
{
    if (spec.urls.isEmpty()) {
        if (error) {
            *error = QObject::tr("组件 %1 没有可用的下载地址。").arg(spec.id);
        }
        return false;
    }

    QProgressDialog* installProgress = nullptr;
    if (parentWidget) {
        installProgress = new QProgressDialog(
            QObject::tr("正在安装组件..."),
            QObject::tr("取消"),
            0,
            100,
            parentWidget);
        installProgress->setWindowModality(Qt::WindowModal);
        installProgress->setMinimumDuration(0);
        installProgress->show();
    }

    auto closeProgress = [&]() {
        if (installProgress) {
            installProgress->close();
            installProgress->deleteLater();
            installProgress = nullptr;
        }
    };

    const QString tempRoot = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("cgplay_component_install"));
    QDir().mkpath(tempRoot);
    const QString archivePath = QDir(tempRoot).filePath(spec.archiveName);
    const QString extractRoot = QDir(tempRoot).filePath(spec.id + QStringLiteral("_extract"));
    QDir(extractRoot).removeRecursively();
    QFile::remove(archivePath);

    QString sourceUrl;
    QStringList downloadErrors;
    for (const QString& url : spec.urls) {
        writeComponentLog(QStringLiteral("download component %1 from %2").arg(spec.id, url));
        if (installProgress) {
            installProgress->setRange(0, 100);
            installProgress->setValue(0);
            installProgress->setLabelText(QObject::tr("正在下载组件：%1").arg(QUrl(url).host()));
        }

        QString downloadError;
        if (DownloadService::instance().downloadFileFromUrls({url}, archivePath, parentWidget, &downloadError)) {
            sourceUrl = url;
            const qint64 archiveSize = QFileInfo(archivePath).exists() ? QFileInfo(archivePath).size() : -1;
            writeComponentLog(QStringLiteral("download component %1 ok, bytes=%2").arg(spec.id).arg(archiveSize));
            break;
        }
        writeComponentLog(QStringLiteral("download component %1 failed: %2").arg(spec.id, downloadError));
        downloadErrors.push_back(downloadError);
    }

    if (sourceUrl.isEmpty()) {
        if (error) {
            *error = downloadErrors.join(QStringLiteral("\n\n"));
        }
        closeProgress();
        return false;
    }

    if (installProgress) {
        installProgress->setRange(0, 100);
        installProgress->setValue(96);
        installProgress->setLabelText(QObject::tr("正在校验组件..."));
    }
    writeComponentLog(QStringLiteral("verify component %1").arg(spec.id));
    if (!_verifyFileSha256(archivePath, spec.sha256, error)) {
        closeProgress();
        return false;
    }

    if (installProgress) {
        installProgress->setValue(98);
        installProgress->setLabelText(QObject::tr("正在解压组件..."));
    }
    writeComponentLog(QStringLiteral("extract component %1").arg(spec.id));
    if (!_extractArchive(archivePath, extractRoot, installProgress, error)) {
        closeProgress();
        return false;
    }

    if (installProgress) {
        installProgress->setValue(99);
        installProgress->setLabelText(QObject::tr("正在复制组件..."));
    }
    writeComponentLog(QStringLiteral("copy component %1").arg(spec.id));
    if (!_copyExtractedPayload(extractRoot, spec, error)) {
        closeProgress();
        return false;
    }

    QJsonObject components = _installedState.value(QStringLiteral("components")).toObject();
    components.insert(spec.id, QJsonObject{
        {QStringLiteral("version"), spec.version},
        {QStringLiteral("installedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("sourceUrl"), sourceUrl}
    });
    _installedState.insert(QStringLiteral("components"), components);
    _saveState();

    QFile::remove(archivePath);
    QDir(extractRoot).removeRecursively();
    if (installProgress) {
        installProgress->setValue(100);
        installProgress->setLabelText(QObject::tr("组件安装完成"));
    }
    writeComponentLog(QStringLiteral("install component %1 complete").arg(spec.id));
    closeProgress();
    return true;
}


bool ComponentManager::_verifyFileSha256(const QString& filePath, const QString& expectedSha256, QString* error) const
{
    if (expectedSha256.isEmpty()) {
        return true;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Unable to open file for verification: %1").arg(filePath);
        }
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        hash.addData(file.read(1024 * 1024));
    }

    const QString actual = QString::fromLatin1(hash.result().toHex()).toLower();
    if (actual == expectedSha256) {
        return true;
    }

    if (error) {
        *error = QStringLiteral("Component checksum mismatch. Expected %1, got %2").arg(expectedSha256, actual);
    }
    return false;
}

bool ComponentManager::_extractArchive(
    const QString& archivePath,
    const QString& destinationPath,
    QProgressDialog* progressDialog,
    QString* error) const
{
    constexpr int kExtractTimeoutMs = 3 * 60 * 1000;
    QDir().mkpath(destinationPath);
    JobContext context(kExtractTimeoutMs);
    QProcess process;
    process.start(
        QStringLiteral("powershell"),
        {
            QStringLiteral("-NoProfile"),
            QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
            QStringLiteral("-Command"),
            QStringLiteral("Expand-Archive -Path '%1' -DestinationPath '%2' -Force")
                .arg(QDir::toNativeSeparators(archivePath))
                .arg(QDir::toNativeSeparators(destinationPath))
        },
        QIODevice::ReadOnly);

    QEventLoop loop;
    QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
    if (progressDialog) {
        QObject::connect(progressDialog, &QProgressDialog::canceled, &loop, [&context]() {
            context.cancel();
        });
    }
    QTimer stopPoll;
    stopPoll.setInterval(25);
    QObject::connect(&stopPoll, &QTimer::timeout, &loop, [&]() {
        if (!context.shouldStop() || process.state() == QProcess::NotRunning) {
            return;
        }
        process.terminate();
        QTimer::singleShot(750, &loop, [&process]() {
            if (process.state() != QProcess::NotRunning) {
                process.kill();
            }
        });
    });
    stopPoll.start();

    ProcessOutcome outcome;
    if (process.state() == QProcess::Starting && !process.waitForStarted(15 * 1000)) {
        outcome.state = JobState::Failed;
        outcome.standardError = process.errorString().toUtf8();
    } else {
        if (process.state() != QProcess::NotRunning) {
            loop.exec();
        }
        outcome = context.waitForProcess(process, 25, kExtractTimeoutMs);
        if (context.isCancellationRequested()) {
            outcome.state = JobState::Canceled;
        } else if (context.hasTimedOut()) {
            outcome.state = JobState::TimedOut;
        }
    }
    stopPoll.stop();
    if (!outcome.succeeded()) {
        if (error) {
            *error = QString::fromUtf8(outcome.standardError).trimmed();
            if (error->isEmpty()) {
                *error = outcome.state == JobState::TimedOut
                    ? QObject::tr("压缩包解压超时：%1").arg(archivePath)
                    : QObject::tr("压缩包解压失败：%1").arg(archivePath);
            }
        }
        return false;
    }
    return true;
}

bool ComponentManager::_copyExtractedPayload(const QString& extractRoot, const ComponentSpec& spec, QString* error) const
{
    QString payloadRoot = extractRoot;

    QDirIterator topLevel(extractRoot, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
    QStringList childDirs;
    while (topLevel.hasNext()) {
        childDirs.push_back(topLevel.next());
    }
    if (childDirs.size() == 1) {
        payloadRoot = childDirs.front();
    }

    const QString destRoot = componentRootPath(spec.id);
    if (destRoot.isEmpty()) {
        if (error) {
            *error = QObject::tr("组件目标路径为空：%1").arg(spec.id);
        }
        return false;
    }

    QDir().mkpath(destRoot);
    QDirIterator files(payloadRoot, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString sourceFile = files.next();
        const QString relative = QDir(payloadRoot).relativeFilePath(sourceFile);
        const QString targetFile = QDir(destRoot).filePath(relative);
        QDir().mkpath(QFileInfo(targetFile).absolutePath());
        QFile::remove(targetFile);
        if (!QFile::copy(sourceFile, targetFile)) {
            if (error) {
                *error = QObject::tr("无法复制组件文件：%1").arg(relative);
            }
            return false;
        }
    }

    return true;
}

QString ComponentManager::_resolveManifestUrl() const
{
    if (!_manifestUrl.isEmpty()) {
        return _manifestUrl;
    }

    return _manifestRoot.value(QStringLiteral("manifestUrl")).toString().trimmed();
}

QStringList ComponentManager::_resolveManifestUrls() const
{
    QStringList urls;
    if (!_manifestUrl.isEmpty()) {
        urls.push_back(_manifestUrl);
    }
    for (const QString& url : jsonStringList(_manifestRoot, QStringLiteral("manifestUrls"))) {
        if (!urls.contains(url)) {
            urls.push_back(url);
        }
    }
    const QString legacy = _manifestRoot.value(QStringLiteral("manifestUrl")).toString().trimmed();
    if (!legacy.isEmpty() && !urls.contains(legacy)) {
        urls.push_back(legacy);
    }
    return urls;
}

} // namespace cgplay
