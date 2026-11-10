#include "ComponentManager.h"
#include "DownloadService.h"
#include "ComponentPayloadStore.h"
#include "ComponentManifestPolicy.h"
#include "ComponentRuntimePaths.h"
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
#include <QLockFile>
#include <QPointer>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QThread>
#include <QRegularExpression>
#include <atomic>
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
    QWriteLocker metadataGuard(&_metadataLock);
    _manifestUrl = url.trimmed();
}

QString ComponentManager::manifestUrl() const
{
    return _resolveManifestUrl();
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
    QReadLocker metadataGuard(&_metadataLock);
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
    QReadLocker metadataGuard(&_metadataLock);
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
    QReadLocker metadataGuard(&_metadataLock);
    return _manifestRoot.value(QStringLiteral("appVersion")).toString().trimmed();
}

qint64 ComponentManager::remoteInstallerSize(const QString& mode) const
{
    QReadLocker metadataGuard(&_metadataLock);
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
    if (!qApp || QThread::currentThread() != qApp->thread()) {
        if (error) *error = QObject::tr("组件未就绪，请先在界面线程完成修复。");
        return false;
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
    QReadLocker metadataGuard(&_metadataLock);
    const QString normalized = normalizeComponentId(id);
    const ComponentSpec spec = _componentSpec(normalized);
    if (!spec.isValid()) return {};
    const QJsonObject installed = _installedState.value(QStringLiteral("components")).toObject()
        .value(normalized).toObject();
    if (installed.contains(QStringLiteral("payloadRoot"))) {
        // A corrupt/missing managed root must not silently fall back to an old
        // bundle while the state claims that a repaired version is installed.
        return component_payload_store::resolvePublishedRoot(componentsRootPath(), normalized,
            installed.value(QStringLiteral("payloadRoot")).toString());
    }
    if (spec.installSubdir.isEmpty() || spec.installSubdir == QStringLiteral(".")) return appRootPath();
    if (!component_payload_store::safeRelative(spec.installSubdir)) return {};
    return QDir(appRootPath()).filePath(spec.installSubdir);
}

QJsonObject ComponentManager::runtimeRootsSnapshot() const
{
    QReadLocker metadataGuard(&_metadataLock);
    QJsonObject roots;
    const QJsonObject installed = _installedState.value(QStringLiteral("components")).toObject();
    for (const QString& id : {QStringLiteral("python-runtime"), QStringLiteral("tools"),
                             QStringLiteral("ffmpeg"), QStringLiteral("recording-runtime")}) {
        const QString selectedVersion = installed.value(id).toObject().value(QStringLiteral("version")).toString();
        roots.insert(id, QJsonObject{{QStringLiteral("root"), componentRootPath(id)},
            {QStringLiteral("version"), selectedVersion.isEmpty() ? _componentSpec(id).version : selectedVersion},
            {QStringLiteral("managed"), installed.value(id).toObject().contains(QStringLiteral("payloadRoot"))}});
    }
    return roots;
}

QString ComponentManager::componentVersion(const QString& id) const
{
    QReadLocker metadataGuard(&_metadataLock);
    const QJsonObject installed = _installedState
        .value(QStringLiteral("components"))
        .toObject()
        .value(normalizeComponentId(id))
        .toObject();
    return installed.value(QStringLiteral("version")).toString();
}

QJsonObject ComponentManager::manifestDocument() const
{
    QReadLocker metadataGuard(&_metadataLock);
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
    if (!_loadManifestDocument(file.readAll(), nullptr)) return false;
    QWriteLocker metadataGuard(&_metadataLock);
    _bundledManifest = _manifestRoot;
    return true;
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

    QWriteLocker metadataGuard(&_metadataLock);
    _manifestRoot = component_manifest_policy::mergePayload(doc.object(), _bundledManifest);
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

bool ComponentManager::_saveState(const QJsonObject& state, QString* error) const
{
    return component_payload_store::writeState(stateFilePath(), state, error);
}

ComponentSpec ComponentManager::_componentSpec(const QString& id) const
{
    QReadLocker metadataGuard(&_metadataLock);
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
    QReadLocker metadataGuard(&_metadataLock);
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
            if (!QFileInfo(absolute).isFile() || QFileInfo(absolute).size() <= 0) {
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

    if (spec.id == QStringLiteral("core")) {
        if (error) *error = QObject::tr("主程序更新请使用完整安装包，退出后安装。");
        return false;
    }
    if (!QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(spec.sha256).hasMatch()) {
        if (error) *error = QObject::tr("组件缺少有效 SHA-256，拒绝下载替换。");
        return false;
    }
    QLockFile installationLock(QDir(componentsRootPath()).filePath(QStringLiteral("component-install.lock")));
    installationLock.setStaleLockTime(0);
    if (!installationLock.tryLock()) {
        if (error) *error = QObject::tr("另一个组件安装任务正在执行，请稍后重试。");
        return false;
    }
    const auto canceled = std::make_shared<std::atomic_bool>(false);
    QObject installationLifetime;
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, &installationLifetime,
        [canceled]() { canceled->store(true); });
    if (parentWidget) QObject::connect(parentWidget, &QObject::destroyed, &installationLifetime,
        [canceled]() { canceled->store(true); });
    QPointer<QProgressDialog> installProgress;
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
        QObject::connect(installProgress, &QProgressDialog::canceled, &installationLifetime,
            [canceled]() { canceled->store(true); });
    }

    auto closeProgress = [&]() {
        if (installProgress) {
            installProgress->close();
            installProgress->deleteLater();
            installProgress = nullptr;
        }
    };

    QTemporaryDir downloadDirectory(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("cgplay_component_XXXXXX")));
    if (!downloadDirectory.isValid()) {
        if (error) *error = QObject::tr("无法创建组件下载暂存目录。");
        closeProgress();
        return false;
    }
    downloadDirectory.setAutoRemove(false);
    const QString downloadRoot = downloadDirectory.path();
    const auto cleanupDownload = qScopeGuard([downloadRoot, canceled]() {
        // Do not race an unfinished canceled worker during shutdown. The
        // private unselected directory can be reclaimed by later background GC.
        if (canceled->load() || !qApp || QCoreApplication::closingDown()) return;
        JobRunner::start(QStringLiteral("component.download.cleanup"), qApp, 0,
            [downloadRoot](JobContext&) {
                if (QFileInfo(downloadRoot).fileName().startsWith(QStringLiteral("cgplay_component_")) &&
                    !component_payload_store::linked(downloadRoot)) QDir(downloadRoot).removeRecursively();
                return JobOutcome::success();
            });
    });
    const QString archivePath = QDir(downloadDirectory.path()).filePath(QStringLiteral("component.zip"));
    const QString extractRoot = QDir(downloadDirectory.path()).filePath(QStringLiteral("extracted"));

    QString sourceUrl;
    QStringList downloadErrors;
    for (const QString& url : spec.urls) {
        if (canceled->load()) break;
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

    if (sourceUrl.isEmpty() || canceled->load()) {
        if (error) {
            *error = canceled->load()
                ? QObject::tr("组件安装已取消，原安装保持不变。")
                : downloadErrors.join(QStringLiteral("\n\n"));
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
    QEventLoop verifyLoop;
    JobOutcome verification;
    auto* verifyJob = JobRunner::start(QStringLiteral("component.archive.verify"), &verifyLoop,
        30 * 60 * 1000, [archivePath, expectedSha = spec.sha256, canceled](JobContext& context) {
            QByteArray actual;
            QString failure;
            const auto shouldStop = [&]() {
                if (canceled->load()) context.cancel();
                return context.shouldStop();
            };
            if (!component_payload_store::digestFile(archivePath, &actual, shouldStop, &failure))
                return shouldStop() ? JobOutcome::canceled(failure)
                                    : JobOutcome::failure(QStringLiteral("component-read-failed"), failure);
            if (QString::fromLatin1(actual.toHex()) != expectedSha)
                return JobOutcome::failure(QStringLiteral("component-checksum-mismatch"), QObject::tr("组件 SHA-256 不匹配。"));
            return JobOutcome::success();
        });
    QObject::connect(verifyJob, &JobHandle::finished, &verifyLoop, [&](const JobOutcome& result) {
        verification = result;
        verifyLoop.quit();
    });
    if (installProgress) QObject::connect(installProgress, &QProgressDialog::canceled, verifyJob, &JobHandle::cancel);
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, verifyJob, &JobHandle::cancel);
    verifyLoop.exec();
    if (!verification.succeeded() || canceled->load()) {
        verifyJob->cancel();
        if (error) *error = canceled->load() ? QObject::tr("组件安装已取消，原安装保持不变。") : verification.errorMessage;
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
    writeComponentLog(QStringLiteral("stage component %1").arg(spec.id));
    QEventLoop stageLoop;
    JobOutcome stageOutcome;
    const QString componentsRoot = componentsRootPath();
    const ComponentRuntimePaths validationRuntime = captureComponentRuntimePaths();
    auto* stageJob = JobRunner::start(QStringLiteral("component.stage.%1").arg(spec.id), &stageLoop,
        30 * 60 * 1000, [extractRoot, componentsRoot, spec, canceled, validationRuntime](JobContext& context) {
            QJsonObject candidate;
            QString failure;
            const auto shouldStop = [&]() {
                if (canceled->load()) context.cancel();
                return context.shouldStop();
            };
            const bool staged = component_payload_store::prepare(extractRoot, componentsRoot, spec.id,
                spec.version, spec.requiredFiles, &candidate, &failure, shouldStop,
                [&](int percent) { context.reportProgress(percent, QStringLiteral("component.verify")); });
            if (shouldStop()) return JobOutcome::canceled(failure);
            if (!staged) return JobOutcome::failure(QStringLiteral("component-stage-failed"), failure);
            if (!component_payload_store::validateRuntime(spec.id,
                    candidate.value(QStringLiteral("absoluteRoot")).toString(), validationRuntime.pythonExecutable,
                    shouldStop, &failure)) return JobOutcome::failure(QStringLiteral("component-runtime-invalid"), failure);
            return JobOutcome::success(candidate);
        });
    QObject::connect(stageJob, &JobHandle::finished, &stageLoop, [&](const JobOutcome& outcome) {
        stageOutcome = outcome;
        stageLoop.quit();
    });
    if (installProgress) {
        QObject::connect(installProgress, &QProgressDialog::canceled, stageJob, &JobHandle::cancel);
        QObject::connect(stageJob, &JobHandle::progressChanged, installProgress,
            [installProgress](int percent, const QString&) { if (installProgress) installProgress->setValue(qMin(99, 98 + percent / 100)); });
    }
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, stageJob, &JobHandle::cancel);
    stageLoop.exec();
    if (!stageOutcome.succeeded() || canceled->load()) {
        stageJob->cancel();
        if (error) *error = canceled->load() ? QObject::tr("组件安装已取消，原安装保持不变。") : stageOutcome.errorMessage;
        closeProgress();
        return false;
    }
    const QJsonObject candidate = stageOutcome.value.toJsonObject();
    const QString selectedRoot = candidate.value(QStringLiteral("payloadRoot")).toString();
    if (component_payload_store::resolvePublishedRoot(componentsRoot, spec.id, selectedRoot).isEmpty()) {
        if (error) *error = QObject::tr("组件暂存根目录无效，原安装保持不变。");
        closeProgress();
        return false;
    }
    // The long I/O is done. Only the small atomic state commit remains.
    if (installProgress) installProgress->setCancelButton(nullptr);
    QJsonObject nextState{{QStringLiteral("components"), QJsonObject{}}};
    QFile priorState(stateFilePath());
    if (priorState.exists()) {
        if (!priorState.open(QIODevice::ReadOnly)) {
            if (error) *error = QObject::tr("无法读取原组件状态，原安装保持不变。");
            closeProgress();
            return false;
        }
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(priorState.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            if (error) *error = QObject::tr("原组件状态无效，拒绝覆盖。");
            closeProgress();
            return false;
        }
        nextState = document.object();
        priorState.close();
    }
    QJsonObject components = nextState.value(QStringLiteral("components")).toObject();
    components.insert(spec.id, QJsonObject{
        {QStringLiteral("version"), spec.version},
        {QStringLiteral("payloadRoot"), selectedRoot},
        {QStringLiteral("installedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("sourceUrl"), sourceUrl}
    });
    nextState.insert(QStringLiteral("components"), components);
    if (!_saveState(nextState, error)) {
        closeProgress();
        return false;
    }
    {
        QWriteLocker metadataGuard(&_metadataLock);
        _installedState = nextState;
    }
    // Keep every formerly selected version, and unselected failed candidates,
    // until a separate bounded GC can establish that no running job needs it.
    if (installProgress) {
        installProgress->setValue(100);
        installProgress->setLabelText(QObject::tr("组件安装完成"));
    }
    writeComponentLog(QStringLiteral("install component %1 complete").arg(spec.id));
    closeProgress();
    return true;
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
    const auto quotePath = [](const QString& path) {
        QString escaped = QDir::toNativeSeparators(path);
        escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
        return QLatin1Char('\'') + escaped + QLatin1Char('\'');
    };
    process.start(
        QStringLiteral("powershell"),
        {
            QStringLiteral("-NoProfile"),
            QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
            QStringLiteral("-Command"),
            // This destination is a fresh temporary staging directory. The
            // .NET API treats both paths literally, including brackets.
            QStringLiteral("$ErrorActionPreference='Stop'; Add-Type -AssemblyName System.IO.Compression.FileSystem; "
                           "[System.IO.Compression.ZipFile]::ExtractToDirectory(%1, %2)")
                .arg(quotePath(archivePath), quotePath(destinationPath))
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

QString ComponentManager::_resolveManifestUrl() const
{
    return _resolveManifestUrls().value(0);
}

QStringList ComponentManager::_resolveManifestUrls() const
{
    QReadLocker metadataGuard(&_metadataLock);
    return component_manifest_policy::sourceUrls(_bundledManifest, _manifestUrl);
}

} // namespace cgplay
