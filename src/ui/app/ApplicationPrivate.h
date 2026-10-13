#pragma once

#include "Application.h"
#include "plugins/api/HostExtensionPoints.h"
#include "SubtitleSessionController.h"
#include "FullscreenController.h"

#include <QPointer>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <memory>

class QLabel;
class QAction;
class QDialog;
class QDockWidget;
class QProgressDialog;
class QScrollArea;
class QSplitter;
class QTimer;
class QVariantAnimation;
class QVBoxLayout;
class QWidget;

namespace cgplay {

class PerformanceService;
class CommandRegistry;
class CommandDispatcher;
class AIAgentWorkspace;
class IPlaybackService;
class IMediaService;
class ISettingsService;
class ISessionService;
class IAnnotationService;
class JobHandle;
struct UpdateTransferState;
struct UpdateInstallResult;
class InputBindingStore;
class SettingsProfileService;
class AppearanceController;
class WorkspaceController;
class ToolbarController;
struct CommandDescriptor;
struct MenuContribution;
struct ToolbarContribution;
struct PanelContribution;

struct MainWindow::Private
{
    std::shared_ptr<OcioManager> ocioManager;
    std::shared_ptr<CacheManager> cacheManager;
    std::shared_ptr<IPlaybackService> playbackCtrl;
    std::shared_ptr<IMediaService> mediaService;
    std::shared_ptr<ISettingsService> windowSettings;
    std::shared_ptr<ISettingsService> userSettings;
    std::unique_ptr<InputBindingStore> inputBindings;
    std::unique_ptr<SettingsProfileService> settingsProfileService;
    std::unique_ptr<AppearanceController> appearanceController;
    std::unique_ptr<WorkspaceController> workspaceController;
    std::unique_ptr<ToolbarController> toolbarController;
    QTimer* gamepadPollTimer = nullptr;
    QTimer* appearanceRefreshTimer = nullptr;
    unsigned short gamepadButtons = 0;
    IAnnotationService* annotationService = nullptr;

    NavigationRail* navRail = nullptr;
    TopBar* topBar = nullptr;
    QPointer<QDialog> settingsDialog;
    QPointer<QScrollArea> settingsDragScrollArea;
    QPointer<QWidget> settingsDragGrabWidget;
    bool settingsScrollDragging = false;
    int settingsScrollStartGlobalY = 0;
    int settingsScrollStartValue = 0;
    QPointer<ViewerWidget> viewer;
    TimelineWidget* timeline = nullptr;
    PlaylistPanel* playlist = nullptr;
    CompareToolbar* compareBar = nullptr;
    QPointer<AnnotationToolbar> annoToolbar;
    QPointer<ReviewPanel> reviewPanel;
    PlaybackBar* playbackBar = nullptr;
    QPointer<QDockWidget> aiDock;
    QPointer<AIAgentWorkspace> aiWorkspace;
    QAction* renderExportAction = nullptr;
    QAction* videoExportAction = nullptr;
    QAction* workspaceSaveAction = nullptr;
    QAction* workspaceResetAction = nullptr;
    QPointer<QLabel> generatedSubtitleLabel;
    QAction* translationToggleAction = nullptr;
    bool translationStripVisible = false;
    SubtitleSessionController subtitleSession;
    QPointer<QProgressDialog> subtitleGenerationProgress;
    QTimer* generatedSubtitleRefreshTimer = nullptr;
    QString translationPlaybackMode = QStringLiteral("quick-playback");
    QPointer<QProgressDialog> translationPrePlaybackWaitProgress;
    QTimer* translationPrePlaybackWaitTimer = nullptr;
    QWidget* viewerShell = nullptr;
    QVBoxLayout* viewerShellLayout = nullptr;

    QSplitter* horzSplitter = nullptr;
    QSplitter* leftSplitter = nullptr;
    QSplitter* centerSplitter = nullptr;

    ISessionService* sessionMgr = nullptr;

    QLabel* lblCodec = nullptr;
    QLabel* lblRes = nullptr;
    QLabel* lblBitrate = nullptr;
    QLabel* lblFPS = nullptr;
    QLabel* lblDecoder = nullptr;
    QLabel* lblDropped = nullptr;
    QLabel* lblCache = nullptr;
    QLabel* lblCPU = nullptr;
    QLabel* lblGPU = nullptr;
    QLabel* lblMem = nullptr;
    QLabel* lblOcio = nullptr;
    QLabel* lblView = nullptr;
    QLabel* lblUpdate = nullptr;
    PerformanceService* performanceService = nullptr;
    QVector<CommandDescriptor> commandDescriptors;
    std::unique_ptr<CommandRegistry> commandRegistry;
    std::unique_ptr<CommandDispatcher> commandDispatcher;
    QHash<QString, QString> commandDefaultShortcuts;
    QVector<MenuContribution> menuContributions;
    QVector<ToolbarContribution> toolbarContributions;
    QVector<PanelContribution> panelContributions;

    QString currentPath;
    QString currentMediaFingerprint;
    quint64 mediaGenerationId = 0;
    quint64 mediaProbeGeneration = 0;
    QPointer<JobHandle> mediaProbeJob;
    bool autoPlayPending = false;
    QString remoteUpdateVersion;
    bool updateAvailable = false;
    bool updateDownloadInProgress = false;
    bool updateCheckInProgress = false;
    bool updateCheckInteractive = false;
    bool updateClosing = false;
    QJsonObject latestUpdateResult;
    std::shared_ptr<UpdateTransferState> updateCheckState;
    std::shared_ptr<UpdateTransferState> updateDownloadState;
    QString verifiedUpdateVersion;
    std::shared_ptr<UpdateInstallResult> verifiedUpdateInstaller;
    bool leftVisible = true;
    bool rightVisible = true;
    int lastLeftPanelWidth = 160;
    int lastRightPanelWidth = 160;
    QAction* leftPanelAction = nullptr;
    QAction* rightPanelAction = nullptr;
    int activeSidePanel = 2;
    int sidePanelLayoutMode = -1;
    bool sidePanelLayoutQueued = false;
    bool sidePanelDragging = false;
    bool sidePanelLayoutApplying = false;
    int sidePanelDragBoundary = 0;
    int sidePanelDragStartGlobalX = 0;
    QList<int> sidePanelDragStartSizes;
    QPointer<QWidget> sidePanelCursorWidget;
    bool sidePanelDragOverrideCursor = false;
    std::unique_ptr<FullscreenController> fullscreen;
    bool annoToolsVisible = true;
    bool auxDocksRestoredAfterShow = false;
    bool splitterLayoutRestoredAfterShow = false;
    bool windowStateRestored = false;
    bool generatedSubtitleLayoutQueued = false;
};

} // namespace cgplay
