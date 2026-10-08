#pragma once
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <atomic>
#include <memory>

namespace cgplay {
// Workers share values only, never windows, settings or component managers.
struct UpdateTransferState {
    std::atomic_bool cancelled{false};
    std::atomic<qint64> received{0};
};
struct UpdateCheckResult {
    bool refreshed = false;
    bool updateAvailable = false;
    QString currentVersion;
    QString remoteVersion;
    QString installerUrl;
    QString sha256;
    qint64 installerSize = 0;
    QString releaseNotes;
    QString error;
    QJsonObject toJson() const;
    static UpdateCheckResult fromJson(const QJsonObject& json);
};
struct UpdateInstallResult {
    bool downloaded = false;
    QString installerPath;
    QString sha256;
    QString error;
};
class UpdateService {
public:
    static UpdateService& instance();
    static QUrl latestReleaseUrl();
    static UpdateCheckResult parseRelease(const QByteArray& json, const QString& currentVersion);
    // Blocking operations run on workers. Overrides are explicit test injection;
    // downloaded metadata and environment variables cannot change these endpoints.
    UpdateCheckResult checkForUpdates(const QString& currentVersion,
        const std::shared_ptr<UpdateTransferState>& state,
        const QUrl& endpoint = latestReleaseUrl());
    UpdateInstallResult downloadInstaller(const UpdateCheckResult& release,
        const std::shared_ptr<UpdateTransferState>& state,
        const QUrl& transportOverride = {}, const QString& temporaryRoot = {});
    // Only the live GUI launches installation, after user confirmation.
    static bool launchInstaller(const UpdateInstallResult& installer, QString* error);
    static QString installerLaunchScript(const UpdateInstallResult& installer, qint64 processId);
private:
    UpdateService() = default;
};
} // namespace cgplay
