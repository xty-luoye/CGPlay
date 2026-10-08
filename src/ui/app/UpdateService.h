#pragma once

#include <QJsonObject>
#include <QString>

class QWidget;

namespace cgplay {

struct UpdateCheckResult
{
    bool refreshed = false;
    bool updateAvailable = false;
    QString currentVersion;
    QString remoteVersion;
    QString error;

    QJsonObject toJson() const;
    static UpdateCheckResult fromJson(const QJsonObject& json);
};

struct UpdateInstallResult
{
    bool downloaded = false;
    bool launched = false;
    QString targetVersion;
    QString installerPath;
    QString launcherPath;
    QString error;
};

class UpdateService
{
public:
    static UpdateService& instance();

    UpdateCheckResult checkForUpdates(const QString& currentVersion, bool refreshRemote = true);
    UpdateInstallResult downloadAndLaunchInstaller(
        const QString& targetVersion,
        QWidget* parentWidget = nullptr);

private:
    UpdateService() = default;
};

} // namespace cgplay
