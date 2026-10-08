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

bool CodexPlugin::resolveRuntimeConfig(
    CodexAppServerSession::Config* config,
    QString* sourceStatus,
    QString* error) const
{
    if (!config) {
        if (error) *error = tr("无法创建 Codex 运行配置。");
        return false;
    }

    auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"));
    const QString executableOverride = settings
        ? settings->value(QStringLiteral("codex/runtime/executableOverride")).toString().trimmed()
        : QString();
    const QString executablePath = discoverCodexExecutable(executableOverride);
    if (executablePath.isEmpty()) {
        if (error) {
            *error = executableOverride.isEmpty()
                ? tr("内置 Codex CLI 运行时缺失或不可运行，请修复或重新安装 CGPlay 完整版。")
                : tr("高级覆盖指定的 Codex 可执行文件不可运行。");
        }
        return false;
    }

    const QString workingDirectory = discoverProjectRoot();
    if (workingDirectory.isEmpty() || !QDir(workingDirectory).exists()) {
        if (error) *error = tr("无法自动确定当前项目目录。");
        return false;
    }

    const QString codexHome = QDir(
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("codex-home"));
    if (codexHome.isEmpty() || !QDir().mkpath(codexHome)) {
        if (error) *error = tr("无法创建专用 CODEX_HOME。");
        return false;
    }

    config->executablePath = executablePath;
    config->codexHome = codexHome;
    config->workingDirectory = workingDirectory;
    config->gracefulStopTimeoutMs = 3000;
    if (sourceStatus) {
        *sourceStatus = tr("Codex：%1\nCODEX_HOME：%2\n工作目录：%3")
            .arg(executablePath, codexHome, workingDirectory);
    }
    return true;
}

bool CodexPlugin::resolveAiWorkspaceConfig(
    QString* providerId,
    QString* baseUrl,
    QString* model,
    QStringList* availableModels,
    QProcessEnvironment* environment,
    QString* sourceStatus,
    QString* error) const
{
    auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"));
    auto* credentials = ServiceLocator::getService<IAICredentialStore>();
    if (!settings || !credentials || !environment) {
        if (error) *error = tr("AI 工作台服务尚未就绪。");
        return false;
    }

    QString resolvedProviderId = firstNonEmptySetting(settings, {
        "ai/workspace/providerId", "ai/connection/providerId"});
    if (resolvedProviderId.isEmpty() || resolvedProviderId.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
        resolvedProviderId = QStringLiteral("openai");
    }
    const QString providerDisplayName = firstNonEmptySetting(settings, {
        "ai/connection/providerName", "ai/connection/providerType"});
    QString resolvedBaseUrl = firstNonEmptySetting(settings, {
        "ai/connection/baseUrl", "ai/providers/openai/baseUrl"});
    QString resolvedModel = firstNonEmptySetting(settings, {
        "ai/workspace/model", "ai/connection/model", "ai/connection/recommendedModel", "ai/providers/openai/model"});
    QStringList resolvedModels = settings->value(QStringLiteral("ai/connection/availableModels")).toStringList();
    appendUniqueModel(&resolvedModels, settings->value(QStringLiteral("ai/connection/recommendedModel")).toString());
    appendUniqueModel(&resolvedModels, resolvedModel);
    if (resolvedModel.isEmpty() && !resolvedModels.isEmpty()) resolvedModel = resolvedModels.front();
    if (resolvedBaseUrl.isEmpty() || resolvedModel.isEmpty()) {
        if (error) *error = tr("AI 工作台尚未配置 API 地址或模型。");
        return false;
    }

    QStringList compatibleProtocols = settings
        ->value(QStringLiteral("ai/connection/compatibleProtocols"))
        .toStringList();
    const QString detectedProtocol = settings
        ->value(QStringLiteral("ai/connection/detectedProtocol"))
        .toString()
        .trimmed();
    for (QString& protocol : compatibleProtocols) protocol = protocol.trimmed().toLower();
    const bool responsesVerified = compatibleProtocols.contains(QStringLiteral("openai_responses"));
    const bool nativeProtocol = detectedProtocol.startsWith(QStringLiteral("anthropic")) ||
        detectedProtocol.startsWith(QStringLiteral("gemini")) ||
        detectedProtocol.startsWith(QStringLiteral("ollama"));
    const bool legacyCapabilityUnknown = compatibleProtocols.isEmpty() && !nativeProtocol;
    if (!responsesVerified && !legacyCapabilityUnknown) {
        if (error) {
            *error = tr("AI 工作台已识别当前 API，但未验证 OpenAI Responses 能力；本机 Codex app-server 不再支持 Chat wire API。");
        }
        return false;
    }
    const QString verifiedResponsesEndpoint = settings
        ->value(QStringLiteral("ai/connection/responsesEndpoint"))
        .toString()
        .trimmed();
    if (responsesVerified && !verifiedResponsesEndpoint.isEmpty()) {
        const QString verifiedBaseUrl = responsesBaseUrl(verifiedResponsesEndpoint);
        if (!verifiedBaseUrl.isEmpty()) resolvedBaseUrl = verifiedBaseUrl;
    }

    QProcessEnvironment resolvedEnvironment = QProcessEnvironment::systemEnvironment();
    QByteArray secret = loadAiWorkspaceSecret(
        credentials,
        resolvedProviderId,
        providerDisplayName,
        detectedProtocol,
        resolvedBaseUrl);
    bool credentialReady = false;
    if (!secret.isEmpty()) {
        QString secretText = QString::fromUtf8(secret);
        resolvedEnvironment.insert(QStringLiteral("CGPLAY_AI_WORKSPACE_API_KEY"), secretText);
        secretText.fill(QChar());
        secret.fill('\0');
        credentialReady = true;
    } else {
        QStringList environmentKeys{QStringLiteral("AI_API_KEY")};
        const QString credentialCategory = QStringLiteral("%1 %2 %3 %4")
            .arg(resolvedProviderId, providerDisplayName, detectedProtocol, resolvedBaseUrl)
            .toLower();
        if (credentialCategory.contains(QStringLiteral("anthropic")) ||
            credentialCategory.contains(QStringLiteral("claude"))) {
            environmentKeys.push_back(QStringLiteral("ANTHROPIC_API_KEY"));
        } else if (credentialCategory.contains(QStringLiteral("gemini")) ||
                   credentialCategory.contains(QStringLiteral("googleapis.com")) ||
                   credentialCategory.contains(QStringLiteral("google.ai"))) {
            environmentKeys << QStringLiteral("GEMINI_API_KEY") << QStringLiteral("GOOGLE_API_KEY");
        } else if (credentialCategory.contains(QStringLiteral("azure")) ||
                   credentialCategory.contains(QStringLiteral("/openai/deployments/"))) {
            environmentKeys.push_back(QStringLiteral("AZURE_OPENAI_API_KEY"));
        } else if (credentialCategory.contains(QStringLiteral("qwen")) ||
                   credentialCategory.contains(QStringLiteral("dashscope")) ||
                   credentialCategory.contains(QStringLiteral("aliyuncs.com"))) {
            environmentKeys << QStringLiteral("DASHSCOPE_API_KEY") << QStringLiteral("QWEN_API_KEY");
        } else if (QUrl(resolvedBaseUrl).host().compare(QStringLiteral("api.openai.com"), Qt::CaseInsensitive) == 0) {
            environmentKeys.push_back(QStringLiteral("OPENAI_API_KEY"));
        }
        for (const QString& name : environmentKeys) {
            const QString existing = resolvedEnvironment.value(name).trimmed();
            if (!existing.isEmpty()) {
                resolvedEnvironment.insert(QStringLiteral("CGPLAY_AI_WORKSPACE_API_KEY"), existing);
                credentialReady = true;
                break;
            }
        }
    }
    for (const char* name : {
             "AI_API_KEY",
             "OPENAI_API_KEY",
             "DASHSCOPE_API_KEY",
             "AZURE_OPENAI_API_KEY",
             "ANTHROPIC_API_KEY",
             "GEMINI_API_KEY",
             "GOOGLE_API_KEY"}) {
        resolvedEnvironment.remove(QString::fromLatin1(name));
    }
    if (!credentialReady) {
        if (error) *error = tr("AI 工作台没有可用的安全凭据。");
        return false;
    }

    if (providerId) *providerId = resolvedProviderId;
    if (baseUrl) *baseUrl = resolvedBaseUrl;
    if (model) *model = resolvedModel;
    if (availableModels) *availableModels = resolvedModels;
    *environment = resolvedEnvironment;
    if (sourceStatus) {
        const QString capability = responsesVerified
            ? tr("Responses 已验证")
            : tr("Responses 启动时验证（旧配置）");
        const QString providerLabel = providerDisplayName.isEmpty()
            ? resolvedProviderId
            : providerDisplayName;
        *sourceStatus = QString::fromUtf8(u8"%1  |  API：%2\n模型：%3  |  %4  |  密钥：安全凭据（不显示）")
            .arg(providerLabel, QUrl(resolvedBaseUrl).host(), resolvedModel, capability);
    }
    return true;
}

bool CodexPlugin::validateWorkspaceConfig(
    const CodexAppServerSession::Config& config,
    const QString& providerId,
    const QString& baseUrl,
    const QString& model,
    QString* error) const
{
    if (config.executablePath.isEmpty() || config.codexHome.isEmpty() || providerId.isEmpty() ||
        baseUrl.isEmpty() || model.isEmpty()) {
        if (error) *error = tr("自动运行配置或 AI 工作台配置不完整。");
        return false;
    }
    const QFileInfo executable(config.executablePath);
    if (!executable.isAbsolute() || !executable.exists() || !executable.isFile()) {
        if (error) *error = tr("自动发现的 Codex 可执行文件不可用。");
        return false;
    }
    if (!QFileInfo(config.codexHome).isAbsolute()) {
        if (error) *error = tr("自动生成的 CODEX_HOME 不是绝对目录。");
        return false;
    }
    if (!config.workingDirectory.isEmpty()) {
        const QDir workingDirectory(config.workingDirectory);
        if (!workingDirectory.isAbsolute() || !workingDirectory.exists()) {
            if (error) *error = tr("自动确定的工作目录不可用。");
            return false;
        }
    }
    if (!hasSafeIdentifier(providerId)) {
        if (error) *error = tr("AI 工作台的服务商 ID 无效。");
        return false;
    }
    if (model.contains('\r') || model.contains('\n')) {
        if (error) *error = tr("AI 工作台的模型名称无效。");
        return false;
    }

    const QUrl providerUrl(baseUrl);
    if (!providerUrl.isValid() || providerUrl.host().isEmpty() ||
        (providerUrl.scheme() != QStringLiteral("https") && providerUrl.scheme() != QStringLiteral("http"))) {
        if (error) *error = tr("AI 工作台的 API 地址必须是 HTTP 或 HTTPS URL。");
        return false;
    }
    if (!providerUrl.userInfo().isEmpty() || hasSensitiveCredentialQuery(providerUrl)) {
        if (error) *error = tr("AI 工作台的 API 地址不能包含 API Key 或访问令牌。");
        return false;
    }

    return true;
}

bool CodexPlugin::writeProviderConfig(
    const QString& codexHome,
    const QString& providerId,
    const QString& baseUrl,
    const QString& model,
    const QString& apiKeyEnvironmentVariable,
    QString* error) const
{
    const QFileInfo codexHomeInfo(codexHome);
    if (!codexHomeInfo.isAbsolute()) {
        if (error) *error = tr("CODEX_HOME 必须是绝对目录。");
        return false;
    }
    if (!QDir().mkpath(codexHome)) {
        if (error) *error = tr("无法创建配置的 CODEX_HOME 目录。");
        return false;
    }

    QUrl providerUrl(baseUrl);
    if (!providerUrl.isValid() || providerUrl.host().isEmpty() ||
        !providerUrl.userInfo().isEmpty() || hasSensitiveCredentialQuery(providerUrl)) {
        if (error) *error = tr("无法为 Codex 写入安全的服务商 API 地址。");
        return false;
    }

    const bool azureProvider = isAzureProviderUrl(providerUrl);
    const QList<QPair<QString, QString>> queryItems =
        QUrlQuery(providerUrl).queryItems(QUrl::FullyDecoded);
    providerUrl.setQuery(QString());
    providerUrl.setFragment(QString());
    const QString providerBaseUrl = providerUrl.toString(QUrl::FullyEncoded);

    const QString codexProviderId = QStringLiteral("cgplay-%1").arg(providerId);
    const QString configPath = QDir(codexHome).filePath(QStringLiteral("config.toml"));
    QString configText = QStringLiteral(
        "model = %1\n"
        "model_provider = %2\n\n"
        "[model_providers.%3]\n"
        "name = %4\n"
        "base_url = %5\n"
        "wire_api = \"responses\"\n")
        .arg(
            tomlString(model),
            tomlString(codexProviderId),
            codexProviderId,
            tomlString(azureProvider ? QStringLiteral("Azure") : QStringLiteral("CGPlay %1").arg(providerId)),
            tomlString(providerBaseUrl));
    if (azureProvider) {
        configText += QStringLiteral("env_http_headers = { %1 = %2 }\n")
            .arg(tomlString(QStringLiteral("api-key")), tomlString(apiKeyEnvironmentVariable));
    } else {
        configText += QStringLiteral("env_key = %1\n").arg(tomlString(apiKeyEnvironmentVariable));
    }
    if (!queryItems.isEmpty()) {
        configText += QStringLiteral("query_params = %1\n").arg(tomlInlineStringTable(queryItems));
    }
    configText += QStringLiteral("requires_openai_auth = false\n");

    // The per-session CODEX_HOME must retain user MCP definitions. The provider
    // overlay is generated here, but MCP servers belong to the user's config.
    const QString userConfigPath = QDir::home().filePath(QStringLiteral(".codex/config.toml"));
    QFile userConfig(userConfigPath);
    if (userConfig.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString source = QString::fromUtf8(userConfig.readAll());
        const int mcpStart = source.indexOf(QStringLiteral("[mcp_servers]"));
        if (mcpStart >= 0) {
            const QString mcpSection = source.mid(mcpStart).trimmed();
            if (!mcpSection.isEmpty()) configText += QStringLiteral("\n") + mcpSection + QStringLiteral("\n");
        }
    }

    QSaveFile configFile(configPath);
    const QByteArray configBytes = configText.toUtf8();
    if (!configFile.open(QIODevice::WriteOnly) || configFile.write(configBytes) != configBytes.size() || !configFile.commit()) {
        if (error) *error = tr("无法写入专用 Codex 服务商配置。");
        return false;
    }
    return true;
}

void CodexPlugin::connectWorkspace(CodexAgentWorkspace* workspace)
{
    connect(_orchestrator.get(), &LocalCodexOrchestrator::event, workspace, [workspace](const QString& text) {
        workspace->appendWorkbenchEvent(QStringLiteral("agents"), QStringLiteral("[Local orchestrator] %1").arg(text));
    });
    connect(_orchestrator.get(), &LocalCodexOrchestrator::taskCompleted, workspace, [workspace](const QString& summary) { workspace->submitWorkbenchContext(summary); });
    connect(workspace, &CodexAgentWorkspace::orchestratorCreateRequested, this, [this, workspace](const QString& objective, int count) {
        CodexAppServerSession::Config config;
        QString runtimeStatus;
        QString error;
        if (!resolveRuntimeConfig(&config, &runtimeStatus, &error)) { workspace->appendWorkbenchEvent(QStringLiteral("agents"), error); return; }
        QString providerId, baseUrl, model, sourceStatus;
        QStringList models;
        QProcessEnvironment environment;
        if (!resolveAiWorkspaceConfig(&providerId, &baseUrl, &model, &models, &environment, &sourceStatus, &error)) { workspace->appendWorkbenchEvent(QStringLiteral("agents"), error); return; }
        config.environment = environment;
        const QString privateKey = environment.value(QStringLiteral("CGPLAY_AI_WORKSPACE_API_KEY")).trimmed();
        const QString category = (providerId + QLatin1Char(' ') + baseUrl).toLower();
        if (!privateKey.isEmpty() && category.contains(QStringLiteral("azure"))) config.environment.insert(QStringLiteral("AZURE_OPENAI_API_KEY"), privateKey);
        else if (!privateKey.isEmpty() && QUrl(baseUrl).host().compare(QStringLiteral("api.openai.com"), Qt::CaseInsensitive) == 0) config.environment.insert(QStringLiteral("OPENAI_API_KEY"), privateKey);
        const QString selected = _selectedModel.isEmpty() ? model : _selectedModel;
        if (!validateWorkspaceConfig(config, providerId, baseUrl, selected, &error) || !writeProviderConfig(config.codexHome, providerId, baseUrl, selected, QStringLiteral("CGPLAY_AI_WORKSPACE_API_KEY"), &error)) { workspace->appendWorkbenchEvent(QStringLiteral("agents"), error); return; }
        if (!_orchestrator->createTask(objective, count, config, selected, _selectedReasoningEffort, _approvalMode, &error)) workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Local orchestrator create failed: %1").arg(error));
    });
    connect(workspace, &CodexAgentWorkspace::orchestratorManageRequested, this, [this, workspace] {
        const QJsonArray tasks = _orchestrator->tasks();
        QStringList display;
        for (const QJsonValue& value : tasks) { const QJsonObject task = value.toObject(); display << QStringLiteral("%1 [%2] %3").arg(task.value(QStringLiteral("id")).toString(), task.value(QStringLiteral("status")).toString(), task.value(QStringLiteral("objective")).toString()); }
        if (display.isEmpty()) { workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("No cgplay.orchestrator.v1 tasks.")); return; }
        bool ok = false; const QString chosen = QInputDialog::getItem(workspace, tr("Local orchestrator"), tr("Task"), display, 0, false, &ok); if (!ok) return;
        const QString taskId = chosen.section(QLatin1Char(' '), 0, 0);
        const QString action = QInputDialog::getItem(workspace, tr("Local orchestrator"), tr("Action"), {tr("Cancel"), tr("Merge results"), tr("Retry as new local task")}, 0, false, &ok); if (!ok) return;
        QString error;
        bool succeeded = false;
        if (action == tr("Cancel")) succeeded = _orchestrator->cancelTask(taskId, &error);
        else if (action == tr("Merge results")) succeeded = _orchestrator->mergeTask(taskId, &error);
        else {
            QJsonObject task;
            for (const QJsonValue& value : tasks) if (value.toObject().value(QStringLiteral("id")).toString() == taskId) { task = value.toObject(); break; }
            CodexAppServerSession::Config config; QString runtimeStatus;
            QString providerId, baseUrl, model, sourceStatus; QStringList models; QProcessEnvironment environment;
            succeeded = !task.isEmpty() && resolveRuntimeConfig(&config, &runtimeStatus, &error) && resolveAiWorkspaceConfig(&providerId, &baseUrl, &model, &models, &environment, &sourceStatus, &error);
            if (succeeded) { config.environment = environment; const QString selected = _selectedModel.isEmpty() ? model : _selectedModel; succeeded = _orchestrator->createTask(task.value(QStringLiteral("objective")).toString(), task.value(QStringLiteral("agentCount")).toInt(1), config, selected, _selectedReasoningEffort, _approvalMode, &error); }
        }
        workspace->appendWorkbenchEvent(QStringLiteral("agents"), succeeded ? tr("Local orchestrator action completed: %1").arg(taskId) : tr("Local orchestrator action failed: %1").arg(error));
    });
    connect(workspace, &CodexAgentWorkspace::activationRequested, this, &CodexPlugin::ensureWorkspaceStarted);
    connect(workspace, &CodexAgentWorkspace::retryRequested, this, &CodexPlugin::retryWorkspaceStart);
    connect(workspace, &CodexAgentWorkspace::newConversationRequested, this, &CodexPlugin::startNewThreadFromWorkspace);
    connect(workspace, &CodexAgentWorkspace::sessionListRequested, this, &CodexPlugin::requestThreadList);
    // A history item is a resumable conversation, not a read-only transcript.
    // Opening it makes subsequent prompts continue in that same thread.
    connect(workspace, &CodexAgentWorkspace::sessionReadRequested, this, &CodexPlugin::resumeThread);
    connect(workspace, &CodexAgentWorkspace::sessionResumeRequested, this, &CodexPlugin::resumeThread);
    connect(workspace, &CodexAgentWorkspace::sessionForkRequested, this, &CodexPlugin::forkThread);
    connect(workspace, &CodexAgentWorkspace::sessionArchiveRequested, this, &CodexPlugin::archiveThread);
    connect(workspace, &CodexAgentWorkspace::importGeneratedImageRequested, this, [workspace](const QString& path) {
        const QFileInfo info(path);
        auto* playback = reinterpret_cast<IPlaybackService*>(
            QCoreApplication::instance()->property("cgplay.codex.playbackService").toULongLong());
        if (!playback || !info.isAbsolute() || !info.isFile()) {
            workspace->setError(QObject::tr("Generated image is no longer available: %1").arg(path));
            return;
        }
        playback->openFile(info.absoluteFilePath());
        workspace->setProperty("codexImportedGeneratedImage", info.absoluteFilePath());
        workspace->appendLogLine(QObject::tr("Imported generated image: %1").arg(info.absoluteFilePath()));
    });
    connect(workspace, &CodexAgentWorkspace::openProjectRequested, this, [this] {
        CodexAppServerSession::Config config = _session.config();
        if (config.workingDirectory.trimmed().isEmpty()) {
            QString status;
            QString error;
            resolveRuntimeConfig(&config, &status, &error);
        }
        if (!config.workingDirectory.trimmed().isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(config.workingDirectory));
        }
    });
    connect(workspace, &CodexAgentWorkspace::stopRequested, this, [this, workspace] {
        if (!_activeTurnId.isEmpty()) {
            QString error;
            if (!interruptTurn(&error)) workspace->appendLogLine(tr("Interrupt failed: %1").arg(error));
            return;
        }
        stopAppServer();
    });
    connect(workspace, &CodexAgentWorkspace::modelSelectionChanged, this, [this](const QString& model) {
        if (model.trimmed().isEmpty()) return;
        const bool changed = _selectedModel.compare(model.trimmed(), Qt::CaseInsensitive) != 0;
        _selectedModel = model.trimmed();
        refreshReasoningOptions();
        if (auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"))) {
            settings->setValue(QStringLiteral("codex/workspace/model"), _selectedModel);
            settings->sync();
        }
        if (changed) sendThreadSettingsUpdate();
    });
    connect(workspace, &CodexAgentWorkspace::reasoningEffortChanged, this, [this](const QString& effort) {
        const QString normalized = effort.trimmed().toLower();
        const QStringList supported = _reasoningEffortsByModel.value(normalizedModelKey(_selectedModel));
        if (!supported.contains(normalized, Qt::CaseInsensitive)) return;
        const bool changed = _selectedReasoningEffort.compare(normalized, Qt::CaseInsensitive) != 0;
        _selectedReasoningEffort = normalized;
        if (auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"))) {
            settings->setValue(
                QStringLiteral("codex/workspace/reasoningEffort"),
                _selectedReasoningEffort);
            settings->sync();
        }
        if (changed) sendThreadSettingsUpdate();
    });
    connect(workspace, &CodexAgentWorkspace::approvalModeChanged, this, [this](const QString& mode) {
        if (mode != QStringLiteral("request") && mode != QStringLiteral("auto") && mode != QStringLiteral("full")) {
            return;
        }
        const bool changed = _approvalMode != mode;
        _approvalMode = mode;
        if (auto* settings = ServiceLocator::getService<ISettingsService>(QStringLiteral("user"))) {
            settings->setValue(QStringLiteral("codex/workspace/approvalMode"), _approvalMode);
            settings->sync();
        }
        if (changed && _protocolState == ProtocolState::Ready && _session.isRunning()) {
            startNewThreadFromWorkspace();
        }
    });
    connect(workspace, &CodexAgentWorkspace::promptSubmitted, this, [this, workspace](const QString& prompt) {
        QString error;
        if (!_activeTurnId.isEmpty()) {
            if (!steerTurn(prompt, &error)) {
                workspace->setError(error);
                return;
            }
            workspace->markPromptAccepted();
            workspace->appendWorkbenchEvent(QStringLiteral("task"), tr("Current task guidance sent."));
            return;
        }
        const bool submitted = submitPrompt(prompt, &error);
        if (!submitted) {
            workspace->setError(error);
            workspace->completeTurn(QStringLiteral("failed"));
            return;
        }
        workspace->markPromptAccepted();
    });
    connect(workspace, &CodexAgentWorkspace::mcpReloadRequested, this, [this, workspace] {
        QString error;
        if (reloadMcpServers(&error)) workspace->appendLogLine(tr("MCP reload requested."));
        else workspace->appendLogLine(tr("MCP reload failed: %1").arg(error));
    });
    connect(workspace, &CodexAgentWorkspace::mcpLoginRequested, this, [this, workspace](const QString& server) {
        QString error;
        workspace->appendWorkbenchEvent(QStringLiteral("extensions"), loginMcpServer(server, &error)
            ? tr("MCP OAuth started: %1").arg(server)
            : tr("MCP OAuth failed: %1").arg(error));
    });
    connect(workspace, &CodexAgentWorkspace::mcpResourceReadRequested, this, [this, workspace](const QString& server, const QString& uri) {
        QString error;
        workspace->appendWorkbenchEvent(QStringLiteral("extensions"), readMcpResource(server, uri, &error)
            ? tr("MCP resource requested: %1").arg(uri)
            : tr("MCP resource failed: %1").arg(error));
    });
    connect(workspace, &CodexAgentWorkspace::skillsRefreshRequested, this, [this, workspace] {
        QString error;
        workspace->appendWorkbenchEvent(QStringLiteral("extensions"), listSkills(true, &error)
            ? tr("Skills refresh requested.") : tr("Skills refresh failed: %1").arg(error));
    });
    connect(workspace, &CodexAgentWorkspace::localSkillManageRequested, this, [this, workspace] {
        CodexAppServerSession::Config config; QString status, error;
        if (!resolveRuntimeConfig(&config, &status, &error)) { workspace->appendWorkbenchEvent(QStringLiteral("extensions"), error); return; }
        const QString skillsRoot = QDir(config.codexHome).filePath(QStringLiteral("skills")); QDir().mkpath(skillsRoot);
        bool ok = false; const QString action = QInputDialog::getItem(workspace, tr("Local Skills"), tr("Action"), {tr("Install"), tr("Enable"), tr("Disable"), tr("Uninstall")}, 0, false, &ok); if (!ok) return;
        if (action == tr("Install")) {
            const QString source = QFileDialog::getExistingDirectory(workspace, tr("Select Skill directory")); if (source.isEmpty()) return;
            if (!QFileInfo(QDir(source).filePath(QStringLiteral("SKILL.md"))).isFile()) { workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("Skill install rejected: SKILL.md is missing.")); return; }
            const QString name = QFileInfo(source).fileName(); const QString target = QDir(skillsRoot).filePath(name);
            if (!copyDirectoryTree(source, target, &error)) workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("Skill install failed: %1").arg(error));
            else { workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("Skill installed: %1").arg(name)); listSkills(true); }
            return;
        }
        QStringList names = QDir(skillsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        if (names.isEmpty()) { workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("No local Skills found.")); return; }
        const QString name = QInputDialog::getItem(workspace, tr("Local Skills"), tr("Skill"), names, 0, false, &ok); if (!ok) return;
        const QString source = QDir(skillsRoot).filePath(name);
        if (action == tr("Enable") || action == tr("Disable")) {
            const bool enabled = action == tr("Enable"); qint64 requestId = 0;
            if (!sendNativeRpc(QStringLiteral("skills/config/write"), QJsonObject{{QStringLiteral("enabled"), enabled}, {QStringLiteral("name"), name}, {QStringLiteral("path"), QJsonValue::Null}}, &requestId, &error)) workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("Skill state change failed: %1").arg(error));
            else { workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("Skill state RPC sent: %1 (id=%2)").arg(name).arg(requestId)); listSkills(true); }
            return;
        }
        if (QMessageBox::question(workspace, tr("Uninstall Skill"), name, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        const QString trash = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("skill_trash/%1_%2").arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_hhmmss")), name)); QDir().mkpath(QFileInfo(trash).absolutePath());
        if (!QDir().rename(source, trash)) workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("Skill uninstall failed."));
        else { workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("Skill moved to local trash: %1").arg(name)); listSkills(true); }
    });
    connect(workspace, &CodexAgentWorkspace::appManageRequested, this, [this, workspace] {
        const QJsonArray apps = workspace->property("codexApps").toJsonArray();
        QStringList labels; for (const QJsonValue& value : apps) { const QJsonObject app = value.toObject(); labels << QStringLiteral("%1 [%2]").arg(app.value(QStringLiteral("name")).toString(app.value(QStringLiteral("id")).toString()), app.value(QStringLiteral("status")).toString()); }
        if (labels.isEmpty()) { workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("No Apps returned by app/list.")); return; }
        bool ok = false; const QString label = QInputDialog::getItem(workspace, tr("Codex Apps"), tr("App"), labels, 0, false, &ok); if (!ok) return;
        const QJsonObject app = apps.at(labels.indexOf(label)).toObject(); const QString action = QInputDialog::getItem(workspace, tr("Codex Apps"), tr("Action"), {tr("Authorize"), tr("Launch")}, 0, false, &ok); if (!ok) return;
        if (action == tr("Authorize")) { const QString server = app.value(QStringLiteral("mcpServerName")).toString(app.value(QStringLiteral("server"  )).toString()); QString error; if (server.isEmpty() || !loginMcpServer(server, &error)) workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("App authorization unavailable: %1").arg(error)); else workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("App OAuth started through MCP server: %1").arg(server)); return; }
        const QUrl url(app.value(QStringLiteral("url")).toString(app.value(QStringLiteral("launchUrl")).toString()));
        if (!url.isValid() || (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http"))) workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("App launch rejected: catalog has no safe HTTP(S) URL."));
        else { QDesktopServices::openUrl(url); workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("App launched: %1").arg(url.toDisplayString())); }
    });
    connect(workspace, &CodexAgentWorkspace::mcpToolDialogRequested, this, [this, workspace] {
        bool ok = false;
        const QString server = QInputDialog::getText(workspace, tr("MCP tool"), tr("Server"), QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || server.isEmpty()) return;
        const QString tool = QInputDialog::getText(workspace, tr("MCP tool"), tr("Tool"), QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || tool.isEmpty()) return;
        const QString json = QInputDialog::getMultiLineText(workspace, tr("MCP tool"), tr("Arguments JSON"), QStringLiteral("{}"), &ok);
        if (!ok) return;
        QJsonParseError parseError;
        const QJsonDocument arguments = QJsonDocument::fromJson(json.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !arguments.isObject()) { workspace->appendWorkbenchEvent(QStringLiteral("extensions"), tr("Invalid MCP arguments JSON.")); return; }
        QString error;
        workspace->appendWorkbenchEvent(QStringLiteral("extensions"), callMcpTool(server, tool, arguments.object(), &error)
            ? tr("MCP tool requested: %1/%2").arg(server, tool)
            : tr("MCP tool failed: %1").arg(error));
    });
    connect(workspace, &CodexAgentWorkspace::browserContextSubmitted, this, [workspace](const QString& url, const QString& title, const QString& selection) {
        const QString context = QObject::tr("Analyze this browser page as current context.\nURL: %1\nTitle: %2%3")
            .arg(url, title, selection.trimmed().isEmpty() ? QString() : QObject::tr("\nSelected text:\n%1").arg(selection.left(12000)));
        workspace->submitWorkbenchContext(context);
        workspace->appendWorkbenchEvent(QStringLiteral("browser"), QObject::tr("Current page sent to Codex: %1").arg(url));
    });
    connect(workspace, &CodexAgentWorkspace::browserSnapshotSubmitted, this, [this, workspace](const QStringList& paths, const QString& prompt) {
        QString error;
        const bool submitted = paths.isEmpty()
            ? submitPrompt(prompt, &error)
            : submitImagesPrompt(paths, prompt, &error, QStringLiteral("browser-snapshot"));
        if (!submitted) { workspace->appendWorkbenchEvent(QStringLiteral("browser"), tr("Browser snapshot send failed: %1").arg(error)); return; }
        workspace->markPromptAccepted();
    });
    connect(workspace, &CodexAgentWorkspace::sessionSearchRequested, this, [this, workspace](const QString& query) {
        requestThreadList();
        const QString needle = query.trimmed();
        if (needle.isEmpty()) {
            workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Session search reset; current thread list refreshed."));
            return;
        }
        const QJsonArray threads = workspace->property("codexSessionList").toJsonArray();
        QStringList matches;
        for (const QJsonValue& value : threads) {
            const QJsonObject thread = value.toObject();
            const QString haystack = QString::fromUtf8(QJsonDocument(thread).toJson(QJsonDocument::Compact));
            if (haystack.contains(needle, Qt::CaseInsensitive)) {
                matches << QStringLiteral("%1  %2").arg(thread.value(QStringLiteral("id")).toString(), thread.value(QStringLiteral("name")).toString());
            }
        }
        workspace->appendWorkbenchEvent(QStringLiteral("agents"), matches.isEmpty()
            ? tr("No loaded session metadata matches: %1").arg(needle)
            : tr("Session matches for %1:\n%2").arg(needle, matches.join(QLatin1Char('\n'))));
        QFutureWatcher<QStringList>* watcher = new QFutureWatcher<QStringList>(workspace);
        connect(watcher, &QFutureWatcher<QStringList>::finished, workspace, [workspace, watcher, needle] {
            const QStringList results = watcher->result();
            workspace->appendWorkbenchEvent(QStringLiteral("agents"), results.isEmpty()
                ? QObject::tr("No local transcript matches: %1").arg(needle)
                : QObject::tr("Local transcript matches for %1:\n%2").arg(needle, results.join(QLatin1Char('\n'))));
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run([needle] { return searchLocalThreadHistory(needle); }));
    });
    connect(workspace, &CodexAgentWorkspace::sessionBulkArchiveRequested, this, [this, workspace] {
        const QJsonArray threads = workspace->property("codexSessionList").toJsonArray();
        QStringList candidates;
        for (const QJsonValue& value : threads) {
            const QJsonObject thread = value.toObject();
            const QString id = thread.value(QStringLiteral("id")).toString();
            if (!id.isEmpty() && id != _threadId) candidates << QStringLiteral("%1 %2").arg(id, thread.value(QStringLiteral("name")).toString());
        }
        if (candidates.isEmpty()) { workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("No inactive sessions available for archive.")); return; }
        bool ok = false;
        const QString selected = QInputDialog::getMultiLineText(workspace, tr("Archive sessions"), tr("One session ID per line. Current session is excluded."), {}, &ok);
        if (!ok) return;
        const QStringList requested = selected.split(QRegularExpression(QStringLiteral("[\\r\\n,; ]+")), Qt::SkipEmptyParts);
        int count = 0;
        for (const QString& id : requested) {
            if (id == _threadId || !candidates.join(QLatin1Char('\n')).contains(id)) continue;
            archiveThread(id); ++count;
        }
        workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Archive requested for %1 sessions.").arg(count));
    });
    connect(workspace, &CodexAgentWorkspace::sessionImportRequested, this, [this, workspace] {
        const QString source = QFileDialog::getOpenFileName(workspace, tr("Import Codex session"), {}, tr("Codex JSONL (*.jsonl)")); if (source.isEmpty()) return;
        QFile input(source); if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) { workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Session import failed: cannot read file.")); return; }
        const QByteArray first = input.readLine(); QJsonParseError parseError; const QJsonDocument firstDocument = QJsonDocument::fromJson(first, &parseError);
        if (parseError.error != QJsonParseError::NoError || !firstDocument.isObject()) { workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Session import rejected: invalid JSONL.")); return; }
        CodexAppServerSession::Config config; QString status, error; if (!resolveRuntimeConfig(&config, &status, &error)) { workspace->appendWorkbenchEvent(QStringLiteral("agents"), error); return; }
        const QString targetDir = QDir(config.codexHome).filePath(QStringLiteral("sessions/imported/%1").arg(QDate::currentDate().toString(QStringLiteral("yyyy/MM/dd")))); QDir().mkpath(targetDir);
        const QString target = QDir(targetDir).filePath(QFileInfo(source).fileName());
        if (QFileInfo(target).exists() || !QFile::copy(source, target)) workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Session import failed or already exists."));
        else { workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Session imported: %1").arg(target)); requestThreadList(); }
    });
    connect(workspace, &CodexAgentWorkspace::sessionDeleteRequested, this, [this, workspace] {
        bool ok = false; const QString id = QInputDialog::getText(workspace, tr("Delete local session"), tr("Thread ID"), QLineEdit::Normal, {}, &ok).trimmed(); if (!ok || id.isEmpty()) return;
        if (id == _threadId) { workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("The current session cannot be deleted.")); return; }
        if (QMessageBox::question(workspace, tr("Delete local session"), tr("Permanently delete session %1 through Codex app-server?").arg(id), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        QString error; if (!sendNativeRpc(QStringLiteral("thread/delete"), QJsonObject{{QStringLiteral("threadId"), id}}, nullptr, &error)) workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Session delete failed: %1").arg(error));
        else { workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Session delete requested: %1").arg(id)); requestThreadList(); }
    });
    connect(workspace, &CodexAgentWorkspace::sessionBulkRestoreRequested, this, [this, workspace] {
        bool ok = false; const QString ids = QInputDialog::getMultiLineText(workspace, tr("Restore sessions"), tr("One archived thread ID per line"), {}, &ok); if (!ok) return;
        int count = 0; for (const QString& id : ids.split(QRegularExpression(QStringLiteral("[\\r\\n,; ]+")), Qt::SkipEmptyParts)) { QString error; if (sendNativeRpc(QStringLiteral("thread/unarchive"), QJsonObject{{QStringLiteral("threadId"), id}}, nullptr, &error)) ++count; else workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Restore failed %1: %2").arg(id, error)); }
        workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Restore requested for %1 sessions.").arg(count)); requestThreadList();
    });
    connect(workspace, &CodexAgentWorkspace::sessionExportRequested, this, [workspace] {
        const QString target = QFileDialog::getSaveFileName(workspace, tr("Export Codex sessions"),
            QDir::home().filePath(QStringLiteral("codex_sessions.json")), tr("JSON (*.json)"));
        if (target.isEmpty()) return;
        QJsonObject output{{QStringLiteral("schemaVersion"), 1},
                           {QStringLiteral("exportedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                           {QStringLiteral("sessions"), workspace->property("codexSessionList").toJsonArray()}};
        QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(output).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
            workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Session export failed."));
            return;
        }
        workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Session metadata exported: %1").arg(target));
    });
    connect(workspace, &CodexAgentWorkspace::diffFileActionRequested, this, [this, workspace](const QString& requestedPath, const QString& action) {
        const QString root = _session.config().workingDirectory.isEmpty() ? QDir::currentPath() : _session.config().workingDirectory;
        const QFileInfo candidate(QDir(root).filePath(requestedPath));
        const QString canonicalRoot = QDir(root).canonicalPath();
        const QString canonicalFile = candidate.canonicalFilePath();
        if (canonicalRoot.isEmpty() || canonicalFile.isEmpty() || !canonicalFile.startsWith(canonicalRoot + QLatin1Char('/'), Qt::CaseInsensitive)) {
            workspace->appendWorkbenchEvent(QStringLiteral("changes"), tr("Rejected path outside the project: %1").arg(requestedPath));
            return;
        }
        QStringList args;
        if (action == tr("暂存")) args = {QStringLiteral("add"), QStringLiteral("--"), requestedPath};
        else if (action == tr("取消暂存")) args = {QStringLiteral("restore"), QStringLiteral("--staged"), QStringLiteral("--"), requestedPath};
        else {
            if (QMessageBox::question(workspace, tr("Discard file changes"), tr("Discard all uncommitted changes in %1?").arg(requestedPath), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
            args = {QStringLiteral("restore"), QStringLiteral("--worktree"), QStringLiteral("--"), requestedPath};
        }
        const JobOutcome gitJob = runProcessJob(this, QStringLiteral("codex.git-file-action"), QStringLiteral("git"), args, root, 15000);
        const QVariantMap gitDetails = gitJob.value.toMap();
        const QString output = QString::fromLocal8Bit(gitDetails.value(QStringLiteral("stdout")).toByteArray() + gitDetails.value(QStringLiteral("stderr")).toByteArray()).trimmed();
        workspace->appendWorkbenchEvent(QStringLiteral("changes"), gitJob.succeeded()
            ? tr("Git file operation completed: %1").arg(requestedPath)
            : tr("Git file operation failed: %1").arg(output.isEmpty() ? jobError(gitJob, tr("Unknown error")) : output));
    });
    connect(workspace, &CodexAgentWorkspace::diffPatchActionRequested, this, [this, workspace](const QString& patch, bool reverse) {
        if (!patch.contains(QStringLiteral("diff --git ")) || patch.contains(QRegularExpression(QStringLiteral("(?m)^diff --git .*\\.\\./")))) {
            workspace->appendWorkbenchEvent(QStringLiteral("changes"), tr("Patch rejected: it must be a project Git diff without path traversal."));
            return;
        }
        const QString root = _session.config().workingDirectory.isEmpty() ? QDir::currentPath() : _session.config().workingDirectory;
        QStringList args{QStringLiteral("apply"), QStringLiteral("--cached"), QStringLiteral("--recount")};
        if (reverse) args << QStringLiteral("--reverse");
        const JobOutcome patchJob = runProcessJob(this, QStringLiteral("codex.git-apply-patch"), QStringLiteral("git"), args, root, 15000, patch.toUtf8());
        const QVariantMap patchDetails = patchJob.value.toMap();
        const QString output = QString::fromLocal8Bit(patchDetails.value(QStringLiteral("stdout")).toByteArray() + patchDetails.value(QStringLiteral("stderr")).toByteArray()).trimmed();
        workspace->appendWorkbenchEvent(QStringLiteral("changes"), patchJob.succeeded()
            ? (reverse ? tr("Staged hunk rejected.") : tr("Hunk accepted into the Git index."))
            : tr("Patch application failed: %1").arg(output.isEmpty() ? jobError(patchJob, tr("Unknown error")) : output));
    });
    connect(workspace, &CodexAgentWorkspace::diffConflictRequested, this, [this, workspace] {
        const QString root = _session.config().workingDirectory.isEmpty() ? QDir::currentPath() : _session.config().workingDirectory;
        const JobOutcome conflictJob = runProcessJob(this, QStringLiteral("codex.git-conflict-scan"), QStringLiteral("git"), {QStringLiteral("diff"), QStringLiteral("--name-only"), QStringLiteral("--diff-filter=U")}, root, 15000);
        if (!conflictJob.succeeded()) { workspace->appendWorkbenchEvent(QStringLiteral("changes"), jobError(conflictJob, tr("Conflict scan failed."))); return; }
        const QString files = QString::fromLocal8Bit(conflictJob.value.toMap().value(QStringLiteral("stdout")).toByteArray()).trimmed();
        workspace->appendWorkbenchEvent(QStringLiteral("changes"), files.isEmpty() ? tr("No unresolved Git conflicts.") : tr("Unresolved conflict files:\n%1").arg(files));
    });
    connect(workspace, &CodexAgentWorkspace::diffExportRequested, this, [this, workspace] {
        const QString root = _session.config().workingDirectory.isEmpty() ? QDir::currentPath() : _session.config().workingDirectory;
        const QString target = QFileDialog::getSaveFileName(workspace, tr("Export Git patch"), QDir::home().filePath(QStringLiteral("cgplay_changes.patch")), tr("Patch (*.patch *.diff)")); if (target.isEmpty()) return;
        const JobOutcome exportJob = runProcessJob(this, QStringLiteral("codex.git-export-patch"), QStringLiteral("git"), {QStringLiteral("diff"), QStringLiteral("--binary"), QStringLiteral("--find-renames"), QStringLiteral("--"), QStringLiteral(".")}, root, 30000);
        if (!exportJob.succeeded()) { workspace->appendWorkbenchEvent(QStringLiteral("changes"), jobError(exportJob, tr("Patch export failed."))); return; }
        const QByteArray patch = exportJob.value.toMap().value(QStringLiteral("stdout")).toByteArray(); QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly) || file.write(patch) != patch.size() || !file.commit()) workspace->appendWorkbenchEvent(QStringLiteral("changes"), tr("Patch export failed."));
        else workspace->appendWorkbenchEvent(QStringLiteral("changes"), tr("Patch exported: %1").arg(target));
    });
    connect(workspace, &CodexAgentWorkspace::localTaskRequested, this, [workspace](const QString& objective) {
        const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QDir().mkpath(directory);
        const QString path = QDir(directory).filePath(QStringLiteral("cgplay_local_tasks.jsonl"));
        QJsonObject task{{QStringLiteral("protocol"), QStringLiteral("cgplay.local-task.v1")},
                         {QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                         {QStringLiteral("objective"), objective},
                         {QStringLiteral("status"), QStringLiteral("queued")},
                         {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) { workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Local task queue unavailable.")); return; }
        file.write(QJsonDocument(task).toJson(QJsonDocument::Compact)); file.write("\n");
        workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("CGPlay local background task queued: %1").arg(objective));
    });
    connect(workspace, &CodexAgentWorkspace::localTaskManageRequested, this, [this, workspace] {
        const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("cgplay_local_tasks.jsonl"));
        QFile input(path);
        QJsonArray tasks;
        if (input.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!input.atEnd()) {
                QJsonParseError error;
                const QJsonDocument document = QJsonDocument::fromJson(input.readLine(), &error);
                if (error.error == QJsonParseError::NoError && document.isObject()) tasks.push_back(document.object());
            }
        }
        QStringList choices;
        for (const QJsonValue& value : tasks) {
            const QJsonObject task = value.toObject();
            choices << QStringLiteral("%1 [%2] %3").arg(task.value(QStringLiteral("id")).toString().left(8), task.value(QStringLiteral("status")).toString(), task.value(QStringLiteral("objective")).toString());
        }
        if (choices.isEmpty()) { workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("No local background tasks.")); return; }
        bool ok = false;
        const QString selected = QInputDialog::getItem(workspace, tr("CGPlay local tasks"), tr("Task"), choices, 0, false, &ok);
        if (!ok) return;
        const int index = choices.indexOf(selected);
        if (index < 0) return;
        const QString action = QInputDialog::getItem(workspace, tr("CGPlay local tasks"), tr("Action"), {tr("Cancel"), tr("Retry"), tr("Delete")}, 0, false, &ok);
        if (!ok) return;
        QJsonObject task = tasks.at(index).toObject();
        if (action == tr("Delete")) {
            tasks.removeAt(index);
            QSaveFile output(path);
            if (!output.open(QIODevice::WriteOnly)) { workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Local task queue save failed.")); return; }
            for (const QJsonValue& value : tasks) { output.write(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)); output.write("\n"); }
            if (!output.commit()) { workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Local task queue commit failed.")); return; }
            workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Local task deleted."));
            return;
        }
        task.insert(QStringLiteral("status"), action == tr("Cancel") ? QStringLiteral("cancelled") : QStringLiteral("queued"));
        task.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        tasks.replace(index, task);
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly)) { workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Local task queue save failed.")); return; }
        for (const QJsonValue& value : tasks) { output.write(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)); output.write("\n"); }
        if (!output.commit()) { workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Local task queue commit failed.")); return; }
        workspace->appendWorkbenchEvent(QStringLiteral("agents"), tr("Local task updated: %1").arg(task.value(QStringLiteral("id")).toString()));
        if (action == tr("Retry")) QTimer::singleShot(0, this, &CodexPlugin::processLocalTaskQueue);
    });
    connect(workspace, &CodexAgentWorkspace::rulesAndConfigRequested, this, [this, workspace] {
        const QString root = _session.config().workingDirectory.isEmpty() ? QDir::currentPath() : _session.config().workingDirectory;
        QStringList lines{tr("Project: %1").arg(root)};
        QStringList agentFiles;
        QDir current(root);
        while (current.exists()) {
            const QString agents = current.filePath(QStringLiteral("AGENTS.md"));
            if (QFileInfo(agents).isFile()) agentFiles.prepend(agents);
            if (!current.cdUp()) break;
        }
        QStringList merged;
        for (int index = 0; index < agentFiles.size(); ++index) {
            QFile file(agentFiles.at(index)); QString content;
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) content = QString::fromUtf8(file.readAll()).trimmed();
            lines << tr("AGENTS.md priority %1 (later overrides earlier): %2\n%3").arg(index + 1).arg(agentFiles.at(index), content.left(10000));
            merged << tr("# Priority %1: %2\n%3").arg(index + 1).arg(agentFiles.at(index), content);
        }
        lines << tr("Effective merged AGENTS rules (root to leaf):\n%1").arg(merged.join(QStringLiteral("\n\n"))).left(30000);
        const QString projectConfig = QDir(root).filePath(QStringLiteral(".codex/config.toml"));
        const QString globalConfig = QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)).filePath(QStringLiteral(".codex/config.toml"));
        lines << tr("Project config: %1").arg(QFileInfo(projectConfig).isFile() ? projectConfig : tr("not found"));
        lines << tr("Global config: %1").arg(QFileInfo(globalConfig).isFile() ? globalConfig : tr("not found"));
        lines << tr("Profiles, hooks, and feature flags are shown from app-server runtime responses; secrets are never displayed.");
        workspace->appendWorkbenchEvent(QStringLiteral("rules"), lines.join(QLatin1Char('\n')));
        QString error;
        for (const QString& method : {QStringLiteral("config/read"), QStringLiteral("hooks/list"), QStringLiteral("experimentalFeature/list"), QStringLiteral("permissionProfile/list")}) {
            if (!sendNativeRpc(method, {}, nullptr, &error)) workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("%1 unavailable: %2").arg(method, error));
        }
    });
    connect(workspace, &CodexAgentWorkspace::configEditRequested, this, [this, workspace](const QString& scope) {
        const QString root = _session.config().workingDirectory.isEmpty() ? QDir::currentPath() : _session.config().workingDirectory;
        const QString path = scope == QStringLiteral("project")
            ? QDir(root).filePath(QStringLiteral(".codex/config.toml"))
            : QDir::home().filePath(QStringLiteral(".codex/config.toml"));
        QFile input(path);
        QString content;
        if (input.open(QIODevice::ReadOnly | QIODevice::Text)) content = QString::fromUtf8(input.readAll());
        bool accepted = false;
        const QString updated = QInputDialog::getMultiLineText(workspace, tr("Edit Codex config"), path, content, &accepted);
        if (!accepted || updated == content) return;
        static const QRegularExpression prohibited(QStringLiteral("(?im)^\\s*(?:env_key|experimental_bearer_token|api[_-]?key|token|authorization)\\s*="));
        if (prohibited.match(updated).hasMatch()) {
            workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("Config save rejected: credential fields are managed securely outside this editor."));
            return;
        }
        QDir().mkpath(QFileInfo(path).absolutePath());
        if (!content.trimmed().isEmpty()) {
            QSaveFile backup(path + QStringLiteral(".cgplay.bak"));
            if (!backup.open(QIODevice::WriteOnly) || backup.write(content.toUtf8()) != content.toUtf8().size() || !backup.commit()) { workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("Config backup failed; save cancelled.")); return; }
        }
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly) || output.write(updated.toUtf8()) != updated.toUtf8().size() || !output.commit()) {
            workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("Config save failed: %1").arg(path));
            return;
        }
        workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("Config saved: %1. Restart Codex session to apply it.").arg(path));
    });
    connect(workspace, &CodexAgentWorkspace::configRollbackRequested, this, [this, workspace](const QString& scope) {
        const QString root = _session.config().workingDirectory.isEmpty() ? QDir::currentPath() : _session.config().workingDirectory;
        const QString path = scope == QStringLiteral("project") ? QDir(root).filePath(QStringLiteral(".codex/config.toml")) : QDir::home().filePath(QStringLiteral(".codex/config.toml"));
        QFile backup(path + QStringLiteral(".cgplay.bak"));
        if (!backup.open(QIODevice::ReadOnly | QIODevice::Text)) { workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("No configuration backup available.")); return; }
        const QString text = QString::fromUtf8(backup.readAll());
        static const QRegularExpression prohibited(QStringLiteral("(?im)^\\s*(?:env_key|experimental_bearer_token|api[_-]?key|token|authorization)\\s*="));
        if (prohibited.match(text).hasMatch()) { workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("Configuration backup rejected: it contains a credential field.")); return; }
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly) || output.write(text.toUtf8()) != text.toUtf8().size() || !output.commit()) { workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("Configuration rollback failed.")); return; }
        workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("Configuration restored from latest backup."));
    });
    connect(workspace, &CodexAgentWorkspace::reviewRequested, this, [this, workspace] {
        QString error;
        if (startReview(&error)) workspace->appendLogLine(tr("Review requested for uncommitted changes."));
        else workspace->appendLogLine(tr("Review failed: %1").arg(error));
    });
    connect(workspace, &CodexAgentWorkspace::setGoalRequested, this, [this, workspace] {
        bool accepted = false;
        const QString objective = QInputDialog::getText(workspace, tr("Thread goal"), tr("Objective"), QLineEdit::Normal, {}, &accepted);
        if (!accepted || objective.trimmed().isEmpty()) return;
        QString error;
        if (setThreadGoal(objective.trimmed(), &error)) workspace->appendLogLine(tr("Thread goal set."));
        else workspace->appendLogLine(tr("Goal set failed: %1").arg(error));
    });
    connect(workspace, &CodexAgentWorkspace::clearGoalRequested, this, [this, workspace] {
        QString error;
        if (clearThreadGoal(&error)) workspace->appendLogLine(tr("Thread goal cleared."));
        else workspace->appendLogLine(tr("Goal clear failed: %1").arg(error));
    });
    connect(workspace, &CodexAgentWorkspace::refreshNativeCatalogRequested, this, [this, workspace] {
        QString error;
        if (refreshNativeCatalog(&error)) workspace->appendLogLine(tr("Native Codex catalogs refreshed."));
        else workspace->appendLogLine(tr("Native catalog refresh failed: %1").arg(error));
    });
    connect(workspace, &CodexAgentWorkspace::managePluginRequested, this, [this, workspace] {
        bool accepted = false;
        const QString operation = QInputDialog::getItem(workspace, tr("Codex plugins"), tr("Operation"),
            {tr("Read"), tr("Install"), tr("Uninstall")}, 0, false, &accepted);
        if (!accepted) return;
        const QString name = QInputDialog::getText(workspace, tr("Codex plugins"), tr("Plugin name"), QLineEdit::Normal, {}, &accepted).trimmed();
        if (!accepted || name.isEmpty()) return;
        if (operation == tr("Uninstall") && QMessageBox::question(workspace, tr("Uninstall plugin"), name,
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        QString error;
        const bool ok = operation == tr("Read") ? readPlugin(name, &error)
            : (operation == tr("Install") ? installPlugin(name, &error) : uninstallPlugin(name, &error));
        workspace->appendLogLine(ok ? tr("Plugin request sent: %1 %2").arg(operation, name)
                                    : tr("Plugin request failed: %1").arg(error));
    });
    connect(workspace, &CodexAgentWorkspace::addMarketplaceRequested, this, [this, workspace] {
        bool accepted = false;
        const QString source = QInputDialog::getText(workspace, tr("Codex Marketplace"), tr("Source"), QLineEdit::Normal, {}, &accepted).trimmed();
        if (!accepted || source.isEmpty()) return;
        QString error;
        workspace->appendLogLine(addMarketplace(source, &error) ? tr("Marketplace add requested.") : tr("Marketplace add failed: %1").arg(error));
    });
    connect(workspace, &CodexAgentWorkspace::imagesSubmitted, this, [this, workspace](const QStringList& paths, const QString& prompt) {
        QString error;
        const bool submitted = !_activeTurnId.isEmpty()
            ? steerImagesTurn(paths, prompt, &error)
            : submitImagesPrompt(paths, prompt, &error, QStringLiteral("user-attachment"));
        if (!submitted) workspace->appendLogLine(tr("Image input failed: %1").arg(error));
        else if (!_activeTurnId.isEmpty()) {
            workspace->markPromptAccepted();
            workspace->appendWorkbenchEvent(QStringLiteral("task"), tr("Image guidance sent to current task."));
        }
    });
    connect(workspace, &CodexAgentWorkspace::nativeRpcConsoleRequested, this, [this, workspace] {
        bool accepted = false;
        const QStringList officialMethods{
            QStringLiteral("thread/unsubscribe"), QStringLiteral("thread/name/set"), QStringLiteral("thread/goal/get"), QStringLiteral("thread/metadata/update"), QStringLiteral("thread/loaded/list"), QStringLiteral("thread/inject_items"),
            QStringLiteral("skills/extraRoots/set"), QStringLiteral("marketplace/remove"), QStringLiteral("marketplace/upgrade"), QStringLiteral("plugin/installed"), QStringLiteral("plugin/skill/read"), QStringLiteral("plugin/share/save"), QStringLiteral("plugin/share/updateTargets"), QStringLiteral("plugin/share/list"), QStringLiteral("plugin/share/checkout"), QStringLiteral("plugin/share/delete"),
            QStringLiteral("fs/readFile"), QStringLiteral("fs/writeFile"), QStringLiteral("fs/createDirectory"), QStringLiteral("fs/getMetadata"), QStringLiteral("fs/readDirectory"), QStringLiteral("fs/remove"), QStringLiteral("fs/copy"), QStringLiteral("fs/watch"), QStringLiteral("fs/unwatch"), QStringLiteral("fuzzyFileSearch"),
            QStringLiteral("windowsSandbox/setupStart"), QStringLiteral("account/login/cancel"), QStringLiteral("account/rateLimitResetCredit/consume"), QStringLiteral("account/workspaceMessages/read"), QStringLiteral("account/sendAddCreditsNudgeEmail"), QStringLiteral("feedback/upload"), QStringLiteral("config/value/write"), QStringLiteral("config/batchWrite"), QStringLiteral("configRequirements/read"), QStringLiteral("externalAgentConfig/detect"), QStringLiteral("externalAgentConfig/import"), QStringLiteral("externalAgentConfig/import/readHistories"),
            QStringLiteral("command/exec"), QStringLiteral("command/exec/write"), QStringLiteral("command/exec/terminate"), QStringLiteral("command/exec/resize")
        };
        const QString method = QInputDialog::getItem(workspace, tr("Codex 官方协议"), tr("选择方法（可编辑）"), officialMethods, 0, true, &accepted).trimmed();
        if (!accepted || method.isEmpty()) return;
        const QString json = QInputDialog::getMultiLineText(workspace, tr("Codex native RPC"), tr("Params JSON"), QStringLiteral("{}"), &accepted);
        if (!accepted) return;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            workspace->appendLogLine(tr("Native RPC params are not a JSON object: %1").arg(parseError.errorString()));
            return;
        }
        qint64 requestId = 0;
        QString error;
        if (invokeOfficialCapability(method, document.object(), &error)) workspace->appendLogLine(tr("Official capability sent: %1").arg(method));
        else workspace->appendLogLine(tr("Native RPC failed: %1").arg(error));
    });
    connect(workspace, &CodexAgentWorkspace::refreshRuntimeStatusRequested, this, [this, workspace] {
        const QStringList methods{QStringLiteral("account/read"), QStringLiteral("account/usage/read"),
            QStringLiteral("account/rateLimits/read"), QStringLiteral("permissionProfile/list"),
            QStringLiteral("collaborationMode/list"), QStringLiteral("experimentalFeature/list"),
            QStringLiteral("windowsSandbox/readiness"), QStringLiteral("hooks/list")};
        for (const QString& method : methods) {
            QString error;
            if (!sendNativeRpc(method, {}, nullptr, &error)) workspace->appendLogLine(tr("%1 failed: %2").arg(method, error));
        }
    });
    connect(workspace, &CodexAgentWorkspace::exportDiagnosticsRequested, this, [this, workspace] {
        const QString target = QFileDialog::getSaveFileName(workspace, tr("Export redacted Codex diagnostics"), QDir::home().filePath(QStringLiteral("cgplay_codex_diagnostics.json")), tr("JSON (*.json)")); if (target.isEmpty()) return;
        QJsonObject output{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {QStringLiteral("localOnly"), true}, {QStringLiteral("diagnostics"), diagnostics()}, {QStringLiteral("nativeResponses"), _nativeResponses}, {QStringLiteral("mcpServers"), _mcpServers}, {QStringLiteral("orchestratorTasks"), _orchestrator->tasks()}};
        const QByteArray redacted = redactSensitiveText(QJsonDocument(output).toJson(QJsonDocument::Indented));
        QSaveFile file(target); if (!file.open(QIODevice::WriteOnly) || file.write(redacted) != redacted.size() || !file.commit()) { workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("Diagnostic export failed.")); return; }
        workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("Redacted diagnostics exported: %1").arg(target));
    });
    connect(workspace, &CodexAgentWorkspace::accountLoginRequested, this, [this, workspace] {
        QString error; if (!sendNativeRpc(QStringLiteral("account/login/start"), QJsonObject{{QStringLiteral("type"), QStringLiteral("chatgpt")}, {QStringLiteral("useHostedLoginSuccessPage"), true}}, nullptr, &error)) workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("Account login unavailable: %1").arg(error)); else workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("Codex account login started."));
    });
    connect(workspace, &CodexAgentWorkspace::accountLogoutRequested, this, [this, workspace] {
        QString error; if (!sendNativeRpc(QStringLiteral("account/logout"), {}, nullptr, &error)) workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("Account logout failed: %1").arg(error)); else workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("Codex account logout requested."));
    });
    connect(workspace, &CodexAgentWorkspace::featureFlagRequested, this, [this, workspace] {
        bool ok = false; const QString name = QInputDialog::getText(workspace, tr("Feature flag"), tr("Feature name"), QLineEdit::Normal, {}, &ok).trimmed(); if (!ok || name.isEmpty()) return;
        const QString value = QInputDialog::getItem(workspace, tr("Feature flag"), tr("Enablement"), {tr("Enabled"), tr("Disabled")}, 0, false, &ok); if (!ok) return;
        QString error; if (!sendNativeRpc(QStringLiteral("experimentalFeature/enablement/set"), QJsonObject{{QStringLiteral("enablement"), QJsonObject{{name, value == tr("Enabled")}}}}, nullptr, &error)) workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("Feature flag update failed: %1").arg(error)); else workspace->appendWorkbenchEvent(QStringLiteral("rules"), tr("Feature flag update requested: %1").arg(name));
    });
    connect(workspace, &CodexAgentWorkspace::threadRollbackRequested, this, [this, workspace] {
        if (_threadId.isEmpty()) return; bool ok = false; const int turns = QInputDialog::getInt(workspace, tr("Rollback thread"), tr("Turns to remove"), 1, 1, 100, 1, &ok); if (!ok) return;
        QString error; if (!sendNativeRpc(QStringLiteral("thread/rollback"), QJsonObject{{QStringLiteral("threadId"), _threadId}, {QStringLiteral("numTurns"), turns}}, nullptr, &error)) workspace->appendWorkbenchEvent(QStringLiteral("changes"), tr("Thread rollback failed: %1").arg(error)); else workspace->appendWorkbenchEvent(QStringLiteral("changes"), tr("Thread rollback requested."));
    });
    connect(workspace, &CodexAgentWorkspace::terminalCommandRequested, this, [this, workspace](const QString& command) {
        if (!_terminalProcess || _terminalProcess->state() == QProcess::NotRunning) {
            const CodexAppServerSession::Config config = _session.config();
            const QString cwd = config.workingDirectory.isEmpty() ? QDir::currentPath() : config.workingDirectory;
            _terminalProcess = std::make_unique<QProcess>(this);
            _terminalProcess->setWorkingDirectory(cwd);
            _terminalProcess->setProcessChannelMode(QProcess::SeparateChannels);
            connect(_terminalProcess.get(), &QProcess::readyReadStandardOutput, this, [this, workspace] {
                const QString output = QString::fromLocal8Bit(_terminalProcess->readAllStandardOutput());
                if (!output.trimmed().isEmpty()) {
                    workspace->appendWorkbenchEvent(QStringLiteral("terminal"), output.trimmed());
                    const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_terminal.jsonl"));
                    QFile file(path); if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) { file.write(QJsonDocument(QJsonObject{{QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {QStringLiteral("type"), QStringLiteral("output")}, {QStringLiteral("text"), output.trimmed()}}).toJson(QJsonDocument::Compact)); file.write("\n"); }
                }
            });
            connect(_terminalProcess.get(), &QProcess::readyReadStandardError, this, [this, workspace] {
                const QString output = QString::fromLocal8Bit(_terminalProcess->readAllStandardError());
                if (!output.trimmed().isEmpty()) {
                    workspace->appendWorkbenchEvent(QStringLiteral("terminal"), QStringLiteral("[stderr] %1").arg(output.trimmed()));
                    const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_terminal.jsonl"));
                    QFile file(path); if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) { file.write(QJsonDocument(QJsonObject{{QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {QStringLiteral("type"), QStringLiteral("stderr")}, {QStringLiteral("text"), output.trimmed()}}).toJson(QJsonDocument::Compact)); file.write("\n"); }
                }
            });
            connect(_terminalProcess.get(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, workspace](int code, QProcess::ExitStatus status) {
                workspace->appendWorkbenchEvent(QStringLiteral("terminal"), tr("Terminal closed: exit=%1 status=%2").arg(code).arg(static_cast<int>(status)));
                _terminalTimeoutTimer->stop(); _terminalProcess.reset();
            });
            _terminalProcess->start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-NoExit"), QStringLiteral("-Command"), QStringLiteral("-")});
            if (!_terminalProcess->waitForStarted(5000)) {
                workspace->appendWorkbenchEvent(QStringLiteral("terminal"), tr("Terminal start failed: %1").arg(_terminalProcess->errorString()));
                _terminalProcess.reset();
                return;
            }
            workspace->appendWorkbenchEvent(QStringLiteral("terminal"), tr("Interactive PowerShell started in %1 (pid=%2)").arg(cwd).arg(_terminalProcess->processId()));
        }
        workspace->appendWorkbenchEvent(QStringLiteral("terminal"), QStringLiteral("> %1").arg(command));
        _terminalHistory.removeAll(command); _terminalHistory.prepend(command); while (_terminalHistory.size() > 100) _terminalHistory.removeLast();
        { const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_terminal.jsonl")); QFile file(path); if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) { file.write(QJsonDocument(QJsonObject{{QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {QStringLiteral("type"), QStringLiteral("command")}, {QStringLiteral("pid"), static_cast<qint64>(_terminalProcess->processId())}, {QStringLiteral("text"), command}}).toJson(QJsonDocument::Compact)); file.write("\n"); } }
        _terminalProcess->write(command.toLocal8Bit());
        _terminalProcess->write("\r\n");
        _terminalProcess->waitForBytesWritten(1000);
        _terminalTimeoutTimer->start(10 * 60 * 1000);
    });
    connect(workspace, &CodexAgentWorkspace::terminalStopRequested, this, [this, workspace] {
        if (_terminalProcess && _terminalProcess->state() != QProcess::NotRunning) {
            _terminalProcess->kill();
            _terminalTimeoutTimer->stop();
            workspace->appendWorkbenchEvent(QStringLiteral("terminal"), tr("Terminal command stopped."));
        }
    });
    connect(workspace, &CodexAgentWorkspace::terminalRestartRequested, this, [this, workspace] {
        if (_terminalProcess && _terminalProcess->state() != QProcess::NotRunning) _terminalProcess->kill();
        _terminalTimeoutTimer->stop();
        workspace->appendWorkbenchEvent(QStringLiteral("terminal"), tr("Terminal restart requested. Enter a command to start a new session."));
    });
    connect(workspace, &CodexAgentWorkspace::terminalHistoryRequested, this, [this, workspace] {
        const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("codex_terminal.jsonl"));
        QFile file(path); QStringList entries;
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) while (!file.atEnd()) { const QJsonDocument doc = QJsonDocument::fromJson(file.readLine()); const QJsonObject row = doc.object(); if (row.value(QStringLiteral("type")).toString() == QStringLiteral("command")) entries << row.value(QStringLiteral("text")).toString(); }
        entries.removeDuplicates(); workspace->appendWorkbenchEvent(QStringLiteral("terminal"), entries.isEmpty() ? tr("No saved terminal history.") : entries.mid(0, 100).join(QLatin1Char('\n')));
    });
    connect(workspace, &CodexAgentWorkspace::terminalStatusRequested, this, [this, workspace] {
        if (!_terminalProcess || _terminalProcess->state() == QProcess::NotRunning) { workspace->appendWorkbenchEvent(QStringLiteral("terminal"), tr("No interactive terminal process.")); return; }
        const JobOutcome statusJob = runProcessJob(this, QStringLiteral("codex.terminal-status"), QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), QStringLiteral("Get-CimInstance Win32_Process | Where-Object { $_.ProcessId -eq %1 -or $_.ParentProcessId -eq %1 } | Select-Object ProcessId,ParentProcessId,Name,CommandLine | Format-Table -AutoSize").arg(_terminalProcess->processId())}, {}, 10000);
        if (!statusJob.succeeded()) { workspace->appendWorkbenchEvent(QStringLiteral("terminal"), jobError(statusJob, tr("Process status query failed."))); return; }
        const QVariantMap statusDetails = statusJob.value.toMap();
        workspace->appendWorkbenchEvent(QStringLiteral("terminal"), QString::fromLocal8Bit(statusDetails.value(QStringLiteral("stdout")).toByteArray() + statusDetails.value(QStringLiteral("stderr")).toByteArray()).trimmed());
    });
    connect(workspace, &CodexAgentWorkspace::diffRequested, this, [this, workspace] {
        const QString cwd = _session.config().workingDirectory.isEmpty() ? QDir::currentPath() : _session.config().workingDirectory;
        const JobOutcome statusJob = runProcessJob(this, QStringLiteral("codex.git-status"), QStringLiteral("git"), {QStringLiteral("status"), QStringLiteral("--porcelain=v1"), QStringLiteral("--untracked-files=all")}, cwd, 15000);
        if (statusJob.succeeded()) workspace->appendWorkbenchEvent(QStringLiteral("changes"), tr("Changed file tree:\n%1").arg(QString::fromLocal8Bit(statusJob.value.toMap().value(QStringLiteral("stdout")).toByteArray()).trimmed()));
        const JobOutcome diffJob = runProcessJob(this, QStringLiteral("codex.git-diff"), QStringLiteral("git"), {QStringLiteral("diff"), QStringLiteral("--no-ext-diff"), QStringLiteral("--find-renames"), QStringLiteral("--"), QStringLiteral(".")}, cwd, 15000);
        if (!diffJob.succeeded()) { workspace->appendWorkbenchEvent(QStringLiteral("changes"), jobError(diffJob, tr("git diff failed."))); return; }
        const QString output = QString::fromLocal8Bit(diffJob.value.toMap().value(QStringLiteral("stdout")).toByteArray()).trimmed();
        workspace->appendWorkbenchEvent(QStringLiteral("changes"), output.isEmpty() ? tr("No uncommitted Git diff.") : output);
    });
    connect(workspace, &CodexAgentWorkspace::agentTaskRequested, this, [this, workspace](const QString& objective) {
        QString error;
        if (setThreadGoal(objective, &error)) {
            workspace->appendWorkbenchEvent(QStringLiteral("task"), tr("Codex task created: %1").arg(objective));
        } else {
            workspace->appendWorkbenchEvent(QStringLiteral("task"), tr("Task creation failed: %1").arg(error));
        }
    });
    connect(workspace, &CodexAgentWorkspace::mcpToolCallRequested, this,
        [this, workspace](const QString& server, const QString& tool, const QJsonObject& arguments) {
            QString error;
            if (callMcpTool(server, tool, arguments, &error)) {
                workspace->appendLogLine(tr("MCP tool requested: %1/%2").arg(server, tool));
            } else {
                workspace->appendLogLine(tr("MCP tool failed: %1").arg(error));
            }
        });
    connect(this, &CodexPlugin::appServerStarted, workspace, [workspace] {
        workspace->setConnectionStatus(QObject::tr("正在初始化"), true);
        workspace->setRetryAvailable(false);
        workspace->setPromptEnabled(false);
    });
    connect(this, &CodexPlugin::appServerStopped, workspace, [workspace](int, QProcess::ExitStatus) {
        workspace->setConnectionStatus(QObject::tr("已停止"), false);
        workspace->setRetryAvailable(true);
        workspace->setTurnInProgress(false);
        workspace->setPromptEnabled(false);
    });
    connect(this, &CodexPlugin::standardErrorLineReceived, workspace, [workspace](const QByteArray& line) {
        workspace->appendLogLine(QString::fromUtf8(line));
    });
    connect(this, &CodexPlugin::appServerError, workspace, [workspace](const QString& error) {
        workspace->appendLogLine(error);
        workspace->setError(error);
        workspace->setRetryAvailable(true);
    });
    connect(this, &CodexPlugin::protocolError, workspace, [workspace](const QString& error) {
        workspace->appendLogLine(error);
        workspace->setError(error);
        workspace->setPromptEnabled(false);
    });
    connect(this, &CodexPlugin::protocolStateChanged, workspace, [workspace](const QString& state) {
        workspace->appendLogLine(QObject::tr("协议状态：%1").arg(displayProtocolState(state)));
        const bool ready = state == QStringLiteral("ready");
        workspace->setSessionActionsEnabled(
            ready,
            state == QStringLiteral("turnInProgress")
                ? QObject::tr("当前任务完成后可切换会话")
                : QObject::tr("Codex 会话尚未就绪"));
        if (state == QStringLiteral("initializing") || state == QStringLiteral("startingThread")) {
            workspace->setConnectionStatus(QObject::tr("正在连接"), true);
        }
    });
    connect(this, &CodexPlugin::threadReady, workspace, [workspace](const QString& threadId) {
        workspace->setConnectionStatus(QObject::tr("Codex 在线"), true);
        workspace->setRetryAvailable(false);
        workspace->setTurnInProgress(false);
        workspace->setPromptEnabled(true);
        workspace->setSessionActionsEnabled(true);
        workspace->setProperty("codexCurrentThreadId", threadId);
    });
    connect(this, &CodexPlugin::turnStarted, workspace, [workspace](const QString&, const QString&) {
        workspace->setConnectionStatus(QObject::tr("Codex 在线"), true);
        workspace->setTurnInProgress(true);
        workspace->setPromptEnabled(true);
        workspace->markTurnStarted();
        workspace->setSessionActionsEnabled(false, QObject::tr("当前任务完成后可切换会话"));
    });
    connect(this,
        &CodexPlugin::agentMessageDelta,
        workspace,
        [this, workspace](const QString&, const QString&, const QString&, const QString& delta) {
            workspace->appendStreamText(delta);
            const QString normalized = delta.toLower();
            if (!imageApiConfigured() &&
                (normalized.contains(QStringLiteral("native image-generation tool")) ||
                 normalized.contains(QStringLiteral("image_gen tool")) ||
                 normalized.contains(QStringLiteral("`image_gen` tool"))) &&
                normalized.contains(QStringLiteral("not available"))) {
                workspace->setImageGenerationCapability(
                    false,
                    QObject::tr("当前会话未暴露原生图片生成工具"));
            }
        });
    connect(this, &CodexPlugin::turnCompleted, workspace, [workspace](const QJsonObject& turn) {
        const QString status = turn.value(QStringLiteral("status")).toString();
        workspace->appendLogLine(QObject::tr("本轮状态：%1。").arg(displayTurnStatus(status)));
        workspace->completeTurn(status);
        workspace->setTurnInProgress(false);
        workspace->setConnectionStatus(QObject::tr("Codex 在线"), true);
        workspace->setSessionActionsEnabled(true);
        workspace->setPromptEnabled(true);
    });
}


} // namespace cgplay
