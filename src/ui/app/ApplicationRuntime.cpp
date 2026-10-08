#include "Application.h"
#include "ApplicationRuntimeSupport.h"

#include "SecondaryWindow.h"

#include "common/core/OverlayRuntimeDebug.h"
#include "common/core/ServiceLocator.h"
#include "common/events/EventBus.h"
#include "common/events/api/IEventBus.h"
#include "common/events/api/EventTypes.h"
#include "common/theme/ThemeService.h"
#include "ai/AIContextBuilder.h"
#include "ai/AIProviderManager.h"
#include "ai/AIWorkflowService.h"
#include "ai/MediaFrameSnapshotService.h"
#include "ai/OpenAIResponsesProvider.h"
#include "ai/SubtitleGenerationService.h"
#include "ai/TranslationPlaybackStrategy.h"
#include "ai/WindowsDpapiCredentialStore.h"
#include "ai/SubtitleCredentialMigration.h"
#include "ai/discovery/AIProviderDetector.h"
#include "plugins/PluginManager.h"
#include "plugins/annotation/AnnotationPlugin.h"
#include "plugins/ocio/OcioPlugin.h"
#include "plugins/quicklook/QuickLookPlugin.h"
#include "annotation/AnnotationManager.h"
#include "annotation/AnnotationOverlayProvider.h"
#include "annotation/api/IAnnotationService.h"
#include "cache/CacheManager.h"
#include "component/ComponentManager.h"
#include "core/session/api/ISessionContributor.h"
#include "hwdecode/HardwareDecodeManager.h"
#include "media/MediaProbe.h"
#include "media/MediaService.h"
#include "ocio/OcioManager.h"
#include "playback/PlaybackController.h"
#include "playback/api/IPlaybackService.h"
#include "playback/PlaybackStats.h"
#include "settings/api/ISettingsService.h"
#include "settings/SettingsService.h"
#include "viewer/TlViewport.h"
#include "viewer/ViewerWidget.h"
#include "features/annotation/api/IAnnotationViewBridge.h"
#include "viewer/api/IActivePlaybackView.h"
#include "viewer/api/IOverlayHost.h"
#include "viewer/api/IOverlayProvider.h"
#include "viewer/api/IViewerCoordinateMapper.h"

#include <QAction>
#include <QAbstractButton>
#include <QComboBox>
#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QDockWidget>
#include <QStatusBar>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QImage>
#include <QIcon>
#include <QLineEdit>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QScreen>
#include <QStandardPaths>
#include <QSet>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QGraphicsDropShadowEffect>
#include <QVector>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <cstdlib>
#include <cstdio>

namespace cgplay {

using namespace application_runtime;

namespace {

void logCapabilityEvent(
    const QString& action,
    const QString& capability,
    const QString& owner,
    const QString& viewId = {},
    const QString& pluginId = {})
{
    QJsonObject details{
        { QStringLiteral("action"), action },
        { QStringLiteral("capability"), capability },
        { QStringLiteral("owner"), owner }
    };
    if (!viewId.isEmpty()) {
        details.insert(QStringLiteral("viewId"), viewId);
    }
    if (!pluginId.isEmpty()) {
        details.insert(QStringLiteral("pluginId"), pluginId);
    }
    runtimeCapabilityLog(
        QStringLiteral("ApplicationRuntime"),
        QStringLiteral("capability.%1").arg(action),
        details);
}

void logRuntimeEventPublished(const QString& eventName, const QString& viewId, const QString& reason = {})
{
    QJsonObject details{
        { QStringLiteral("eventName"), eventName },
        { QStringLiteral("viewId"), viewId }
    };
    if (!reason.isEmpty()) {
        details.insert(QStringLiteral("reason"), reason);
    }
    runtimeLifecycleLog(
        QStringLiteral("ApplicationRuntime"),
        QStringLiteral("event.publish"),
        details);
}

} // namespace

Application::Application(int& argc, char** argv)
    : QApplication(argc, argv)
{
    QElapsedTimer startupTimer;
    startupTimer.start();
    qint64 previousStartupPhaseMs = 0;
    const auto recordStartupPhase = [&](const char* stage) {
        if (!_overlayDebugMode) return;
        const qint64 elapsedMs = startupTimer.elapsed();
        std::fprintf(stderr, "[StartupTiming] {\"scope\":\"Application\",\"stage\":\"%s\",\"elapsedMs\":%lld,\"phaseMs\":%lld}\n",
            stage, static_cast<long long>(elapsedMs), static_cast<long long>(elapsedMs - previousStartupPhaseMs));
        std::fflush(stderr);
        previousStartupPhaseMs = elapsedMs;
    };
    setApplicationName("CGPlay");
    setApplicationVersion("1.0.7.11");
    setOrganizationName("CGPlay");
    _initStyle();
    _parseArgs();
    if (isAutomationMode() && !_automationVisibleMode) {
        _backgroundAutomationMode = true;
    }
    setProperty("cgplay.automationBackground", _backgroundAutomationMode);
    QString settingsApplication = QStringLiteral("CGPlay");
    QString windowSettingsApplication = QStringLiteral("CGPlay2");
    if (!_automationSettingsNamespace.isEmpty() && isAutomationMode()) {
        settingsApplication = QStringLiteral("CGPlayAutomation_%1").arg(_automationSettingsNamespace);
        windowSettingsApplication = settingsApplication + QStringLiteral("Window");
        setApplicationName(settingsApplication);
    }
    _initComponentManager();
    recordStartupPhase("bootstrap");
    if (_benchmarkMode) {
        setProperty("cgplay.benchmarkMode", true);
    }
    if (_captureUiMode) {
        setProperty("cgplay.captureUiMode", true);
    }
    if (_dumpRuntimeMode) {
        setProperty("cgplay.runtimeDumpMode", true);
        setProperty("cgplay.benchmarkMode", true);
    }
    if (_playerSmokeMode) {
        setProperty("cgplay.playerSmokeMode", true);
        setProperty("cgplay.benchmarkMode", true);
    }
    if (_subtitleGenerationSmokeMode) {
        setProperty("cgplay.playerSmokeMode", true);
        setProperty("cgplay.benchmarkMode", true);
        setProperty("cgplay.subtitleGenerationSmokeMode", true);
    }
    if (_subtitleCacheDisplaySmokeMode) {
        setProperty("cgplay.playerSmokeMode", true);
        setProperty("cgplay.benchmarkMode", true);
        setProperty("cgplay.subtitleCacheDisplaySmokeMode", true);
    }
    if (_subtitleSwitchSequenceSmokeMode) {
        setProperty("cgplay.playerSmokeMode", true);
        setProperty("cgplay.benchmarkMode", true);
        setProperty("cgplay.subtitleSwitchSequenceSmokeMode", true);
    }
    if (_subtitleRefinedFallbackSmokeMode) {
        setProperty("cgplay.playerSmokeMode", true);
        setProperty("cgplay.benchmarkMode", true);
        setProperty("cgplay.subtitleRefinedFallbackSmokeMode", true);
    }
    if (_recoveryPromptSmokeMode) {
        setProperty("cgplay.benchmarkMode", true);
        setProperty("cgplay.recoveryPromptSmokeMode", true);
    }
    if (_qwenAsrProviderSmokeMode) {
        setProperty("cgplay.qwenAsrProviderSmokeMode", true);
    }
    if (_componentCheckMode) {
        setProperty("cgplay.componentCheckMode", true);
    }
    if (_disablePluginFallback) {
        setProperty("cgplay.disablePluginFallback", true);
    }
    if (_overlayDebugMode) {
        setProperty("cgplay.overlayDebug", true);
    }
    if (_phase915ViewerRebuildMode || _phase915PluginReloadMode || _phase915FallbackToggleMode) {
        setProperty("cgplay.phase915Mode", true);
        setProperty("cgplay.playerSmokeMode", true);
        setProperty("cgplay.benchmarkMode", true);
    }
    if (_phase11PerformanceBaselineMode) {
        setProperty("cgplay.phase11PerformanceBaselineMode", true);
        setProperty("cgplay.playerSmokeMode", true);
        setProperty("cgplay.benchmarkMode", true);
    }
    if (_phase14PerformanceBaselineMode || _phase14StressMode) {
        setProperty("cgplay.phase14Mode", true);
        setProperty("cgplay.playerSmokeMode", true);
        setProperty("cgplay.benchmarkMode", true);
    }
    _ocioMgr = std::make_shared<OcioManager>();
    _ocioMgr->setEnabled(false);
    _cacheMgr = std::make_shared<CacheManager>();
    _annoMgr = std::make_shared<AnnotationManager>();
    _mediaService = std::make_shared<MediaService>();
    _windowSettings = std::make_shared<SettingsService>(QStringLiteral("CGPlay"), windowSettingsApplication);
    _userSettings = std::make_shared<SettingsService>(QStringLiteral("CGPlay"), settingsApplication);
    refreshAppearanceSettings();
    _hwDecodeMgr = std::make_shared<HardwareDecodeManager>();
    _hwDecodeMgr->probe();
    auto* eventBus = new EventBus(this);
    _pluginManager = std::make_unique<PluginManager>(this);
    ServiceLocator::registerService<AnnotationManager>(_annoMgr);
    if (!_disablePluginFallback) {
        // TODO(Phase13-remove): Transitional fallback service registration for annotation capability migration.
        ServiceLocator::registerService<IAnnotationService>(static_cast<IAnnotationService*>(_annoMgr.get()));
        ServiceLocator::registerService<ISessionContributor>(static_cast<ISessionContributor*>(_annoMgr.get()));
    }
    ServiceLocator::registerService<OcioManager>(_ocioMgr);
    ServiceLocator::registerService<CacheManager>(_cacheMgr);
    ServiceLocator::registerService<HardwareDecodeManager>(_hwDecodeMgr);
    ServiceLocator::registerService<IEventBus>(eventBus);
    ServiceLocator::registerService<IMediaService>(_mediaService);
    ServiceLocator::registerService<ISettingsService>(_windowSettings, Application::kWindowSettingsService);
    ServiceLocator::registerService<ISettingsService>(_userSettings, Application::kUserSettingsService);
    _aiCredentialStore = std::make_shared<WindowsDpapiCredentialStore>();
    const SubtitleCredentialMigrationResult subtitleCredentialMigration =
        migrateLegacySubtitleCredentials(_userSettings.get(), _aiCredentialStore.get());
    if (!subtitleCredentialMigration.success) {
        qWarning() << "[Security] Subtitle credential migration failed:"
                   << subtitleCredentialMigration.errorMessage;
    }
    _aiContextBuilder = std::make_shared<AIContextBuilder>();
    _aiFrameSnapshotService = std::make_shared<MediaFrameSnapshotService>();
    _aiProviderManager = std::make_shared<AIProviderManager>(eventBus);
    _aiWorkflowService = std::make_shared<AIWorkflowService>(
        _aiProviderManager.get(),
        _aiContextBuilder.get(),
        _aiFrameSnapshotService.get(),
        eventBus);
    _aiProviderDetector = std::make_shared<AIProviderDetector>(
        _aiCredentialStore.get(),
        _userSettings.get(),
        eventBus);
    _aiProviderManager->registerProvider(
        std::make_shared<OpenAIResponsesProvider>(_aiCredentialStore.get(), _userSettings.get()));
    const QString preferredAIProviderId =
        _userSettings->value(QStringLiteral("ai/workspace/providerId")).toString().trimmed();
    if (!preferredAIProviderId.isEmpty()) {
        _aiProviderManager->setDefaultProviderId(preferredAIProviderId);
    }
    ServiceLocator::registerService<IAICredentialStore>(_aiCredentialStore);
    ServiceLocator::registerService<IAIContextBuilder>(_aiContextBuilder);
    ServiceLocator::registerService<IMediaFrameSnapshotService>(_aiFrameSnapshotService);
    ServiceLocator::registerService<IAIProviderDetector>(_aiProviderDetector);
    ServiceLocator::registerService<IAIProviderManager>(_aiProviderManager);
    ServiceLocator::registerService<IAIWorkflowService>(_aiWorkflowService);
    ServiceLocator::registerService<PluginManager>(_pluginManager.get());
    ServiceLocator::registerService<IActivePlaybackView>(static_cast<IActivePlaybackView*>(this));
    recordStartupPhase("services");
    const QString pluginsDir = QDir(applicationDirPath()).filePath(QStringLiteral("plugins"));
    const QStringList loadedPluginIds = _pluginManager->loadPluginsFromDirectory(pluginsDir);
    if (!loadedPluginIds.isEmpty()) {
        qInfo() << "[Plugins] Loaded dynamic plugins:" << loadedPluginIds;
    }

    if (!_disablePluginFallback) {
        if (!_pluginManager->hasPlugin(QStringLiteral("annotation"))) {
            _pluginManager->registerPlugin(std::make_unique<AnnotationPlugin>());
        }
        if (!_pluginManager->hasPlugin(QStringLiteral("ocio"))) {
            _pluginManager->registerPlugin(std::make_unique<OcioPlugin>());
        }
        if (!_pluginManager->hasPlugin(QStringLiteral("quicklook"))) {
            _pluginManager->registerPlugin(std::make_unique<QuickLookPlugin>());
        }
    }
    _pluginManager->initializePlugins();
    recordStartupPhase("plugins");
    _mainWindow = std::make_unique<MainWindow>();
    recordStartupPhase("mainWindow");
    refreshAppearanceSettings();
    recordStartupPhase("appearance");
    QCoreApplication::instance()->setProperty(
        "cgplay.codex.playbackService",
        QVariant::fromValue<qulonglong>(reinterpret_cast<qulonglong>(_mainWindow->playbackController())));

    // The capture path must match the normal user layout: the Codex workspace
    // is a right-side dock in the reference UI.  Keep it disabled only for
    // non-visual benchmark/dump modes where it can affect timing or teardown.
    // (The legacy condition `_captureUiMode || _dumpRuntimeMode` was too broad:
    // capture UI is visual and must retain the right-side Codex dock.)
    const bool suppressCodexWorkspace = property("cgplay.benchmarkMode").toBool() ||
        _dumpRuntimeMode || _componentCheckMode || _qwenAsrProviderSmokeMode;
    if (!suppressCodexWorkspace) {
        if (auto* codexPlugin = _pluginManager->pluginObject(QStringLiteral("codex"))) {
        QWidget* codexWorkspace = nullptr;
        const bool eagerCodexWorkspace = _captureUiMode || _codexWorkbenchSmokeMode;
        const bool workspaceResolved = !eagerCodexWorkspace || QMetaObject::invokeMethod(
            codexPlugin,
            "workspaceWidget",
            Qt::DirectConnection,
            Q_RETURN_ARG(QWidget*, codexWorkspace));
        if (eagerCodexWorkspace && (!workspaceResolved || !codexWorkspace)) {
            qWarning() << "[Codex] Dynamic plugin did not provide a workspace widget";
        }
        if (!eagerCodexWorkspace || codexWorkspace) {
            auto* codexDock = new QDockWidget(QStringLiteral("Codex 工作台"), _mainWindow.get());
            codexDock->setObjectName(QStringLiteral("CodexAgentWorkspaceDock"));
            codexDock->setAllowedAreas(Qt::RightDockWidgetArea);
            codexDock->setFeatures(QDockWidget::DockWidgetClosable);
            // Keep the released default layout: the Codex workbench has the
            // same 460px right rail as the reference workspace. A narrower
            // override changes the player geometry and hides composer controls.
            codexDock->setMinimumWidth(460);
            codexDock->setMaximumWidth(520);
            codexDock->setBaseSize(460, 0);
            codexDock->setStyleSheet(
                "QDockWidget{background:#171B20;color:#D8DEE7;border-left:1px solid rgba(255,255,255,0.04);}"
                "QDockWidget::title{background:#111418;padding:10px 12px;color:#D8DEE7;font-weight:600;"
                "border-bottom:1px solid rgba(255,255,255,0.05);}");
            auto* codexDockTitleBar = new QWidget(codexDock);
            codexDockTitleBar->setFixedHeight(0);
            codexDock->setTitleBarWidget(codexDockTitleBar);
            if (codexWorkspace) {
                codexDock->setWidget(codexWorkspace);
                QObject::connect(codexWorkspace, SIGNAL(closeRequested()), codexDock, SLOT(hide()));
            } else {
                auto* placeholder = new QWidget(codexDock);
                auto* placeholderLayout = new QVBoxLayout(placeholder);
                placeholderLayout->setContentsMargins(24, 24, 24, 24);
                placeholderLayout->addStretch();
                auto* placeholderLabel = new QLabel(QStringLiteral("Codex 工作台按需加载"), placeholder);
                placeholderLabel->setAlignment(Qt::AlignCenter);
                auto* loadButton = new QPushButton(QStringLiteral("打开 Codex"), placeholder);
                loadButton->setObjectName(QStringLiteral("CodexLazyLoadButton"));
                loadButton->setMinimumHeight(36);
                placeholderLayout->addWidget(placeholderLabel);
                placeholderLayout->addWidget(loadButton);
                placeholderLayout->addStretch();
                codexDock->setWidget(placeholder);
                 connect(loadButton, &QPushButton::clicked, codexDock,
                     [codexPlugin, codexDock, placeholder, placeholderLabel, loadButton] {
                        loadButton->setEnabled(false);
                        placeholderLabel->setText(QStringLiteral("正在加载 Codex..."));
                        QTimer::singleShot(0, codexDock,
                            [codexPlugin, codexDock, placeholder, placeholderLabel, loadButton] {
                                QWidget* workspace = nullptr;
                                const bool resolved = QMetaObject::invokeMethod(
                                    codexPlugin,
                                    "workspaceWidget",
                                    Qt::DirectConnection,
                                    Q_RETURN_ARG(QWidget*, workspace));
                                if (!resolved || !workspace) {
                                    placeholderLabel->setText(QStringLiteral("Codex 加载失败"));
                                    loadButton->setEnabled(true);
                                    return;
                                }
                                codexDock->setWidget(workspace);
                                QObject::connect(workspace, SIGNAL(closeRequested()), codexDock, SLOT(hide()));
                                 placeholder->deleteLater();
                             });
                     });
                // Keep workspace construction off the startup stack, but do not
                // leave the user at a manual "Open Codex" gate on every launch.
                // The queued click preserves the fast first paint and replaces
                // the placeholder as soon as the event loop is idle.
                QTimer::singleShot(0, loadButton, [loadButton] {
                    if (loadButton->isEnabled()) loadButton->click();
                });
             }

            auto* aiDock = _mainWindow->findChild<QDockWidget*>(QStringLiteral("AIAgentWorkspaceDock"));
            if (aiDock) {
                aiDock->hide();
                if (_windowSettings) {
                    _windowSettings->setValue(QStringLiteral("ai/workspaceVisible"), false);
                }
            }
            _mainWindow->addDockWidget(Qt::RightDockWidgetArea, codexDock);
            if (aiDock) {
                _mainWindow->tabifyDockWidget(aiDock, codexDock);
                connect(aiDock, &QDockWidget::visibilityChanged, _mainWindow.get(),
                    [mainWindow = _mainWindow.get(), aiDock, codexDock](bool visible) {
                        if (!visible) return;
                        mainWindow->tabifyDockWidget(codexDock, aiDock);
                        aiDock->raise();
                    });
            }
            _mainWindow->resizeDocks({codexDock}, {460}, Qt::Horizontal);

            // Codex opens on every launch; a user-hidden dock applies only to the current session.
            codexDock->setVisible(true);
            codexDock->raise();
            if (_windowSettings) {
                _windowSettings->setValue(QStringLiteral("codex/workspaceVisible"), true);
            }
            connect(codexDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
                if (_windowSettings && visible && !QCoreApplication::closingDown()) {
                    _windowSettings->setValue(QStringLiteral("codex/workspaceVisible"), true);
                }
            });

            QAction* codexWorkspaceAction = codexDock->toggleViewAction();
            codexWorkspaceAction->setObjectName(QStringLiteral("CodexWorkspaceViewAction"));
            codexWorkspaceAction->setText(QStringLiteral("Codex 工作台"));

            const auto findTopLevelMenu = [this](const QString& title) -> QMenu* {
                if (!_mainWindow || !_mainWindow->menuBar()) {
                    return nullptr;
                }
                for (QAction* menuAction : _mainWindow->menuBar()->actions()) {
                    QMenu* menu = menuAction ? menuAction->menu() : nullptr;
                    if (menu && menu->title() == title) {
                        return menu;
                    }
                }
                return nullptr;
            };
            const auto addDockToggleAction = [codexWorkspaceAction](QMenu* menu) {
                if (!menu || menu->actions().contains(codexWorkspaceAction)) {
                    return;
                }
                if (!menu->actions().isEmpty() && !menu->actions().constLast()->isSeparator()) {
                    menu->addSeparator();
                }
                menu->addAction(codexWorkspaceAction);
            };
            addDockToggleAction(findTopLevelMenu(QStringLiteral("视图")));
            addDockToggleAction(findTopLevelMenu(QStringLiteral("窗口")));
            connect(codexWorkspaceAction, &QAction::triggered, codexDock, [codexDock](bool checked) {
                if (!checked) {
                    return;
                }
                QTimer::singleShot(0, codexDock, [codexDock] {
                    codexDock->show();
                    codexDock->raise();
                });
            });
        }
        }
    }

    recordStartupPhase("workspaces");
    _activateMainView();
    _refreshAnnotationCapabilityRuntime();
    recordStartupPhase("activateView");
    runtimeLifecycleLog(
        QStringLiteral("ApplicationRuntime"),
        QStringLiteral("startup"),
        QJsonObject{
            { QStringLiteral("overlayDebugEnabled"), _overlayDebugMode },
            { QStringLiteral("fallbackEnabled"), !_disablePluginFallback }
        });
}

Application::~Application()
{
    for (auto* sw : _secondaryWindows) {
        delete sw;
    }
    _secondaryWindows.clear();
}

bool Application::isAutomationMode() const
{
    return _backgroundAutomationMode ||
        _captureUiMode ||
        _codexWorkbenchSmokeMode ||
        _playerSmokeMode ||
        _subtitleGenerationSmokeMode ||
        _subtitleCacheDisplaySmokeMode ||
        _subtitleSwitchSequenceSmokeMode ||
        _subtitleRefinedFallbackSmokeMode ||
        _qwenAsrProviderSmokeMode ||
        _recoveryPromptSmokeMode ||
        _dumpRuntimeMode ||
        _benchmarkMode ||
        _componentCheckMode ||
        _phase915ViewerRebuildMode ||
        _phase915PluginReloadMode ||
        _phase915FallbackToggleMode ||
        _phase11PerformanceBaselineMode ||
        _phase14PerformanceBaselineMode ||
        _phase14StressMode;
}

bool Application::notify(QObject* receiver, QEvent* event)
{
    if (_backgroundAutomationMode && event && event->type() == QEvent::Show) {
        if (auto* widget = qobject_cast<QWidget*>(receiver); widget && widget->isWindow()) {
            widget->setAttribute(Qt::WA_DontShowOnScreen, true);
            widget->setAttribute(Qt::WA_ShowWithoutActivating, true);
        }
    }
    return QApplication::notify(receiver, event);
}

PluginManager* Application::pluginManager() const
{
    return _pluginManager.get();
}

int Application::run()
{
    if (_benchmarkMode) {
        return _runPlaybackBenchmark();
    }
    if (_captureUiMode) {
        return _runUiCapture();
    }
    if (_codexWorkbenchSmokeMode) {
        return _runCodexWorkbenchSmoke();
    }
    if (_playerSmokeMode) {
        return _runPlayerSmokeTest();
    }
    if (_subtitleGenerationSmokeMode) {
        return _runSubtitleGenerationSmoke();
    }
    if (_subtitleCacheDisplaySmokeMode) {
        return _runSubtitleCacheDisplaySmoke();
    }
    if (_subtitleSwitchSequenceSmokeMode) {
        return _runSubtitleSwitchSequenceSmoke();
    }
    if (_subtitleRefinedFallbackSmokeMode) {
        return _runSubtitleRefinedFallbackSmoke();
    }
    if (_qwenAsrProviderSmokeMode) {
        return _runQwenAsrProviderSmoke();
    }
    if (_recoveryPromptSmokeMode) {
        return _runRecoveryPromptSmoke();
    }
    if (_componentCheckMode) {
        return _runComponentCheck();
    }
    if (_dumpRuntimeMode) {
        return _runRuntimeDump();
    }
    if (_phase915ViewerRebuildMode) {
        return _runPhase915ViewerRebuild();
    }
    if (_phase915PluginReloadMode) {
        return _runPhase915PluginReload();
    }
    if (_phase915FallbackToggleMode) {
        return _runPhase915FallbackToggle();
    }
    if (_phase11PerformanceBaselineMode) {
        return _runPhase11PerformanceBaseline();
    }
    if (_phase14PerformanceBaselineMode) {
        return _runPhase14PerformanceBaseline();
    }
    if (_phase14StressMode) {
        return _runPhase14Stress();
    }

    _mainWindow->show();
    QTimer::singleShot(0, this, [this] {
        int visiblePlatformWindows = 0;
        int hiddenAutomationWindows = 0;
        for (QWidget* widget : topLevelWidgets()) {
            if (!widget || !widget->isVisible()) continue;
            if (widget->testAttribute(Qt::WA_DontShowOnScreen)) {
                ++hiddenAutomationWindows;
            } else {
                ++visiblePlatformWindows;
            }
        }
        runtimeLifecycleLog(
            QStringLiteral("ApplicationRuntime"),
            QStringLiteral("main.ready"),
            QJsonObject{
                {QStringLiteral("background"), _backgroundAutomationMode},
                {QStringLiteral("settingsConstructedAtStartup"),
                    _mainWindow && _mainWindow->findChild<QWidget*>(QStringLiteral("CGPlaySettingsDialog")) != nullptr},
                {QStringLiteral("visiblePlatformWindows"), visiblePlatformWindows},
                {QStringLiteral("hiddenTopLevelWindows"), hiddenAutomationWindows}
            });
    });
    const QStringList args = arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QFileInfo fi(args[i]);
        if (fi.exists()) {
            _mainWindow->openFile(args[i]);
            break;
        }
    }
    return exec();
}

void Application::_initComponentManager()
{
    ComponentManager& manager = ComponentManager::instance();
    const QString envManifest = QString::fromLocal8Bit(qgetenv("CGPLAY_MANIFEST_URL")).trimmed();
    if (!envManifest.isEmpty()) {
        manager.setManifestUrl(envManifest);
    }
}

void Application::openNewWindow(bool preferSecondaryScreen)
{
    const int idx = static_cast<int>(_secondaryWindows.size());
    auto* sw = new SecondaryWindow(_ocioMgr, _cacheMgr, idx);
    _attachViewLifecycle(sw->viewerWidget());
    connect(sw, &SecondaryWindow::closed, this, &Application::closeSecondaryWindow);
    if (preferSecondaryScreen) {
        const QList<QScreen*> screens = QGuiApplication::screens();
        QScreen* current = _mainWindow ? _mainWindow->screen() : QGuiApplication::primaryScreen();
        QScreen* target = nullptr;
        for (QScreen* screen : screens) {
            if (screen && screen != current) { target = screen; break; }
        }
        if (target) {
            sw->setGeometry(target->availableGeometry());
            sw->showMaximized();
        } else {
            sw->show();
        }
    } else {
        sw->show();
    }
    _secondaryWindows.push_back(sw);
}

void Application::closeSecondaryWindow(int index)
{
    if (index < 0 || index >= static_cast<int>(_secondaryWindows.size())) {
        return;
    }
    auto* sw = _secondaryWindows[index];
    const bool wasActive = sw && sw->viewerWidget() && sw->viewerWidget()->viewId() == _activeViewId;
    if (sw && sw->viewerWidget()) {
        _invalidateView(sw->viewerWidget()->viewId());
    }
    _secondaryWindows.erase(_secondaryWindows.begin() + index);
    for (int i = index; i < static_cast<int>(_secondaryWindows.size()); ++i) {
        _secondaryWindows[i]->_index = i;
    }
    if (wasActive && _mainWindow && _mainWindow->viewerWidget()) {
        _activateView(_mainWindow->viewerWidget());
    }
    sw->deleteLater();
}

bool Application::rebuildMainViewerForRuntime()
{
    if (!_mainWindow || !_mainWindow->rebuildViewerForRuntime()) {
        return false;
    }
    _attachViewLifecycle(_mainWindow->viewerWidget());
    return _mainWindow->viewerWidget() != nullptr;
}

bool Application::activateViewForRuntime(const QString& viewId)
{
    if (viewId.isEmpty()) {
        return false;
    }
    if (_mainWindow && _mainWindow->viewerWidget() && _mainWindow->viewerWidget()->viewId() == viewId) {
        _activateView(_mainWindow->viewerWidget());
        return _activeViewId == viewId;
    }
    for (auto* sw : _secondaryWindows) {
        if (sw && sw->viewerWidget() && sw->viewerWidget()->viewId() == viewId) {
            _activateView(sw->viewerWidget());
            return _activeViewId == viewId;
        }
    }
    return false;
}

bool Application::unloadPluginForRuntime(const QString& pluginId)
{
    if (!_pluginManager || pluginId.isEmpty()) {
        return false;
    }
    const bool unloaded = _pluginManager->unloadPlugin(pluginId);
    if (!unloaded) {
        return false;
    }
    _refreshAnnotationCapabilityRuntime();
    return true;
}

bool Application::reloadPluginForRuntime(const QString& pluginId)
{
    if (!_pluginManager || pluginId.isEmpty()) {
        return false;
    }
    const bool reloaded = _pluginManager->reloadPlugin(pluginId);
    if (!reloaded) {
        return false;
    }
    _refreshAnnotationCapabilityRuntime();
    return true;
}

void Application::setPluginFallbackEnabledForRuntime(bool enabled)
{
    if (_disablePluginFallback == !enabled) {
        return;
    }
    _disablePluginFallback = !enabled;
    setProperty("cgplay.disablePluginFallback", _disablePluginFallback);
    runtimeCapabilityLog(
        QStringLiteral("ApplicationRuntime"),
        QStringLiteral("fallback.toggle"),
        QJsonObject{
            { QStringLiteral("enabled"), enabled },
            { QStringLiteral("activeViewId"), _activeViewId }
        });
    _refreshAnnotationCapabilityRuntime();
}

void Application::_refreshAnnotationCapabilityRuntime()
{
    const bool annotationPluginActive = _pluginManager && _pluginManager->hasPlugin(QStringLiteral("annotation"));
    const bool fallbackEnabled = !_disablePluginFallback;
    runtimeCapabilityLog(
        QStringLiteral("ApplicationRuntime"),
        QStringLiteral("annotationCapability.refresh"),
        QJsonObject{
            { QStringLiteral("annotationPluginActive"), annotationPluginActive },
            { QStringLiteral("fallbackEnabled"), fallbackEnabled },
            { QStringLiteral("activeViewId"), _activeViewId }
        });

    const auto clearFallbackRuntimeServices = [this]() {
        if (_annotationFallbackOverlayProvider) {
            if (ServiceLocator::getService<IOverlayProvider>() == _annotationFallbackOverlayProvider.get()) {
                ServiceLocator::registerService<IOverlayProvider>(static_cast<IOverlayProvider*>(nullptr));
                logCapabilityEvent(
                    QStringLiteral("unregister"),
                    QStringLiteral("IOverlayProvider"),
                    QStringLiteral("annotationFallback"),
                    _activeViewId);
            }
            if (ServiceLocator::getService<IAnnotationViewBridge>() ==
                static_cast<IAnnotationViewBridge*>(_annotationFallbackOverlayProvider.get())) {
                ServiceLocator::registerService<IAnnotationViewBridge>(static_cast<IAnnotationViewBridge*>(nullptr));
                logCapabilityEvent(
                    QStringLiteral("unregister"),
                    QStringLiteral("IAnnotationViewBridge"),
                    QStringLiteral("annotationFallback"),
                    _activeViewId);
            }
        }
    };
    const auto publishFallbackOverlayInvalidated = [this]() {
        if (_activeViewId.isEmpty()) {
            return;
        }
        if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
            eventBus->publish(OverlayHostInvalidatedEvent{ _activeViewId });
        }
    };

    if (annotationPluginActive) {
        if (_annotationFallbackOverlayProvider) {
            publishFallbackOverlayInvalidated();
            runtimeOverlayLog(
                QStringLiteral("ApplicationRuntime"),
                QStringLiteral("fallbackOverlay.invalidate"),
                QJsonObject{ { QStringLiteral("viewId"), _activeViewId } });
        }
        clearFallbackRuntimeServices();
        _annotationFallbackOverlayProvider.reset();
    } else if (fallbackEnabled && _annoMgr) {
        if (!_annotationFallbackOverlayProvider) {
            // TODO(Phase13-remove): Transitional fallback overlay provider for annotation migration compatibility.
            _annotationFallbackOverlayProvider = std::make_unique<AnnotationOverlayProvider>(_annoMgr.get());
            runtimeOverlayLog(
                QStringLiteral("ApplicationRuntime"),
                QStringLiteral("fallbackOverlay.created"),
                QJsonObject{ { QStringLiteral("activeViewId"), _activeViewId } });
        }
        // TODO(Phase13-remove): Transitional fallback capability publication; delete after plugin-only annotation runtime is mandatory.
        ServiceLocator::registerService<IAnnotationService>(static_cast<IAnnotationService*>(_annoMgr.get()));
        ServiceLocator::registerService<ISessionContributor>(static_cast<ISessionContributor*>(_annoMgr.get()));
        ServiceLocator::registerService<IOverlayProvider>(
            static_cast<IOverlayProvider*>(_annotationFallbackOverlayProvider.get()));
        ServiceLocator::registerService<IAnnotationViewBridge>(
            static_cast<IAnnotationViewBridge*>(_annotationFallbackOverlayProvider.get()));
        logCapabilityEvent(
            QStringLiteral("register"),
            QStringLiteral("IAnnotationService"),
            QStringLiteral("annotationFallback"),
            _activeViewId);
        logCapabilityEvent(
            QStringLiteral("register"),
            QStringLiteral("ISessionContributor"),
            QStringLiteral("annotationFallback"),
            _activeViewId);
        logCapabilityEvent(
            QStringLiteral("register"),
            QStringLiteral("IOverlayProvider"),
            QStringLiteral("annotationFallback"),
            _activeViewId);
        logCapabilityEvent(
            QStringLiteral("register"),
            QStringLiteral("IAnnotationViewBridge"),
            QStringLiteral("annotationFallback"),
            _activeViewId);
        if (!_activeViewId.isEmpty()) {
            _annotationFallbackOverlayProvider->bind(_activeViewId);
        }
    } else {
        if (_annotationFallbackOverlayProvider) {
            publishFallbackOverlayInvalidated();
            runtimeOverlayLog(
                QStringLiteral("ApplicationRuntime"),
                QStringLiteral("fallbackOverlay.invalidate"),
                QJsonObject{ { QStringLiteral("viewId"), _activeViewId } });
        }
        clearFallbackRuntimeServices();
        _annotationFallbackOverlayProvider.reset();
        ServiceLocator::registerService<IAnnotationService>(static_cast<IAnnotationService*>(nullptr));
        ServiceLocator::registerService<ISessionContributor>(static_cast<ISessionContributor*>(nullptr));
        ServiceLocator::registerService<IOverlayProvider>(static_cast<IOverlayProvider*>(nullptr));
        ServiceLocator::registerService<IAnnotationViewBridge>(static_cast<IAnnotationViewBridge*>(nullptr));
        logCapabilityEvent(
            QStringLiteral("unregister"),
            QStringLiteral("IAnnotationService"),
            QStringLiteral("annotationFallback"),
            _activeViewId);
        logCapabilityEvent(
            QStringLiteral("unregister"),
            QStringLiteral("ISessionContributor"),
            QStringLiteral("annotationFallback"),
            _activeViewId);
        logCapabilityEvent(
            QStringLiteral("unregister"),
            QStringLiteral("IOverlayProvider"),
            QStringLiteral("annotationFallback"),
            _activeViewId);
        logCapabilityEvent(
            QStringLiteral("unregister"),
            QStringLiteral("IAnnotationViewBridge"),
            QStringLiteral("annotationFallback"),
            _activeViewId);
    }

    if (_mainWindow) {
        _mainWindow->refreshAnnotationCapabilityForRuntime();
    }
}

void Application::_activateMainView()
{
    if (!_mainWindow) {
        return;
    }
    _attachViewLifecycle(_mainWindow->viewerWidget());
}

void Application::_attachViewLifecycle(ViewerWidget* viewer)
{
    if (!viewer) {
        return;
    }

    connect(viewer, &ViewerWidget::viewActivated, this, [this, viewer](const QString&) {
        _activateView(viewer);
    });
    connect(viewer, &ViewerWidget::viewInvalidated, this, [this](const QString& viewId) {
        _invalidateView(viewId);
    });
    connect(viewer, &ViewerWidget::coordinateMapperChanged, this, [this, viewer](const QString& viewId) {
        if (viewId.isEmpty()) {
            return;
        }
        if (_activeViewer == viewer && _activeViewId == viewId) {
            if (auto* mapper = dynamic_cast<IViewerCoordinateMapper*>(viewer)) {
                ServiceLocator::registerService<IViewerCoordinateMapper>(mapper);
                logCapabilityEvent(
                    QStringLiteral("register"),
                    QStringLiteral("IViewerCoordinateMapper"),
                    QStringLiteral("activeView"),
                    viewId);
            }
        }
        if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
            eventBus->publish(CoordinateMapperChangedEvent{ viewId });
        }
    });
    connect(viewer, &ViewerWidget::coordinateMapperInvalidated, this, [this, viewer](const QString& viewId) {
        if (viewId.isEmpty()) {
            return;
        }
        if (_activeViewer == viewer && _activeViewId == viewId) {
            ServiceLocator::registerService<IViewerCoordinateMapper>(static_cast<IViewerCoordinateMapper*>(nullptr));
            logCapabilityEvent(
                QStringLiteral("unregister"),
                QStringLiteral("IViewerCoordinateMapper"),
                QStringLiteral("activeView"),
                viewId);
        }
        if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
            eventBus->publish(CoordinateMapperInvalidatedEvent{ viewId });
        }
    });
    connect(viewer, &ViewerWidget::transformChanged, this, [this](const QString& viewId) {
        if (viewId.isEmpty()) {
            return;
        }
        if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
            logRuntimeEventPublished(QStringLiteral("ViewTransformChangedEvent"), viewId);
            eventBus->publish(ViewTransformChangedEvent{ viewId });
        }
    });
    connect(viewer, &ViewerWidget::viewportResizedForView, this, [this](const QString& viewId) {
        if (viewId.isEmpty()) {
            return;
        }
        if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
            logRuntimeEventPublished(QStringLiteral("ViewportResizedEvent"), viewId);
            eventBus->publish(ViewportResizedEvent{ viewId });
        }
    });
    _activateView(viewer);
}

void Application::_activateView(ViewerWidget* viewer)
{
    if (!viewer) {
        return;
    }
    QCoreApplication::instance()->setProperty(
        "cgplay.codex.activeViewport",
        QVariant::fromValue<qulonglong>(reinterpret_cast<qulonglong>(viewer->viewport())));

    const QString newViewId = viewer->viewId();
    if (newViewId.isEmpty() || _activeViewId == newViewId) {
        return;
    }

    const QString oldViewId = _activeViewId;
    runtimeLifecycleLog(
        QStringLiteral("ApplicationRuntime"),
        QStringLiteral("activeView.switch.begin"),
        QJsonObject{
            { QStringLiteral("oldViewId"), oldViewId },
            { QStringLiteral("newViewId"), newViewId }
        });
    if (!oldViewId.isEmpty()) {
        if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
            logRuntimeEventPublished(QStringLiteral("ActiveViewInvalidatedEvent"), oldViewId, QStringLiteral("switch"));
            eventBus->publish(ActiveViewInvalidatedEvent{ oldViewId });
            logRuntimeEventPublished(QStringLiteral("OverlayHostInvalidatedEvent"), oldViewId, QStringLiteral("switch"));
            eventBus->publish(OverlayHostInvalidatedEvent{ oldViewId });
            logRuntimeEventPublished(QStringLiteral("CoordinateMapperInvalidatedEvent"), oldViewId, QStringLiteral("switch"));
            eventBus->publish(CoordinateMapperInvalidatedEvent{ oldViewId });
        }
        Q_EMIT activeViewInvalidated(oldViewId);
        ServiceLocator::registerService<IPlaybackService>(static_cast<IPlaybackService*>(nullptr));
        ServiceLocator::registerService<IOverlayHost>(static_cast<IOverlayHost*>(nullptr));
        ServiceLocator::registerService<IViewerCoordinateMapper>(static_cast<IViewerCoordinateMapper*>(nullptr));
        logCapabilityEvent(
            QStringLiteral("unregister"),
            QStringLiteral("IPlaybackService"),
            QStringLiteral("activeView"),
            oldViewId);
        logCapabilityEvent(
            QStringLiteral("unregister"),
            QStringLiteral("IOverlayHost"),
            QStringLiteral("activeView"),
            oldViewId);
        logCapabilityEvent(
            QStringLiteral("unregister"),
            QStringLiteral("IViewerCoordinateMapper"),
            QStringLiteral("activeView"),
            oldViewId);
        runtimeLifecycleLog(
            QStringLiteral("ApplicationRuntime"),
            QStringLiteral("activeView.invalidated"),
            QJsonObject{ { QStringLiteral("viewId"), oldViewId }, { QStringLiteral("reason"), QStringLiteral("switch") } });
    }

    _activeViewer = viewer;
    _activeViewId = newViewId;
    ServiceLocator::registerService<IPlaybackService>(viewer->playbackService());
    logCapabilityEvent(
        QStringLiteral("register"),
        QStringLiteral("IPlaybackService"),
        QStringLiteral("activeView"),
        _activeViewId);
    if (auto* host = dynamic_cast<IOverlayHost*>(viewer)) {
        ServiceLocator::registerService<IOverlayHost>(host);
        logCapabilityEvent(
            QStringLiteral("register"),
            QStringLiteral("IOverlayHost"),
            QStringLiteral("activeView"),
            _activeViewId);
    }
    if (auto* mapper = dynamic_cast<IViewerCoordinateMapper*>(viewer)) {
        ServiceLocator::registerService<IViewerCoordinateMapper>(mapper);
        logCapabilityEvent(
            QStringLiteral("register"),
            QStringLiteral("IViewerCoordinateMapper"),
            QStringLiteral("activeView"),
            _activeViewId);
    }
    if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
        logRuntimeEventPublished(QStringLiteral("ActiveViewChangedEvent"), _activeViewId);
        eventBus->publish(ActiveViewChangedEvent{ _activeViewId });
        logRuntimeEventPublished(QStringLiteral("OverlayHostChangedEvent"), _activeViewId);
        eventBus->publish(OverlayHostChangedEvent{ _activeViewId });
        logRuntimeEventPublished(QStringLiteral("CoordinateMapperChangedEvent"), _activeViewId);
        eventBus->publish(CoordinateMapperChangedEvent{ _activeViewId });
    }
    Q_EMIT activeViewChanged(_activeViewId);
    runtimeLifecycleLog(
        QStringLiteral("ApplicationRuntime"),
        QStringLiteral("activeView.changed"),
        QJsonObject{ { QStringLiteral("viewId"), _activeViewId } });
}

void Application::_invalidateView(const QString& viewId)
{
    if (viewId.isEmpty()) {
        return;
    }
    if (_activeViewId == viewId) {
        if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
            logRuntimeEventPublished(QStringLiteral("ActiveViewInvalidatedEvent"), viewId, QStringLiteral("close"));
            eventBus->publish(ActiveViewInvalidatedEvent{ viewId });
            logRuntimeEventPublished(QStringLiteral("OverlayHostInvalidatedEvent"), viewId, QStringLiteral("close"));
            eventBus->publish(OverlayHostInvalidatedEvent{ viewId });
            logRuntimeEventPublished(QStringLiteral("CoordinateMapperInvalidatedEvent"), viewId, QStringLiteral("close"));
            eventBus->publish(CoordinateMapperInvalidatedEvent{ viewId });
        }
        Q_EMIT activeViewInvalidated(viewId);
        _activeViewId.clear();
        _activeViewer.clear();
        ServiceLocator::registerService<IPlaybackService>(static_cast<IPlaybackService*>(nullptr));
        ServiceLocator::registerService<IOverlayHost>(static_cast<IOverlayHost*>(nullptr));
        ServiceLocator::registerService<IViewerCoordinateMapper>(static_cast<IViewerCoordinateMapper*>(nullptr));
        logCapabilityEvent(
            QStringLiteral("unregister"),
            QStringLiteral("IPlaybackService"),
            QStringLiteral("activeView"),
            viewId);
        logCapabilityEvent(
            QStringLiteral("unregister"),
            QStringLiteral("IOverlayHost"),
            QStringLiteral("activeView"),
            viewId);
        logCapabilityEvent(
            QStringLiteral("unregister"),
            QStringLiteral("IViewerCoordinateMapper"),
            QStringLiteral("activeView"),
            viewId);
        runtimeLifecycleLog(
            QStringLiteral("ApplicationRuntime"),
            QStringLiteral("activeView.invalidated"),
            QJsonObject{ { QStringLiteral("viewId"), viewId }, { QStringLiteral("reason"), QStringLiteral("close") } });
        return;
    }
    if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
        logRuntimeEventPublished(QStringLiteral("OverlayHostInvalidatedEvent"), viewId, QStringLiteral("inactiveClose"));
        eventBus->publish(OverlayHostInvalidatedEvent{ viewId });
        logRuntimeEventPublished(QStringLiteral("CoordinateMapperInvalidatedEvent"), viewId, QStringLiteral("inactiveClose"));
        eventBus->publish(CoordinateMapperInvalidatedEvent{ viewId });
    }
    runtimeLifecycleLog(
        QStringLiteral("ApplicationRuntime"),
        QStringLiteral("inactiveView.invalidated"),
        QJsonObject{ { QStringLiteral("viewId"), viewId } });
}

void Application::_initStyle()
{
    setStyle("Fusion");
    const auto defaultTokens = cgplay::buildThemeTokens(cgplay::defaultThemeForMode(cgplay::ThemeMode::Dark));
    setPalette(cgplay::buildThemePalette(defaultTokens));

    setStyleSheet(R"(
        QMainWindow { background: transparent; }
        * { font-family: "Segoe UI", sans-serif; font-size: 13px; }
        QToolTip { color: #D8DEE7; background: rgba(18,23,29,0.94); border: 1px solid rgba(255,255,255,0.10); padding: 6px 10px; border-radius: 6px; }
        QMenuBar { background: rgba(11,16,22,0.86); color: #D8DEE7; border: none; padding: 0; }
        QMenuBar::item { padding: 8px 14px; background: transparent; }
        QMenuBar::item:selected { background: rgba(255,255,255,0.06); }
        QMenu { background: rgba(18,23,29,0.96); color: #D8DEE7; border: 1px solid rgba(255,255,255,0.10); padding: 4px; border-radius: 8px; }
        QMenu::item { padding: 8px 36px 8px 16px; border-radius: 4px; }
        QMenu::item:selected { background: rgba(255,138,61,0.18); }
        QMenu::separator { height: 1px; background: rgba(255,255,255,0.07); margin: 4px 10px; }
        QSplitter::handle { background: transparent; border: none; }
        QSplitter::handle:hover { background: transparent; border: none; }
        QStatusBar { background: rgba(10,14,18,0.82); color: #B8C2CE; border-top: 1px solid rgba(255,255,255,0.08); min-height: 30px; }
        QStatusBar::item { border: none; }
        QStatusBar QLabel { color: #B8C2CE; padding: 2px 8px; font-size: 11px; background: transparent; }
        QListView { background: transparent; color: #D8DEE7; border: none; outline: none; }
        QListView::item:selected { background: rgba(255,138,61,0.2); }
        QListView::item:hover { background: rgba(255,255,255,0.03); }
        QScrollBar:vertical { background: transparent; width: 8px; margin: 0; border: none; }
        QScrollBar::handle:vertical { background: rgba(255,255,255,0.13); min-height: 30px; border-radius: 4px; }
        QScrollBar::handle:vertical:hover { background: rgba(255,255,255,0.22); }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar:horizontal { background: transparent; height: 8px; }
        QScrollBar::handle:horizontal { background: rgba(255,255,255,0.13); min-width: 30px; border-radius: 4px; }
        QScrollBar::handle:horizontal:hover { background: rgba(255,255,255,0.22); }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
        QPushButton { background: #171B20; color: #D8DEE7; border: 1px solid rgba(255,255,255,0.08); padding: 6px 16px; border-radius: 6px; }
        QPushButton:hover { background: rgba(255,138,61,0.15); border-color: #FF8A3D; }
        QPushButton:pressed { background: #FF8A3D; color: #fff; }
        QPushButton:disabled { color: #5a5a5a; background: #111418; }
        QToolButton { color: #9AA4B2; background: transparent; border: none; padding: 4px; border-radius: 4px; }
        QToolButton:hover { background: rgba(255,138,61,0.15); color: #D8DEE7; }
        QToolButton:pressed { background: #FF8A3D; color: #fff; }
        QToolButton:checked { background: rgba(255,138,61,0.25); color: #FF8A3D; }
        QLineEdit { background: #111418; color: #D8DEE7; border: 1px solid #252B33; padding: 6px 10px; border-radius: 6px; selection-background-color: #FF8A3D; }
        QLineEdit:focus { border-color: rgba(255,138,61,0.5); }
        QTabWidget::pane { border: none; background: #171B20; }
        QTabBar::tab { background: transparent; color: #9AA4B2; padding: 8px 18px; border-bottom: 2px solid transparent; }
        QTabBar::tab:selected { color: #FF8A3D; border-bottom: 2px solid #FF8A3D; }
        QTabBar::tab:hover { color: #D8DEE7; }
        QSlider::groove:horizontal { background: #252B33; height: 4px; border-radius: 2px; }
        QSlider::handle:horizontal { background: #FF8A3D; width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }
        QSlider::sub-page:horizontal { background: #FF8A3D; border-radius: 2px; }
        QComboBox { background: #111418; color: #D8DEE7; border: 1px solid #252B33; padding: 4px 10px; border-radius: 6px; }
        QComboBox:hover { border-color: rgba(255,138,61,0.3); }
        QComboBox QAbstractItemView { background: #171B20; color: #D8DEE7; border: 1px solid #252B33; selection-background-color: rgba(255,138,61,0.2); }
        QCheckBox { color: #9AA4B2; spacing: 6px; }
        QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #252B33; border-radius: 3px; background: #111418; }
        QCheckBox::indicator:checked { background: #FF8A3D; border-color: #FF8A3D; }
    )");
    setProperty("cgplay.baseStyleSheet", styleSheet());
}

void Application::refreshAppearanceSettings()
{
    if (property("cgplay.deferAppearanceRefresh").toBool()) {
        setProperty("cgplay.appearanceRefreshPending", true);
        return;
    }
    setProperty("cgplay.appearanceRefreshPending", false);
    cgplay::ThemeDefinition storedTheme;
    bool hasStoredTheme = false;
    if (_userSettings) {
        const QString themeName = _userSettings->value(QStringLiteral("appearance/themeName")).toString();
        if (!themeName.isEmpty() && cgplay::ThemeService::names().contains(cgplay::ThemeService::sanitizeName(themeName))) {
            storedTheme = cgplay::ThemeService::load(themeName);
            hasStoredTheme = true;
        }
    }
    const bool modeExplicit = _userSettings && _userSettings->contains(QStringLiteral("appearance/mode"));
    const QString modeValue = modeExplicit
        ? _userSettings->value(QStringLiteral("appearance/mode")).toString()
        : (hasStoredTheme ? cgplay::themeModeName(storedTheme.mode) : QStringLiteral("dark"));
    const cgplay::ThemeMode mode = cgplay::themeModeFromName(modeValue);

    cgplay::ThemeDefinition definition = cgplay::defaultThemeForMode(mode);
    if (hasStoredTheme && (!modeExplicit || storedTheme.mode == mode)) {
        definition = storedTheme;
        definition.mode = mode;
    }
    const auto readColor = [this](const QString& key, QColor* target) {
        if (!_userSettings || !_userSettings->contains(key) || !target) return;
        const QColor value(_userSettings->value(key).toString());
        if (value.isValid()) *target = value;
    };
    const auto readInt = [this](const QString& key, int* target, int minimum, int maximum) {
        if (!_userSettings || !_userSettings->contains(key) || !target) return;
        bool ok = false;
        const int value = _userSettings->value(key).toInt(&ok);
        if (ok) *target = qBound(minimum, value, maximum);
    };
    const auto readString = [this](const QString& key, QString* target) {
        if (!_userSettings || !_userSettings->contains(key) || !target) return;
        // Empty asset paths are meaningful: clearing the image field must
        // remove a previously selected background instead of silently keeping
        // the value from a named theme.
        *target = _userSettings->value(key).toString().trimmed();
    };
    const auto readBool = [this](const QString& key, bool* target) {
        if (!_userSettings || !_userSettings->contains(key) || !target) return;
        *target = _userSettings->value(key).toBool();
    };
    // Presets are authoritative.  Custom values stay stored while the user
    // previews Dark/Light/Glass/High Contrast, then become active again when
    // Custom is selected.  Applying every old custom key on top of a preset
    // made all five mode choices look nearly identical.
    if (mode == cgplay::ThemeMode::Custom) {
        readColor(QStringLiteral("appearance/backgroundColor"), &definition.background);
        readColor(QStringLiteral("appearance/backgroundSecondary"), &definition.backgroundSecondary);
        readColor(QStringLiteral("appearance/panelColor"), &definition.panel);
        readColor(QStringLiteral("appearance/toolbarColor"), &definition.toolbar);
        readColor(QStringLiteral("appearance/timelineColor"), &definition.timeline);
        readColor(QStringLiteral("appearance/textColor"), &definition.text);
        readColor(QStringLiteral("appearance/borderColor"), &definition.border);
        readColor(QStringLiteral("appearance/accentColor"), &definition.accent);
        QString backgroundTypeValue;
        readString(QStringLiteral("appearance/backgroundType"), &backgroundTypeValue);
        if (!backgroundTypeValue.isEmpty()) {
            definition.backgroundType = cgplay::backgroundTypeFromName(backgroundTypeValue);
        }
        readString(QStringLiteral("appearance/backgroundImage"), &definition.backgroundImage);
        readString(QStringLiteral("appearance/texturePath"), &definition.texturePath);
        readString(QStringLiteral("appearance/fillMode"), &definition.fillMode);
        // Migrate older profiles where the file path was saved but the UI
        // forgot to switch the renderer from solid mode.  A valid image path
        // is an explicit user choice and must take precedence over that stale
        // solid marker; reset-theme remains safe because it clears the path.
        if (definition.backgroundType == cgplay::BackgroundType::Solid) {
            const QString candidate = definition.backgroundImage.trimmed();
            if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
                definition.backgroundType = cgplay::BackgroundType::Image;
            }
        }
        // A newly selected image should be visible immediately.  Older
        // profiles may not contain surface opacity keys at all, in which case
        // the historical opaque defaults (96/92/96) hide nearly the whole
        // backdrop.  Seed glass-friendly defaults only for missing keys; once
        // the user changes a slider, their explicit value remains authoritative.
        if ((definition.backgroundType == cgplay::BackgroundType::Image ||
             definition.backgroundType == cgplay::BackgroundType::Texture) &&
            !definition.backgroundImage.trimmed().isEmpty() && _userSettings) {
            const auto seedOpacity = [this](const QString& key, int value, int* target) {
                if (!_userSettings->contains(key)) {
                    _userSettings->setValue(key, value);
                    if (target) *target = value;
                }
            };
            seedOpacity(QStringLiteral("appearance/panelOpacity"), 58, &definition.panelOpacity);
            seedOpacity(QStringLiteral("appearance/toolbarOpacity"), 52, &definition.toolbarOpacity);
            seedOpacity(QStringLiteral("appearance/timelineOpacity"), 68, &definition.timelineOpacity);
            _userSettings->sync();
        }
        readInt(QStringLiteral("appearance/backgroundOpacity"), &definition.backgroundOpacity, 20, 100);
        readInt(QStringLiteral("appearance/panelOpacity"), &definition.panelOpacity, 35, 100);
        readInt(QStringLiteral("appearance/toolbarOpacity"), &definition.toolbarOpacity, 35, 100);
        readInt(QStringLiteral("appearance/timelineOpacity"), &definition.timelineOpacity, 45, 100);
        readInt(QStringLiteral("appearance/subtitleOpacity"), &definition.subtitleOpacity, 20, 100);
        readInt(QStringLiteral("appearance/viewerOpacity"), &definition.viewerOpacity, 20, 100);
        readInt(QStringLiteral("appearance/vignette"), &definition.vignette, 0, 100);
        readInt(QStringLiteral("appearance/blurRadius"), &definition.blurRadius, 0, 64);
        readInt(QStringLiteral("appearance/brightness"), &definition.brightness, 0, 200);
        readInt(QStringLiteral("appearance/saturation"), &definition.saturation, 0, 200);
        readInt(QStringLiteral("appearance/shadowStrength"), &definition.shadowStrength, 0, 100);
        readBool(QStringLiteral("appearance/dynamicBackground"), &definition.dynamicBackground);
    }
    definition.normalize();
    if (mode == cgplay::ThemeMode::HighContrast) {
        // High contrast overrides semantic surfaces/text, but it must not make
        // a selected background asset silently disappear.  Preserve the
        // background asset and its transform controls while using the
        // accessibility-safe colors and panel opacities from the preset.
        const cgplay::BackgroundType requestedBackgroundType = definition.backgroundType;
        const QString requestedBackgroundImage = definition.backgroundImage;
        const QString requestedTexturePath = definition.texturePath;
        const QString requestedFillMode = definition.fillMode;
        const int requestedBackgroundOpacity = definition.backgroundOpacity;
        const int requestedViewerOpacity = definition.viewerOpacity;
        const int requestedBrightness = definition.brightness;
        const int requestedSaturation = definition.saturation;
        const int requestedBlurRadius = definition.blurRadius;
        const int requestedVignette = definition.vignette;
        const bool requestedDynamicBackground = definition.dynamicBackground;
        definition = cgplay::defaultThemeForMode(cgplay::ThemeMode::HighContrast);
        definition.backgroundType = requestedBackgroundType;
        definition.backgroundImage = requestedBackgroundImage;
        definition.texturePath = requestedTexturePath;
        definition.fillMode = requestedFillMode;
        definition.backgroundOpacity = requestedBackgroundOpacity;
        definition.viewerOpacity = requestedViewerOpacity;
        definition.brightness = requestedBrightness;
        definition.saturation = requestedSaturation;
        definition.blurRadius = requestedBlurRadius;
        definition.vignette = requestedVignette;
        definition.dynamicBackground = requestedDynamicBackground;
        definition.normalize();
    }
    const cgplay::ThemeTokens tokens = cgplay::buildThemeTokens(definition);
    setProperty("cgplay.backgroundColor", definition.background.name(QColor::HexArgb));
    setProperty("cgplay.backgroundSecondary", definition.backgroundSecondary.name(QColor::HexArgb));
    setProperty("cgplay.backgroundOpacity", definition.backgroundOpacity);
    setProperty("cgplay.panelOpacity", definition.panelOpacity);
    setProperty("cgplay.toolbarOpacity", definition.toolbarOpacity);
    setProperty("cgplay.timelineOpacity", definition.timelineOpacity);
    setProperty("cgplay.subtitleOpacity", definition.subtitleOpacity);
    setProperty("cgplay.viewerOpacity", definition.viewerOpacity);
    setProperty("cgplay.backgroundType", cgplay::backgroundTypeName(definition.backgroundType));
    const QString backdropPath = definition.backgroundType == cgplay::BackgroundType::Texture
        ? (definition.texturePath.trimmed().isEmpty() ? definition.backgroundImage : definition.texturePath)
        : definition.backgroundImage;
    setProperty("cgplay.backgroundImage", backdropPath.trimmed());
    setProperty("cgplay.backgroundImageConfigured", !backdropPath.trimmed().isEmpty());
    setProperty("cgplay.backgroundFillMode", definition.fillMode);
    setProperty("cgplay.fillMode", definition.fillMode);
    setProperty("cgplay.backgroundVignette", definition.vignette);
    setProperty("cgplay.backgroundBrightness", definition.brightness);
    setProperty("cgplay.backgroundSaturation", definition.saturation);
    setProperty("cgplay.backgroundBlurRadius", definition.blurRadius);
    setProperty("cgplay.shadowStrength", definition.shadowStrength);
    setProperty("cgplay.dynamicBackground", definition.dynamicBackground);
    setProperty("cgplay.themeMode", cgplay::themeModeName(mode));
    setProperty("cgplay.panelColor", tokens.surfacePanel.name(QColor::HexArgb));
    setProperty("cgplay.toolbarColor", tokens.surfaceToolbar.name(QColor::HexArgb));
    setProperty("cgplay.timelineColor", tokens.surfaceTimeline.name(QColor::HexArgb));
    setProperty("cgplay.viewerColor", tokens.surfaceViewer.name(QColor::HexArgb));
    setProperty("cgplay.dialogColor", tokens.surfaceDialog.name(QColor::HexArgb));
    setProperty("cgplay.textColor", tokens.textPrimary.name(QColor::HexArgb));
    setProperty("cgplay.borderColor", tokens.borderNormal.name(QColor::HexArgb));
    setProperty("cgplay.accentColor", tokens.accentNormal.name(QColor::HexArgb));
    setProperty("cgplay.useConfiguredBackdrop", true);
    setPalette(cgplay::buildThemePalette(tokens));

    const auto surfaceCssColor = [](QColor color, int opacity) {
        color.setAlpha(qRound(qBound(0, opacity, 100) * 255.0 / 100.0));
        return color.name(QColor::HexArgb);
    };
    const QString panelSurfaceCss = surfaceCssColor(definition.panel, definition.panelOpacity);
    const QString toolbarSurfaceCss = surfaceCssColor(definition.toolbar, definition.toolbarOpacity);
    const QString timelineSurfaceCss = surfaceCssColor(definition.timeline, definition.timelineOpacity);
    const auto prepareTransparentSurface = [](QWidget* surface) {
        if (!surface) return;
        surface->setAttribute(Qt::WA_StyledBackground, true);
        surface->setAttribute(Qt::WA_TranslucentBackground, true);
        surface->setAutoFillBackground(false);
    };

    const int buttonOpacity = qBound(30,
        mode == cgplay::ThemeMode::Custom && _userSettings
            ? _userSettings->value(QStringLiteral("appearance/buttonOpacity"), 88).toInt()
            : 88,
        100);
    setProperty("cgplay.buttonOpacity", buttonOpacity);
    const QColor buttonSurface = cgplay::composeSurface(tokens.surfacePanel, tokens.surfaceToolbar, buttonOpacity);
    QString css = QStringLiteral(
        "QMainWindow{background:%1;color:%2;} QWidget#cgplayAppearanceHost{background:transparent;color:%2;} "
        "QWidget{color:%2;} QLabel,QCheckBox,QRadioButton{color:%2;} "
        "QPushButton{background:%3;color:%2;border:1px solid %4;} QPushButton:hover{background:%5;border-color:%6;} "
        "QPushButton:pressed{background:%7;color:%8;} QPushButton:disabled{background:%9;color:%10;border-color:%4;} "
        "QToolButton{color:%11;background:transparent;} QToolButton:hover{background:%5;color:%2;} "
        "QToolButton:pressed,QToolButton:checked{background:%7;color:%8;} "
        "QTabBar::tab{color:%11;} QTabBar::tab:selected{color:%6;border-bottom-color:%6;} "
        "QLineEdit,QPlainTextEdit,QTextEdit,QComboBox{background:%12;color:%2;border:1px solid %4;selection-background-color:%13;selection-color:%14;} "
        "QComboBox QAbstractItemView{background:%15;color:%2;border:1px solid %4;selection-background-color:%13;selection-color:%14;} "
        "QSlider::handle:horizontal,QSlider::sub-page:horizontal{background:%6;} QSlider::groove:horizontal{background:%16;} "
        "QCheckBox::indicator{background:%12;border:1px solid %4;} QCheckBox::indicator:checked{background:%6;border-color:%6;} "
        "QComboBox:hover,QLineEdit:focus,QPlainTextEdit:focus{border-color:%6;} "
        "QDockWidget,QGroupBox,QTabWidget::pane{background:%15;color:%2;border-color:%4;} "
        "QToolBar,QMenuBar{background:%17;color:%2;} QMenuBar::item:selected{background:%5;} "
        "QMenu{background:%15;color:%2;border:1px solid %4;} QMenu::item:selected{background:%13;color:%2;} "
        "QStatusBar{background:%18;color:%11;border-top:1px solid %4;} QStatusBar QLabel{color:%11;background:transparent;} "
        "QToolTip{background:%19;color:%20;border:1px solid %4;} "
        "QLabel#GeneratedSubtitleOverlayLabel{background:%21;color:%2;} QWidget#cgplayViewerShell{background:%22;}");
    css = css.arg(tokens.surfaceMain.name(QColor::HexArgb))
        .arg(tokens.textPrimary.name(QColor::HexArgb))
        .arg(buttonSurface.name(QColor::HexArgb))
        .arg(tokens.borderNormal.name(QColor::HexArgb))
        .arg(tokens.accentHover.name(QColor::HexArgb))
        .arg(tokens.accentNormal.name(QColor::HexArgb))
        .arg(tokens.accentPressed.name(QColor::HexArgb))
        .arg(tokens.textOnAccent.name(QColor::HexArgb))
        .arg(tokens.surfaceMain.name(QColor::HexArgb))
        .arg(tokens.textDisabled.name(QColor::HexArgb))
        .arg(tokens.textSecondary.name(QColor::HexArgb))
        .arg(tokens.surfaceTimeline.name(QColor::HexArgb))
        .arg(tokens.selection.name(QColor::HexArgb))
        .arg(tokens.selectionText.name(QColor::HexArgb))
        .arg(tokens.surfacePanel.name(QColor::HexArgb))
        .arg(tokens.sliderTrack.name(QColor::HexArgb))
        .arg(tokens.surfaceToolbar.name(QColor::HexArgb))
        .arg(tokens.surfaceTimeline.name(QColor::HexArgb))
        .arg(tokens.tooltipBackground.name(QColor::HexArgb))
        .arg(tokens.tooltipText.name(QColor::HexArgb))
        .arg(tokens.subtitleBackground.name(QColor::HexArgb))
        .arg(tokens.surfaceViewer.name(QColor::HexArgb));
    const QString baseStyle = property("cgplay.baseStyleSheet").toString();
    setStyleSheet((baseStyle.isEmpty() ? styleSheet() : baseStyle) + css);
    // Theme refresh used to enumerate every widget repeatedly for provider
    // panels, button restoration and named button styling.  Capture one stable
    // snapshot for this pass; refreshTheme does not create user widgets.
    const QList<QWidget*> allWidgets = QApplication::allWidgets();
    if (_mainWindow) {
        if (auto* viewerShell = _mainWindow->findChild<QWidget*>(QStringLiteral("cgplayViewerShell"))) {
            viewerShell->setStyleSheet(QStringLiteral("background:%1;color:%2;").arg(tokens.surfaceViewer.name(QColor::HexArgb), tokens.textPrimary.name(QColor::HexArgb)));
        }
        if (auto* viewer = _mainWindow->viewerWidget()) {
            // ViewerWidget owns local chrome styles, so palette propagation alone
            // cannot refresh its buttons after a runtime theme edit.
            QEvent paletteChange(QEvent::PaletteChange);
            QCoreApplication::sendEvent(viewer, &paletteChange);
        }
        const QString surfaceStyle = QStringLiteral("background:%1;");
        for (const auto& item : {std::pair<QString, QString>{QStringLiteral("cgplayTopBarSurface"), toolbarSurfaceCss}, {QStringLiteral("cgplayCustomToolbar"), toolbarSurfaceCss}, {QStringLiteral("cgplayPlaylistSurface"), panelSurfaceCss}, {QStringLiteral("cgplayTimelineSurface"), timelineSurfaceCss}, {QStringLiteral("cgplayPlaybackBarSurface"), toolbarSurfaceCss}}) {
            if (auto* surface = _mainWindow->findChild<QWidget*>(item.first)) {
                prepareTransparentSurface(surface);
                surface->setStyleSheet(surfaceStyle.arg(
                    item.first == QStringLiteral("cgplayTopBarSurface")
                        ? QStringLiteral("transparent") : item.second));
            }
        }
        if (auto* playbackBar = _mainWindow->findChild<QWidget*>(QStringLiteral("cgplayPlaybackBarSurface"))) {
            for (auto* label : playbackBar->findChildren<QLabel*>()) {
                label->setStyleSheet(QStringLiteral("QLabel{color:%1;background:%2;border:1px solid %3;border-radius:7px;padding:4px 10px;font-weight:700;}")
                    .arg(tokens.textPrimary.name(QColor::HexArgb), timelineSurfaceCss, tokens.borderNormal.name(QColor::HexArgb)));
            }
            const QString volumeStyle = QStringLiteral(
                "QSlider::groove:horizontal{background:%1;height:3px;border-radius:2px;}"
                "QSlider::handle:horizontal{background:%2;width:14px;height:14px;margin:-6px 0;border-radius:7px;border:1px solid %3;}"
                "QSlider::sub-page:horizontal{background:%4;border-radius:2px;}"
            ).arg(tokens.sliderTrack.name(QColor::HexArgb), tokens.textPrimary.name(QColor::HexArgb), tokens.borderNormal.name(QColor::HexArgb), tokens.accentNormal.name(QColor::HexArgb));
            for (auto* slider : playbackBar->findChildren<QSlider*>()) slider->setStyleSheet(volumeStyle);
        }
        const int globalShadow = definition.mode == cgplay::ThemeMode::HighContrast ? 0 : qBound(0, definition.shadowStrength, 100);
        const auto applyGlobalShadow = [globalShadow](QWidget* widget) {
            if (!widget) return;
            if (auto* effect = qobject_cast<QGraphicsDropShadowEffect*>(widget->graphicsEffect())) {
                if (!widget->property("cgplay.globalShadow").toBool()) return;
                if (globalShadow <= 0) {
                    widget->setGraphicsEffect(nullptr);
                    widget->setProperty("cgplay.globalShadow", false);
                } else {
                    effect->setBlurRadius(2.0 + globalShadow * 0.16);
                    effect->setColor(QColor(0, 0, 0, qRound(globalShadow * 1.4)));
                    effect->setOffset(0, 2);
                }
                return;
            }
            if (globalShadow <= 0) return;
            auto* effect = new QGraphicsDropShadowEffect(widget);
            effect->setBlurRadius(2.0 + globalShadow * 0.16);
            effect->setColor(QColor(0, 0, 0, qRound(globalShadow * 1.4)));
            effect->setOffset(0, 2);
            widget->setGraphicsEffect(effect);
            widget->setProperty("cgplay.globalShadow", true);
        };
        for (const QString& surfaceName : {QStringLiteral("cgplayTopBarSurface"), QStringLiteral("cgplayTimelineSurface"), QStringLiteral("cgplayPlaybackBarSurface")}) {
            applyGlobalShadow(_mainWindow->findChild<QWidget*>(surfaceName));
        }
        for (const QString& surfaceName : {QStringLiteral("cgplayPlaylistSurface"), QStringLiteral("ReviewPanel")}) {
            if (auto* panel = _mainWindow->findChild<QWidget*>(surfaceName)) {
                if (panel->property("cgplay.globalShadow").toBool()) {
                    panel->setGraphicsEffect(nullptr);
                    panel->setProperty("cgplay.globalShadow", false);
                }
            }
        }
        if (auto* topBar = _mainWindow->findChild<QWidget*>(QStringLiteral("cgplayTopBarSurface"))) {
            const QString menuButtonStyle = QStringLiteral(
                "QPushButton{color:%1;background:transparent;border:none;padding:0 12px;font-family:'Microsoft YaHei UI','Segoe UI';font-size:13px;}"
                "QPushButton:hover{color:%2;background:%3;} QPushButton:pressed{color:%4;}")
                .arg(tokens.textSecondary.name(QColor::HexArgb), tokens.textPrimary.name(QColor::HexArgb),
                     tokens.selection.name(QColor::HexArgb), tokens.accentNormal.name(QColor::HexArgb));
            const QString windowButtonStyle = QStringLiteral(
                "QPushButton{color:%1;background:transparent;border:none;border-radius:7px;}"
                "QPushButton:hover{color:%2;background:%3;}")
                .arg(tokens.textSecondary.name(QColor::HexArgb), tokens.textPrimary.name(QColor::HexArgb), tokens.selection.name(QColor::HexArgb));
            for (auto* button : topBar->findChildren<QPushButton*>()) {
                button->setStyleSheet(button->height() >= 50 ? menuButtonStyle : windowButtonStyle);
            }
            for (auto* label : topBar->findChildren<QLabel*>()) {
                const bool logo = label->text().compare(QStringLiteral("CGPlay"), Qt::CaseInsensitive) == 0;
                label->setStyleSheet(QStringLiteral("QLabel{color:%1;background:transparent;font-family:'Segoe UI';font-size:%2px;%3;}")
                    .arg(tokens.textPrimary.name(QColor::HexArgb))
                    .arg(logo ? 16 : 12)
                    .arg(logo ? QStringLiteral("font-weight:700") : QString()));
            }
        }
        if (auto* playlist = _mainWindow->findChild<QWidget*>(QStringLiteral("cgplayPlaylistSurface"))) {
            prepareTransparentSurface(playlist);
            playlist->setStyleSheet(QStringLiteral("QWidget#cgplayPlaylistSurface{background:%1;color:%2;}")
                .arg(panelSurfaceCss, tokens.textPrimary.name(QColor::HexArgb)));
            const QString editStyle = QStringLiteral(
                "QLineEdit,QComboBox{background:%1;color:%2;border:1px solid %3;border-radius:6px;padding:5px 8px;}"
                "QComboBox QAbstractItemView{background:%4;color:%2;border:1px solid %3;selection-background-color:%5;}")
                .arg(tokens.surfaceTimeline.name(QColor::HexArgb), tokens.textPrimary.name(QColor::HexArgb),
                     tokens.borderNormal.name(QColor::HexArgb), tokens.surfacePanel.name(QColor::HexArgb), tokens.selection.name(QColor::HexArgb));
            for (auto* edit : playlist->findChildren<QLineEdit*>()) edit->setStyleSheet(editStyle);
            for (auto* combo : playlist->findChildren<QComboBox*>()) combo->setStyleSheet(editStyle);
            for (auto* label : playlist->findChildren<QLabel*>()) label->setStyleSheet(QStringLiteral("QLabel{color:%1;background:transparent;}").arg(tokens.textPrimary.name(QColor::HexArgb)));
            for (auto* button : playlist->findChildren<QToolButton*>()) button->setStyleSheet(QStringLiteral(
                "QToolButton{color:%1;background:%2;border:1px solid %3;border-radius:6px;} QToolButton:hover{background:%4;color:%5;} QToolButton:pressed{background:%6;color:%7;}")
                .arg(tokens.textSecondary.name(QColor::HexArgb), tokens.surfaceToolbar.name(QColor::HexArgb), tokens.borderNormal.name(QColor::HexArgb),
                     tokens.selection.name(QColor::HexArgb), tokens.textPrimary.name(QColor::HexArgb), tokens.accentPressed.name(QColor::HexArgb), tokens.textOnAccent.name(QColor::HexArgb)));
        }
        if (auto* review = _mainWindow->findChild<QWidget*>(QStringLiteral("ReviewPanel"))) {
            prepareTransparentSurface(review);
            review->setStyleSheet(QStringLiteral("QWidget#ReviewPanel{background:transparent;color:%1;}")
                .arg(tokens.textPrimary.name(QColor::HexArgb)));
            if (auto* tabs = review->findChild<QTabWidget*>()) {
                tabs->setStyleSheet(QStringLiteral(
                    "QTabWidget{background:transparent;color:%1;} QTabWidget::pane{border:none;background:transparent;} QTabBar{background:transparent;}"
                    "QTabBar::tab{background:transparent;color:%1;padding:10px 11px 11px;border-bottom:2px solid transparent;}"
                    "QTabBar::tab:selected,QTabBar::tab:hover{color:%2;border-bottom-color:%3;}")
                    .arg(tokens.textSecondary.name(QColor::HexArgb), tokens.textPrimary.name(QColor::HexArgb), tokens.accentNormal.name(QColor::HexArgb)));
                for (int index = 0; index < tabs->count(); ++index) {
                    if (auto* page = tabs->widget(index)) {
                        page->setAttribute(Qt::WA_StyledBackground, true);
                        page->setStyleSheet(QStringLiteral("background:transparent;color:%1;")
                            .arg(tokens.textPrimary.name(QColor::HexArgb)));
                    }
                }
            }
            const QString reviewEditStyle = QStringLiteral(
                "QLineEdit,QComboBox{background:%1;color:%2;border:1px solid %3;border-radius:6px;padding:5px 8px;}"
                "QComboBox QAbstractItemView{background:%4;color:%2;border:1px solid %3;selection-background-color:%5;}")
                .arg(tokens.surfaceTimeline.name(QColor::HexArgb), tokens.textPrimary.name(QColor::HexArgb), tokens.borderNormal.name(QColor::HexArgb),
                     tokens.surfacePanel.name(QColor::HexArgb), tokens.selection.name(QColor::HexArgb));
            for (auto* edit : review->findChildren<QLineEdit*>()) edit->setStyleSheet(reviewEditStyle);
            for (auto* combo : review->findChildren<QComboBox*>()) combo->setStyleSheet(reviewEditStyle);
            for (auto* label : review->findChildren<QLabel*>()) label->setStyleSheet(QStringLiteral("QLabel{color:%1;background:transparent;}").arg(tokens.textPrimary.name(QColor::HexArgb)));
        }
        if (auto* timeline = _mainWindow->findChild<QWidget*>(QStringLiteral("cgplayTimelineSurface"))) {
            for (auto* label : timeline->findChildren<QLabel*>()) {
                label->setStyleSheet(QStringLiteral("QLabel{color:%1;background:%2;border:1px solid %3;border-radius:6px;padding:2px 10px;font-size:12px;font-weight:700;}")
                    .arg(tokens.textPrimary.name(QColor::HexArgb), tokens.surfaceTimeline.name(QColor::HexArgb), tokens.borderFocus.name(QColor::HexArgb)));
            }
        }
        // Provider-owned panels may not expose the fallback object name.  The
        // surface role is the stable contract used by both implementations.
        for (QWidget* candidate : allWidgets) {
            if (!candidate || candidate == _mainWindow.get()) continue;
            if (candidate->property("cgplay.surfaceRole").toString() != QStringLiteral("panel")) continue;
            prepareTransparentSurface(candidate);
            candidate->setStyleSheet(QStringLiteral("background:%1;color:%2;")
                .arg(panelSurfaceCss, tokens.textPrimary.name(QColor::HexArgb)));
        }
        const QString dockStyle = QStringLiteral(
            "QDockWidget{background:%1;color:%2;border:0;}"
            "QDockWidget::title{background:%4;padding:10px 12px;color:%2;font-weight:600;border:0;}")
                .arg(panelSurfaceCss, tokens.textPrimary.name(QColor::HexArgb),
                 tokens.borderNormal.name(QColor::HexArgb), toolbarSurfaceCss);
        for (const QString& dockName : {QStringLiteral("CodexAgentWorkspaceDock"), QStringLiteral("AIAgentWorkspaceDock")}) {
            if (auto* dock = _mainWindow->findChild<QDockWidget*>(dockName)) {
                prepareTransparentSurface(dock);
                dock->setStyleSheet(dockStyle);
            }
        }
        const QString dialogStyle = QStringLiteral(
            "QDialog#CGPlaySettingsDialog{background:%1;color:%2;} QWidget#AISettingsWorkspaceWidget{background:%1;} QTabWidget::pane{background:%1;} QGroupBox{background:%3;}"
            "QListWidget#SettingsCategoryNavigation,QListWidget#SettingsSearchResults{background:%3;color:%2;border:1px solid %4;border-radius:6px;padding:4px;}"
            "QListWidget#SettingsCategoryNavigation::item,QListWidget#SettingsSearchResults::item{padding:6px;border-radius:4px;}"
            "QListWidget#SettingsCategoryNavigation::item:selected,QListWidget#SettingsSearchResults::item:selected{background:%5;color:%6;}"
            "QListWidget#SettingsCategoryNavigation:focus,QListWidget#SettingsSearchResults:focus,QLineEdit#SettingsSearchEdit:focus{border:1px solid %5;}"
            "QLineEdit#SettingsSearchEdit{background:%3;color:%2;border:1px solid %4;border-radius:6px;padding:4px 8px;}"
            "QLabel#SettingsPageDescription,QLabel#SettingsSearchEmptyState{color:%7;}"
            "QPushButton#SettingsSaveShortcuts{background:%5;color:%6;border-radius:4px;padding:6px 14px;}")
            .arg(tokens.surfaceDialog.name(QColor::HexArgb), tokens.textPrimary.name(QColor::HexArgb),
                 tokens.surfacePanel.name(QColor::HexArgb), tokens.borderNormal.name(QColor::HexArgb),
                 tokens.accentNormal.name(QColor::HexArgb), tokens.textOnAccent.name(QColor::HexArgb),
                 tokens.textSecondary.name(QColor::HexArgb));
        setProperty("cgplay.settingsDialogStyleSheet", dialogStyle);
        if (auto* settingsDialog = _mainWindow->findChild<QDialog*>(QStringLiteral("CGPlaySettingsDialog"))) {
            settingsDialog->setStyleSheet(dialogStyle);
        }
        if (auto* codexWorkspace = _mainWindow->findChild<QWidget*>(QStringLiteral("CodexAgentWorkspace"))) {
            // Codex owns a complete surface stylesheet, including the
            // QScrollArea viewport, header, composer shell, and workbench
            // dialog.  Calling its refresh entry point avoids re-appending a
            // stale constructor stylesheet on every theme change.
            QMetaObject::invokeMethod(codexWorkspace, "refreshTheme", Qt::DirectConnection);
        }
        if (_mainWindow->menuBar()) {
            _mainWindow->menuBar()->setStyleSheet(QStringLiteral("QMenuBar{background:%1;color:%2;} QMenuBar::item:selected{background:%3;} QMenu{background:%4;color:%2;} QMenu::item:selected{background:%3;}")
                .arg(tokens.surfaceToolbar.name(QColor::HexArgb), tokens.textPrimary.name(QColor::HexArgb), tokens.selection.name(QColor::HexArgb), tokens.surfacePanel.name(QColor::HexArgb)));
        }
        if (_mainWindow->statusBar()) {
            _mainWindow->statusBar()->setStyleSheet(QStringLiteral("QStatusBar{background:%1;color:%2;border-top:1px solid %3;} QStatusBar QLabel{color:%2;background:transparent;}")
                .arg(timelineSurfaceCss, tokens.textSecondary.name(QColor::HexArgb), tokens.borderNormal.name(QColor::HexArgb)));
        }
    }
    for (QWidget* widget : allWidgets) {
        if (widget) widget->update();
    }
    const auto applyButtonStyle = [this, &tokens, buttonOpacity, mode](QWidget* widget, const QString& commandId) {
        if (!widget) return;
        const auto colorSetting = [this, &commandId, mode](const QString& field, const QColor& fallback) {
            if (mode != cgplay::ThemeMode::Custom || !_userSettings) return fallback;
            const QColor value(_userSettings->value(QStringLiteral("appearance/%1/%2").arg(field, commandId)).toString());
            return value.isValid() ? value : fallback;
        };
        const auto intSetting = [this, &commandId, mode](const QString& field, int fallback, int minimum, int maximum) {
            if (mode != cgplay::ThemeMode::Custom || !_userSettings) return fallback;
            bool ok = false;
            const int value = _userSettings->value(QStringLiteral("appearance/%1/%2").arg(field, commandId)).toInt(&ok);
            return ok ? qBound(minimum, value, maximum) : fallback;
        };
        const auto boolSetting = [this, &commandId, mode](const QString& field, bool fallback) {
            if (mode != cgplay::ThemeMode::Custom || !_userSettings) return fallback;
            return _userSettings->value(QStringLiteral("appearance/%1/%2").arg(field, commandId), fallback).toBool();
        };
        const int localOpacity = intSetting(QStringLiteral("buttonOpacity"), buttonOpacity, 30, 100);
        const QColor background = cgplay::composeSurface(colorSetting(QStringLiteral("buttonColor"), tokens.surfacePanel), tokens.surfaceToolbar, localOpacity);
        const QColor hover = colorSetting(QStringLiteral("buttonHover"), tokens.accentHover);
        const QColor pressed = colorSetting(QStringLiteral("buttonPressed"), tokens.accentPressed);
        const QColor disabled = colorSetting(QStringLiteral("buttonDisabled"), tokens.textDisabled);
        const QColor text = colorSetting(QStringLiteral("buttonText"), tokens.textPrimary);
        const QColor border = colorSetting(QStringLiteral("buttonBorder"), tokens.borderNormal);
        const QColor icon = colorSetting(QStringLiteral("buttonIcon"), text);
        const int borderOpacity = intSetting(QStringLiteral("buttonBorderOpacity"), border.alpha() * 100 / 255, 0, 100);
        QColor borderSurface = border;
        borderSurface.setAlpha(qRound(borderOpacity * 255.0 / 100.0));
        const int radius = intSetting(QStringLiteral("buttonRadius"), 6, 0, 24);
        const int iconSize = intSetting(QStringLiteral("buttonIconSize"), 18, 8, 64);
        const int buttonWidth = intSetting(QStringLiteral("buttonWidth"), 0, 0, 240);
        const int buttonHeight = intSetting(QStringLiteral("buttonHeight"), 0, 0, 120);
        const bool showText = boolSetting(QStringLiteral("buttonShowText"), true);
        const bool showIcon = boolSetting(QStringLiteral("buttonShowIcon"), true);
        const int shadowStrength = intSetting(QStringLiteral("buttonShadow"), 0, 0, 100);
        widget->setProperty("cgplay.buttonIconColor", icon.name(QColor::HexArgb));
        auto* abstractButton = qobject_cast<QAbstractButton*>(widget);
        if (abstractButton && !widget->property("cgplay.buttonStyleCaptured").toBool()) {
            widget->setProperty("cgplay.originalText", abstractButton->text());
            widget->setProperty("cgplay.originalIcon", QVariant::fromValue(abstractButton->icon()));
            widget->setProperty("cgplay.originalIconSize", abstractButton->iconSize());
            widget->setProperty("cgplay.originalStyleSheet", widget->styleSheet());
            widget->setProperty("cgplay.originalMinimumSize", widget->minimumSize());
            widget->setProperty("cgplay.originalMaximumSize", widget->maximumSize());
            if (auto* tool = qobject_cast<QToolButton*>(widget)) {
                widget->setProperty("cgplay.originalToolButtonStyle", static_cast<int>(tool->toolButtonStyle()));
            }
            widget->setProperty("cgplay.buttonStyleCaptured", true);
        }
        if (abstractButton) {
            const QIcon currentIcon = abstractButton->icon();
            const qint64 lastStyledIconKey = widget->property("cgplay.lastStyledIconKey").toLongLong();
            if (showIcon && !currentIcon.isNull() && currentIcon.cacheKey() != lastStyledIconKey) {
                widget->setProperty("cgplay.originalIcon", QVariant::fromValue(currentIcon));
            }
            abstractButton->setText(showText ? widget->property("cgplay.originalText").toString() : QString());
            const QIcon sourceIcon = widget->property("cgplay.originalIcon").value<QIcon>();
            abstractButton->setIcon(showIcon ? sourceIcon : QIcon());
        }
        if (auto* tool = qobject_cast<QToolButton*>(widget)) {
            tool->setIconSize(QSize(iconSize, iconSize));
            if (!showIcon && !showText) {
                tool->setToolButtonStyle(Qt::ToolButtonTextOnly);
            } else if (!showIcon) {
                tool->setToolButtonStyle(Qt::ToolButtonTextOnly);
            } else if (!showText) {
                tool->setToolButtonStyle(Qt::ToolButtonIconOnly);
            } else {
                tool->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            }
        }
        const QSize originalMinimum = widget->property("cgplay.originalMinimumSize").toSize();
        const QSize originalMaximum = widget->property("cgplay.originalMaximumSize").toSize();
        widget->setMinimumSize(
            buttonWidth > 0 ? buttonWidth : originalMinimum.width(),
            buttonHeight > 0 ? buttonHeight : originalMinimum.height());
        widget->setMaximumSize(
            buttonWidth > 0 ? buttonWidth : originalMaximum.width(),
            buttonHeight > 0 ? buttonHeight : originalMaximum.height());
        if (auto* existingShadow = qobject_cast<QGraphicsDropShadowEffect*>(widget->graphicsEffect())) {
            if (shadowStrength > 0) {
                existingShadow->setBlurRadius(2.0 + shadowStrength * 0.18);
                existingShadow->setColor(QColor(0, 0, 0, qRound(shadowStrength * 1.8)));
                existingShadow->setOffset(0, 2);
            } else {
                widget->setGraphicsEffect(nullptr);
                widget->setProperty("cgplay.buttonShadowApplied", false);
            }
        } else if (shadowStrength > 0) {
            auto* shadow = new QGraphicsDropShadowEffect(widget);
            shadow->setBlurRadius(2.0 + shadowStrength * 0.18);
            shadow->setColor(QColor(0, 0, 0, qRound(shadowStrength * 1.8)));
            shadow->setOffset(0, 2);
            widget->setGraphicsEffect(shadow);
            widget->setProperty("cgplay.buttonShadowApplied", true);
        }
        widget->setStyleSheet(QStringLiteral("QToolButton,QPushButton{background:%1;color:%2;border:1px solid %3;border-radius:%4px;} QToolButton:hover,QPushButton:hover{background:%5;} QToolButton:pressed,QPushButton:pressed,QToolButton:checked,QPushButton:checked{background:%6;color:%7;border-color:%6;} QToolButton:disabled,QPushButton:disabled{background:%8;color:%9;border-color:%3;}")
            .arg(background.name(QColor::HexArgb), text.name(QColor::HexArgb), borderSurface.name(QColor::HexArgb)).arg(radius)
            .arg(hover.name(QColor::HexArgb), pressed.name(QColor::HexArgb), tokens.textOnAccent.name(QColor::HexArgb), disabled.name(QColor::HexArgb), disabled.name(QColor::HexArgb)));
        widget->setProperty("cgplay.customStyleApplied", true);
        if (showIcon && icon.isValid() && !widget->property("cgplay.preserveStateIcon").toBool()) {
            if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
                const QSize size = button->iconSize().isValid() ? button->iconSize() : QSize(iconSize, iconSize);
                QPixmap pixmap = button->icon().pixmap(size, QIcon::Normal, QIcon::Off);
                if (!pixmap.isNull()) {
                    QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
                    for (int y = 0; y < image.height(); ++y) {
                        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
                        for (int x = 0; x < image.width(); ++x) {
                            const int alpha = qAlpha(row[x]);
                            if (alpha > 0) row[x] = qRgba(icon.red(), icon.green(), icon.blue(), alpha);
                        }
                    }
                    button->setIcon(QIcon(QPixmap::fromImage(image)));
                    widget->setProperty("cgplay.lastStyledIconKey", button->icon().cacheKey());
                }
            }
        }
    };
    const auto applyNamedButtonStyle = [&applyButtonStyle, &allWidgets](const QString& objectName, const QString& commandId) {
        for (QWidget* candidate : allWidgets) {
            if (candidate && candidate->objectName() == objectName) applyButtonStyle(candidate, commandId);
        }
    };
    for (QWidget* widget : allWidgets) {
        auto* button = qobject_cast<QAbstractButton*>(widget);
        if (!button) continue;
        if (widget->property("cgplay.buttonStyleCaptured").toBool()) {
            button->setText(widget->property("cgplay.originalText").toString());
            button->setIcon(widget->property("cgplay.originalIcon").value<QIcon>());
            button->setIconSize(widget->property("cgplay.originalIconSize").toSize());
            widget->setMinimumSize(widget->property("cgplay.originalMinimumSize").toSize());
            widget->setMaximumSize(widget->property("cgplay.originalMaximumSize").toSize());
            widget->setStyleSheet(widget->property("cgplay.originalStyleSheet").toString());
            if (auto* tool = qobject_cast<QToolButton*>(widget)) {
                tool->setToolButtonStyle(static_cast<Qt::ToolButtonStyle>(
                    widget->property("cgplay.originalToolButtonStyle").toInt()));
            }
            if (widget->property("cgplay.buttonShadowApplied").toBool()) {
                widget->setGraphicsEffect(nullptr);
                widget->setProperty("cgplay.buttonShadowApplied", false);
            }
            widget->setProperty("cgplay.lastStyledIconKey", QVariant());
        }
        widget->setProperty("cgplay.customStyleApplied", false);
    }
    applyNamedButtonStyle(QStringLiteral("PlaybackBarPlayToggle"), QStringLiteral("playback.toggle"));
    applyNamedButtonStyle(QStringLiteral("PlaybackBarPreviousFrame"), QStringLiteral("playback.previousFrame"));
    applyNamedButtonStyle(QStringLiteral("PlaybackBarNextFrame"), QStringLiteral("playback.nextFrame"));
    applyNamedButtonStyle(QStringLiteral("PlaybackBarTranslationToggle"), QStringLiteral("translation.toggle"));
    applyNamedButtonStyle(QStringLiteral("PlaybackBarTranslationMode"), QStringLiteral("translation.mode"));
    applyNamedButtonStyle(QStringLiteral("PlaybackBarSpeed"), QStringLiteral("playback.setSpeed"));
    applyNamedButtonStyle(QStringLiteral("PlaybackBarLoop"), QStringLiteral("playback.loop"));
    applyNamedButtonStyle(QStringLiteral("PlaybackBarMute"), QStringLiteral("audio.openVolume"));
    for (QWidget* widget : allWidgets) {
        if (!widget || !widget->property("cgplay.customButton").toBool()) continue;
        const QString commandId = widget->property("commandId").toString().trimmed();
        if (!commandId.isEmpty()) applyButtonStyle(widget, commandId);
    }
    if (mode == cgplay::ThemeMode::Custom && _userSettings) {
        const QStringList styleFields = {
            QStringLiteral("buttonColor"), QStringLiteral("buttonHover"),
            QStringLiteral("buttonPressed"), QStringLiteral("buttonDisabled"),
            QStringLiteral("buttonText"), QStringLiteral("buttonIcon"),
            QStringLiteral("buttonBorder"), QStringLiteral("buttonRadius"),
            QStringLiteral("buttonIconSize"), QStringLiteral("buttonOpacity"),
            QStringLiteral("buttonBorderOpacity"), QStringLiteral("buttonShadow"),
            QStringLiteral("buttonWidth"), QStringLiteral("buttonHeight"),
            QStringLiteral("buttonShowText"), QStringLiteral("buttonShowIcon")
        };
        for (QWidget* widget : allWidgets) {
            auto* button = qobject_cast<QAbstractButton*>(widget);
            if (!button || button->objectName().isEmpty()) continue;
            QString styleId = button->property("commandId").toString().trimmed();
            if (styleId.isEmpty()) styleId = button->property("cgplay.style.id").toString().trimmed();
            if (styleId.isEmpty()) continue;
            bool hasOverride = false;
            for (const QString& field : styleFields) {
                if (_userSettings->contains(QStringLiteral("appearance/%1/%2").arg(field, styleId))) {
                    hasOverride = true;
                    break;
                }
            }
            button->setProperty("cgplay.customStyleApplied", hasOverride);
            if (hasOverride) applyButtonStyle(button, styleId);
        }
    }
    if (_mainWindow) {
        const int opacity = qBound(40, _userSettings ? _userSettings->value(QStringLiteral("appearance/windowOpacity"), 100).toInt() : 100, 100);
        _mainWindow->setWindowOpacity(opacity / 100.0);
#ifdef Q_OS_WIN
        const HWND hwnd = reinterpret_cast<HWND>(_mainWindow->winId());
        if (hwnd) {
            LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            if (opacity < 100) {
                if (!(style & WS_EX_LAYERED)) {
                    style |= WS_EX_LAYERED;
                    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style);
                }
                SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(qRound(opacity * 255.0 / 100.0)), LWA_ALPHA);
            } else if (style & WS_EX_LAYERED) {
                SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
        }
#endif
    }
}

void Application::_parseArgs()
{
    const QStringList args = arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args[i];
        auto nextValue = [&](const QString& option) -> QString {
            if (i + 1 >= args.size()) {
                qWarning() << "[Args] Missing value for" << option;
                return {};
            }
            ++i;
            return args[i];
        };

        if (arg == QStringLiteral("--benchmark-playback")) {
            _benchmarkMode = true;
            _benchmarkMedia = nextValue(arg);
        } else if (arg == QStringLiteral("--benchmark-output")) {
            _benchmarkOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--benchmark-duration-ms")) {
            bool ok = false;
            const int value = nextValue(arg).toInt(&ok);
            if (ok && value > 0) {
                _benchmarkDurationMs = value;
            }
        } else if (arg == QStringLiteral("--benchmark-warmup-ms")) {
            bool ok = false;
            const int value = nextValue(arg).toInt(&ok);
            if (ok && value >= 0) {
                _benchmarkWarmupMs = value;
            }
        } else if (arg == QStringLiteral("--capture-ui")) {
            _captureUiMode = true;
        } else if (arg == QStringLiteral("--capture-demo")) {
            _captureUiMode = true;
            _captureUiDemo = true;
        } else if (arg == QStringLiteral("--capture-output")) {
            _captureUiOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--capture-media")) {
            _captureUiMedia = nextValue(arg);
        } else if (arg == QStringLiteral("--capture-delay-ms")) {
            bool ok = false;
            const int value = nextValue(arg).toInt(&ok);
            if (ok && value >= 0) {
                _captureUiDelayMs = value;
            }
        } else if (arg == QStringLiteral("--capture-width")) {
            bool ok = false;
            const int value = nextValue(arg).toInt(&ok);
            if (ok && value > 0) {
                _captureUiWidth = value;
            }
        } else if (arg == QStringLiteral("--capture-height")) {
            bool ok = false;
            const int value = nextValue(arg).toInt(&ok);
            if (ok && value > 0) {
                _captureUiHeight = value;
            }
        } else if (arg == QStringLiteral("--smoke-codex-workbench")) {
            _codexWorkbenchSmokeMode = true;
        } else if (arg == QStringLiteral("--smoke-codex-workbench-output")) {
            _codexWorkbenchSmokeOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--smoke-player")) {
            _playerSmokeMode = true;
            _playerSmokeMedia = nextValue(arg);
        } else if (arg == QStringLiteral("--smoke-output")) {
            _playerSmokeOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--smoke-subtitle-generation")) {
            _subtitleGenerationSmokeMode = true;
            _subtitleGenerationSmokeMedia = nextValue(arg);
        } else if (arg == QStringLiteral("--smoke-subtitle-generation-output")) {
            _subtitleGenerationSmokeOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--smoke-subtitle-generation-mock")) {
            _subtitleGenerationSmokeMock = true;
        } else if (arg == QStringLiteral("--smoke-subtitle-generation-frame")) {
            bool ok = false;
            const int value = nextValue(arg).toInt(&ok);
            if (ok && value >= 0) {
                _subtitleGenerationSmokeFrame = value;
            }
        } else if (arg == QStringLiteral("--smoke-subtitle-generation-cancel-after-ms")) {
            bool ok = false;
            const int value = nextValue(arg).toInt(&ok);
            if (ok && value > 0) {
                _subtitleGenerationSmokeCancelAfterMs = value;
            }
        } else if (arg == QStringLiteral("--smoke-subtitle-generation-playback-sample-ms")) {
            bool ok = false;
            const int value = nextValue(arg).toInt(&ok);
            if (ok && value > 0) {
                _subtitleGenerationSmokePlaybackSampleDurationMs = value;
            }
        } else if (arg == QStringLiteral("--smoke-subtitle-generation-playback-sample-interval-ms")) {
            bool ok = false;
            const int value = nextValue(arg).toInt(&ok);
            if (ok && value > 0) {
                _subtitleGenerationSmokePlaybackSampleIntervalMs = value;
            }
        } else if (arg == QStringLiteral("--smoke-subtitle-generation-playback-mode")) {
            _subtitleGenerationSmokePlaybackMode = nextValue(arg);
        } else if (arg == QStringLiteral("--smoke-subtitle-generation-high-quality-wait-timeout-ms")) {
            bool ok = false;
            const int value = nextValue(arg).toInt(&ok);
            if (ok && value > 0) {
                _subtitleGenerationSmokeHighQualityWaitTimeoutMs = value;
            }
        } else if (arg == QStringLiteral("--smoke-subtitle-generation-high-quality-target-seconds")) {
            bool ok = false;
            const double value = nextValue(arg).toDouble(&ok);
            if (ok && value > 0.0) {
                _subtitleGenerationSmokeHighQualityTargetSeconds = value;
            }
        } else if (arg == QStringLiteral("--smoke-subtitle-generation-mock-online-worker")) {
            _subtitleGenerationSmokeMockOnlineWorker = true;
        } else if (arg == QStringLiteral("--smoke-subtitle-generation-mock-ocr-worker")) {
            _subtitleGenerationSmokeMockOcrWorker = true;
        } else if (arg == QStringLiteral("--smoke-subtitle-generation-mock-repair-worker")) {
            _subtitleGenerationSmokeMockRepairWorker = true;
        } else if (arg == QStringLiteral("--smoke-subtitle-cache-display")) {
            _subtitleCacheDisplaySmokeMode = true;
            _subtitleCacheDisplaySmokeMedia = nextValue(arg);
        } else if (arg == QStringLiteral("--smoke-subtitle-cache-display-output")) {
            _subtitleCacheDisplaySmokeOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--smoke-subtitle-cache-display-frames")) {
            const QStringList parts = nextValue(arg).split(QLatin1Char(','), Qt::SkipEmptyParts);
            _subtitleCacheDisplaySmokeFrames.clear();
            for (const QString& part : parts) {
                bool ok = false;
                const int value = part.trimmed().toInt(&ok);
                if (ok && value >= 0) {
                    _subtitleCacheDisplaySmokeFrames.push_back(value);
                }
            }
        } else if (arg == QStringLiteral("--smoke-subtitle-switch-sequence")) {
            _subtitleSwitchSequenceSmokeMode = true;
            _subtitleSwitchSequenceSmokeMedia =
                nextValue(arg).split(QLatin1Char('|'), Qt::SkipEmptyParts);
        } else if (arg == QStringLiteral("--smoke-subtitle-switch-output")) {
            _subtitleSwitchSequenceSmokeOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--smoke-subtitle-switch-frames")) {
            const QStringList parts = nextValue(arg).split(QLatin1Char(','), Qt::SkipEmptyParts);
            _subtitleSwitchSequenceSmokeFrames.clear();
            for (const QString& part : parts) {
                bool ok = false;
                const int value = part.trimmed().toInt(&ok);
                if (ok && value >= 0) {
                    _subtitleSwitchSequenceSmokeFrames.push_back(value);
                }
            }
        } else if (arg == QStringLiteral("--smoke-subtitle-refined-fallback")) {
            _subtitleRefinedFallbackSmokeMode = true;
            _subtitleRefinedFallbackSmokeMedia = nextValue(arg);
        } else if (arg == QStringLiteral("--smoke-subtitle-refined-fallback-output")) {
            _subtitleRefinedFallbackSmokeOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--smoke-subtitle-refined-fallback-frame")) {
            bool ok = false;
            const int value = nextValue(arg).toInt(&ok);
            if (ok && value >= 0) {
                _subtitleRefinedFallbackSmokeFrame = value;
            }
        } else if (arg == QStringLiteral("--smoke-qwen-asr-provider")) {
            _qwenAsrProviderSmokeMode = true;
        } else if (arg == QStringLiteral("--smoke-qwen-asr-provider-output")) {
            _qwenAsrProviderSmokeOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--smoke-recovery-prompt")) {
            _recoveryPromptSmokeMode = true;
        } else if (arg == QStringLiteral("--smoke-recovery-prompt-output")) {
            _recoveryPromptSmokeOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--check-components")) {
            _componentCheckMode = true;
        } else if (arg == QStringLiteral("--check-components-output")) {
            _componentCheckOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--dump-runtime")) {
            _dumpRuntimeMode = true;
        } else if (arg == QStringLiteral("--dump-runtime-output")) {
            _dumpRuntimeOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--automation-settings-namespace")) {
            QString value = nextValue(arg).trimmed();
            value.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")));
            _automationSettingsNamespace = value.left(64);
        } else if (arg == QStringLiteral("--automation-background")) {
            _backgroundAutomationMode = true;
        } else if (arg == QStringLiteral("--automation-visible")) {
            _automationVisibleMode = true;
            _backgroundAutomationMode = false;
        } else if (arg == QStringLiteral("--disable-plugin-fallback")) {
            _disablePluginFallback = true;
        } else if (arg == QStringLiteral("--overlay-debug")) {
            _overlayDebugMode = true;
        } else if (arg == QStringLiteral("--phase9-15-media")) {
            _phase915Media = nextValue(arg);
        } else if (arg == QStringLiteral("--phase9-15-viewer-rebuild")) {
            _phase915ViewerRebuildMode = true;
        } else if (arg == QStringLiteral("--phase9-15-viewer-rebuild-output")) {
            _phase915ViewerRebuildOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--phase9-15-plugin-reload")) {
            _phase915PluginReloadMode = true;
        } else if (arg == QStringLiteral("--phase9-15-plugin-reload-output")) {
            _phase915PluginReloadOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--phase9-15-fallback-toggle")) {
            _phase915FallbackToggleMode = true;
        } else if (arg == QStringLiteral("--phase9-15-fallback-toggle-output")) {
            _phase915FallbackToggleOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--phase11-performance-baseline")) {
            _phase11PerformanceBaselineMode = true;
        } else if (arg == QStringLiteral("--phase11-performance-baseline-output")) {
            _phase11PerformanceBaselineOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--phase14-media")) {
            _phase14Media = nextValue(arg);
        } else if (arg == QStringLiteral("--phase14-performance-baseline")) {
            _phase14PerformanceBaselineMode = true;
        } else if (arg == QStringLiteral("--phase14-performance-baseline-output")) {
            _phase14PerformanceBaselineOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--phase14-stress")) {
            _phase14StressMode = true;
        } else if (arg == QStringLiteral("--phase14-stress-output")) {
            _phase14StressOutput = nextValue(arg);
        } else if (arg == QStringLiteral("--phase14-viewer-rebuild-iterations")) {
            bool ok = false;
            const int value = nextValue(arg).toInt(&ok);
            if (ok && value >= 0) {
                _phase14ViewerRebuildIterations = value;
            }
        } else if (arg == QStringLiteral("--phase14-window-switch-iterations")) {
            bool ok = false;
            const int value = nextValue(arg).toInt(&ok);
            if (ok && value >= 0) {
                _phase14WindowSwitchIterations = value;
            }
        } else if (arg == QStringLiteral("--phase14-plugin-reload-iterations")) {
            bool ok = false;
            const int value = nextValue(arg).toInt(&ok);
            if (ok && value >= 0) {
                _phase14PluginReloadIterations = value;
            }
        } else if (arg == QStringLiteral("--phase14-fallback-toggle-iterations")) {
            bool ok = false;
            const int value = nextValue(arg).toInt(&ok);
            if (ok && value >= 0) {
                _phase14FallbackToggleIterations = value;
            }
        }
    }
}


} // namespace cgplay
