#include "UpdateService.h"

#include "component/ComponentManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>
#include <QStringConverter>

namespace cgplay {

QJsonObject UpdateCheckResult::toJson() const
{
    return QJsonObject{
        {QStringLiteral("refreshed"), refreshed},
        {QStringLiteral("updateAvailable"), updateAvailable},
        {QStringLiteral("currentVersion"), currentVersion},
        {QStringLiteral("remoteVersion"), remoteVersion},
        {QStringLiteral("error"), error}
    };
}

UpdateCheckResult UpdateCheckResult::fromJson(const QJsonObject& json)
{
    UpdateCheckResult result;
    result.refreshed = json.value(QStringLiteral("refreshed")).toBool(false);
    result.updateAvailable = json.value(QStringLiteral("updateAvailable")).toBool(false);
    result.currentVersion = json.value(QStringLiteral("currentVersion")).toString().trimmed();
    result.remoteVersion = json.value(QStringLiteral("remoteVersion")).toString().trimmed();
    result.error = json.value(QStringLiteral("error")).toString().trimmed();
    return result;
}

UpdateService& UpdateService::instance()
{
    static UpdateService service;
    return service;
}

UpdateCheckResult UpdateService::checkForUpdates(const QString& currentVersion, bool refreshRemote)
{
    UpdateCheckResult result;
    result.currentVersion = currentVersion.trimmed();

    ComponentManager& manager = ComponentManager::instance();
    if (refreshRemote) {
        QString error;
        result.refreshed = manager.refreshRemoteManifest(&error);
        result.error = error;
    } else {
        result.refreshed = true;
    }
    result.remoteVersion = manager.remoteAppVersion();
    result.updateAvailable = manager.isAppUpdateAvailable(result.currentVersion);
    return result;
}

UpdateInstallResult UpdateService::downloadAndLaunchInstaller(
    const QString& targetVersion,
    QWidget* parentWidget)
{
    UpdateInstallResult result;
    result.targetVersion = targetVersion.trimmed();

    if (result.targetVersion.isEmpty()) {
        result.error = QStringLiteral("Target version is empty");
        return result;
    }

    ComponentManager& manager = ComponentManager::instance();
    QString downloadError;
    QStringList installerUrls;
    const QString overrideUrls = qEnvironmentVariable("CGPLAY_UPDATE_INSTALLER_URLS").trimmed();
    for (QString url : overrideUrls.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        url = url.trimmed();
        url.replace(QStringLiteral("{version}"), result.targetVersion);
        if (!url.isEmpty() && !installerUrls.contains(url)) {
            installerUrls.push_back(url);
        }
    }
    if (installerUrls.isEmpty()) {
        installerUrls = {
            QStringLiteral("https://cgplay-app.netlify.app/downloads/CGPlay_Setup_%1_full.exe").arg(result.targetVersion),
            QStringLiteral("https://cgplay-app.netlify.app/downloads/CGPlay_Setup_%1_lite.exe").arg(result.targetVersion),
            QStringLiteral("https://github.com/xty-luoye/CGPlay/releases/download/v%1/CGPlay_Setup_%1_full.exe").arg(result.targetVersion),
            QStringLiteral("https://github.com/xty-luoye/CGPlay/releases/download/v%1/CGPlay_Setup_%1_lite.exe").arg(result.targetVersion)
        };
    }

    const QString tempDir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("cgplay_update"));
    QDir().mkpath(tempDir);
    result.installerPath = QDir(tempDir).filePath(
        QStringLiteral("CGPlay_Setup_%1.exe").arg(result.targetVersion));

    if (!manager.downloadFileFromUrls(installerUrls, result.installerPath, parentWidget, &downloadError)) {
        result.error = downloadError;
        return result;
    }
    result.downloaded = true;

    result.launcherPath = QDir(tempDir).filePath(
        QStringLiteral("CGPlay_Run_Update_%1.cmd").arg(result.targetVersion));
    QFile launcherFile(result.launcherPath);
    if (!launcherFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        result.error = QStringLiteral("Unable to write launcher script: %1").arg(result.launcherPath);
        return result;
    }

    QTextStream stream(&launcherFile);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "@echo off\r\n";
    stream << "setlocal\r\n";
    stream << "set \"INSTALLER=" << QDir::toNativeSeparators(result.installerPath) << "\"\r\n";
    stream << "for /L %%i in (1,1,60) do (\r\n";
    stream << "  tasklist /FI \"IMAGENAME eq CGPlay.exe\" | find /I \"CGPlay.exe\" >nul\r\n";
    stream << "  if errorlevel 1 goto start_installer\r\n";
    stream << "  timeout /t 1 /nobreak >nul\r\n";
    stream << ")\r\n";
    stream << ":start_installer\r\n";
    stream << "start \"CGPlay Update\" \"%INSTALLER%\"\r\n";
    stream << "endlocal\r\n";
    launcherFile.close();

    result.launched = QProcess::startDetached(
        QStringLiteral("cmd.exe"),
        { QStringLiteral("/C"), QDir::toNativeSeparators(result.launcherPath) });
    if (!result.launched) {
        result.error = QStringLiteral("Unable to launch installer: %1").arg(result.launcherPath);
    }

    return result;
}

} // namespace cgplay
