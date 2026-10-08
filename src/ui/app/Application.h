#pragma once
// CGPlay Application.h — Phase 1: New main layout
#include <QApplication>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <vector>

#include "viewer/api/IActivePlaybackView.h"

class QMessageBox;

namespace cgplay {

class IPlaybackService;
class IMediaService;
class ISettingsService;
class ViewerWidget;
class MainWindow;
class SecondaryWindow;
class PluginManager;
class AIAgentWorkspace;
class ICommandProvider;
class IMenuProvider;
class IToolbarProvider;
class IPanelProvider;
class IAICredentialStore;
class IAIContextBuilder;
class IMediaFrameSnapshotService;
class IAIProviderDetector;
class IAIProviderManager;
class IAIWorkflowService;
class AnnotationManager;
class OcioManager;
class CacheManager;
class HardwareDecodeManager;
class AnnotationOverlayProvider;
struct MediaInfo;
struct GeneratedSubtitleCue;
struct TranslationEnhancementScheduleRequest;
struct UpdateInstallResult;

class Application : public QApplication, public IActivePlaybackView
{
    Q_OBJECT
    Q_INTERFACES(cgplay::IActivePlaybackView)
public:
    static inline const QString kWindowSettingsService = QStringLiteral("window");
    static inline const QString kUserSettingsService = QStringLiteral("user");

    Application(int& argc, char** argv);
    ~Application() override;
    int run();
    void openNewWindow(bool preferSecondaryScreen = false);
    void closeSecondaryWindow(int index);
    bool isAutomationMode() const;
    bool isBackgroundAutomationMode() const { return _backgroundAutomationMode; }
    bool rebuildMainViewerForRuntime();
    bool activateViewForRuntime(const QString& viewId);
    bool unloadPluginForRuntime(const QString& pluginId);
    bool reloadPluginForRuntime(const QString& pluginId);
    void setPluginFallbackEnabledForRuntime(bool enabled);
    bool pluginFallbackEnabled() const { return !_disablePluginFallback; }

    QString activeViewId() const override { return _activeViewId; }
    bool hasActiveView() const override { return !_activeViewId.isEmpty() && !_activeViewer.isNull(); }

    std::shared_ptr<AnnotationManager>   annotationManager()   const { return _annoMgr; }
    std::shared_ptr<OcioManager>         ocioManager()         const { return _ocioMgr; }
    std::shared_ptr<CacheManager>        cacheManager()        const { return _cacheMgr; }
    std::shared_ptr<HardwareDecodeManager> hwDecodeManager()   const { return _hwDecodeMgr; }
    std::shared_ptr<IMediaService>       mediaService()        const { return _mediaService; }
    PluginManager* pluginManager() const;
    void refreshAppearanceSettings();

Q_SIGNALS:
    void activeViewChanged(const QString& viewId);
    void activeViewInvalidated(const QString& viewId);

protected:
    bool notify(QObject* receiver, QEvent* event) override;

private:
    void _initStyle();
    void _parseArgs();
    void _initComponentManager();
    void _attachViewLifecycle(ViewerWidget* viewer);
    void _activateView(ViewerWidget* viewer);
    void _invalidateView(const QString& viewId);
    void _activateMainView();
    int _runPlaybackBenchmark();
    int _runUiCapture();
    int _runCodexWorkbenchSmoke();
    int _runPlayerSmokeTest();
    int _runSubtitleGenerationSmoke();
    int _runSubtitleCacheDisplaySmoke();
    int _runSubtitleSwitchSequenceSmoke();
    int _runSubtitleRefinedFallbackSmoke();
    int _runQwenAsrProviderSmoke();
    int _runRecoveryPromptSmoke();
    int _runComponentCheck();
    int _runRuntimeDump();
    int _runPhase915ViewerRebuild();
    int _runPhase915PluginReload();
    int _runPhase915FallbackToggle();
    int _runPhase11PerformanceBaseline();
    int _runPhase14PerformanceBaseline();
    int _runPhase14Stress();
    void _refreshAnnotationCapabilityRuntime();
    QJsonObject _captureRuntimeDump() const;
    bool _writeRuntimeDump(const QString& outputPath, const QJsonObject& dump) const;

    std::unique_ptr<MainWindow>          _mainWindow;
    std::shared_ptr<AnnotationManager>   _annoMgr;
    std::shared_ptr<OcioManager>         _ocioMgr;
    std::shared_ptr<CacheManager>        _cacheMgr;
    std::shared_ptr<HardwareDecodeManager> _hwDecodeMgr;
    std::shared_ptr<IMediaService>       _mediaService;
    std::shared_ptr<ISettingsService>    _windowSettings;
    std::shared_ptr<ISettingsService>    _userSettings;
    std::shared_ptr<IAICredentialStore>  _aiCredentialStore;
    std::shared_ptr<IAIContextBuilder>   _aiContextBuilder;
    std::shared_ptr<IMediaFrameSnapshotService> _aiFrameSnapshotService;
    std::shared_ptr<IAIProviderDetector> _aiProviderDetector;
    std::shared_ptr<IAIProviderManager>  _aiProviderManager;
    std::shared_ptr<IAIWorkflowService>  _aiWorkflowService;
    std::unique_ptr<PluginManager>       _pluginManager;
    std::unique_ptr<AnnotationOverlayProvider> _annotationFallbackOverlayProvider;
    QPointer<ViewerWidget> _activeViewer;
    QString _activeViewId;
    std::vector<SecondaryWindow*> _secondaryWindows;

    bool _benchmarkMode = false;
    QString _benchmarkMedia;
    QString _benchmarkOutput;
    int _benchmarkDurationMs = 12000;
    int _benchmarkWarmupMs = 3000;
    bool _captureUiMode = false;
    bool _captureUiDemo = false;
    QString _captureUiOutput;
    QString _captureUiMedia;
    int _captureUiDelayMs = 1200;
    int _captureUiWidth = 1500;
    int _captureUiHeight = 840;
    bool _codexWorkbenchSmokeMode = false;
    QString _codexWorkbenchSmokeOutput;
    bool _playerSmokeMode = false;
    QString _playerSmokeMedia;
    QString _playerSmokeOutput;
    bool _subtitleGenerationSmokeMode = false;
    bool _subtitleGenerationSmokeMock = false;
    QString _subtitleGenerationSmokeMedia;
    QString _subtitleGenerationSmokeOutput;
    int _subtitleGenerationSmokeFrame = 0;
    int _subtitleGenerationSmokeCancelAfterMs = 0;
    int _subtitleGenerationSmokePlaybackSampleDurationMs = 0;
    int _subtitleGenerationSmokePlaybackSampleIntervalMs = 1000;
    QString _subtitleGenerationSmokePlaybackMode;
    int _subtitleGenerationSmokeHighQualityWaitTimeoutMs = 0;
    double _subtitleGenerationSmokeHighQualityTargetSeconds = 0.0;
    bool _subtitleGenerationSmokeMockOnlineWorker = false;
    bool _subtitleGenerationSmokeMockOcrWorker = false;
    bool _subtitleGenerationSmokeMockRepairWorker = false;
    bool _subtitleCacheDisplaySmokeMode = false;
    QString _subtitleCacheDisplaySmokeMedia;
    QString _subtitleCacheDisplaySmokeOutput;
    QVector<int> _subtitleCacheDisplaySmokeFrames;
    bool _subtitleSwitchSequenceSmokeMode = false;
    QStringList _subtitleSwitchSequenceSmokeMedia;
    QString _subtitleSwitchSequenceSmokeOutput;
    QVector<int> _subtitleSwitchSequenceSmokeFrames;
    bool _subtitleRefinedFallbackSmokeMode = false;
    QString _subtitleRefinedFallbackSmokeMedia;
    QString _subtitleRefinedFallbackSmokeOutput;
    int _subtitleRefinedFallbackSmokeFrame = 0;
    bool _qwenAsrProviderSmokeMode = false;
    QString _qwenAsrProviderSmokeOutput;
    bool _recoveryPromptSmokeMode = false;
    QString _recoveryPromptSmokeOutput;
    bool _componentCheckMode = false;
    QString _componentCheckOutput;
    bool _dumpRuntimeMode = false;
    QString _dumpRuntimeOutput;
    QString _automationSettingsNamespace;
    bool _backgroundAutomationMode = false;
    bool _automationVisibleMode = false;
    bool _disablePluginFallback = false;
    bool _overlayDebugMode = false;
    bool _phase915ViewerRebuildMode = false;
    bool _phase915PluginReloadMode = false;
    bool _phase915FallbackToggleMode = false;
    bool _phase11PerformanceBaselineMode = false;
    bool _phase14PerformanceBaselineMode = false;
    bool _phase14StressMode = false;
    QString _phase915Media;
    QString _phase14Media;
    QString _phase915ViewerRebuildOutput;
    QString _phase915PluginReloadOutput;
    QString _phase915FallbackToggleOutput;
    QString _phase11PerformanceBaselineOutput;
    QString _phase14PerformanceBaselineOutput;
    QString _phase14StressOutput;
    int _phase14ViewerRebuildIterations = 1000;
    int _phase14WindowSwitchIterations = 1000;
    int _phase14PluginReloadIterations = 100;
    int _phase14FallbackToggleIterations = 100;
};

// ─── Forward declarations ─────────────────────────────────────────────────
class PlaylistPanel;
class IPlaybackService;
class CompareToolbar;
class AnnotationToolbar;
class ReviewPanel;
class ViewerWidget;
class TimelineWidget;
class TitleBar;
class TopBar;
class NavigationRail;
class PlaybackBar;

// ─── MainWindow ───────────────────────────────────────────────────────────
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(std::shared_ptr<AnnotationManager> annoMgr = nullptr,
                        std::shared_ptr<OcioManager>       ocio = nullptr,
                        std::shared_ptr<CacheManager>      cache = nullptr,
                        std::shared_ptr<IMediaService>     mediaService = nullptr,
                        std::shared_ptr<ISettingsService>  windowSettings = nullptr,
                        std::shared_ptr<ISettingsService>  userSettings = nullptr,
                        QWidget* parent = nullptr);
    ~MainWindow() override;

    void openFile(const QString& path);
    void prepareCaptureDemoState(const QString& preferredMedia = {});
    void checkVersionAndComponents(bool interactive);
    PlaylistPanel*      playlistPanel()     const;
    IPlaybackService*   playbackController() const;
    CompareToolbar*     compareToolbar()    const;
    ViewerWidget*       viewerWidget()      const;
    QMessageBox*        createRecoverySessionPromptForSmoke();
    QJsonObject captureRuntimeAnnotationState() const;
    QJsonObject runPlayerSmokeChecks(const QString& mediaPath);
    QJsonObject runSubtitleGenerationSmokeChecks(
        const QString& mediaPath,
        const QString& outputPath,
        bool mockWithoutApi,
        int frame,
        int cancelAfterMs = 0,
        int playbackSampleDurationMs = 0,
        int playbackSampleIntervalMs = 1000);
    QJsonObject runSubtitleCacheDisplaySmokeChecks(
        const QString& mediaPath,
        const QString& outputPath,
        const QVector<int>& frames);
    QJsonObject runSubtitleSwitchSequenceSmokeChecks(
        const QStringList& mediaPaths,
        const QString& outputPath,
        const QVector<int>& frames);
    QJsonObject runSubtitleRefinedFallbackSmokeChecks(
        const QString& mediaPath,
        int frame);
    QJsonObject runQwenAsrProviderSmokeChecks(const QString& screenshotPath = {});
    QJsonObject runPhase915ViewerRebuildChecks(const QString& mediaPath);
    QJsonObject runPhase915PluginReloadChecks(const QString& mediaPath);
    QJsonObject runPhase915FallbackToggleChecks(const QString& mediaPath);
    QJsonObject runPhase14PerformanceBaselineChecks(const QString& mediaPath);
    QJsonObject runPhase14StressChecks(
        const QString& mediaPath,
        int viewerRebuildIterations,
        int windowSwitchIterations,
        int pluginReloadIterations,
        int fallbackToggleIterations);
    bool rebuildViewerForRuntime();
    void refreshAnnotationCapabilityForRuntime();

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void _setupUI();
    void _setupAIAgentWorkspace();
    void _setupMenuBar();
    void _setupStatusBar();
    void _connectSignals();
    void _loadHostExtensionContributions();
    void _applyMenuContributions(QMenuBar* menuBar);
    void _applyToolbarContributions();
    void _rebuildCustomToolbar();
    void _refreshCommandPresentation();
    bool _executeCommandId(const QString& commandId, bool checked = false);
    void _applyPanelContributions();
    void _cleanupDuplicateReviewPanels();
    void _connectAnnotationToolbarSignals(AnnotationToolbar* toolbar);
    void _restoreState();
    void _restoreSplitterLayoutIfNeeded();
    void _saveState();
    void _autoLoadReview();
    void _onAnnotationModeToggled(bool active);
    void _closeCurrentMedia();
    void _openFileWithMediaInfo(
        const QString& path,
        double sequenceFpsOverride,
        const MediaInfo& mediaInfo);
    void _toggleFullScreen();
    QJsonArray _runFullscreenSmokeChecks();
    QJsonArray _runSettingsSmokeChecks();
    QJsonObject _runMediaOpenSmokeCheck(const QString& mediaPath);
    void _showFullScreenChromeTemporarily();
    void _hideFullScreenChrome();
    void _setFullScreenChromeVisible(bool visible, bool forceApply = false);
    void _setFullScreenCursorHidden(bool hidden);
    void _checkVersionAndComponents(bool interactive);
    void _generateSubtitlesForCurrentMedia(double startSeconds = -1.0, bool background = false);
    void _onPlaybackTranslationToggled(bool visible);
    void _setTranslationPlaybackMode(const QString& modeId);
    bool _shouldStartHighQualityPrePlaybackWait() const;
    void _startHighQualityPrePlaybackWait();
    void _updateHighQualityPrePlaybackWait(const QString& reason = {});
    void _finishHighQualityPrePlaybackWait(
        const QString& result,
        bool timedOut = false,
        bool canceled = false,
        bool degradedToQuick = false);
    TranslationEnhancementScheduleRequest _buildTranslationEnhancementRequest(
        bool manualRequested = false,
        bool executeWorkers = false,
        bool allowWhilePlayback = false) const;
    void _startCurrentMediaHighQualityEnhancement(
        const QString& reason = {},
        double preferredStartSeconds = -1.0);
    void _scheduleHighQualityVisualContinuation();
    QJsonObject _translationPlaybackStrategyDiagnostics(bool explicitlyRequested = false) const;
    QJsonObject _translationGuardReportDiagnostics(
        const QJsonObject& serviceDiagnostics = {},
        const QJsonObject& playbackSampling = {}) const;
    void _publishTranslationPlaybackStrategyDiagnostics(bool explicitlyRequested = false) const;
    void _setupGeneratedSubtitleOverlay();
    void _queueGeneratedSubtitleOverlayLayout();
    void _layoutGeneratedSubtitleOverlay();
    void _loadGeneratedSubtitleTrack();
    void _updateGeneratedSubtitleForFrame(int frame);
    void _scheduleGeneratedSubtitleContinuation(double preferredStartSeconds = -1.0, int delayMs = 0);
    void _scheduleGeneratedSubtitleRefinement(
        const QString& mediaPath,
        const QVector<GeneratedSubtitleCue>& cues,
        bool mockWithoutApi);
    void _showMissingComponentsPrompt(const QStringList& missing, bool interactive);
    void _checkForUpdates(bool interactive, bool refreshRemote = true);
    bool _downloadAndLaunchInstaller(const QString& targetVersion);
    void _offerDownloadedUpdate(UpdateInstallResult installer);
    void _scheduleBackgroundUpdateCheck();
    void _presentUpdateResult(const QJsonObject& result);
    void _applyBackgroundUpdateResult(const QJsonObject& result);
    void _setUpdateStatusBadge(const QString& text, const QString& color, const QString& toolTip = {});
    double _promptSequenceFps(const QString& path) const;
    void _connectReviewPanelSignals(ReviewPanel* panel);
    void _installSettingsWidgets(ReviewPanel* panel);
    void _restoreAuxDocksAfterShow();
    void _applyAdaptiveSidePanelLayout(bool force = false);
    void _layoutTranslationStrip();
    void _setTranslationStripVisible(bool visible, bool refreshRuntime = false);

    struct Private;
    std::unique_ptr<Private> _p;
    std::shared_ptr<AnnotationManager> _annoMgr;
    bool _annoMode = false;
};

} // namespace cgplay
