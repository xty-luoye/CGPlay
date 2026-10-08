#pragma once

#include "common/jobs/JobSystem.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkReply>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <initializer_list>

namespace cgplay {

class IAICredentialStore;
class ISettingsService;

namespace codex_plugin_detail {

inline constexpr auto kDynamicToolSchemaVersion = "20260714-artifact-steer-dcc-v1";

struct NetworkJobResult
{
    JobState state = JobState::Failed;
    QByteArray body;
    QNetworkReply::NetworkError networkError = QNetworkReply::UnknownNetworkError;
    QString errorString;
    QByteArray contentType;
    int httpStatus = 0;

    bool succeeded() const
    {
        return state == JobState::Succeeded && networkError == QNetworkReply::NoError;
    }
};

JobOutcome runSynchronousJob(
    const QString& id,
    QObject* owner,
    int timeoutMs,
    JobRunner::Worker worker);
JobOutcome runProcessJob(
    QObject* owner,
    const QString& id,
    const QString& program,
    const QStringList& arguments,
    const QString& workingDirectory,
    int timeoutMs,
    const QByteArray& standardInput = {});
NetworkJobResult waitForNetworkJob(QNetworkReply* reply, JobContext& context, int operationTimeoutMs);
bool waitForJobDelay(JobContext& context, int delayMs);
QString jobError(const JobOutcome& outcome, const QString& fallback);
const QSet<QString>& officialCapabilityMethods();
void appendMediaAssetRecord(const QString& kind, const QString& path, const QJsonObject& extra = {});
QJsonObject loadLocalThreadHistory(const QString& threadId);
QStringList searchLocalThreadHistory(const QString& query);
QString tomlString(QString value);
bool hasSafeIdentifier(const QString& value);
bool copyDirectoryTree(const QString& sourcePath, const QString& targetPath, QString* error);
bool isSensitiveCredentialQueryKey(const QString& value);
bool hasSensitiveCredentialQuery(const QUrl& url);
QByteArray loadImageCredentialFallback();
bool isAzureProviderUrl(const QUrl& url);
QString responsesBaseUrl(QString endpoint);
QString tomlInlineStringTable(const QList<QPair<QString, QString>>& items);
QString displayProtocolState(const QString& state);
QString displayTurnStatus(const QString& status);
QJsonObject toolTextResult(bool success, const QJsonObject& payload);
QJsonObject toolError(const QString& code, const QString& message);
QString firstNonEmptySetting(ISettingsService* settings, std::initializer_list<const char*> keys);
QString workbenchMediaEndpoint(ISettingsService* settings, const QString& kind);
QString workbenchMediaModel(ISettingsService* settings);
void insertExecutable(QJsonObject* executables, const QString& name, const QString& path);
QList<QFileInfo> versionedDirectories(const QString& root, const QStringList& filters);
QJsonObject discoverSoftwareExecutables();
QString resolveSoftwareExecutable(const QString& requested);
QString artifactPathFromToolResult(const QString& tool, const QJsonObject& result);
QString dynamicToolSignature(const QJsonArray& tools);
QByteArray loadAiWorkspaceSecret(
    IAICredentialStore* store,
    const QString& providerId,
    const QString& providerDisplayName,
    const QString& detectedProtocol,
    const QString& baseUrl);
void appendUniqueModel(QStringList* models, const QString& model);
QString normalizedModelKey(const QString& model);
bool isRunnableCodexExecutable(const QString& path);
QString discoverCodexExecutable(const QString& configuredOverride = {});
QString findProjectRootFrom(QString startPath);
QString discoverProjectRoot();
QString approvalPolicyForMode(const QString& mode);
QString sandboxForMode(const QString& mode);
QByteArray redactSensitiveText(QByteArray text);
QString describeThreadItem(const QJsonObject& item);

} // namespace codex_plugin_detail
} // namespace cgplay
