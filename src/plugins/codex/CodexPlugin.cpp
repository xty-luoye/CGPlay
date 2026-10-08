#include "CodexPlugin.h"
#include "CodexPluginSupport.h"

#include "common/jobs/JobSystem.h"
#include "common/core/ServiceLocator.h"
#include "ai/api/IAICredentialStore.h"
#include "ai/api/IAIProviderDetector.h"
#include "settings/api/ISettingsService.h"
#include "playback/api/IPlaybackService.h"
#include "annotation/api/IAnnotationService.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFile>
#include <QFutureWatcher>
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QImageReader>
#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QMessageBox>
#include <QEventLoop>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QHttpMultiPart>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QThread>
#include <QWidget>
#include <QUrl>
#include <QUrlQuery>

#include <utility>
#include <QUuid>
#include <QVersionNumber>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#endif

#include <QtConcurrent>

#include <algorithm>

namespace cgplay {

using namespace codex_plugin_detail;

namespace codex_plugin_detail {

const QSet<QString>& officialCapabilityMethods()
{
    static const QSet<QString> methods{
        QStringLiteral("thread/unsubscribe"), QStringLiteral("thread/name/set"), QStringLiteral("thread/goal/get"), QStringLiteral("thread/metadata/update"), QStringLiteral("thread/shellCommand"), QStringLiteral("thread/approveGuardianDeniedAction"), QStringLiteral("thread/loaded/list"), QStringLiteral("thread/inject_items"), QStringLiteral("thread/delete"), QStringLiteral("thread/unarchive"), QStringLiteral("thread/rollback"),
        QStringLiteral("skills/extraRoots/set"), QStringLiteral("skills/config/write"), QStringLiteral("marketplace/remove"), QStringLiteral("marketplace/upgrade"), QStringLiteral("plugin/installed"), QStringLiteral("plugin/skill/read"), QStringLiteral("plugin/share/save"), QStringLiteral("plugin/share/updateTargets"), QStringLiteral("plugin/share/list"), QStringLiteral("plugin/share/checkout"), QStringLiteral("plugin/share/delete"),
        QStringLiteral("fs/readFile"), QStringLiteral("fs/writeFile"), QStringLiteral("fs/createDirectory"), QStringLiteral("fs/getMetadata"), QStringLiteral("fs/readDirectory"), QStringLiteral("fs/remove"), QStringLiteral("fs/copy"), QStringLiteral("fs/watch"), QStringLiteral("fs/unwatch"), QStringLiteral("fuzzyFileSearch"),
        QStringLiteral("windowsSandbox/setupStart"), QStringLiteral("account/login/start"), QStringLiteral("account/login/cancel"), QStringLiteral("account/logout"), QStringLiteral("account/rateLimitResetCredit/consume"), QStringLiteral("account/workspaceMessages/read"), QStringLiteral("account/sendAddCreditsNudgeEmail"), QStringLiteral("feedback/upload"), QStringLiteral("command/exec"), QStringLiteral("command/exec/write"), QStringLiteral("command/exec/terminate"), QStringLiteral("command/exec/resize"), QStringLiteral("experimentalFeature/enablement/set"), QStringLiteral("config/read"),
        QStringLiteral("externalAgentConfig/detect"), QStringLiteral("externalAgentConfig/import"), QStringLiteral("externalAgentConfig/import/readHistories"), QStringLiteral("config/value/write"), QStringLiteral("config/batchWrite"), QStringLiteral("configRequirements/read")
    };
    return methods;
}

} // namespace codex_plugin_detail

CodexPlugin::CodexPlugin(QObject* parent)
    : QObject(parent)
    , _session(this)
{
    _localFileWatcher = new QFileSystemWatcher(this);
    connect(_localFileWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
        if (_workspace) _workspace->appendWorkbenchEvent(QStringLiteral("changes"), tr("Local file changed: %1").arg(path));
        if (QFileInfo::exists(path) && _localFileWatcher && !_localFileWatcher->files().contains(path)) _localFileWatcher->addPath(path);
    });
    connect(_localFileWatcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString& path) {
        if (_workspace) _workspace->appendWorkbenchEvent(QStringLiteral("changes"), tr("Local directory changed: %1").arg(path));
    });
    _orchestrator = std::make_unique<LocalCodexOrchestrator>(this);
    _localTaskTimer = new QTimer(this);
    _localTaskTimer->setInterval(1500);
    connect(_localTaskTimer, &QTimer::timeout, this, &CodexPlugin::processLocalTaskQueue);
    connect(_localTaskTimer, &QTimer::timeout, this, [this] {
        static int maintenanceTicks = 0;
        if (++maintenanceTicks < 40) return;
        maintenanceTicks = 0;
        const QStringList files{
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_async_tasks.jsonl")),
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_terminal.jsonl"))};
        for (const QString& path : files) {
            QFile input(path); if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            QList<QByteArray> lines; while (!input.atEnd()) { lines.push_back(input.readLine()); while (lines.size() > 500) lines.removeFirst(); }
            QSaveFile output(path); if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) continue;
            for (const QByteArray& line : lines) output.write(line); output.commit();
        }
    });
    _terminalTimeoutTimer = new QTimer(this);
    _terminalTimeoutTimer->setSingleShot(true);
    connect(_terminalTimeoutTimer, &QTimer::timeout, this, [this] {
        if (_terminalProcess && _terminalProcess->state() != QProcess::NotRunning) _terminalProcess->kill();
        if (_workspace) _workspace->appendWorkbenchEvent(QStringLiteral("terminal"), tr("Terminal session stopped after timeout."));
    });
    connect(&_session,
        &CodexAppServerSession::stateChanged,
        this,
        &CodexPlugin::appServerStateChanged);
    connect(&_session,
        &CodexAppServerSession::sessionStarted,
        this,
        [this] {
            emit appServerStarted();
            beginInitialize();
        });
    connect(&_session,
        &CodexAppServerSession::sessionStopped,
        this,
        [this](int exitCode, QProcess::ExitStatus exitStatus) {
            _pendingRequests.clear();
            _pendingThreadSettings.clear();
            _threadId.clear();
            _activeTurnId.clear();
            if (_protocolState != ProtocolState::Disabled) setProtocolState(ProtocolState::Disabled);
            emit appServerStopped(exitCode, exitStatus);
            if (_restartAfterStop) {
                _restartAfterStop = false;
                QTimer::singleShot(0, this, &CodexPlugin::ensureWorkspaceStarted);
            }
        });
    connect(&_session,
        &CodexAppServerSession::jsonRpcLineReceived,
        this,
        [this](const QByteArray& utf8Line) {
            emit jsonRpcLineReceived(utf8Line);
            handleJsonRpcLine(utf8Line);
        });
    connect(&_session,
        &CodexAppServerSession::standardErrorLineReceived,
        this,
        [this](const QByteArray& line) {
            emit standardErrorLineReceived(redactSensitiveText(line));
        });
    connect(&_session, &CodexAppServerSession::sessionError, this, [this](const QString& message) {
        _lastError = message;
        _pendingRequests.clear();
        _pendingThreadSettings.clear();
        _threadId.clear();
        _activeTurnId.clear();
        if (_protocolState != ProtocolState::Disabled &&
            _protocolState != ProtocolState::Stopping) {
            setProtocolState(ProtocolState::Failed);
        }
        emit appServerError(message);
    });
}

QString CodexPlugin::id() const
{
    return QStringLiteral("codex");
}

void CodexPlugin::initialize()
{
    if (_initialized) return;
    // The workspace requests startup when its Dock becomes visible.
    _initialized = true;
    if (_localTaskTimer) _localTaskTimer->start();
    if (_localFileWatcher) {
        for (const QString& path : std::as_const(_suspendedFileWatchPaths)) {
            if (QFileInfo::exists(path)) _localFileWatcher->addPath(path);
        }
        for (const QString& path : std::as_const(_suspendedDirectoryWatchPaths)) {
            if (QDir(path).exists()) _localFileWatcher->addPath(path);
        }
    }
    _suspendedFileWatchPaths.clear();
    _suspendedDirectoryWatchPaths.clear();
}

void CodexPlugin::shutdown()
{
    if (!_initialized) return;
    _initialized = false;
    if (_localTaskTimer) _localTaskTimer->stop();
    if (_terminalTimeoutTimer) _terminalTimeoutTimer->stop();
    if (_localFileWatcher) {
        _suspendedFileWatchPaths = _localFileWatcher->files();
        _suspendedDirectoryWatchPaths = _localFileWatcher->directories();
        if (!_suspendedFileWatchPaths.isEmpty()) {
            _localFileWatcher->removePaths(_suspendedFileWatchPaths);
        }
        if (!_suspendedDirectoryWatchPaths.isEmpty()) {
            _localFileWatcher->removePaths(_suspendedDirectoryWatchPaths);
        }
    }
    if (_terminalProcess && _terminalProcess->state() != QProcess::NotRunning) _terminalProcess->kill();
    for (JobHandle* job : std::as_const(_asyncToolJobs)) {
        if (job) job->cancel();
    }
    _asyncToolJobs.clear();
    for (JobContext* job : std::as_const(_activeNetworkJobs)) {
        if (job) job->cancel();
    }
    _activeNetworkJobs.clear();
    _restartAfterStop = false;
    _pendingRequests.clear();
    _pendingThreadSettings.clear();
    _pendingThreadIds.clear();
    _threadId.clear();
    _activeTurnId.clear();
    if (_session.isRunning()) {
        setProtocolState(ProtocolState::Stopping);
        _session.stop();
    } else {
        setProtocolState(ProtocolState::Disabled);
    }
}

bool CodexPlugin::startAppServer(const CodexAppServerSession::Config& config, QString* error)
{
    if (!_initialized) {
        const QString message = tr("Codex 插件尚未初始化。");
        _lastError = message;
        if (error) *error = message;
        return false;
    }

    if (config.executablePath.trimmed().isEmpty() || config.codexHome.trimmed().isEmpty()) {
        const QString message = tr("Codex 尚未配置 Codex 可执行文件和 CODEX_HOME，无法启动。");
        _lastError = message;
        if (error) *error = message;
        return false;
    }

    _configured = true;
    _nextRequestId = 1;
    _pendingRequests.clear();
    _pendingThreadSettings.clear();
    _lastConfirmedThreadSettingsRequestId = 0;
    _threadId.clear();
    _activeTurnId.clear();
    _reasoningEffortsByModel.clear();
    _defaultReasoningEffortByModel.clear();
    if (_workspace) _workspace->setReasoningOptions({}, {});
    setProtocolState(ProtocolState::Initializing);
    _lastError.clear();

    QString sessionError;
    if (!_session.start(config, &sessionError)) {
        _configured = false;
        _lastError = sessionError;
        if (error) *error = sessionError;
        return false;
    }
    return true;
}

bool CodexPlugin::sendJsonRpc(const QJsonObject& message, QString* error)
{
    QString sessionError;
    if (_session.sendJsonRpc(message, &sessionError)) return true;

    _lastError = sessionError;
    if (error) *error = sessionError;
    return false;
}

bool CodexPlugin::sendNativeRpc(const QString& method, const QJsonObject& params, qint64* requestId, QString* error)
{
    const QString normalized = method.trimmed();
    if (normalized.isEmpty() || normalized.contains(QRegularExpression(QStringLiteral("[\\r\\n]")))) {
        if (error) *error = QStringLiteral("app-server method must be a non-empty single-line string.");
        return false;
    }
    const qint64 id = _nextRequestId++;
    if (requestId) *requestId = id;
    if (!sendJsonRpc(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("method"), normalized}, {QStringLiteral("params"), params}}, error)) return false;
    _nativeRequestMethods.insert(id, normalized);
    return true;
}

bool CodexPlugin::reloadMcpServers(QString* error)
{
    return sendNativeRpc(QStringLiteral("config/mcpServer/reload"), {}, nullptr, error);
}

bool CodexPlugin::callMcpTool(const QString& server, const QString& tool, const QJsonObject& arguments, QString* error)
{
    if (server.trimmed().isEmpty() || tool.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("MCP server and tool are required.");
        return false;
    }
    if (_threadId.isEmpty()) { if (error) *error = QStringLiteral("A current thread is required for MCP tool calls."); return false; }
    return sendNativeRpc(QStringLiteral("mcpServer/tool/call"), QJsonObject{
        {QStringLiteral("server"), server.trimmed()},
        {QStringLiteral("tool"), tool.trimmed()},
        {QStringLiteral("threadId"), _threadId},
        {QStringLiteral("arguments"), arguments}}, nullptr, error);
}

bool CodexPlugin::loginMcpServer(const QString& server, QString* error)
{
    if (server.trimmed().isEmpty()) { if (error) *error = QStringLiteral("MCP server is required."); return false; }
    QJsonObject params{{QStringLiteral("name"), server.trimmed()}};
    if (!_threadId.isEmpty()) params.insert(QStringLiteral("threadId"), _threadId);
    return sendNativeRpc(QStringLiteral("mcpServer/oauth/login"), params, nullptr, error);
}

bool CodexPlugin::readMcpResource(const QString& server, const QString& uri, QString* error)
{
    if (server.trimmed().isEmpty() || uri.trimmed().isEmpty()) { if (error) *error = QStringLiteral("MCP server and resource URI are required."); return false; }
    QJsonObject params{{QStringLiteral("server"), server.trimmed()}, {QStringLiteral("uri"), uri.trimmed()}};
    if (!_threadId.isEmpty()) params.insert(QStringLiteral("threadId"), _threadId);
    return sendNativeRpc(QStringLiteral("mcpServer/resource/read"), params, nullptr, error);
}

bool CodexPlugin::interruptTurn(QString* error)
{
    if (_threadId.isEmpty() || _activeTurnId.isEmpty()) { if (error) *error = QStringLiteral("No active turn."); return false; }
    return sendNativeRpc(QStringLiteral("turn/interrupt"), {{QStringLiteral("threadId"), _threadId}, {QStringLiteral("turnId"), _activeTurnId}}, nullptr, error);
}

bool CodexPlugin::steerTurn(const QString& text, QString* error)
{
    if (_threadId.isEmpty() || _activeTurnId.isEmpty() || text.trimmed().isEmpty()) { if (error) *error = QStringLiteral("Active turn and steering text are required."); return false; }
    return sendNativeRpc(QStringLiteral("turn/steer"), {{QStringLiteral("threadId"), _threadId}, {QStringLiteral("expectedTurnId"), _activeTurnId}, {QStringLiteral("input"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text.trimmed()}}}}}, nullptr, error);
}

bool CodexPlugin::steerImagesTurn(const QStringList& paths, const QString& text, QString* error)
{
    if (_threadId.isEmpty() || _activeTurnId.isEmpty() || paths.isEmpty()) {
        if (error) *error = QStringLiteral("Active turn and at least one image are required.");
        return false;
    }
    QJsonArray input{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                 {QStringLiteral("text"), text.trimmed().isEmpty() ? QStringLiteral("Use these images as additional guidance.") : text.trimmed()}}};
    for (const QString& path : paths) {
        const QFileInfo image(path);
        if (!image.isAbsolute() || !image.isFile()) {
            if (error) *error = QStringLiteral("Every steering image must be an existing absolute file.");
            return false;
        }
        input.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("localImage")},
                                    {QStringLiteral("path"), image.absoluteFilePath()},
                                    {QStringLiteral("detail"), QStringLiteral("auto")}});
    }
    return sendNativeRpc(
        QStringLiteral("turn/steer"),
        {{QStringLiteral("threadId"), _threadId},
         {QStringLiteral("expectedTurnId"), _activeTurnId},
         {QStringLiteral("input"), input}},
        nullptr,
        error);
}

bool CodexPlugin::compactThread(QString* error) { return !_threadId.isEmpty() && sendNativeRpc(QStringLiteral("thread/compact/start"), {{QStringLiteral("threadId"), _threadId}}, nullptr, error); }
bool CodexPlugin::setThreadGoal(const QString& objective, QString* error) { return !_threadId.isEmpty() && sendNativeRpc(QStringLiteral("thread/goal/set"), {{QStringLiteral("threadId"), _threadId}, {QStringLiteral("objective"), objective}}, nullptr, error); }
bool CodexPlugin::clearThreadGoal(QString* error) { return !_threadId.isEmpty() && sendNativeRpc(QStringLiteral("thread/goal/clear"), {{QStringLiteral("threadId"), _threadId}}, nullptr, error); }
bool CodexPlugin::startReview(QString* error) { return !_threadId.isEmpty() && sendNativeRpc(QStringLiteral("review/start"), {{QStringLiteral("threadId"), _threadId}, {QStringLiteral("target"), QJsonObject{{QStringLiteral("type"), QStringLiteral("uncommittedChanges")}}}}, nullptr, error); }
bool CodexPlugin::refreshNativeCatalog(QString* error)
{
    if (!sendNativeRpc(QStringLiteral("mcpServerStatus/list"), {}, nullptr, error)) return false;
    if (!listSkills(true, error)) return false;
    if (!sendNativeRpc(QStringLiteral("plugin/list"), {}, nullptr, error)) return false;
    return sendNativeRpc(QStringLiteral("app/list"), {}, nullptr, error);
}

bool CodexPlugin::listSkills(bool forceReload, QString* error)
{
    return sendNativeRpc(QStringLiteral("skills/list"), {{QStringLiteral("forceReload"), forceReload}}, nullptr, error);
}

bool CodexPlugin::readPlugin(const QString& pluginName, QString* error)
{
    if (pluginName.trimmed().isEmpty()) { if (error) *error = QStringLiteral("pluginName is required."); return false; }
    return sendNativeRpc(QStringLiteral("plugin/read"), {{QStringLiteral("pluginName"), pluginName.trimmed()}}, nullptr, error);
}

bool CodexPlugin::installPlugin(const QString& pluginName, QString* error)
{
    if (pluginName.trimmed().isEmpty()) { if (error) *error = QStringLiteral("pluginName is required."); return false; }
    return sendNativeRpc(QStringLiteral("plugin/install"), {{QStringLiteral("pluginName"), pluginName.trimmed()}}, nullptr, error);
}

bool CodexPlugin::uninstallPlugin(const QString& pluginName, QString* error)
{
    if (pluginName.trimmed().isEmpty()) { if (error) *error = QStringLiteral("pluginName is required."); return false; }
    return sendNativeRpc(QStringLiteral("plugin/uninstall"), {{QStringLiteral("pluginName"), pluginName.trimmed()}}, nullptr, error);
}

bool CodexPlugin::addMarketplace(const QString& source, QString* error)
{
    if (source.trimmed().isEmpty()) { if (error) *error = QStringLiteral("marketplace source is required."); return false; }
    return sendNativeRpc(QStringLiteral("marketplace/add"), {{QStringLiteral("source"), source.trimmed()}}, nullptr, error);
}

bool CodexPlugin::invokeOfficialCapability(const QString& method, const QJsonObject& params, QString* error)
{
    const QString normalized = method.trimmed();
    if (!officialCapabilityMethods().contains(normalized)) {
        if (error) *error = QStringLiteral("Unsupported or unsafe official capability: %1").arg(normalized);
        return false;
    }
    return sendNativeRpc(normalized, params, nullptr, error);
}

bool CodexPlugin::submitPrompt(const QString& prompt, QString* error)
{
    if (_protocolState != ProtocolState::Ready || _threadId.isEmpty()) {
        const QString message = tr("Codex 会话尚未准备好接收新消息。");
        _lastError = message;
        if (error) *error = message;
        return false;
    }
    if (prompt.trimmed().isEmpty()) {
        const QString message = tr("Codex 消息不能为空。");
        _lastError = message;
        if (error) *error = message;
        return false;
    }

    const QString normalizedPrompt = prompt.toLower();
    const bool requestsCurrentFrame = normalizedPrompt.contains(QStringLiteral("当前画面")) ||
        normalizedPrompt.contains(QStringLiteral("当前帧")) ||
        normalizedPrompt.contains(QStringLiteral("播放器画面")) ||
        normalizedPrompt.contains(QStringLiteral("current frame")) ||
        normalizedPrompt.contains(QStringLiteral("player screen"));
    if (requestsCurrentFrame) {
        auto* viewport = reinterpret_cast<QWidget*>(QCoreApplication::instance()->property("cgplay.codex.activeViewport").toULongLong());
        if (viewport && viewport->isVisible()) {
            const QPixmap frame = viewport->grab();
            if (!frame.isNull()) {
                const QString directory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_frames"));
                QDir().mkpath(directory);
                const QString path = QDir(directory).filePath(QStringLiteral("auto_frame_%1.png").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
                if (frame.save(path, "PNG")) return submitImagesPrompt({path}, prompt, error, QStringLiteral("auto-frame"));
            }
        }
    }

    const QString guardedPrompt = prompt + QStringLiteral(
        "\n\n[CGPlay host policy for generated files and DCC projects]\n"
        "For requests that require Houdini, Blender, Maya, Nuke, or another installed DCC, call "
        "cgplay.software_detect first. Trust its returned absolute executable paths; do not infer that "
        "software is unavailable merely because a shell PATH lookup fails. Use cgplay.software_run or "
        "cgplay.workspace_run_process with the detected executable. Put the final deliverable inside the "
        "current workspace and call cgplay.workspace_register_artifact for the final file. Do not present "
        "a helper script, source file, or future filename as the completed deliverable. Do not finish until "
        "the final file exists and artifact registration succeeds.\n"
        "\n\n[CGPlay host policy for image requests]\n"
        "If this request asks to generate an image, use cgplay.generate_image when that tool is "
        "registered for this turn; otherwise use native image_gen.imagegen when it is registered. "
        "Never use shell, Python, PowerShell, curl, "
        "or custom image code as an image-generation fallback. Never call the official OpenAI API "
        "directly, never read, request, suggest, or require OPENAI_API_KEY, and never reinterpret the "
        "current text Workbench credential as an image credential. If neither image tool is registered "
        "for this turn, state that image generation is not configured or supported "
        "and stop without attempting another endpoint or credential.");

    QJsonObject params{
        {QStringLiteral("threadId"), _threadId},
        {QStringLiteral("clientUserMessageId"), QStringLiteral("cgplay-%1").arg(_nextRequestId)},
        {QStringLiteral("model"), _selectedModel},
        {QStringLiteral("approvalPolicy"), approvalPolicyForMode(_approvalMode)},
        {QStringLiteral("input"), QJsonArray{QJsonObject{
            {QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("text"), guardedPrompt}
        }}}
    };
    if (!_selectedReasoningEffort.isEmpty()) {
        params.insert(QStringLiteral("effort"), _selectedReasoningEffort);
    }
    if (!sendRequest(PendingRequest::TurnStart, QStringLiteral("turn/start"), params, error)) {
        return false;
    }

    setProtocolState(ProtocolState::TurnInProgress);
    return true;
}

bool CodexPlugin::submitImagePrompt(const QString& path, const QString& prompt, QString* error)
{
    return submitImagesPrompt({path}, prompt, error, QStringLiteral("user-attachment"));
}

bool CodexPlugin::submitImagesPrompt(const QStringList& paths, const QString& prompt, QString* error, const QString& source)
{
    if (_protocolState != ProtocolState::Ready || _threadId.isEmpty() || paths.isEmpty()) {
        if (error) *error = QStringLiteral("A ready thread and at least one image are required.");
        return false;
    }
    QJsonArray input{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), prompt.trimmed().isEmpty() ? QStringLiteral("Inspect these images.") : prompt.trimmed()}}};
    for (const QString& path : paths) {
        const QFileInfo image(path);
        if (!image.isAbsolute() || !image.exists() || !image.isFile()) {
            if (error) *error = QStringLiteral("Every image path must be an existing absolute file.");
            return false;
        }
        input.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("localImage")}, {QStringLiteral("path"), image.absoluteFilePath()}, {QStringLiteral("detail"), QStringLiteral("auto")}});
    }
    QJsonObject params{{QStringLiteral("threadId"), _threadId},
        {QStringLiteral("clientUserMessageId"), QStringLiteral("cgplay-images-%1").arg(_nextRequestId)},
        {QStringLiteral("model"), _selectedModel},
        {QStringLiteral("approvalPolicy"), approvalPolicyForMode(_approvalMode)},
        {QStringLiteral("input"), input}};
    if (!_selectedReasoningEffort.isEmpty()) params.insert(QStringLiteral("effort"), _selectedReasoningEffort);
    if (!sendRequest(PendingRequest::TurnStart, QStringLiteral("turn/start"), params, error)) return false;
    QJsonArray recordedPaths;
    for (const QString& path : paths) recordedPaths.push_back(path);
    appendMediaAssetRecord(QStringLiteral("chat-image-input"), paths.first(), QJsonObject{
        {QStringLiteral("threadId"), _threadId},
        {QStringLiteral("prompt"), prompt.trimmed().isEmpty() ? QStringLiteral("Inspect these images.") : prompt.trimmed()},
        {QStringLiteral("paths"), recordedPaths},
        {QStringLiteral("source"), source.trimmed().isEmpty() ? QStringLiteral("user-attachment") : source.trimmed()},
        {QStringLiteral("submissionId"), QUuid::createUuid().toString(QUuid::WithoutBraces)}
    });
    setProtocolState(ProtocolState::TurnInProgress);
    return true;
}

void CodexPlugin::stopAppServer()
{
    setProtocolState(ProtocolState::Stopping);
    _session.stop();
}

QWidget* CodexPlugin::workspaceWidget()
{
    if (_workspace) return _workspace.get();

    _workspace = std::make_unique<CodexAgentWorkspace>();
    connectWorkspace(_workspace.get());
    _workspace->setProperty("codexDetectedSoftware", discoverSoftwareExecutables());
    CodexAppServerSession::Config runtimeConfig;
    QString runtimeStatus;
    QString runtimeError;
    const bool runtimeConfigured = resolveRuntimeConfig(&runtimeConfig, &runtimeStatus, &runtimeError);
    _workspace->setRuntimeSourceStatus(
        runtimeConfigured ? runtimeStatus : runtimeError,
        runtimeConfigured);
    if (runtimeConfigured) {
        _workspace->setProjectPath(runtimeConfig.workingDirectory);
    }

    QString providerId;
    QString baseUrl;
    QString model;
    QStringList availableModels;
    QString sourceStatus;
    QString error;
    QProcessEnvironment environment;
    const bool configured = resolveAiWorkspaceConfig(
        &providerId, &baseUrl, &model, &availableModels, &environment, &sourceStatus, &error);
    _workspace->setAiWorkspaceSourceStatus(configured ? sourceStatus : error, configured);
    if (configured) {
        auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"));
        const QString persistedModel = settings
            ? settings->value(QStringLiteral("codex/workspace/model")).toString().trimmed()
            : QString();
        _availableModels = availableModels;
        _selectedModel = persistedModel.isEmpty() ? model : persistedModel;
        appendUniqueModel(&_availableModels, _selectedModel);
        _workspace->setModelOptions(_availableModels, _selectedModel);

        _selectedReasoningEffort = settings
            ? settings->value(QStringLiteral("codex/workspace/reasoningEffort")).toString().trimmed().toLower()
            : QString();
        _workspace->setReasoningOptions({}, {});

        _approvalMode = settings
            ? settings->value(QStringLiteral("codex/workspace/approvalMode"), QStringLiteral("full"))
                  .toString()
                  .trimmed()
            : QStringLiteral("full");
        if (_approvalMode != QStringLiteral("request") &&
            _approvalMode != QStringLiteral("auto") &&
            _approvalMode != QStringLiteral("full")) {
            _approvalMode = QStringLiteral("full");
        }
        _workspace->setApprovalMode(_approvalMode);
        refreshApiModelsFromDetector();
    }
    return _workspace.get();
}

bool CodexPlugin::isAppServerRunning() const
{
    return _session.isRunning();
}

QJsonObject CodexPlugin::diagnostics() const
{
    const CodexAppServerSession::Config config = _session.config();
    return {
        {QStringLiteral("initialized"), _initialized},
        {QStringLiteral("configured"), _configured},
        {QStringLiteral("running"), _session.isRunning()},
        {QStringLiteral("protocolState"), static_cast<int>(_protocolState)},
        {QStringLiteral("processId"), static_cast<qint64>(_session.processId())},
        {QStringLiteral("state"), static_cast<int>(_session.state())},
        {QStringLiteral("executableConfigured"), !config.executablePath.trimmed().isEmpty()},
        {QStringLiteral("executableExists"), QFileInfo::exists(config.executablePath)},
        {QStringLiteral("codexHomeConfigured"), !config.codexHome.trimmed().isEmpty()},
        {QStringLiteral("threadReady"), !_threadId.isEmpty()},
        {QStringLiteral("turnInProgress"), !_activeTurnId.isEmpty()},
        {QStringLiteral("selectedModel"), _selectedModel},
        {QStringLiteral("selectedReasoningEffort"), _selectedReasoningEffort},
        {QStringLiteral("availableModelCount"), _availableModels.size()},
        {QStringLiteral("capabilityMatrix"), QJsonObject{
            {QStringLiteral("actualTurnStart"), true},
            {QStringLiteral("actualToolRegistration"), QJsonArray{QStringLiteral("cgplay.player_status"), QStringLiteral("cgplay.player_control")}},
            {QStringLiteral("actualToolCalls"), QJsonArray{
                QStringLiteral("cgplay.player_status (when requested by upstream)"),
                QStringLiteral("item/tool/call dispatch path (validated locally)")}},
            {QStringLiteral("sessionOperations"), QJsonArray{QStringLiteral("thread/list"), QStringLiteral("thread/read"), QStringLiteral("thread/resume"), QStringLiteral("thread/fork"), QStringLiteral("thread/archive"), QStringLiteral("thread/start")}},
            {QStringLiteral("usageNotifications"), QJsonArray{QStringLiteral("thread/tokenUsage/updated")}},
            {QStringLiteral("imageGeneration"), QJsonObject{
                {QStringLiteral("registrationSource"), QStringLiteral("upstream provider capabilities")},
                {QStringLiteral("fallbackRegistered"), false},
                {QStringLiteral("providerRequired"), true},
                {QStringLiteral("status"), QStringLiteral("only callable when provider exposes native imageGeneration")}}},
            {QStringLiteral("unsupportedWithoutEvidence"), QJsonArray{QStringLiteral("provider-specific web/MCP/extensions"), QStringLiteral("fallback image endpoint")}}
        }},
        {QStringLiteral("lastError"), _lastError}
    };
}

bool CodexPlugin::sendRequest(
    PendingRequest request,
    const QString& method,
    const QJsonObject& params,
    QString* error)
{
    const qint64 requestId = _nextRequestId++;
    const QJsonObject message{
        {QStringLiteral("id"), requestId},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params}
    };
    _pendingRequests.insert(requestId, request);
    if (request == PendingRequest::ThreadSettingsUpdate) {
        _pendingThreadSettings.insert(
            requestId,
            qMakePair(_selectedModel, _selectedReasoningEffort));
    }
    if (!sendJsonRpc(message, error)) {
        _pendingRequests.remove(requestId);
        _pendingThreadSettings.remove(requestId);
        return false;
    }
    return true;
}

bool CodexPlugin::sendNotification(const QString& method, const QJsonObject& params)
{
    QJsonObject message{{QStringLiteral("method"), method}};
    if (!params.isEmpty()) message.insert(QStringLiteral("params"), params);
    QString error;
    if (sendJsonRpc(message, &error)) return true;

    reportProtocolError(error);
    return false;
}

void CodexPlugin::handleJsonRpcLine(const QByteArray& utf8Line)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(utf8Line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        reportProtocolError(tr("Codex app-server 输出了无效的 JSON-RPC：%1")
                                .arg(parseError.errorString()));
        return;
    }

    const QJsonObject message = document.object();
    const QString method = message.value(QStringLiteral("method")).toString();
    if (_workspace) {
        if (!method.isEmpty()) {
            _workspace->appendLogLine(tr("RPC 通知：%1").arg(method));
        } else {
            _workspace->appendLogLine(tr("RPC 响应：id=%1")
                .arg(message.value(QStringLiteral("id")).toVariant().toString()));
        }
    }
    if (!method.isEmpty()) {
        if (message.contains(QStringLiteral("id"))) emit nativeRpcServerRequest(message);
        else emit nativeRpcNotification(method, message.value(QStringLiteral("params")).toObject());
        if (message.contains(QStringLiteral("id"))) handleServerRequest(message);
        else handleNotification(method, message.value(QStringLiteral("params")).toObject());
        return;
    }

    bool validRequestId = false;
    const qint64 requestId = message.value(QStringLiteral("id")).toVariant().toLongLong(&validRequestId);
    if (!validRequestId) {
        reportProtocolError(tr("Codex app-server 响应缺少数字请求 ID。"));
        return;
    }
    handleResponse(requestId, message);
}

void CodexPlugin::handleResponse(qint64 requestId, const QJsonObject& message)
{
    emit nativeRpcResponse(requestId, message);
    const auto request = _pendingRequests.find(requestId);
    if (request == _pendingRequests.end()) {
        const QString method = _nativeRequestMethods.take(requestId);
        if (!method.isEmpty()) {
            _nativeResponses.insert(method, QJsonObject{{QStringLiteral("result"), message.value(QStringLiteral("result"))}, {QStringLiteral("error"), message.value(QStringLiteral("error"))}});
        }
        if (_workspace) {
            const QString safe = QString::fromUtf8(redactSensitiveText(QJsonDocument(message).toJson(QJsonDocument::Compact)));
            _workspace->setProperty("codexLastNativeRpcResponse", safe);
            _workspace->setProperty("codexNativeResponses", _nativeResponses);
            _workspace->appendLogLine(tr("Native RPC response id=%1: %2").arg(requestId).arg(safe.left(1600)));
            QString category = QStringLiteral("rules");
            if (method.startsWith(QStringLiteral("mcpServer")) || method.startsWith(QStringLiteral("plugin")) ||
                method.startsWith(QStringLiteral("marketplace")) || method == QStringLiteral("skills/list") || method == QStringLiteral("app/list")) {
                category = QStringLiteral("extensions");
            } else if (method.startsWith(QStringLiteral("account")) || method.startsWith(QStringLiteral("permission")) ||
                       method.startsWith(QStringLiteral("experimental")) || method.startsWith(QStringLiteral("hooks")) ||
                       method.startsWith(QStringLiteral("config"))) {
                category = QStringLiteral("rules");
            }
            _workspace->appendWorkbenchEvent(category, tr("%1\n%2").arg(method, safe.left(12000)));
        }
        return;
    }

    const PendingRequest kind = request.value();
    _pendingRequests.erase(request);
    const QPair<QString, QString> requestedThreadSettings = _pendingThreadSettings.take(requestId);
    const QString requestedThreadId = _pendingThreadIds.take(requestId);
    const QJsonObject rpcError = message.value(QStringLiteral("error")).toObject();
    if (!rpcError.isEmpty()) {
        const QString rpcMessage = rpcError.value(QStringLiteral("message")).toString(
            tr("Codex app-server 拒绝了请求。"));
        if (kind == PendingRequest::ModelList) {
            if (_workspace) {
                _workspace->appendLogLine(tr("模型目录获取失败，已保留现有模型：%1").arg(rpcMessage));
            }
            refreshReasoningOptions();
            beginThreadStart();
            return;
        }
        if (kind == PendingRequest::ProviderCapabilities) {
            if (_workspace) {
                _workspace->setImageGenerationCapability(
                    imageApiConfigured(),
                    imageApiConfigured()
                        ? tr("独立图片 API 已配置；原生 image_gen 不可用不影响生图。")
                        : rpcMessage);
            }
            beginModelList();
            return;
        }
        if (kind == PendingRequest::ThreadSettingsUpdate) {
            if (_workspace) {
                _workspace->appendLogLine(tr("模型或推理强度更新失败：%1").arg(rpcMessage));
            }
            return;
        }
        if (kind == PendingRequest::ThreadResume && _resumingSavedThread) {
            _resumingSavedThread = false;
            if (auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"))) {
                settings->remove(QStringLiteral("codex/workspace/lastThreadId"));
                settings->sync();
            }
            beginThreadStart();
            return;
        }
        reportProtocolError(rpcMessage);
        return;
    }

    const QJsonObject result = message.value(QStringLiteral("result")).toObject();
    switch (kind) {
    case PendingRequest::Initialize:
        if (!sendNotification(QStringLiteral("initialized"))) return;
        sendRequest(PendingRequest::McpServerStatus, QStringLiteral("mcpServerStatus/list"), {});
        sendRequest(PendingRequest::SkillsList, QStringLiteral("skills/list"), {});
        sendRequest(PendingRequest::PluginList, QStringLiteral("plugin/list"), {});
        sendRequest(PendingRequest::AppList, QStringLiteral("app/list"), {});
        for (const QString& method : {QStringLiteral("account/read"), QStringLiteral("account/usage/read"),
                 QStringLiteral("account/rateLimits/read"), QStringLiteral("permissionProfile/list"),
                 QStringLiteral("collaborationMode/list"), QStringLiteral("experimentalFeature/list"),
                 QStringLiteral("windowsSandbox/readiness"), QStringLiteral("hooks/list")}) {
            sendNativeRpc(method, {});
        }
        beginProviderCapabilities();
        return;
    case PendingRequest::McpServerStatus:
        if (_workspace) {
            const QJsonArray servers = result.value(QStringLiteral("data")).toArray();
            _mcpServers = servers;
            _workspace->setProperty("codexMcpServers", servers);
            _workspace->appendLogLine(tr("MCP servers: %1").arg(servers.size()));
            for (const QJsonValue& value : servers) {
                const QJsonObject server = value.toObject();
                const QString name = server.value(QStringLiteral("name")).toString();
                const QString auth = server.value(QStringLiteral("authStatus")).toString();
                const int tools = server.value(QStringLiteral("tools")).toObject().size();
                const int resources = server.value(QStringLiteral("resources")).toArray().size();
                _workspace->appendLogLine(tr("MCP %1: auth=%2, tools=%3, resources=%4").arg(name, auth).arg(tools).arg(resources));
            }
            const QString safe = QString::fromUtf8(redactSensitiveText(QJsonDocument(servers).toJson(QJsonDocument::Indented)));
            _workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("MCP servers refreshed: %1\n%2").arg(servers.size()).arg(safe.left(24000)));
        }
        return;
    case PendingRequest::SkillsList:
    case PendingRequest::PluginList:
    case PendingRequest::AppList:
        if (_workspace) {
            const QString label = kind == PendingRequest::SkillsList ? QStringLiteral("skills")
                : (kind == PendingRequest::PluginList ? QStringLiteral("plugins") : QStringLiteral("apps"));
            const QJsonArray values = result.value(QStringLiteral("data")).toArray();
            const char* propertyName = kind == PendingRequest::SkillsList ? "codexSkills" : (kind == PendingRequest::PluginList ? "codexPlugins" : "codexApps");
            _workspace->setProperty(propertyName, values);
            _workspace->appendLogLine(tr("%1 discovered: %2").arg(label).arg(values.size()));
            const QString safe = QString::fromUtf8(redactSensitiveText(QJsonDocument(values).toJson(QJsonDocument::Indented)));
            _workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("%1 discovered: %2\n%3").arg(label, QString::number(values.size()), safe.left(24000)));
        }
        return;
    case PendingRequest::ProviderCapabilities:
        if (_workspace) {
            const bool declared = result.value(QStringLiteral("imageGeneration")).toBool();
            _workspace->setImageGenerationCapability(
                imageApiConfigured() || declared,
                imageApiConfigured()
                    ? tr("独立图片 API 已配置。")
                    : (declared
                        ? tr("服务商声明了 imageGeneration，但当前会话尚未注册原生 image_gen.imagegen 工具")
                        : tr("当前服务商协议未声明 imageGeneration")));
        }
        beginModelList();
        return;
    case PendingRequest::ModelList: {
        const QJsonArray models = result.value(QStringLiteral("data")).toArray();
        for (const QJsonValue& value : models) {
            const QJsonObject item = value.toObject();
            QString model = item.value(QStringLiteral("model")).toString().trimmed();
            if (model.isEmpty()) model = item.value(QStringLiteral("id")).toString().trimmed();
            if (model.isEmpty()) continue;

            appendUniqueModel(&_availableModels, model);
            QStringList efforts;
            const QJsonArray effortOptions = item.value(QStringLiteral("supportedReasoningEfforts")).toArray();
            for (const QJsonValue& effortValue : effortOptions) {
                appendUniqueModel(
                    &efforts,
                    effortValue.toObject().value(QStringLiteral("reasoningEffort")).toString().toLower());
            }
            const QString defaultEffort = item
                .value(QStringLiteral("defaultReasoningEffort"))
                .toString()
                .trimmed()
                .toLower();
            appendUniqueModel(&efforts, defaultEffort);
            const QString key = normalizedModelKey(model);
            _reasoningEffortsByModel.insert(key, efforts);
            _defaultReasoningEffortByModel.insert(key, defaultEffort);
            if (_selectedModel.isEmpty() && item.value(QStringLiteral("isDefault")).toBool()) {
                _selectedModel = model;
            }
        }
        appendUniqueModel(&_availableModels, _selectedModel);
        if (_workspace) _workspace->setModelOptions(_availableModels, _selectedModel);
        refreshReasoningOptions();

        const QString nextCursor = result.value(QStringLiteral("nextCursor")).toString().trimmed();
        if (!nextCursor.isEmpty()) {
            beginModelList(nextCursor);
        } else {
            beginThreadStart();
        }
        return;
    }
    case PendingRequest::ThreadStart: {
        const QString threadId = result.value(QStringLiteral("thread")).toObject()
                                     .value(QStringLiteral("id")).toString();
        if (threadId.isEmpty()) {
            reportProtocolError(tr("Codex thread/start 响应缺少 thread.id。"));
            return;
        }
        _threadId = threadId;
        if (auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"))) {
            settings->setValue(QStringLiteral("codex/workspace/lastThreadId"), _threadId);
            settings->setValue(QStringLiteral("codex/workspace/dynamicToolSignature"), dynamicToolSignature(dynamicTools()));
            settings->setValue(QStringLiteral("codex/workspace/dynamicToolSchemaVersion"), QString::fromLatin1(kDynamicToolSchemaVersion));
            settings->sync();
        }
        _activeTurnId.clear();
        setProtocolState(ProtocolState::Ready);
        sendThreadSettingsUpdate();
        emit threadReady(_threadId);
        return;
    }
    case PendingRequest::TurnStart: {
        const QString turnId = result.value(QStringLiteral("turn")).toObject()
                                   .value(QStringLiteral("id")).toString();
        if (turnId.isEmpty()) {
            reportProtocolError(tr("Codex turn/start 响应缺少 turn.id。"));
            return;
        }
        _activeTurnId = turnId;
        emit turnStarted(_threadId, _activeTurnId);
        return;
    }
    case PendingRequest::ThreadSettingsUpdate:
        if (_workspace && requestId >= _lastConfirmedThreadSettingsRequestId) {
            _lastConfirmedThreadSettingsRequestId = requestId;
            _workspace->setProperty("codexConfirmedModel", requestedThreadSettings.first);
            _workspace->setProperty("codexConfirmedReasoningEffort", requestedThreadSettings.second);
            _workspace->appendLogLine(tr("线程设置已确认：%1 · %2")
                .arg(
                    requestedThreadSettings.first,
                    requestedThreadSettings.second.isEmpty()
                        ? tr("默认推理")
                        : requestedThreadSettings.second));
        }
        return;
    case PendingRequest::ThreadList:
        if (_workspace) {
            const QJsonArray threads = result.value(QStringLiteral("data")).toArray();
            _workspace->setSessionList(threads, _threadId);
            _workspace->setProperty("codexSessionList", threads);
        }
        return;
    case PendingRequest::ThreadRead:
        if (_workspace) {
            QJsonObject thread = result.value(QStringLiteral("thread")).toObject();
            const QJsonObject localHistory = loadLocalThreadHistory(requestedThreadId);
            // App-server history omits local image artifacts. The filtered JSONL is the
            // complete visual transcript; keep the server object only for its metadata.
            if (!localHistory.isEmpty()) {
                thread.insert(QStringLiteral("turns"), localHistory.value(QStringLiteral("turns")));
            }
            _workspace->showThreadHistory(thread);
            _workspace->setProperty("codexSessionReadId", requestedThreadId);
        }
        return;
    case PendingRequest::ThreadResume:
    case PendingRequest::ThreadFork: {
        QJsonObject thread = result.value(QStringLiteral("thread")).toObject();
        const QString id = thread.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) { reportProtocolError(tr("会话响应缺少 thread.id。")); return; }
        _threadId = id;
        _resumingSavedThread = false;
        if (auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"))) {
            settings->setValue(QStringLiteral("codex/workspace/lastThreadId"), _threadId);
            settings->setValue(QStringLiteral("codex/workspace/dynamicToolSignature"), dynamicToolSignature(dynamicTools()));
            settings->setValue(QStringLiteral("codex/workspace/dynamicToolSchemaVersion"), QString::fromLatin1(kDynamicToolSchemaVersion));
            settings->sync();
        }
        _activeTurnId.clear();
        const QJsonObject localHistory = loadLocalThreadHistory(id);
        if (!localHistory.isEmpty()) {
            thread.insert(QStringLiteral("turns"), localHistory.value(QStringLiteral("turns")));
        }
        if (_workspace) {
            _workspace->showThreadHistory(thread);
            _workspace->setConnectionStatus(tr("已连接"), true);
            _workspace->setPromptEnabled(true);
            _workspace->setProperty(kind == PendingRequest::ThreadFork ? "codexSessionForkId" : "codexSessionResumeId", id);
            _workspace->setProperty("codexCurrentThreadId", id);
        }
        setProtocolState(ProtocolState::Ready);
        sendThreadSettingsUpdate();
        emit threadReady(_threadId);
        requestThreadList();
        return;
    }
    case PendingRequest::ThreadArchive:
        if (_workspace) {
            _workspace->appendLogLine(tr("会话已归档：%1").arg(requestedThreadId));
            _workspace->setProperty("codexSessionArchiveId", requestedThreadId);
        }
        requestThreadList();
        return;
    }
}

void CodexPlugin::handleNotification(const QString& method, const QJsonObject& params)
{
    if (method == QStringLiteral("thread/compacted") ||
        method == QStringLiteral("thread/goal/updated") ||
        method == QStringLiteral("thread/goal/cleared") ||
        method == QStringLiteral("item/autoApprovalReview/started") ||
        method == QStringLiteral("item/autoApprovalReview/completed") ||
        method == QStringLiteral("skills/changed") ||
        method == QStringLiteral("app/list/updated")) {
        if (_workspace) {
            _workspace->appendLogLine(tr("%1: %2")
                .arg(method, QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact))));
        }
        return;
    }
    if (method == QStringLiteral("mcpServer/startupStatus/updated")) {
        if (_workspace) {
            _workspace->appendLogLine(tr("MCP status: %1")
                .arg(QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact))));
        }
        return;
    }
    if (method == QStringLiteral("mcpServer/oauthLogin/completed")) {
        if (_workspace) _workspace->appendLogLine(tr("MCP OAuth completed: %1")
            .arg(QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact))));
        sendNativeRpc(QStringLiteral("mcpServerStatus/list"), {});
        return;
    }
    if (method == QStringLiteral("item/mcpToolCall/progress")) {
        if (_workspace) {
            _workspace->appendLogLine(tr("MCP tool progress: %1")
                .arg(QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact))));
        }
        return;
    }
    if (method == QStringLiteral("thread/tokenUsage/updated")) {
        if (_workspace && params.value(QStringLiteral("threadId")).toString() == _threadId)
            _workspace->setTokenUsage(params.value(QStringLiteral("tokenUsage")).toObject());
        return;
    }
    if (method == QStringLiteral("thread/settings/updated")) {
        if (_workspace && params.value(QStringLiteral("threadId")).toString() == _threadId) {
            const QJsonObject settings = params.value(QStringLiteral("threadSettings")).toObject();
            const QString confirmedModel = settings.value(QStringLiteral("model")).toString().trimmed();
            const QString confirmedEffort = settings.value(QStringLiteral("effort")).toString().trimmed();
            _workspace->setProperty("codexConfirmedModel", confirmedModel);
            _workspace->setProperty("codexConfirmedReasoningEffort", confirmedEffort);
            _workspace->appendLogLine(tr("线程设置已生效：%1 · %2")
                .arg(confirmedModel, confirmedEffort.isEmpty() ? tr("默认推理") : confirmedEffort));
        }
        return;
    }
    if (method == QStringLiteral("turn/plan/updated")) {
        if (_workspace && params.value(QStringLiteral("turnId")).toString() == _activeTurnId) {
            _workspace->updateTaskPlan(
                params.value(QStringLiteral("plan")).toArray(),
                params.value(QStringLiteral("explanation")).toString());
            _workspace->appendWorkbenchEvent(QStringLiteral("task"), tr("Task plan updated."));
        }
        return;
    }
    if (method == QStringLiteral("item/started") || method == QStringLiteral("item/completed")) {
        const QJsonObject item = params.value(QStringLiteral("item")).toObject();
        const QString itemId = item.value(QStringLiteral("id")).toString();
        const QString itemType = item.value(QStringLiteral("type")).toString();
        if (_workspace && itemType == QStringLiteral("imageGeneration")) {
            if (method == QStringLiteral("item/started")) {
                _workspace->setImageGenerationCapability(true, QString());
            }
            _workspace->updateImageGeneration(item, method == QStringLiteral("item/completed"));
            _workspace->appendWorkbenchEvent(QStringLiteral("media"), tr("Image generation %1.").arg(method == QStringLiteral("item/completed") ? tr("completed") : tr("started")));
            return;
        }
        if (_workspace && !itemId.isEmpty() &&
            itemType != QStringLiteral("agentMessage") &&
            itemType != QStringLiteral("userMessage")) {
            _workspace->markTaskActivity(
                itemId,
                describeThreadItem(item),
                method == QStringLiteral("item/completed"));
            const QString category = itemType == QStringLiteral("commandExecution") ? QStringLiteral("terminal")
                : (itemType == QStringLiteral("fileChange") ? QStringLiteral("changes")
                : (itemType == QStringLiteral("webSearch") ? QStringLiteral("browser") : QStringLiteral("task")));
            _workspace->appendWorkbenchEvent(category, describeThreadItem(item));
        }
        return;
    }
    if (method == QStringLiteral("item/agentMessage/delta")) {
        emit agentMessageDelta(
            params.value(QStringLiteral("threadId")).toString(),
            params.value(QStringLiteral("turnId")).toString(),
            params.value(QStringLiteral("itemId")).toString(),
            params.value(QStringLiteral("delta")).toString());
        return;
    }
    if (method != QStringLiteral("turn/completed")) {
        if (_workspace) {
            const QString safePayload = QString::fromUtf8(redactSensitiveText(
                QJsonDocument(params).toJson(QJsonDocument::Compact)));
            _workspace->setProperty("codexLastNativeNotificationMethod", method);
            _workspace->setProperty("codexLastNativeNotificationPayload", safePayload);
            _workspace->appendLogLine(tr("Native notification %1: %2").arg(method, safePayload.left(1200)));
        }
        return;
    }

    const QJsonObject turn = params.value(QStringLiteral("turn")).toObject();
    if (turn.isEmpty()) {
        reportProtocolError(tr("Codex turn/completed 通知缺少 turn。"));
        return;
    }
    if (turn.value(QStringLiteral("id")).toString() == _activeTurnId) {
        _activeTurnId.clear();
        setProtocolState(ProtocolState::Ready);
    }
    emit turnCompleted(turn);
    if (!_runningLocalTaskId.isEmpty()) {
        updateLocalTaskStatus(_runningLocalTaskId,
            turn.value(QStringLiteral("status")).toString() == QStringLiteral("completed") ? QStringLiteral("completed") : QStringLiteral("failed"),
            turn.value(QStringLiteral("status")).toString());
        _runningLocalTaskId.clear();
        QTimer::singleShot(0, this, &CodexPlugin::processLocalTaskQueue);
    }
}

void CodexPlugin::updateLocalTaskStatus(const QString& taskId, const QString& status, const QString& detail)
{
    const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("cgplay_local_tasks.jsonl"));
    QFile input(path);
    QJsonArray tasks;
    if (input.open(QIODevice::ReadOnly | QIODevice::Text)) while (!input.atEnd()) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(input.readLine(), &error);
        if (error.error == QJsonParseError::NoError && document.isObject()) tasks.push_back(document.object());
    }
    bool changed = false;
    for (int i = 0; i < tasks.size(); ++i) {
        QJsonObject task = tasks.at(i).toObject();
        if (task.value(QStringLiteral("id")).toString() != taskId) continue;
        task.insert(QStringLiteral("status"), status);
        task.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        if (!detail.isEmpty()) task.insert(QStringLiteral("detail"), detail.left(12000));
        if (status == QStringLiteral("completed") || status == QStringLiteral("failed") || status == QStringLiteral("cancelled")) {
            const QString resultDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("local_task_results"));
            QDir().mkpath(resultDirectory);
            const QString resultPath = QDir(resultDirectory).filePath(taskId + QStringLiteral(".json"));
            QJsonObject result{{QStringLiteral("protocol"), QStringLiteral("cgplay.local-task.v1")}, {QStringLiteral("taskId"), taskId}, {QStringLiteral("status"), status}, {QStringLiteral("detail"), detail.left(12000)}, {QStringLiteral("completedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
            QSaveFile resultFile(resultPath);
            if (resultFile.open(QIODevice::WriteOnly) && resultFile.write(QJsonDocument(result).toJson(QJsonDocument::Indented)) >= 0 && resultFile.commit()) task.insert(QStringLiteral("resultPath"), resultPath);
        }
        tasks.replace(i, task); changed = true; break;
    }
    if (!changed) return;
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) return;
    for (const QJsonValue& value : tasks) { output.write(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)); output.write("\n"); }
    output.commit();
}

void CodexPlugin::processLocalTaskQueue()
{
    if (!_runningLocalTaskId.isEmpty() || _protocolState != ProtocolState::Ready || !_activeTurnId.isEmpty() || _threadId.isEmpty()) return;
    const QString scheduledPath = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_local_scheduled_tasks.jsonl"));
    QFile scheduledInput(scheduledPath);
    if (scheduledInput.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QJsonArray remaining;
        while (!scheduledInput.atEnd()) {
            QJsonParseError parseError; const QJsonDocument document = QJsonDocument::fromJson(scheduledInput.readLine(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) continue;
            QJsonObject task = document.object();
            const QDateTime runAt = QDateTime::fromString(task.value(QStringLiteral("runAt")).toString(), Qt::ISODate);
            if (task.value(QStringLiteral("status")).toString() == QStringLiteral("queued") && runAt.isValid() && runAt <= QDateTime::currentDateTimeUtc()) {
                const QString queuePath = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("cgplay_local_tasks.jsonl"));
                QFile queue(queuePath);
                if (queue.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
                    QJsonObject queued{{QStringLiteral("id"), task.value(QStringLiteral("id")).toString()}, {QStringLiteral("objective"), task.value(QStringLiteral("objective")).toString()}, {QStringLiteral("status"), QStringLiteral("queued")}, {QStringLiteral("scheduledRunAt"), task.value(QStringLiteral("runAt")).toString()}};
                    queue.write(QJsonDocument(queued).toJson(QJsonDocument::Compact)); queue.write("\n");
                    task.insert(QStringLiteral("status"), QStringLiteral("dispatched"));
                }
            }
            remaining.push_back(task);
        }
        QSaveFile scheduledOutput(scheduledPath);
        if (scheduledOutput.open(QIODevice::WriteOnly)) { for (const QJsonValue& value : remaining) { scheduledOutput.write(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)); scheduledOutput.write("\n"); } scheduledOutput.commit(); }
    }
    const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("cgplay_local_tasks.jsonl"));
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    while (!input.atEnd()) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(input.readLine(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) continue;
        const QJsonObject task = document.object();
        if (task.value(QStringLiteral("status")).toString() != QStringLiteral("queued")) continue;
        QString startError;
        if (!submitPrompt(task.value(QStringLiteral("objective")).toString(), &startError)) return;
        _runningLocalTaskId = task.value(QStringLiteral("id")).toString();
        updateLocalTaskStatus(_runningLocalTaskId, QStringLiteral("running"));
        if (_workspace) _workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("CGPlay local task running: %1").arg(task.value(QStringLiteral("objective")).toString()));
        return;
    }
}

void CodexPlugin::handleServerRequest(const QJsonObject& message)
{
    const QString method = message.value(QStringLiteral("method")).toString();
    const QJsonValue id = message.value(QStringLiteral("id"));
    if (method == QStringLiteral("account/chatgptAuthTokens/refresh") || method == QStringLiteral("attestation/generate")) {
        if (id.isUndefined() || id.isNull()) return;
        const QString reason = method.startsWith(QStringLiteral("account/"))
            ? QStringLiteral("CGPlay is using a provider API key and has no ChatGPT account token source.")
            : QStringLiteral("No attestation signer is configured in the CGPlay Workbench runtime.");
        QString error;
        sendJsonRpc(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32001}, {QStringLiteral("message"), reason}}}}, &error);
        if (_workspace) _workspace->appendLogLine(tr("%1 unavailable: %2").arg(method, reason));
        return;
    }
    if (method == QStringLiteral("currentTime/read")) {
        if (id.isUndefined() || id.isNull()) return;
        QString error;
        sendJsonRpc(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("result"), QJsonObject{{QStringLiteral("currentTimeAt"), QDateTime::currentSecsSinceEpoch()}}}}, &error);
        return;
    }
    if (method == QStringLiteral("applyPatchApproval") || method == QStringLiteral("execCommandApproval")) {
        if (id.isUndefined() || id.isNull()) return;
        bool accepted = _approvalMode != QStringLiteral("request");
        if (_approvalMode == QStringLiteral("request")) {
            accepted = QMessageBox::question(_workspace.get(), tr("Codex approval"),
                method == QStringLiteral("applyPatchApproval") ? tr("Codex requests permission to apply a patch.") : tr("Codex requests permission to execute a command."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
        }
        QString error;
        sendJsonRpc(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("result"), QJsonObject{{QStringLiteral("decision"), accepted ? QStringLiteral("approved") : QStringLiteral("denied")}}}}, &error);
        return;
    }
    if (method == QStringLiteral("item/tool/call")) {
        if (id.isUndefined() || id.isNull()) return;
        const QJsonObject params = message.value(QStringLiteral("params")).toObject();
        const QJsonObject result = executeDynamicTool(params);
        if (_workspace) {
            _workspace->setProperty("codexLastHostTool", params.value(QStringLiteral("tool")).toString());
            _workspace->setProperty("codexLastHostToolResult", result);
            const QString artifactPath = artifactPathFromToolResult(
                params.value(QStringLiteral("tool")).toString(), result);
            if (!artifactPath.isEmpty()) _workspace->appendFileArtifact(artifactPath);
        }
        sendDynamicToolResponse(id, result);
        return;
    }
    if (method == QStringLiteral("item/tool/requestUserInput")) {
        if (id.isUndefined() || id.isNull()) return;
        const QJsonObject params = message.value(QStringLiteral("params")).toObject();
        QJsonObject answers;
        for (const QJsonValue& value : params.value(QStringLiteral("questions")).toArray()) {
            const QJsonObject question = value.toObject();
            const QString questionId = question.value(QStringLiteral("id")).toString();
            if (questionId.isEmpty()) continue;
            bool accepted = false;
            QString answer;
            const QJsonArray options = question.value(QStringLiteral("options")).toArray();
            if (!options.isEmpty()) {
                QStringList labels;
                for (const QJsonValue& option : options) labels.push_back(option.toObject().value(QStringLiteral("label")).toString());
                answer = QInputDialog::getItem(_workspace.get(), question.value(QStringLiteral("header")).toString(), question.value(QStringLiteral("question")).toString(), labels, 0, false, &accepted);
            } else {
                answer = QInputDialog::getText(_workspace.get(), question.value(QStringLiteral("header")).toString(), question.value(QStringLiteral("question")).toString(), question.value(QStringLiteral("isSecret")).toBool() ? QLineEdit::Password : QLineEdit::Normal, {}, &accepted);
            }
            if (!accepted) { answers = QJsonObject{}; break; }
            answers.insert(questionId, QJsonObject{{QStringLiteral("answers"), QJsonArray{answer}}});
        }
        QString error;
        sendJsonRpc(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("result"), QJsonObject{{QStringLiteral("answers"), answers}}}}, &error);
        if (_workspace) _workspace->appendLogLine(tr("Interactive tool input completed (%1 answers)." ).arg(answers.size()));
        return;
    }
    if (method == QStringLiteral("mcpServer/elicitation/request")) {
        if (id.isUndefined() || id.isNull()) return;
        const QJsonObject params = message.value(QStringLiteral("params")).toObject();
        const QString mode = params.value(QStringLiteral("mode")).toString();
        QJsonObject result{{QStringLiteral("action"), QStringLiteral("decline")}};
        if (mode == QStringLiteral("url")) {
            const QUrl url(params.value(QStringLiteral("url")).toString());
            if (url.isValid() && (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http")) &&
                QMessageBox::question(_workspace.get(), tr("MCP authorization"), params.value(QStringLiteral("message")).toString(), QMessageBox::Open | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Open) {
                QDesktopServices::openUrl(url);
                result.insert(QStringLiteral("action"), QStringLiteral("accept"));
            }
        } else if (mode == QStringLiteral("form")) {
            const QJsonObject schema = params.value(QStringLiteral("requestedSchema")).toObject();
            QJsonObject content;
            bool acceptedAll = true;
            const QJsonObject properties = schema.value(QStringLiteral("properties")).toObject();
            for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
                const QJsonObject field = it.value().toObject();
                bool accepted = false;
                const QString title = field.value(QStringLiteral("title")).toString(it.key());
                const QJsonArray choices = field.value(QStringLiteral("enum")).toArray();
                if (!choices.isEmpty()) {
                    QStringList labels; for (const QJsonValue& choice : choices) labels.push_back(choice.toString());
                    const QString value = QInputDialog::getItem(_workspace.get(), tr("MCP input"), title, labels, 0, false, &accepted);
                    if (accepted) content.insert(it.key(), value);
                } else if (field.value(QStringLiteral("type")).toString() == QStringLiteral("boolean")) {
                    const auto choice = QMessageBox::question(_workspace.get(), tr("MCP input"), title, QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Cancel);
                    accepted = choice != QMessageBox::Cancel; if (accepted) content.insert(it.key(), choice == QMessageBox::Yes);
                } else {
                    const QString value = QInputDialog::getText(_workspace.get(), tr("MCP input"), title, QLineEdit::Normal, field.value(QStringLiteral("default")).toString(), &accepted);
                    if (accepted) content.insert(it.key(), value);
                }
                if (!accepted) { acceptedAll = false; break; }
            }
            if (acceptedAll) result = QJsonObject{{QStringLiteral("action"), QStringLiteral("accept")}, {QStringLiteral("content"), content}};
        }
        QString error;
        sendJsonRpc(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("result"), result}}, &error);
        if (_workspace) _workspace->appendLogLine(tr("MCP elicitation response: %1").arg(result.value(QStringLiteral("action")).toString()));
        return;
    }
    if (id.isUndefined() || id.isNull()) {
        reportProtocolError(tr("Codex 服务端请求缺少 ID。"));
        return;
    }

    if (method == QStringLiteral("item/commandExecution/requestApproval") ||
        method == QStringLiteral("item/fileChange/requestApproval")) {
        bool accepted = _approvalMode != QStringLiteral("request");
        if (_approvalMode == QStringLiteral("request")) {
            const QString action = method == QStringLiteral("item/fileChange/requestApproval")
                ? tr("Codex 请求修改项目文件。")
                : tr("Codex 请求执行命令。") ;
            accepted = QMessageBox::question(
                _workspace.get(),
                tr("Codex 请求批准"),
                action,
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) == QMessageBox::Yes;
        }
        const QJsonObject response{
            {QStringLiteral("id"), id},
            {QStringLiteral("result"), QJsonObject{
                {QStringLiteral("decision"), accepted ? QStringLiteral("accept") : QStringLiteral("decline")}}}
        };
        QString error;
        if (!sendJsonRpc(response, &error)) reportProtocolError(error);
        return;
    }

    if (method == QStringLiteral("item/permissions/requestApproval")) {
        const QJsonObject params = message.value(QStringLiteral("params")).toObject();
        bool accepted = _approvalMode != QStringLiteral("request");
        if (_approvalMode == QStringLiteral("request")) {
            accepted = QMessageBox::question(
                _workspace.get(),
                tr("Codex 请求批准"),
                tr("Codex 请求扩展当前任务的文件或网络权限。"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) == QMessageBox::Yes;
        }
        const QJsonObject permissions = accepted
            ? params.value(QStringLiteral("permissions")).toObject()
            : QJsonObject{};
        const QJsonObject response{
            {QStringLiteral("id"), id},
            {QStringLiteral("result"), QJsonObject{
                {QStringLiteral("permissions"), permissions},
                {QStringLiteral("scope"), QStringLiteral("turn")}}}
        };
        QString error;
        if (!sendJsonRpc(response, &error)) reportProtocolError(error);
        return;
    }

    reportProtocolError(tr("Codex 请求了不受支持的客户端操作：%1").arg(method));
}

QJsonArray CodexPlugin::dynamicTools() const
{
    const QJsonObject noArguments{{QStringLiteral("type"), QStringLiteral("object")},
                                  {QStringLiteral("additionalProperties"), false}};
    const QJsonObject controls{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("action")}},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("action"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), QJsonArray{QStringLiteral("play"), QStringLiteral("pause"), QStringLiteral("stop"), QStringLiteral("next_frame"), QStringLiteral("previous_frame"), QStringLiteral("seek"), QStringLiteral("goto_start"), QStringLiteral("goto_end"), QStringLiteral("set_loop"), QStringLiteral("set_in"), QStringLiteral("set_out"), QStringLiteral("clear_in_out"), QStringLiteral("set_compare"), QStringLiteral("clear_compare"), QStringLiteral("set_volume"), QStringLiteral("set_mute"), QStringLiteral("set_audio_offset"), QStringLiteral("set_channel_mute")}}}},
            {QStringLiteral("frame"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 0}}},
            {QStringLiteral("loopMode"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 0}, {QStringLiteral("maximum"), 2}}},
            {QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}}},
            {QStringLiteral("volume"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}, {QStringLiteral("minimum"), 0.0}, {QStringLiteral("maximum"), 1.0}}},
            {QStringLiteral("muted"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
            {QStringLiteral("offsetSeconds"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
            {QStringLiteral("channel"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 0}}}
        }}
    };
    const QJsonObject annotationControls{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("action")}},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("action"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), QJsonArray{QStringLiteral("select"), QStringLiteral("delete"), QStringLiteral("update_comment"), QStringLiteral("create_note")}}}},
            {QStringLiteral("annotationId"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
            {QStringLiteral("text"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}, {QStringLiteral("maxLength"), 4000}}}
        }}
    };
    const QJsonObject browserNavigateSchema{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("url")}}, {QStringLiteral("additionalProperties"), false}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("url"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}}}}}};
    const QJsonObject browserSelectorSchema{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("selector")}}, {QStringLiteral("additionalProperties"), false}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}}}, {QStringLiteral("text"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}};
    const QJsonObject browserExtractSchema{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("additionalProperties"), false}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("selector"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}};
    const QJsonObject localTaskSchema{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("objective"), QStringLiteral("runAt")}}, {QStringLiteral("additionalProperties"), false}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("objective"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}, {QStringLiteral("runAt"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}};
    const QJsonObject fuzzySchema{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("query")}}, {QStringLiteral("additionalProperties"), false}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("query"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}, {QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}};
        const QJsonObject localCapabilitySchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("action")}}, {QStringLiteral("additionalProperties"), false}, {QStringLiteral("properties"), QJsonObject{
        {QStringLiteral("action"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), QJsonArray{QStringLiteral("set_fast_mode"), QStringLiteral("set_personality"), QStringLiteral("thread_rename"), QStringLiteral("thread_metadata"), QStringLiteral("plugin_share"), QStringLiteral("marketplace_cache"), QStringLiteral("hook_write"), QStringLiteral("sandbox_status"), QStringLiteral("guardian_restore"), QStringLiteral("file_watch"), QStringLiteral("file_metadata"), QStringLiteral("agent_import"), QStringLiteral("terminal_resize"), QStringLiteral("account_local"), QStringLiteral("mention_resolve"), QStringLiteral("git_commit"), QStringLiteral("git_push_record"), QStringLiteral("git_pr_record"), QStringLiteral("browser_cdp_record"), QStringLiteral("desktop_action"), QStringLiteral("get_local_state")}}}},
        {QStringLiteral("value"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("content"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("message"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("source"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("target"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}
    }}};
    QJsonArray tools{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("player_status")}, {QStringLiteral("description"), QStringLiteral("Read the real current CGPlay player state.")}, {QStringLiteral("inputSchema"), noArguments}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("player_control")}, {QStringLiteral("description"), QStringLiteral("Control CGPlay transport, range, comparison, speed, loop and audio using existing runtime services.")}, {QStringLiteral("inputSchema"), controls}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("annotation_status")}, {QStringLiteral("description"), QStringLiteral("Read CGPlay review annotations and the selected annotation.")}, {QStringLiteral("inputSchema"), noArguments}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("annotation_control")}, {QStringLiteral("description"), QStringLiteral("Select, edit, delete, or create a text review annotation in CGPlay.")}, {QStringLiteral("inputSchema"), annotationControls}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("browser_navigate")}, {QStringLiteral("description"), QStringLiteral("Navigate the embedded CGPlay browser to a URL.")}, {QStringLiteral("inputSchema"), browserNavigateSchema}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("browser_click")}, {QStringLiteral("description"), QStringLiteral("Click one element in the embedded browser using a CSS selector.")}, {QStringLiteral("inputSchema"), browserSelectorSchema}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("browser_type")}, {QStringLiteral("description"), QStringLiteral("Type text into one embedded browser input using a CSS selector.")}, {QStringLiteral("inputSchema"), browserSelectorSchema}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("browser_extract")}, {QStringLiteral("description"), QStringLiteral("Extract visible text from the embedded browser page or a CSS-selected element.")}, {QStringLiteral("inputSchema"), browserExtractSchema}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("schedule_local_task")}, {QStringLiteral("description"), QStringLiteral("Schedule a durable local CGPlay task for a future ISO timestamp.")}, {QStringLiteral("inputSchema"), localTaskSchema}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("workspace_fuzzy_search")}, {QStringLiteral("description"), QStringLiteral("Search workspace file names and text locally without cloud services.")}, {QStringLiteral("inputSchema"), fuzzySchema}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("terminal_snapshot")}, {QStringLiteral("description"), QStringLiteral("Read the persisted local Codex terminal history snapshot.")}, {QStringLiteral("inputSchema"), noArguments}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("local_capability")}, {QStringLiteral("description"), QStringLiteral("Use CGPlay local equivalents for Codex features: settings, sessions, plugin sharing, hooks, sandbox status, account state, mentions, Git records, and guarded desktop actions.")}, {QStringLiteral("inputSchema"), localCapabilitySchema}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("capture_current_frame")}, {QStringLiteral("description"), QStringLiteral("Capture the current CGPlay video frame and attach it as image context for analysis.")}, {QStringLiteral("inputSchema"), noArguments}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("capture_selection_context")}, {QStringLiteral("description"), QStringLiteral("Capture the current frame with real In/Out selection metadata for Codex media analysis.")}, {QStringLiteral("inputSchema"), noArguments}}
    };
    const QJsonObject workspacePathSchema{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("path")}}, {QStringLiteral("additionalProperties"), false}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}}}}}};
    const QJsonObject processSchema{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("program")}}, {QStringLiteral("additionalProperties"), false}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("program"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}}}, {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}, {QStringLiteral("cwd"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}, {QStringLiteral("timeoutMs"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 100}, {QStringLiteral("maximum"), 600000}}}}}};
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("workspace_read_file")}, {QStringLiteral("description"), QStringLiteral("Read a UTF-8 text file inside the configured project workspace.")}, {QStringLiteral("inputSchema"), workspacePathSchema}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("workspace_write_file")}, {QStringLiteral("description"), QStringLiteral("Create or replace a UTF-8 text file inside the configured project workspace.")}, {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("path"), QStringLiteral("content")}}, {QStringLiteral("additionalProperties"), false}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}, {QStringLiteral("content"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}}}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("workspace_list")}, {QStringLiteral("description"), QStringLiteral("List files and directories inside the configured project workspace.")}, {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}}}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("workspace_run_process")}, {QStringLiteral("description"), QStringLiteral("Run a bounded process in the configured project workspace and return stdout, stderr, and exit code.")}, {QStringLiteral("inputSchema"), processSchema}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("workspace_run_process_async")}, {QStringLiteral("description"), QStringLiteral("Run a long process without blocking the Codex/player UI; returns a taskId for polling.")}, {QStringLiteral("inputSchema"), processSchema}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("local_task_status")}, {QStringLiteral("description"), QStringLiteral("Read the status/result of an asynchronous local process task.")}, {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("taskId")}}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("taskId"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}}}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("workspace_register_artifact")}, {QStringLiteral("description"), QStringLiteral("Register an existing final generated file and deliver it to the user as an Open / Save As file item in CGPlay.")}, {QStringLiteral("inputSchema"), workspacePathSchema}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("workspace_search")}, {QStringLiteral("description"), QStringLiteral("Search text files inside the configured project workspace.")}, {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("query")}}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("query"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}, {QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}}}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("workspace_archive")}, {QStringLiteral("description"), QStringLiteral("Create a ZIP archive from a project-relative file or directory.")}, {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("path"), QStringLiteral("output")}}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}, {QStringLiteral("output"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}}}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("media_metadata")}, {QStringLiteral("description"), QStringLiteral("Read current media FPS and frame count using the existing media probe.")}, {QStringLiteral("inputSchema"), noArguments}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("export_media")}, {QStringLiteral("description"), QStringLiteral("Export the current media or In/Out range to a video file.")}, {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("output")}}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("output"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}, {QStringLiteral("startFrame"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}, {QStringLiteral("endFrame"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}}}}}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("task_plan")}, {QStringLiteral("description"), QStringLiteral("Persist a multi-step task plan so execution can continue until every step is complete.")}, {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("steps")}}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("steps"), QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}}}}}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("task_update")}, {QStringLiteral("description"), QStringLiteral("Update task plan status, step, retry count, and final result.")}, {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("status")}}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("status"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}, {QStringLiteral("step"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}}, {QStringLiteral("detail"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}}}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("artifact_copy")}, {QStringLiteral("description"), QStringLiteral("Copy a registered artifact to a user-selected project-relative path.")}, {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("source"), QStringLiteral("destination")}}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("source"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}, {QStringLiteral("destination"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}}}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("software_detect")}, {QStringLiteral("description"), QStringLiteral("Detect installed Houdini, Blender, Maya, and Nuke executables from PATH and standard Windows installation directories.")}, {QStringLiteral("inputSchema"), noArguments}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("software_run")}, {QStringLiteral("description"), QStringLiteral("Run an installed DCC program with bounded arguments in the project workspace.")}, {QStringLiteral("inputSchema"), processSchema}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("review_pack")}, {QStringLiteral("description"), QStringLiteral("Package the current media, annotations, and project metadata into a ZIP review pack.")}, {QStringLiteral("inputSchema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("output")}}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("output"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}}}}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("media_audio_review")}, {QStringLiteral("description"), QStringLiteral("Run ffmpeg loudness and stream analysis for the current media.")}, {QStringLiteral("inputSchema"), noArguments}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("media_hdr_info")}, {QStringLiteral("description"), QStringLiteral("Read HDR/color metadata from the current media.")}, {QStringLiteral("inputSchema"), noArguments}});
    const QJsonObject imageSearchSchema{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("query")}}, {QStringLiteral("additionalProperties"), false}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("query"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}, {QStringLiteral("maxLength"), 240}}}, {QStringLiteral("limit"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 5}}}}}};
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("search_images")}, {QStringLiteral("description"), QStringLiteral("Search Wikimedia Commons for visual reference images and return downloadable thumbnails with source links.")}, {QStringLiteral("inputSchema"), imageSearchSchema}});
    if (imageApiConfigured()) {
        const QJsonObject imageSchema{
            {QStringLiteral("type"), QStringLiteral("object")},
            {QStringLiteral("required"), QJsonArray{QStringLiteral("prompt")}},
            {QStringLiteral("additionalProperties"), false},
            {QStringLiteral("properties"), QJsonObject{
                {QStringLiteral("prompt"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}, {QStringLiteral("maxLength"), 4000}}},
                {QStringLiteral("size"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), QJsonArray{QStringLiteral("1024x1024"), QStringLiteral("1536x1024"), QStringLiteral("1024x1536")}}}}
            }}
        };
        tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("generate_image")}, {QStringLiteral("description"), QStringLiteral("Generate one image through the independently configured image API.")}, {QStringLiteral("inputSchema"), imageSchema}});
    }
    const QJsonObject mediaSchema{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("required"), QJsonArray{QStringLiteral("prompt")}}, {QStringLiteral("additionalProperties"), false}, {QStringLiteral("properties"), QJsonObject{{QStringLiteral("prompt"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}, {QStringLiteral("maxLength"), 4000}}}}}};
    // The Workbench media endpoints are intentionally always exposed. Execution
    // performs the actual credential/provider check and returns its real error.
    // Hiding the tools here leaves the model unable to request a configured
    // inherited endpoint at all.
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("generate_video")}, {QStringLiteral("description"), QStringLiteral("Generate a video through the configured or inherited Workbench video API.")}, {QStringLiteral("inputSchema"), mediaSchema}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("generate_audio")}, {QStringLiteral("description"), QStringLiteral("Generate audio through the configured or inherited Workbench audio API.")}, {QStringLiteral("inputSchema"), mediaSchema}});
    tools.push_back(QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("name"), QStringLiteral("edit_image")}, {QStringLiteral("description"), QStringLiteral("Edit the most recent generated image through the configured or inherited Workbench image-edit API.")}, {QStringLiteral("inputSchema"), mediaSchema}});
    QJsonObject nameSpace{
        {QStringLiteral("type"), QStringLiteral("namespace")},
        {QStringLiteral("name"), QStringLiteral("cgplay")},
        {QStringLiteral("description"), QStringLiteral("On-demand CGPlay player state and reversible transport controls. No polling.")},
        {QStringLiteral("tools"), tools}
    };
    return QJsonArray{nameSpace};
}

void CodexPlugin::beginInitialize()
{
    if (!_initialized) {
        reportProtocolError(tr("Codex 插件关闭后 app-server 仍然启动。"));
        return;
    }

    const QJsonObject params{
        {QStringLiteral("clientInfo"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("cgplay")},
            {QStringLiteral("title"), QStringLiteral("CGPlay")},
            {QStringLiteral("version"), QStringLiteral("0.0.0")}
        }},
        {QStringLiteral("capabilities"), QJsonObject{
            {QStringLiteral("experimentalApi"), true}
        }}
    };
    if (!sendRequest(PendingRequest::Initialize, QStringLiteral("initialize"), params)) return;
    setProtocolState(ProtocolState::Initializing);
}

void CodexPlugin::beginModelList(const QString& cursor)
{
    QJsonObject params{
        {QStringLiteral("includeHidden"), true},
        {QStringLiteral("limit"), 200}
    };
    if (!cursor.trimmed().isEmpty()) {
        params.insert(QStringLiteral("cursor"), cursor.trimmed());
    }

    QString error;
    if (sendRequest(PendingRequest::ModelList, QStringLiteral("model/list"), params, &error)) return;

    if (_workspace) {
        _workspace->appendLogLine(tr("模型目录请求失败，已保留现有模型：%1").arg(error));
    }
    refreshReasoningOptions();
    beginThreadStart();
}

void CodexPlugin::beginProviderCapabilities()
{
    QString error;
    if (!sendRequest(
            PendingRequest::ProviderCapabilities,
            QStringLiteral("modelProvider/capabilities/read"),
            {},
            &error)) {
        if (_workspace) {
            _workspace->setImageGenerationCapability(
                imageApiConfigured(),
                imageApiConfigured()
                    ? tr("独立图片 API 已配置；原生能力探测失败不影响生图。")
                    : error);
        }
        beginModelList();
    }
}

void CodexPlugin::beginThreadStart()
{
    const QString currentToolSignature = dynamicToolSignature(dynamicTools());
    if (!_resumingSavedThread) {
        if (auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"))) {
            const QString previousThreadId = settings->value(QStringLiteral("codex/workspace/lastThreadId")).toString().trimmed();
            const QString previousToolSignature = settings->value(QStringLiteral("codex/workspace/dynamicToolSignature")).toString().trimmed();
            const QString previousToolSchema = settings->value(QStringLiteral("codex/workspace/dynamicToolSchemaVersion")).toString().trimmed();
            // Older sessions predate the capability signature. Their tool list
            // is immutable too, so a missing signature must be treated exactly
            // like a changed signature and cannot be resumed.
            const bool toolsChanged = !previousThreadId.isEmpty() &&
                (previousToolSignature != currentToolSignature || previousToolSchema != QString::fromLatin1(kDynamicToolSchemaVersion));
            if (toolsChanged) {
                settings->remove(QStringLiteral("codex/workspace/lastThreadId"));
                settings->setValue(QStringLiteral("codex/workspace/dynamicToolSignature"), currentToolSignature);
                settings->setValue(QStringLiteral("codex/workspace/dynamicToolSchemaVersion"), QString::fromLatin1(kDynamicToolSchemaVersion));
                settings->sync();
                if (_workspace) _workspace->appendLogLine(tr("Media/tool capability changed; starting a fresh Codex session."));
            } else if (!previousThreadId.isEmpty()) {
                _resumingSavedThread = true;
                QString error;
                const qint64 id = _nextRequestId;
                if (sendRequest(PendingRequest::ThreadResume, QStringLiteral("thread/resume"),
                                {{QStringLiteral("threadId"), previousThreadId}}, &error)) {
                    _pendingThreadIds.insert(id, previousThreadId);
                    setProtocolState(ProtocolState::StartingThread);
                    return;
                }
                _resumingSavedThread = false;
            }
        }
    }
    const CodexAppServerSession::Config config = _session.config();
    QJsonObject params{
        {QStringLiteral("approvalPolicy"), approvalPolicyForMode(_approvalMode)},
        {QStringLiteral("sandbox"), sandboxForMode(_approvalMode)},
        {QStringLiteral("model"), _selectedModel},
        {QStringLiteral("personality"), QStringLiteral("pragmatic")}
    };
    const QJsonArray tools = dynamicTools();
    params.insert(QStringLiteral("dynamicTools"), tools);
    if (_workspace) {
        QStringList names;
        for (const QJsonValue& nameSpaceValue : tools) {
            for (const QJsonValue& toolValue : nameSpaceValue.toObject().value(QStringLiteral("tools")).toArray()) {
                const QString name = toolValue.toObject().value(QStringLiteral("name")).toString();
                if (!name.isEmpty()) names.push_back(name);
            }
        }
        _workspace->appendLogLine(tr("Registered CGPlay tools for this session: %1").arg(names.join(QStringLiteral(", "))));
    }
    if (!config.workingDirectory.trimmed().isEmpty()) {
        params.insert(QStringLiteral("cwd"), config.workingDirectory);
    }
    if (!sendRequest(PendingRequest::ThreadStart, QStringLiteral("thread/start"), params)) return;
    setProtocolState(ProtocolState::StartingThread);
}

void CodexPlugin::refreshApiModelsFromDetector()
{
    if (_apiModelRefreshInProgress) return;
    auto* detector = ServiceLocator::getService<IAIProviderDetector>();
    if (!detector) return;

    _apiModelRefreshInProgress = true;
    auto* watcher = new QFutureWatcher<AIDetectionResult>(this);
    connect(watcher, &QFutureWatcher<AIDetectionResult>::finished, this, [this, watcher] {
        const AIDetectionResult result = watcher->result();
        _apiModelRefreshInProgress = false;
        mergeAvailableModels(result.models, true);
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([detector] {
        return detector->discoverCurrentModels();
    }));
}

void CodexPlugin::mergeAvailableModels(
    const QStringList& models,
    bool persistDetectedModels)
{
    const int previousCount = _availableModels.size();
    for (const QString& model : models) {
        appendUniqueModel(&_availableModels, model);
    }
    appendUniqueModel(&_availableModels, _selectedModel);

    if (persistDetectedModels) {
        if (auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"))) {
            QStringList persisted = settings
                ->value(QStringLiteral("ai/connection/availableModels"))
                .toStringList();
            const int persistedCount = persisted.size();
            for (const QString& model : models) {
                appendUniqueModel(&persisted, model);
            }
            if (persisted.size() != persistedCount) {
                settings->setValue(QStringLiteral("ai/connection/availableModels"), persisted);
                settings->sync();
            }
        }
    }

    if (_workspace && _availableModels.size() != previousCount) {
        _workspace->setModelOptions(_availableModels, _selectedModel);
    }
}

void CodexPlugin::refreshReasoningOptions()
{
    const QString key = normalizedModelKey(_selectedModel);
    const QStringList efforts = _reasoningEffortsByModel.value(key);
    QString preferred = _selectedReasoningEffort.trimmed().toLower();
    if (preferred.isEmpty()) {
        if (auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"))) {
            preferred = settings
                ->value(QStringLiteral("codex/workspace/reasoningEffort"))
                .toString()
                .trimmed()
                .toLower();
        }
    }

    QString selected;
    for (const QString& effort : efforts) {
        if (effort.compare(preferred, Qt::CaseInsensitive) == 0) {
            selected = effort;
            break;
        }
    }
    if (selected.isEmpty()) {
        const QString modelDefault = _defaultReasoningEffortByModel.value(key);
        for (const QString& effort : efforts) {
            if (effort.compare(modelDefault, Qt::CaseInsensitive) == 0) {
                selected = effort;
                break;
            }
        }
    }
    if (selected.isEmpty() && !efforts.isEmpty()) selected = efforts.front();

    _selectedReasoningEffort = selected;
    if (_workspace) _workspace->setReasoningOptions(efforts, _selectedReasoningEffort);
}

void CodexPlugin::sendThreadSettingsUpdate()
{
    if (_threadId.isEmpty() || !_session.isRunning() || _protocolState != ProtocolState::Ready) return;

    QJsonObject params{
        {QStringLiteral("threadId"), _threadId},
        {QStringLiteral("model"), _selectedModel}
    };
    params.insert(
        QStringLiteral("effort"),
        _selectedReasoningEffort.isEmpty()
            ? QJsonValue(QJsonValue::Null)
            : QJsonValue(_selectedReasoningEffort));

    QString error;
    if (!sendRequest(
            PendingRequest::ThreadSettingsUpdate,
            QStringLiteral("thread/settings/update"),
            params,
            &error) &&
        _workspace) {
        _workspace->appendLogLine(tr("模型或推理强度更新失败：%1").arg(error));
    }
}

void CodexPlugin::setProtocolState(ProtocolState state)
{
    if (_protocolState == state) return;
    _protocolState = state;
    switch (_protocolState) {
    case ProtocolState::Disabled: emit protocolStateChanged(QStringLiteral("disabled")); return;
    case ProtocolState::Initializing: emit protocolStateChanged(QStringLiteral("initializing")); return;
    case ProtocolState::StartingThread: emit protocolStateChanged(QStringLiteral("startingThread")); return;
    case ProtocolState::Ready: emit protocolStateChanged(QStringLiteral("ready")); return;
    case ProtocolState::TurnInProgress: emit protocolStateChanged(QStringLiteral("turnInProgress")); return;
    case ProtocolState::Stopping: emit protocolStateChanged(QStringLiteral("stopping")); return;
    case ProtocolState::Failed: emit protocolStateChanged(QStringLiteral("failed")); return;
    }
}

void CodexPlugin::reportProtocolError(const QString& message)
{
    _lastError = message;
    setProtocolState(ProtocolState::Failed);
    if (_workspace) {
        _workspace->setConnectionStatus(tr("连接失败"), false);
        _workspace->setRetryAvailable(true);
    }
    emit protocolError(message);
}

void CodexPlugin::ensureWorkspaceStarted()
{
    if (!_workspace || !_initialized) return;
    if (_protocolState == ProtocolState::Ready ||
        _protocolState == ProtocolState::TurnInProgress ||
        _protocolState == ProtocolState::Initializing ||
        _protocolState == ProtocolState::StartingThread) {
        return;
    }
    if (_session.isRunning()) {
        _restartAfterStop = true;
        _workspace->setConnectionStatus(tr("正在重新连接"), false);
        stopAppServer();
        return;
    }
    _workspace->setRetryAvailable(false);
    _workspace->setConnectionStatus(tr("正在连接"), false);
    startFromWorkspace(_selectedModel, _approvalMode);
}

void CodexPlugin::retryWorkspaceStart()
{
    refreshApiModelsFromDetector();
    if (_workspace) {
        _workspace->setError({});
        _workspace->setRetryAvailable(false);
    }
    if (!_session.isRunning() &&
        (_protocolState == ProtocolState::Initializing ||
         _protocolState == ProtocolState::StartingThread)) {
        setProtocolState(ProtocolState::Failed);
    }
    ensureWorkspaceStarted();
}

void CodexPlugin::startNewThreadFromWorkspace()
{
    if (!_workspace) return;
    if (_protocolState == ProtocolState::TurnInProgress) {
        _workspace->setError(tr("当前任务仍在进行，完成后再新建会话。"));
        return;
    }
    _workspace->setError({});
    _workspace->beginNewConversation();
    _resumingSavedThread = true;
    if (auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"))) {
        settings->remove(QStringLiteral("codex/workspace/lastThreadId"));
        settings->sync();
    }
    _resumingSavedThread = false;
    if (_protocolState == ProtocolState::Ready && _session.isRunning()) {
        _threadId.clear();
        _activeTurnId.clear();
        _workspace->setConnectionStatus(tr("正在创建会话"), true);
        _workspace->setPromptEnabled(false);
        beginThreadStart();
        return;
    }
    ensureWorkspaceStarted();
}

void CodexPlugin::requestThreadList()
{
    if (!_session.isRunning() || (_protocolState != ProtocolState::Ready && _protocolState != ProtocolState::TurnInProgress)) return;
    QJsonObject params{{QStringLiteral("limit"), 30}, {QStringLiteral("sortKey"), QStringLiteral("updated_at")}};
    QString error;
    if (!sendRequest(PendingRequest::ThreadList, QStringLiteral("thread/list"), params, &error) && _workspace) _workspace->setError(error);
}

void CodexPlugin::requestThreadRead(const QString& threadId)
{
    QString error;
    const qint64 id = _nextRequestId;
    if (sendRequest(PendingRequest::ThreadRead, QStringLiteral("thread/read"), {{QStringLiteral("threadId"), threadId}, {QStringLiteral("includeTurns"), true}}, &error)) _pendingThreadIds.insert(id, threadId);
    else if (_workspace) _workspace->setError(error);
}

void CodexPlugin::resumeThread(const QString& threadId)
{
    if (_protocolState == ProtocolState::TurnInProgress) return;
    if (_workspace) { _workspace->setPromptEnabled(false); _workspace->setConnectionStatus(tr("正在恢复会话"), true); }
    QString error;
    const qint64 id = _nextRequestId;
    if (sendRequest(PendingRequest::ThreadResume, QStringLiteral("thread/resume"), {{QStringLiteral("threadId"), threadId}}, &error)) _pendingThreadIds.insert(id, threadId);
    else if (_workspace) _workspace->setError(error);
}

void CodexPlugin::forkThread(const QString& threadId)
{
    if (_protocolState == ProtocolState::TurnInProgress) return;
    if (_workspace) { _workspace->setPromptEnabled(false); _workspace->setConnectionStatus(tr("正在分叉会话"), true); }
    QString error;
    const qint64 id = _nextRequestId;
    if (sendRequest(PendingRequest::ThreadFork, QStringLiteral("thread/fork"), {{QStringLiteral("threadId"), threadId}}, &error)) _pendingThreadIds.insert(id, threadId);
    else if (_workspace) _workspace->setError(error);
}

void CodexPlugin::archiveThread(const QString& threadId)
{
    if (_protocolState == ProtocolState::TurnInProgress || threadId == _threadId) return;
    QString error;
    const qint64 id = _nextRequestId;
    if (sendRequest(PendingRequest::ThreadArchive, QStringLiteral("thread/archive"), {{QStringLiteral("threadId"), threadId}}, &error)) _pendingThreadIds.insert(id, threadId);
    else if (_workspace) _workspace->setError(error);
}

void CodexPlugin::startFromWorkspace(const QString& model, const QString& approvalMode)
{
    if (_workspace) {
        _workspace->setConnectionStatus(tr("正在连接"), false);
        _workspace->setRetryAvailable(false);
        _workspace->setPromptEnabled(false);
    }
    QString error;
    CodexAppServerSession::Config config;
    QString runtimeStatus;
    if (!resolveRuntimeConfig(&config, &runtimeStatus, &error)) {
        if (_workspace) {
            _workspace->setRuntimeSourceStatus(error, false);
            _workspace->setError(error);
        }
        reportProtocolError(error);
        return;
    }

    QString providerId;
    QString baseUrl;
    QString defaultModel;
    QStringList availableModels;
    QString sourceStatus;
    QProcessEnvironment environment;
    if (!resolveAiWorkspaceConfig(
            &providerId, &baseUrl, &defaultModel, &availableModels,
            &environment, &sourceStatus, &error)) {
        if (_workspace) {
            _workspace->setAiWorkspaceSourceStatus(error, false);
            _workspace->setError(error);
        }
        reportProtocolError(error);
        return;
    }

    const QString selectedModel = model.trimmed().isEmpty() ? defaultModel : model.trimmed();
    mergeAvailableModels(availableModels, false);
    appendUniqueModel(&_availableModels, selectedModel);
    config.environment = environment;

    // Never reinterpret a third-party Workbench credential as an official
    // OpenAI credential. Native imagegen may read OPENAI_API_KEY directly.
    const QString privateWorkspaceKey = config.environment
        .value(QStringLiteral("CGPLAY_AI_WORKSPACE_API_KEY"))
        .trimmed();
    const QString providerCategory = (providerId + QLatin1Char(' ') + baseUrl).toLower();
    const QString providerHost = QUrl(baseUrl).host().toLower();
    if (!privateWorkspaceKey.isEmpty()) {
        if (providerCategory.contains(QStringLiteral("azure")) ||
            providerCategory.contains(QStringLiteral("/openai/deployments/"))) {
            config.environment.insert(QStringLiteral("AZURE_OPENAI_API_KEY"), privateWorkspaceKey);
        } else if (providerHost == QStringLiteral("api.openai.com")) {
            config.environment.insert(QStringLiteral("OPENAI_API_KEY"), privateWorkspaceKey);
        }
    }

    if (!validateWorkspaceConfig(config, providerId, baseUrl, selectedModel, &error) ||
        !writeProviderConfig(
            config.codexHome,
            providerId,
            baseUrl,
            selectedModel,
            QStringLiteral("CGPLAY_AI_WORKSPACE_API_KEY"),
            &error)) {
        if (_workspace) _workspace->setError(error);
        reportProtocolError(error);
        return;
    }

    _selectedModel = selectedModel;
    _approvalMode = approvalMode == QStringLiteral("request") || approvalMode == QStringLiteral("auto")
        ? approvalMode
        : QStringLiteral("full");
    if (_workspace) {
        _workspace->setModelOptions(_availableModels, _selectedModel);
        _workspace->setRuntimeSourceStatus(runtimeStatus, true);
        _workspace->setAiWorkspaceSourceStatus(sourceStatus, true);
        _workspace->setProjectPath(config.workingDirectory);
    }
    if (startAppServer(config, &error)) return;

    if (_workspace) _workspace->setError(error);
    reportProtocolError(error);
}

} // namespace cgplay
