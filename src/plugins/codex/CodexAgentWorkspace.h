#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QAction;
class QActionGroup;
class QComboBox;
class QDialog;
class QEvent;
class QWebEngineView;
class QFrame;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QScrollArea;
class QShowEvent;
class QToolButton;
class QTabWidget;
class QVBoxLayout;
QT_END_NAMESPACE

namespace cgplay {

class CodexAgentWorkspace final : public QWidget
{
    Q_OBJECT

public:
    explicit CodexAgentWorkspace(QWidget* parent = nullptr);

signals:
    void activationRequested();
    void retryRequested();
    void newConversationRequested();
    void openProjectRequested();
    void closeRequested();
    void stopRequested();
    void promptSubmitted(const QString& prompt);
    void modelSelectionChanged(const QString& model);
    void reasoningEffortChanged(const QString& effort);
    void approvalModeChanged(const QString& approvalMode);
    void importGeneratedImageRequested(const QString& absolutePath);
    void sessionListRequested();
    void sessionReadRequested(const QString& threadId);
    void sessionResumeRequested(const QString& threadId);
    void sessionForkRequested(const QString& threadId);
    void sessionArchiveRequested(const QString& threadId);
    void mcpReloadRequested();
    void reviewRequested();
    void setGoalRequested();
    void clearGoalRequested();
    void refreshNativeCatalogRequested();
    void managePluginRequested();
    void addMarketplaceRequested();
    void imagesSubmitted(const QStringList& absolutePaths, const QString& prompt);
    void workbenchRequested();
    void importMediaRequested(const QString& absolutePath);
    void terminalCommandRequested(const QString& command);
    void terminalStopRequested();
    void terminalRestartRequested();
    void terminalHistoryRequested();
    void terminalStatusRequested();
    void diffRequested();
    void agentTaskRequested(const QString& objective);
    void nativeRpcConsoleRequested();
    void refreshRuntimeStatusRequested();
    void exportDiagnosticsRequested();
    void accountLoginRequested();
    void accountLogoutRequested();
    void featureFlagRequested();
    void threadRollbackRequested();
    void mcpToolCallRequested(const QString& server, const QString& tool, const QJsonObject& arguments);
    void mcpLoginRequested(const QString& server);
    void mcpResourceReadRequested(const QString& server, const QString& uri);
    void rulesAndConfigRequested();
    void browserContextSubmitted(const QString& url, const QString& title, const QString& selectedText);
    void browserSnapshotSubmitted(const QStringList& paths, const QString& prompt);
    void sessionSearchRequested(const QString& query);
    void sessionExportRequested();
    void diffFileActionRequested(const QString& path, const QString& action);
    void localTaskRequested(const QString& objective);
    void configEditRequested(const QString& scope);
    void configRollbackRequested(const QString& scope);
    void localTaskManageRequested();
    void diffPatchActionRequested(const QString& patch, bool reverse);
    void diffConflictRequested();
    void diffExportRequested();
    void skillsRefreshRequested();
    void mcpToolDialogRequested();
    void sessionBulkArchiveRequested();
    void sessionImportRequested();
    void sessionDeleteRequested();
    void sessionBulkRestoreRequested();
    void localSkillManageRequested();
    void appManageRequested();
    void orchestratorCreateRequested(const QString& objective, int agentCount);
    void orchestratorManageRequested();
    void orchestratorMergeRequested(const QString& taskId);

public slots:
    // Re-apply all Codex surfaces after the application theme or palette changes.
    void refreshTheme();
    void setRuntimeSourceStatus(const QString& status, bool configured);
    void setAiWorkspaceSourceStatus(const QString& status, bool configured);
    void setImageGenerationCapability(bool available, const QString& reason = {});
    void setProjectPath(const QString& path);
    void setModelOptions(const QStringList& models, const QString& selectedModel);
    void setReasoningOptions(const QStringList& efforts, const QString& selectedEffort);
    void setApprovalMode(const QString& approvalMode);
    void setConnectionStatus(const QString& status, bool connected);
    void setPromptEnabled(bool enabled);
    void setRetryAvailable(bool available);
    void appendStreamText(const QString& text);
    void appendLogLine(const QString& text);
    void setError(const QString& error);
    void beginNewConversation();
    void markPromptAccepted();
    void markTurnStarted();
    void updateTaskPlan(const QJsonArray& plan, const QString& explanation);
    void markTaskActivity(const QString& activityId, const QString& label, bool completed);
    void completeTurn(const QString& status);
    Q_INVOKABLE void setTurnInProgress(bool active);
    void setTokenUsage(const QJsonObject& tokenUsage);
    void updateImageGeneration(const QJsonObject& item, bool completed);
    Q_INVOKABLE void appendFileArtifact(const QString& absolutePath, const QString& label = {});
    void setSessionList(const QJsonArray& threads, const QString& currentThreadId);
    void showThreadHistory(const QJsonObject& thread);
    void setSessionActionsEnabled(bool enabled, const QString& reason = {});
    void appendWorkbenchEvent(const QString& category, const QString& text);
    void submitWorkbenchContext(const QString& prompt);
    bool browserNavigate(const QString& url, QString* error = nullptr);
    bool browserClick(const QString& selector, QString* error = nullptr);
    bool browserType(const QString& selector, const QString& text, QString* error = nullptr);
    QString browserExtract(const QString& selector, QString* error = nullptr);
    Q_INVOKABLE bool captureBrowserSnapshot();

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private slots:
    void submitPrompt();
    void refreshActionState();

private:
    QString selectedModel() const;
    QString selectedReasoningEffort() const;
    QString selectedApprovalMode() const;
    QString approvalDisplayName(const QString& mode) const;
    QString reasoningEffortDisplayName(const QString& effort) const;
    void chooseApprovalMode(const QString& mode);
    void chooseReasoningEffort(const QString& effort);
    void setStatusText(const QString& text, bool connected, bool failed = false);
    void setDiagnosticsVisible(bool visible);
    void appendUserMessage(const QString& text, const QStringList& imagePaths = {});
    void appendDetectedFileArtifacts(const QString& text);
    void ensureAssistantMessage();
    void ensureTaskCard();
    QLabel* ensureTaskStep(const QString& id);
    void scrollToBottom();
    void updateEmptyState();
    void updateConversationTitle(const QString& prompt);
    void clearConversationWidgets();
    void showWorkbench();
    void refreshWorkbench();
    void addWorkbenchRow(const QString& category, const QString& text);
    void addImageAttachments(const QStringList& absolutePaths);
    bool pasteClipboardImages();
    void refreshImageAttachments();
    void showImagePreview(const QString& absolutePath);

    QLabel* _titleLabel = nullptr;
    QLabel* _statusLabel = nullptr;
    QLabel* _sourceSummaryLabel = nullptr;
    QLabel* _imageCapabilityLabel = nullptr;
    QLabel* _errorLabel = nullptr;
    QLabel* _runtimeSourceLabel = nullptr;
    QLabel* _aiWorkspaceSourceLabel = nullptr;
    QLabel* _emptyStateLabel = nullptr;
    QLabel* _assistantTextLabel = nullptr;
    QLabel* _assistantMetricsLabel = nullptr;
    QJsonObject _latestTokenUsage;
    QLabel* _taskStatusLabel = nullptr;
    QToolButton* _retryButton = nullptr;
    QToolButton* _newConversationButton = nullptr;
    QToolButton* _headerNewConversationButton = nullptr;
    QToolButton* _sessionsButton = nullptr;
    QToolButton* _projectButton = nullptr;
    QToolButton* _approvalButton = nullptr;
    QToolButton* _reasoningButton = nullptr;
    QToolButton* _sendButton = nullptr;
    QToolButton* _diagnosticsButton = nullptr;
    QToolButton* _overflowButton = nullptr;
    QToolButton* _closeButton = nullptr;
    QToolButton* _taskToggle = nullptr;
    QPlainTextEdit* _logView = nullptr;
    QPlainTextEdit* _promptEdit = nullptr;
    QComboBox* _modelCombo = nullptr;
    QMenu* _approvalMenu = nullptr;
    QMenu* _reasoningMenu = nullptr;
    QMenu* _overflowMenu = nullptr;
    QMenu* _sessionsMenu = nullptr;
    QActionGroup* _approvalGroup = nullptr;
    QActionGroup* _reasoningGroup = nullptr;
    QAction* _stopAction = nullptr;
    QAction* _workbenchAction = nullptr;
    QScrollArea* _conversationScroll = nullptr;
    QWidget* _conversationHost = nullptr;
    QVBoxLayout* _conversationLayout = nullptr;
    QVBoxLayout* _assistantBodyLayout = nullptr;
    QHBoxLayout* _attachmentLayout = nullptr;
    QFrame* _diagnosticsFrame = nullptr;
    QFrame* _taskCard = nullptr;
    QFrame* _taskDetails = nullptr;
    QVBoxLayout* _taskDetailsLayout = nullptr;
    QHash<QString, QAction*> _approvalActions;
    QHash<QString, QAction*> _reasoningActions;
    QHash<QString, QLabel*> _taskSteps;
    QString _projectPath;
    QStringList _attachedImagePaths;
    QSet<QString> _turnArtifactPaths;
    QString _approvalMode = QStringLiteral("full");
    bool _reasoningOptionsAvailable = false;
    bool _runtimeConfigured = false;
    bool _aiConfigured = false;
    bool _connected = false;
    bool _turnInProgress = false;
    bool _promptEnabled = false;
    bool _retryAvailable = false;
    bool _hasUserMessage = false;
    bool _sessionActionsEnabled = false;
    QString _sessionDisabledReason;
    QDialog* _workbenchDialog = nullptr;
    QWebEngineView* _browserView = nullptr;
    QTabWidget* _workbenchTabs = nullptr;
    QHash<QString, QPlainTextEdit*> _workbenchViews;
    QHash<QString, QLineEdit*> _workbenchInputs;
    QJsonArray _sessionSnapshot;
    QStringList _workbenchEvents;
    bool _applyingTheme = false;
};

} // namespace cgplay
