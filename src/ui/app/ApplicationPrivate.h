#pragma once

#include "Application.h"
#include "plugins/api/HostExtensionPoints.h"
#include "services/ai/SubtitleGenerationService.h"

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
    bool subtitleGenerationBusy = false;
    std::shared_ptr<std::atomic_bool> subtitleGenerationCancelRequested;
    QPointer<QFutureWatcher<SubtitleGenerationResult>> subtitleGenerationWatcher;
    bool subtitleRefinementBusy = false;
    std::shared_ptr<std::atomic_bool> subtitleRefinementCancelRequested;
    QPointer<QFutureWatcher<SubtitleRefinementResult>> subtitleRefinementWatcher;
    QString subtitleRefinementLastError;
    QJsonObject subtitleRefinementLastValidation;
    double subtitleRefinementLastCoverageEndSeconds = 0.0;
    bool subtitleContinuationAfterRefinementPending = false;
    double subtitleContinuationAfterRefinementStartSeconds = -1.0;
    QPointer<QProgressDialog> subtitleGenerationProgress;
    QTimer* generatedSubtitleRefreshTimer = nullptr;
    QVector<GeneratedSubtitleCue> generatedSubtitleCues;
    QString generatedSubtitlePath;
    QString generatedSubtitleMediaFingerprint;
    QString subtitleGenerationLastError;
    QString subtitleGenerationActiveTranslatedVttPath;
    QString translationPlaybackMode = QStringLiteral("quick-playback");
    bool translationPlaybackModeWaited = false;
    qint64 translationPlaybackModeWaitMs = 0;
    bool translationPrePlaybackWaitActive = false;
    bool translationPrePlaybackWaitCanceled = false;
    bool translationPrePlaybackWaitTimedOut = false;
    bool translationPrePlaybackWaitDegradedToQuick = false;
    bool translationPrePlaybackWaitPlaybackAlreadyRunning = false;
    QString translationPrePlaybackWaitResult;
    double translationPrePlaybackWaitTargetSeconds = 600.0;
    double translationPrePlaybackWaitCoverageAheadSeconds = 0.0;
    double translationPrePlaybackWaitMaxProcessedSeconds = 0.0;
    bool translationPrePlaybackWaitEnhancementStarted = false;
    qint64 translationPrePlaybackWaitStartMs = 0;
    qint64 translationPrePlaybackWaitTimeoutMs = 600000;
    QPointer<QProgressDialog> translationPrePlaybackWaitProgress;
    QTimer* translationPrePlaybackWaitTimer = nullptr;
    int generatedSubtitleQuickFallbackCueCount = 0;
    int generatedSubtitleRefinedMatchedCueCount = 0;
    int generatedSubtitleEnhancedMatchedCueCount = 0;
    bool generatedSubtitleEnhancedLoaded = false;
    QString generatedSubtitleEnhancedRejectedReason;
    bool generatedSubtitleCurrentCueFallbackSafe = true;
    bool generatedSubtitleRefinedMerged = false;
    bool generatedSubtitleEnhancedMerged = false;
    bool highQualityEnhancementBusy = false;
    QString highQualityEnhancementMediaPath;
    QString highQualityEnhancementLastReason;
    QJsonObject highQualityEnhancementLastDiagnostics;
    QPointer<QFutureWatcher<QJsonObject>> highQualityEnhancementWatcher;
    std::shared_ptr<std::atomic_bool> highQualityEnhancementCancelRequested;
    qint64 highQualityVisualPrefetchLastCheckMs = 0;
    double generatedSubtitleCoverageEndSeconds = 0.0;
    double generatedSubtitleProcessedEndSeconds = 0.0;
    QString generatedSubtitleLastText;
    double generatedSubtitleLastCueStartSeconds = -1.0;
    double generatedSubtitleLastCueEndSeconds = -1.0;
    double generatedSubtitleLastShownAtSeconds = -1.0;
    bool subtitleContinuationScheduled = false;
    double subtitleContinuationStartSeconds = -1.0;
    bool subtitleContinuationQueuedWhileBusy = false;
    double subtitleContinuationQueuedStartSeconds = -1.0;
    int subtitleContinuationRetryCount = 0;
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
    bool fullscreenLeft = true;
    bool fullscreenRight = true;
    bool fullscreenNav = true;
    bool fullscreenTopBar = true;
    bool fullscreenTimeline = true;
    bool fullscreenPlaybackBar = true;
    bool fullscreenStatusBar = true;
    bool fullscreenCompareBar = false;
    bool fullscreenViewerChrome = true;
    bool fullscreenAIDock = true;
    // Tracks the requested fullscreen mode during the native Qt transition.
    // Windows can update isFullScreen() one event later than the key press.
    bool fullscreenActive = false;
    quint64 fullscreenTransitionGeneration = 0;
    Qt::WindowStates fullscreenWindowState = Qt::WindowNoState;
    bool fullscreenRestorePending = false;
    // Do not present intermediate layouts or wake chrome during native entry.
    bool fullscreenEntryPending = false;
    bool fullscreenEntryUpdatesSuspended = false;
    bool fullscreenChromeVisible = false;
    quint64 fullscreenChromeApplyCount = 0;
    bool fullscreenCursorHidden = false;
    QList<int> fullscreenCenterSizes;
    int fullscreenTimelineHeight = 0;
    int fullscreenPlaybackBarHeight = 0;
    bool annoToolsVisible = true;
    bool auxDocksRestoredAfterShow = false;
    bool splitterLayoutRestoredAfterShow = false;
    bool windowStateRestored = false;
    bool generatedSubtitleLayoutQueued = false;
    QTimer* fullscreenChromeTimer = nullptr;
    QTimer* fullscreenMousePollTimer = nullptr;
    QPoint fullscreenLastCursorPos;
};

} // namespace cgplay
