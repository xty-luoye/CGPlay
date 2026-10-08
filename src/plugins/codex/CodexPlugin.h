#pragma once

#include "plugins/api/IPlugin.h"
#include "plugins/codex/CodexAgentWorkspace.h"
#include "services/agent/CodexAppServerSession.h"
#include "plugins/codex/LocalCodexOrchestrator.h"
#include "common/jobs/JobSystem.h"

#include <QJsonObject>
#include <QFutureWatcher>
#include <QPointer>
#include <QHash>
#include <QObject>
#include <QPair>
#include <QProcessEnvironment>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>

class QFileSystemWatcher;

namespace cgplay {

// Thin Qt-plugin owner for an explicitly configured external Codex app-server.
class CodexPlugin final : public QObject, public IPlugin
{
    Q_OBJECT
    Q_INTERFACES(cgplay::IPlugin)
    Q_PLUGIN_METADATA(IID CGPLAY_IPLUGIN_IID)

public:
    explicit CodexPlugin(QObject* parent = nullptr);

    QString id() const override;
    void initialize() override;
    void shutdown() override;

    bool startAppServer(const CodexAppServerSession::Config& config, QString* error = nullptr);
    bool sendJsonRpc(const QJsonObject& message, QString* error = nullptr);
    // Generic app-server bridge for protocol methods not yet represented by a typed UI action.
    bool sendNativeRpc(const QString& method, const QJsonObject& params = {}, qint64* requestId = nullptr, QString* error = nullptr);
    Q_INVOKABLE bool reloadMcpServers(QString* error = nullptr);
    Q_INVOKABLE bool callMcpTool(const QString& server, const QString& tool, const QJsonObject& arguments = {}, QString* error = nullptr);
    Q_INVOKABLE bool loginMcpServer(const QString& server, QString* error = nullptr);
    Q_INVOKABLE bool readMcpResource(const QString& server, const QString& uri, QString* error = nullptr);
    Q_INVOKABLE bool interruptTurn(QString* error = nullptr);
    Q_INVOKABLE bool steerTurn(const QString& text, QString* error = nullptr);
    Q_INVOKABLE bool steerImagesTurn(const QStringList& paths, const QString& text, QString* error = nullptr);
    Q_INVOKABLE bool compactThread(QString* error = nullptr);
    Q_INVOKABLE bool setThreadGoal(const QString& objective, QString* error = nullptr);
    Q_INVOKABLE bool clearThreadGoal(QString* error = nullptr);
    Q_INVOKABLE bool startReview(QString* error = nullptr);
    Q_INVOKABLE bool refreshNativeCatalog(QString* error = nullptr);
    Q_INVOKABLE bool listSkills(bool forceReload = true, QString* error = nullptr);
    Q_INVOKABLE bool readPlugin(const QString& pluginName, QString* error = nullptr);
    Q_INVOKABLE bool installPlugin(const QString& pluginName, QString* error = nullptr);
    Q_INVOKABLE bool uninstallPlugin(const QString& pluginName, QString* error = nullptr);
    Q_INVOKABLE bool addMarketplace(const QString& source, QString* error = nullptr);
    Q_INVOKABLE bool invokeOfficialCapability(const QString& method, const QJsonObject& params = {}, QString* error = nullptr);
    bool submitPrompt(const QString& prompt, QString* error = nullptr);
    bool submitImagePrompt(const QString& path, const QString& prompt, QString* error = nullptr);
    bool submitImagesPrompt(const QStringList& paths, const QString& prompt, QString* error = nullptr, const QString& source = {});
    void stopAppServer();
    Q_INVOKABLE QWidget* workspaceWidget();

    bool isAppServerRunning() const;
    QJsonObject diagnostics() const;

signals:
    void appServerStateChanged(cgplay::CodexAppServerSession::State state);
    void appServerStarted();
    void appServerStopped(int exitCode, QProcess::ExitStatus exitStatus);
    void jsonRpcLineReceived(const QByteArray& utf8Line);
    void standardErrorLineReceived(const QByteArray& utf8Line);
    void appServerError(const QString& message);
    void protocolStateChanged(const QString& state);
    void threadReady(const QString& threadId);
    void turnStarted(const QString& threadId, const QString& turnId);
    void agentMessageDelta(
        const QString& threadId,
        const QString& turnId,
        const QString& itemId,
        const QString& delta);
    void turnCompleted(const QJsonObject& turn);
    void protocolError(const QString& message);
    void nativeRpcResponse(qint64 requestId, const QJsonObject& response);
    void nativeRpcNotification(const QString& method, const QJsonObject& params);
    void nativeRpcServerRequest(const QJsonObject& request);

private:
    enum class ProtocolState
    {
        Disabled,
        Initializing,
        StartingThread,
        Ready,
        TurnInProgress,
        Stopping,
        Failed
    };

    enum class PendingRequest
    {
        Initialize,
        ProviderCapabilities,
        ModelList,
        ThreadStart,
        TurnStart,
        ThreadSettingsUpdate,
        ThreadList,
        ThreadRead,
        ThreadResume,
        ThreadFork,
        ThreadArchive
        , McpServerStatus
        , SkillsList
        , PluginList
        , AppList
    };

    bool sendRequest(
        PendingRequest request,
        const QString& method,
        const QJsonObject& params,
        QString* error = nullptr);
    bool sendNotification(const QString& method, const QJsonObject& params = {});
    void handleJsonRpcLine(const QByteArray& utf8Line);
    void handleResponse(qint64 requestId, const QJsonObject& message);
    void handleNotification(const QString& method, const QJsonObject& params);
    void handleServerRequest(const QJsonObject& message);
    QJsonArray dynamicTools() const;
    QJsonObject executeDynamicTool(const QJsonObject& params);
    bool imageApiConfigured() const;
    bool mediaApiConfigured(const QString& kind) const;
    QJsonObject executeImageGeneration(const QJsonObject& arguments);
    QJsonObject executeImageSearch(const QJsonObject& arguments);
    QJsonObject executeMediaGeneration(const QString& kind, const QJsonObject& arguments);
    void sendDynamicToolResponse(const QJsonValue& id, const QJsonObject& result);
    void beginInitialize();
    void beginModelList(const QString& cursor = {});
    void beginProviderCapabilities();
    void beginThreadStart();
    void refreshApiModelsFromDetector();
    void mergeAvailableModels(const QStringList& models, bool persistDetectedModels);
    void refreshReasoningOptions();
    void sendThreadSettingsUpdate();
    void setProtocolState(ProtocolState state);
    void reportProtocolError(const QString& message);
    void ensureWorkspaceStarted();
    void retryWorkspaceStart();
    void startNewThreadFromWorkspace();
    void requestThreadList();
    void requestThreadRead(const QString& threadId);
    void resumeThread(const QString& threadId);
    void forkThread(const QString& threadId);
    void archiveThread(const QString& threadId);
    void processLocalTaskQueue();
    void updateLocalTaskStatus(const QString& taskId, const QString& status, const QString& detail = {});
    void startFromWorkspace(const QString& model, const QString& approvalMode);
    bool resolveRuntimeConfig(
        CodexAppServerSession::Config* config,
        QString* sourceStatus,
        QString* error) const;
    bool resolveAiWorkspaceConfig(
        QString* providerId,
        QString* baseUrl,
        QString* model,
        QStringList* availableModels,
        QProcessEnvironment* environment,
        QString* sourceStatus,
        QString* error) const;
    bool validateWorkspaceConfig(
        const CodexAppServerSession::Config& config,
        const QString& providerId,
        const QString& baseUrl,
        const QString& model,
        QString* error) const;
    bool writeProviderConfig(
        const QString& codexHome,
        const QString& providerId,
        const QString& baseUrl,
        const QString& model,
        const QString& apiKeyEnvironmentVariable,
        QString* error) const;
    void connectWorkspace(CodexAgentWorkspace* workspace);

    CodexAppServerSession _session;
    std::unique_ptr<CodexAgentWorkspace> _workspace;
    bool _initialized = false;
    bool _configured = false;
    ProtocolState _protocolState = ProtocolState::Disabled;
    qint64 _nextRequestId = 1;
    QHash<qint64, PendingRequest> _pendingRequests;
    QHash<qint64, QPair<QString, QString>> _pendingThreadSettings;
    QHash<qint64, QString> _pendingThreadIds;
    QHash<qint64, QString> _nativeRequestMethods;
    QJsonObject _nativeResponses;
    QString _threadId;
    QString _activeTurnId;
    bool _resumingSavedThread = false;
    QString _lastError;
    QStringList _availableModels;
    QHash<QString, QStringList> _reasoningEffortsByModel;
    QHash<QString, QString> _defaultReasoningEffortByModel;
    QString _selectedModel;
    QString _selectedReasoningEffort;
    QString _approvalMode = QStringLiteral("full");
    bool _apiModelRefreshInProgress = false;
    qint64 _lastConfirmedThreadSettingsRequestId = 0;
    bool _restartAfterStop = false;
    QJsonArray _mcpServers;
    std::unique_ptr<QProcess> _terminalProcess;
    QString _runningLocalTaskId;
    QTimer* _localTaskTimer = nullptr;
    QTimer* _terminalTimeoutTimer = nullptr;
    QFileSystemWatcher* _localFileWatcher = nullptr;
    QStringList _suspendedFileWatchPaths;
    QStringList _suspendedDirectoryWatchPaths;
    QStringList _terminalHistory;
    std::unique_ptr<LocalCodexOrchestrator> _orchestrator;
    QHash<QString, JobHandle*> _asyncToolJobs;
    QSet<JobContext*> _activeNetworkJobs;
};

} // namespace cgplay
