#pragma once

#include <QObject>
#include <QJsonObject>
#include <QString>
#include <QStringList>

class QWidget;
class QProgressDialog;

namespace cgplay {

struct ComponentSpec
{
    QString id;
    QString version;
    QString url;
    QStringList urls;
    QString sha256;
    QString archiveName;
    QString installSubdir;
    QStringList requiredFiles;
    bool optional = false;

    bool isValid() const;
};

class ComponentManager : public QObject
{
    Q_OBJECT
public:
    static ComponentManager& instance();

    void setManifestUrl(const QString& url);
    QString manifestUrl() const;

    bool refreshRemoteManifest(QString* error = nullptr);
    bool hasComponent(const QString& id) const;
    QStringList componentIds() const;
    QStringList missingRequiredComponents() const;
    QString remoteAppVersion() const;
    qint64 remoteInstallerSize(const QString& mode = QStringLiteral("full")) const;
    bool isAppUpdateAvailable(const QString& currentVersion) const;
    bool downloadFromUrls(
        const QStringList& urls,
        QByteArray* out,
        QWidget* parentWidget = nullptr,
        QString* error = nullptr);
    bool downloadFileFromUrls(
        const QStringList& urls,
        const QString& destinationPath,
        QWidget* parentWidget = nullptr,
        QString* error = nullptr);
    bool ensureComponent(
        const QString& id,
        QWidget* parentWidget = nullptr,
        bool allowDownloadPrompt = true,
        QString* error = nullptr);

    QString componentExecutablePath(const QString& id, const QString& fileName) const;
    QString componentRootPath(const QString& id) const;
    QString componentVersion(const QString& id) const;
    QJsonObject manifestDocument() const;

    static QString appRootPath();
    static QString componentsRootPath();
    static QString stateFilePath();
    static QString manifestCachePath();

private:
    explicit ComponentManager(QObject* parent = nullptr);

    bool _loadBundledManifest();
    bool _loadCachedManifest();
    bool _loadManifestDocument(const QByteArray& json, QString* error);
    bool _loadState();
    bool _saveState() const;

    ComponentSpec _componentSpec(const QString& id) const;
    bool _isInstalled(const ComponentSpec& spec) const;
    bool _downloadAndInstall(const ComponentSpec& spec, QWidget* parentWidget, QString* error);
    bool _verifyFileSha256(const QString& filePath, const QString& expectedSha256, QString* error) const;
    bool _extractArchive(
        const QString& archivePath,
        const QString& destinationPath,
        QProgressDialog* progressDialog,
        QString* error) const;
    bool _copyExtractedPayload(const QString& extractRoot, const ComponentSpec& spec, QString* error) const;
    QString _resolveManifestUrl() const;
    QStringList _resolveManifestUrls() const;

    QString _manifestUrl;
    QJsonObject _manifestRoot;
    QJsonObject _installedState;
};

} // namespace cgplay
