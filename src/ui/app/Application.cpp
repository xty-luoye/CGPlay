#include "ApplicationInternal.h"
#include "common/jobs/JobSystem.h"
#include <cstdio>

namespace cgplay {

namespace {
template<typename T>
void sortContributions(QVector<T>& items)
{
    std::sort(items.begin(), items.end(), [](const T& lhs, const T& rhs) {
        if (lhs.order != rhs.order) {
            return lhs.order < rhs.order;
        }
        return lhs.id < rhs.id;
    });
}

bool hasMenuContributionForSlot(const QVector<MenuContribution>& items, const QString& slot)
{
    for (const auto& item : items) {
        if (item.menuSlot == slot) {
            return true;
        }
    }
    return false;
}

bool hasToolbarContributionForSlot(const QVector<ToolbarContribution>& items, const QString& slot)
{
    for (const auto& item : items) {
        if (item.toolbarSlot == slot) {
            return true;
        }
    }
    return false;
}

bool hasPanelContributionForSlot(const QVector<PanelContribution>& items, const QString& slot)
{
    for (const auto& item : items) {
        if (item.panelSlot == slot) {
            return true;
        }
    }
    return false;
}

bool selectVideoExportCodec(
    QWidget* parent,
    bool annotated,
    VideoExportCodec* codec,
    QString* suggestedName,
    QString* fileFilter)
{
    if (!codec || !suggestedName || !fileFilter) {
        return false;
    }
    const QStringList labels{
        QStringLiteral("H.264 (MP4)"),
        QStringLiteral("H.265 / HEVC (MP4)"),
        QStringLiteral("Apple ProRes 422 HQ (MOV)"),
        QStringLiteral("Apple ProRes 4444 + Alpha (MOV)")
    };
    bool accepted = false;
    const QString selected = QInputDialog::getItem(
        parent,
        QStringLiteral("导出编码"),
        QStringLiteral("编码格式："),
        labels,
        0,
        false,
        &accepted);
    if (!accepted) {
        return false;
    }

    const int index = labels.indexOf(selected);
    const bool proRes = index >= 2;
    *codec = index == 1
        ? VideoExportCodec::H265
        : index == 2
            ? VideoExportCodec::ProRes422HQ
            : index == 3
                ? VideoExportCodec::ProRes4444
                : VideoExportCodec::H264;
    *suggestedName = annotated
        ? (proRes ? QStringLiteral("annotated_output.mov") : QStringLiteral("annotated_output.mp4"))
        : (proRes ? QStringLiteral("output.mov") : QStringLiteral("output.mp4"));
    *fileFilter = proRes
        ? QStringLiteral("MOV 视频 (*.mov)")
        : QStringLiteral("MP4 视频 (*.mp4)");
    return true;
}

} // namespace

MainWindow::MainWindow(std::shared_ptr<AnnotationManager> annoMgr,
                       std::shared_ptr<OcioManager>          ocio,
                       std::shared_ptr<CacheManager>         cache,
                       std::shared_ptr<IMediaService>        mediaService,
                       std::shared_ptr<ISettingsService>     windowSettings,
                       std::shared_ptr<ISettingsService>     userSettings,
                       QWidget* parent)
    : QMainWindow(parent), _p(std::make_unique<Private>()),
      _annoMgr(annoMgr ? std::move(annoMgr) : ServiceLocator::getSharedService<AnnotationManager>())
{
    QElapsedTimer startupTimer;
    startupTimer.start();
    qint64 previousStartupPhaseMs = 0;
    const auto recordStartupPhase = [&](const char* stage) {
        if (!qApp || !qApp->property("cgplay.overlayDebug").toBool()) return;
        const qint64 elapsedMs = startupTimer.elapsed();
        std::fprintf(stderr, "[StartupTiming] {\"scope\":\"MainWindow\",\"stage\":\"%s\",\"elapsedMs\":%lld,\"phaseMs\":%lld}\n",
            stage, static_cast<long long>(elapsedMs), static_cast<long long>(elapsedMs - previousStartupPhaseMs));
        std::fflush(stderr);
        previousStartupPhaseMs = elapsedMs;
    };
    setWindowTitle("CGPlay");
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setMinimumSize(1120, 680);
    setAcceptDrops(true);

    _p->ocioManager = ocio ? std::move(ocio) : ServiceLocator::getSharedService<OcioManager>();
    if (!_p->ocioManager) {
        _p->ocioManager = std::make_shared<OcioManager>();
        ServiceLocator::registerService<OcioManager>(_p->ocioManager);
    }

    _p->cacheManager = cache ? std::move(cache) : ServiceLocator::getSharedService<CacheManager>();
    if (!_p->cacheManager) {
        _p->cacheManager = std::make_shared<CacheManager>();
        ServiceLocator::registerService<CacheManager>(_p->cacheManager);
    }

    _p->mediaService = mediaService ? std::move(mediaService) : ServiceLocator::getSharedService<IMediaService>();
    _p->windowSettings = windowSettings ? std::move(windowSettings) : ServiceLocator::getSharedService<ISettingsService>(Application::kWindowSettingsService);
    _p->userSettings = userSettings ? std::move(userSettings) : ServiceLocator::getSharedService<ISettingsService>(Application::kUserSettingsService);
    _p->inputBindings = std::make_unique<InputBindingStore>();
    _p->inputBindings->load();
    _p->settingsProfileService = std::make_unique<SettingsProfileService>();
    _p->appearanceController = std::make_unique<AppearanceController>(*_p->settingsProfileService);
    _p->workspaceController = std::make_unique<WorkspaceController>(*_p->settingsProfileService);
    _p->toolbarController = std::make_unique<ToolbarController>(*_p->settingsProfileService);
    _p->commandRegistry = std::make_unique<CommandRegistry>();
    _p->commandDispatcher = std::make_unique<CommandDispatcher>(_p->commandRegistry.get());
    // XInput is an optional runtime source. Polling keeps unsupported devices harmless
    // while making configured gamepad edges dispatch through the same command mapping.
#ifdef Q_OS_WIN
    _p->gamepadPollTimer = new QTimer(this);
    connect(_p->gamepadPollTimer, &QTimer::timeout, this, [this]() {
        XINPUT_STATE state{};
        const DWORD status = XInputGetState(0, &state);
        const WORD buttons = status == ERROR_SUCCESS ? state.Gamepad.wButtons : 0;
        const WORD rising = static_cast<WORD>(buttons & ~_p->gamepadButtons);
        _p->gamepadButtons = buttons;
        if (!_p->inputBindings || rising == 0) return;
        const std::pair<WORD, const char*> bindings[] = {
            {XINPUT_GAMEPAD_A, "A"}, {XINPUT_GAMEPAD_B, "B"},
            {XINPUT_GAMEPAD_X, "X"}, {XINPUT_GAMEPAD_Y, "Y"},
            {XINPUT_GAMEPAD_LEFT_SHOULDER, "LB"}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB"},
            {XINPUT_GAMEPAD_BACK, "Back"}, {XINPUT_GAMEPAD_START, "Start"},
            {XINPUT_GAMEPAD_LEFT_THUMB, "L3"}, {XINPUT_GAMEPAD_RIGHT_THUMB, "R3"},
            {XINPUT_GAMEPAD_DPAD_UP, "DPadUp"}, {XINPUT_GAMEPAD_DPAD_DOWN, "DPadDown"},
            {XINPUT_GAMEPAD_DPAD_LEFT, "DPadLeft"}, {XINPUT_GAMEPAD_DPAD_RIGHT, "DPadRight"},
        };
        for (const auto& binding : bindings) {
            if (!(rising & binding.first)) continue;
            const QString command = _p->inputBindings->commandFor(
                InputBindingStore::Device::Gamepad, QString::fromLatin1(binding.second));
            if (!command.isEmpty()) _executeCommandId(command);
        }
    });
    _p->gamepadPollTimer->start(50);
#endif
    _p->playbackCtrl = ServiceLocator::getSharedService<IPlaybackService>();
    if (!_p->playbackCtrl) {
        _p->playbackCtrl = std::make_shared<PlaybackController>(_p->cacheManager, _p->ocioManager);
        ServiceLocator::registerService<IPlaybackService>(_p->playbackCtrl);
    }
    recordStartupPhase("playbackServices");

    auto* performanceService = ServiceLocator::getService<PerformanceService>();
    if (!performanceService) {
        _p->performanceService = new PerformanceService(this);
        ServiceLocator::registerService<PerformanceService>(_p->performanceService);
    } else {
        _p->performanceService = performanceService;
    }

    _loadHostExtensionContributions();
    _p->annotationService = ServiceLocator::getService<IAnnotationService>();
    recordStartupPhase("extensions");
    _setupUI();
    recordStartupPhase("setupUi");
    _setupMenuBar();
    recordStartupPhase("setupMenu");
    _setupStatusBar();
    recordStartupPhase("setupStatus");

    _connectSignals();
    recordStartupPhase("connectSignals");
    _restoreState();
    recordStartupPhase("restoreState");

    auto* sessionService = ServiceLocator::getService<ISessionService>();
    if (!sessionService) {
        _p->sessionMgr = new SessionManager(this, _annoMgr, _p->ocioManager, this);
        ServiceLocator::registerService<ISessionService>(_p->sessionMgr);
    } else {
        _p->sessionMgr = sessionService;
    }
    _p->sessionMgr->enableAutoSave();
    _scheduleBackgroundUpdateCheck();

    if (!qApp->property("cgplay.benchmarkMode").toBool() &&
        _p->sessionMgr->hasRecoverySession()) {
        auto* prompt = createRecoverySessionPromptBox(this);
        const auto result = static_cast<QMessageBox::StandardButton>(prompt->exec());
        prompt->deleteLater();
        if (result == QMessageBox::Yes) _p->sessionMgr->loadRecovery();
        else _p->sessionMgr->clearRecovery();
    }
    recordStartupPhase("complete");
}

MainWindow::~MainWindow()
{
    if (_p->updateCheckState) _p->updateCheckState->cancelled.store(true);
    if (_p->updateDownloadState) _p->updateDownloadState->cancelled.store(true);
    ++_p->mediaProbeGeneration;
    if (_p->mediaProbeJob) {
        _p->mediaProbeJob->cancel();
        _p->mediaProbeJob.clear();
    }
    if (_p->subtitleGenerationCancelRequested) {
        _p->subtitleGenerationCancelRequested->store(true);
    }
    if (_p->subtitleRefinementCancelRequested) {
        _p->subtitleRefinementCancelRequested->store(true);
    }
    if (_p->highQualityEnhancementCancelRequested) {
        _p->highQualityEnhancementCancelRequested->store(true);
    }
    _saveState();
    if (_p->sessionMgr) {
        _p->sessionMgr->clearRecovery();
    }
}

PlaylistPanel* MainWindow::playlistPanel() const { return _p->playlist; }
IPlaybackService* MainWindow::playbackController() const { return _p->playbackCtrl.get(); }
CompareToolbar* MainWindow::compareToolbar() const { return _p->compareBar; }
ViewerWidget* MainWindow::viewerWidget() const { return _p->viewer; }

QMessageBox* MainWindow::createRecoverySessionPromptForSmoke()
{
    return createRecoverySessionPromptBox(this);
}

bool MainWindow::rebuildViewerForRuntime()
{
    if (!_p->viewerShell || !_p->viewerShellLayout || !_p->playbackCtrl) {
        return false;
    }

    if (_p->viewer) {
        _p->viewer->invalidateView();
        _p->viewerShellLayout->removeWidget(_p->viewer);
        _p->viewer->deleteLater();
        _p->viewer = nullptr;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    auto* viewer = new ViewerWidget(_p->playbackCtrl, _p->ocioManager, _p->viewerShell);
    _p->viewerShellLayout->insertWidget(0, viewer, 1);
    _p->viewer = viewer;

    connect(_p->viewer, &ViewerWidget::resolutionChanged, this, [this](int w, int h) {
        if (_p->lblRes) {
            _p->lblRes->setText(QString("%1x%2").arg(w).arg(h));
        }
        if (_p->topBar) {
            _p->topBar->resolutionLabel()->setText(QString("%1x%2").arg(w).arg(h));
        }
    });
    connect(_p->viewer, &ViewerWidget::droppedFile, this, [this](const QString& p) {
        QFileInfo fi(p);
        if (_p->currentPath.isEmpty()) {
            _p->compareBar->setShotALabel(fi.completeBaseName());
            openFile(p);
        } else {
            _p->compareBar->setShotBLabel(fi.completeBaseName());
            _p->playbackCtrl->setCompareFile(p);
            _p->compareBar->setCompareMode(5);
        }
    });
    connect(_p->viewer, &ViewerWidget::compareRequested, this, [this] {
        _p->compareBar->setVisible(!_p->compareBar->isVisible());
    });
    connect(_p->viewer, &ViewerWidget::compareTileRequested, this, [this] {
        _p->compareBar->show();
        _p->compareBar->setCompareMode(7);
    });
    connect(_p->viewer, &ViewerWidget::fullscreenRequested, this, [this] {
        _toggleFullScreen();
    });

    _p->viewer->show();
    _p->viewer->setFocus(Qt::OtherFocusReason);
    _setupGeneratedSubtitleOverlay();
    _layoutGeneratedSubtitleOverlay();
    return true;
}

void MainWindow::refreshAnnotationCapabilityForRuntime()
{
    _p->annotationService = ServiceLocator::getService<IAnnotationService>();
    const bool capabilityAvailable = hasAnnotationCapability(_p->annotationService, _annoMgr.get());

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    if (!capabilityAvailable) {
        if (_p->annoToolbar) {
            _p->annoToolbar->hide();
            _p->annoToolbar->deleteLater();
        }
        if (_p->reviewPanel) {
            _p->reviewPanel->hide();
            _p->reviewPanel->setParent(nullptr);
            _p->reviewPanel->deleteLater();
        }
        _p->annoToolbar = nullptr;
        _p->reviewPanel = nullptr;
        _onAnnotationModeToggled(false);
        return;
    }

    _loadHostExtensionContributions();

    if (!_p->reviewPanel) {
        _applyPanelContributions();
        if (_p->reviewPanel && _p->horzSplitter && _p->horzSplitter->indexOf(_p->reviewPanel) < 0) {
            _p->horzSplitter->addWidget(_p->reviewPanel);
        }
    }

    if (_p->reviewPanel) {
        _connectReviewPanelSignals(_p->reviewPanel);
    }

    if (_p->reviewPanel && !_p->annoToolbar) {
        _applyToolbarContributions();
    }

    _cleanupDuplicateReviewPanels();

    if (_p->reviewPanel && _p->horzSplitter) {
        const int reviewIndex = _p->horzSplitter->indexOf(_p->reviewPanel);
        if (reviewIndex >= 0) {
            _p->horzSplitter->setCollapsible(reviewIndex, true);
            _p->horzSplitter->setStretchFactor(reviewIndex, 0);
        }
    }
    if (_p->reviewPanel) {
        _p->reviewPanel->show();
    }
    if (_p->annoToolbar) {
        _p->annoToolbar->setVisible(_p->annoToolsVisible);
    }
    _onAnnotationModeToggled(_p->annoToolbar && _p->annoToolsVisible);
}

void MainWindow::_setupUI()
{
    auto* central = createThemedBackdrop(this);
    auto* mainVLayout = new QVBoxLayout(central);
    mainVLayout->setContentsMargins(0, 0, 0, 0);
    mainVLayout->setSpacing(0);
    _p->topBar = new TopBar(central);
    _p->topBar->setObjectName(QStringLiteral("cgplayTopBarSurface"));
    _p->topBar->setAttribute(Qt::WA_StyledBackground, true);
    _p->topBar->setProperty("cgplay.surfaceRole", QStringLiteral("toolbar"));
    mainVLayout->addWidget(_p->topBar, 0);

    auto* customToolbar = new QWidget(central);
    customToolbar->setObjectName(QStringLiteral("cgplayCustomToolbar"));
    customToolbar->setAttribute(Qt::WA_StyledBackground, true);
    customToolbar->setProperty("cgplay.surfaceRole", QStringLiteral("toolbar"));
    auto* customToolbarLayout = new QHBoxLayout(customToolbar);
    customToolbarLayout->setContentsMargins(16, 4, 16, 4);
    customToolbarLayout->setSpacing(6);
    customToolbarLayout->addStretch();
    customToolbar->hide();
    mainVLayout->addWidget(customToolbar, 0);
    _rebuildCustomToolbar();

    auto* contentHost = new QWidget(central);
    contentHost->setObjectName(QStringLiteral("cgplayContentHost"));
    contentHost->setAttribute(Qt::WA_StyledBackground, true);
    contentHost->setStyleSheet("background: transparent;");
    auto* contentLayout = new QVBoxLayout(contentHost);
    contentLayout->setContentsMargins(16, 0, 16, 8);
    contentLayout->setSpacing(0);

    _p->horzSplitter = new QSplitter(Qt::Horizontal, contentHost);
    _p->horzSplitter->setObjectName(QStringLiteral("cgplayMainSplitter"));
    _p->horzSplitter->setHandleWidth(8);
    _p->horzSplitter->setChildrenCollapsible(true);
    _p->horzSplitter->setOpaqueResize(true);
    _p->horzSplitter->setStyleSheet(
        "QSplitter::handle{background:transparent;border:none;margin:0;padding:0;}"
        "QSplitter::handle:hover{background:rgba(255,138,61,0.22);border:none;}");

    _p->navRail = new NavigationRail(_p->horzSplitter);
    _p->navRail->setFixedWidth(0);
    _p->navRail->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    _p->navRail->hide();

    _p->playlist = new PlaylistPanel(_p->playbackCtrl, _p->horzSplitter);
    _p->playlist->setObjectName(QStringLiteral("cgplayPlaylistSurface"));
    _p->playlist->setProperty("cgplay.surfaceRole", QStringLiteral("panel"));
    _p->playlist->setMinimumWidth(0);
    _p->playlist->setMaximumWidth(520);
    _p->playlist->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

    _p->centerSplitter = new QSplitter(Qt::Vertical, _p->horzSplitter);
    _p->centerSplitter->setObjectName(QStringLiteral("cgplayCenterSplitter"));
    auto* centerSplitter = _p->centerSplitter;
    centerSplitter->setHandleWidth(1);
    centerSplitter->setChildrenCollapsible(false);
    centerSplitter->setOpaqueResize(true);
    centerSplitter->setStyleSheet(
        "QSplitter::handle{background:rgba(255,255,255,0.03);}"
        "QSplitter::handle:hover{background:rgba(255,140,50,0.22);}");

    auto* viewerShell = new QWidget(centerSplitter);
    viewerShell->setObjectName(QStringLiteral("cgplayViewerShell"));
    viewerShell->setAttribute(Qt::WA_StyledBackground, true);
    viewerShell->setProperty("cgplay.surfaceRole", QStringLiteral("viewer"));
    viewerShell->setStyleSheet(QStringLiteral("QWidget#cgplayViewerShell{background:%1;}").arg(viewerSurfaceColor().name(QColor::HexArgb)));
    auto* viewerShellLayout = new QVBoxLayout(viewerShell);
    viewerShellLayout->setContentsMargins(0, 0, 0, 0);
    viewerShellLayout->setSpacing(0);
    _p->viewerShell = viewerShell;
    _p->viewerShellLayout = viewerShellLayout;

    _p->viewer = new ViewerWidget(_p->playbackCtrl, _p->ocioManager, viewerShell);
    viewerShellLayout->addWidget(_p->viewer, 1);

    _p->compareBar = new CompareToolbar(viewerShell);
    _p->compareBar->setFixedHeight(34);
    _p->compareBar->hide();
    viewerShellLayout->addWidget(_p->compareBar, 0);

    _p->timeline = new TimelineWidget(_p->playbackCtrl, centerSplitter);
    _p->timeline->setObjectName(QStringLiteral("cgplayTimelineSurface"));
    _p->timeline->setProperty("cgplay.surfaceRole", QStringLiteral("timeline"));
    if (_p->ocioManager) {
        _p->timeline->setPreviewTransformSettings(_p->ocioManager->previewTransformSettings());
    }
    _p->playbackBar = new PlaybackBar(_p->playbackCtrl, centerSplitter);
    _p->playbackBar->setObjectName(QStringLiteral("cgplayPlaybackBarSurface"));
    _p->playbackBar->setProperty("cgplay.surfaceRole", QStringLiteral("toolbar"));
    const QVector<std::pair<QString, QString>> transportButtons = {
        {QStringLiteral("PlaybackBarPlayToggle"), QStringLiteral("playback.toggle")},
        {QStringLiteral("PlaybackBarPreviousFrame"), QStringLiteral("playback.previousFrame")},
        {QStringLiteral("PlaybackBarNextFrame"), QStringLiteral("playback.nextFrame")},
    };
    for (const auto& [objectName, commandId] : transportButtons) {
        auto* button = _p->playbackBar->findChild<QToolButton*>(objectName);
        if (!button) continue;
        QObject::disconnect(button, nullptr, _p->playbackBar, nullptr);
        connect(button, &QToolButton::clicked, this, [this, commandId] {
            _executeCommandId(commandId);
        });
    }
    const QVector<std::pair<QString, QString>> viewerZoomButtons = {
        {QStringLiteral("ViewerZoomOut"), QStringLiteral("view.zoomOut")},
        {QStringLiteral("ViewerZoomIn"), QStringLiteral("view.zoomIn")},
    };
    for (const auto& [objectName, commandId] : viewerZoomButtons) {
        auto* button = _p->viewer->findChild<QToolButton*>(objectName);
        if (!button) continue;
        QObject::disconnect(button, nullptr, _p->viewer, nullptr);
        connect(button, &QToolButton::clicked, this, [this, commandId] {
            _executeCommandId(commandId);
        });
    }
    _p->translationPlaybackMode = TranslationPlaybackStrategy::normalizeModeId(
        _p->windowSettings
            ? _p->windowSettings->value(
                  QStringLiteral("ai/subtitles/playbackMode"),
                  TranslationPlaybackStrategy::quickPlaybackModeId()).toString()
            : TranslationPlaybackStrategy::quickPlaybackModeId());
    _p->playbackBar->setTranslationMode(_p->translationPlaybackMode);
    connect(_p->playbackBar, &PlaybackBar::translationModeChanged, this, [this](const QString& modeId) {
        _setTranslationPlaybackMode(modeId);
    });
    connect(_p->playbackBar, &PlaybackBar::translationVisibilityToggled, this, [this](bool visible) {
        _onPlaybackTranslationToggled(visible);
        if (_p->windowSettings) {
            _p->windowSettings->setValue(QStringLiteral("ai/subtitles/translationVisible"), visible);
        }
    });
    centerSplitter->setStretchFactor(0, 1);
    centerSplitter->setStretchFactor(1, 0);
    centerSplitter->setStretchFactor(2, 0);
    centerSplitter->setSizes({900, 110, 54});

    _p->horzSplitter->addWidget(_p->navRail);
    _p->horzSplitter->addWidget(_p->playlist);
    _p->horzSplitter->addWidget(centerSplitter);

    _applyPanelContributions();
    if (_p->reviewPanel && _p->horzSplitter->indexOf(_p->reviewPanel) < 0) {
        _p->reviewPanel->setMinimumWidth(0);
        _p->reviewPanel->setMaximumWidth(520);
        _p->reviewPanel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        _p->horzSplitter->addWidget(_p->reviewPanel);
    }

    _p->horzSplitter->setCollapsible(0, true);
    _p->horzSplitter->setCollapsible(1, true);
    _p->horzSplitter->setCollapsible(2, false);
    if (_p->reviewPanel) {
        _p->horzSplitter->setCollapsible(3, true);
    }

    _p->horzSplitter->setStretchFactor(0, 0);
    _p->horzSplitter->setStretchFactor(1, 0);
    _p->horzSplitter->setStretchFactor(2, 1);
    if (_p->reviewPanel) {
        _p->horzSplitter->setStretchFactor(3, 0);
        _p->horzSplitter->setSizes({0, 320, 1200, 320});
    } else {
        _p->horzSplitter->setSizes({0, 320, 1520});
    }
    for (int i = 1; i < _p->horzSplitter->count(); ++i) {
        if (auto* handle = _p->horzSplitter->handle(i)) handle->setCursor(Qt::SplitHCursor);
    }
    connect(_p->horzSplitter, &QSplitter::splitterMoved, this, [this](int, int) {
        if (_p->sidePanelLayoutApplying || !_p->horzSplitter) return;
        const QList<int> sizes = _p->horzSplitter->sizes();
        if (sizes.size() < 3) return;
        if (sizes[1] > 0) _p->lastLeftPanelWidth = qBound(1, sizes[1], 520);
        const bool leftVisible = sizes[1] > 0;
        const bool leftVisibilityChanged = _p->leftVisible != leftVisible;
        _p->leftVisible = leftVisible;
        if (_p->reviewPanel && sizes.size() > 3) {
            if (sizes.constLast() > 0) _p->lastRightPanelWidth = qBound(1, sizes.constLast(), 520);
            const bool rightVisible = sizes.constLast() > 0;
            const bool rightVisibilityChanged = _p->rightVisible != rightVisible;
            _p->rightVisible = rightVisible;
            if (_p->rightPanelAction && rightVisibilityChanged) {
                const QSignalBlocker blocker(_p->rightPanelAction);
                _p->rightPanelAction->setChecked(_p->rightVisible);
            }
        }
        if (_p->leftPanelAction && leftVisibilityChanged) {
            const QSignalBlocker blocker(_p->leftPanelAction);
            _p->leftPanelAction->setChecked(_p->leftVisible);
        }
    });

    contentLayout->addWidget(_p->horzSplitter, 1);
    mainVLayout->addWidget(contentHost, 1);

    _applyToolbarContributions();
    _cleanupDuplicateReviewPanels();

    setCentralWidget(central);
    _setupAIAgentWorkspace();
    _setupGeneratedSubtitleOverlay();

    connect(_p->navRail, &NavigationRail::pageChanged, this, [this](int page) {
        _p->compareBar->setVisible(page == NavigationRail::Compare);
        switch (page) {
        case NavigationRail::Playlist:
            _p->leftVisible = true;
            _p->activeSidePanel = 1;
            break;
        case NavigationRail::Review:
        case NavigationRail::Compare:
            _p->rightVisible = true;
            _p->activeSidePanel = 2;
            if (_p->reviewPanel) _p->reviewPanel->setCurrentTab(0);
            break;
        case NavigationRail::Versions:
            _p->rightVisible = true;
            _p->activeSidePanel = 2;
            if (_p->reviewPanel) _p->reviewPanel->setCurrentTab(1);
            break;
        case NavigationRail::Timeline:
            _p->rightVisible = true;
            _p->activeSidePanel = 2;
            if (_p->reviewPanel) _p->reviewPanel->setCurrentTab(2);
            break;
        case NavigationRail::Settings:
            QTimer::singleShot(0, this, [this]() {
                if (_p->topBar && _p->topBar->settingsButton()) {
                    _p->topBar->settingsButton()->click();
                }
            });
            break;
        }
        _applyAdaptiveSidePanelLayout(true);
    });
    connect(_p->topBar->settingsButton(), &QPushButton::clicked, this, [this] {
        if (!_p->settingsDialog) _installSettingsWidgets(_p->reviewPanel);
        if (_p->settingsDialog) {
            qApp->setProperty("cgplay.deferAppearanceRefresh", true);
            auto syncCheck = [this](const QString& objectName, bool checked) {
                if (auto* check = _p->settingsDialog->findChild<QCheckBox*>(objectName)) {
                    const QSignalBlocker blocker(check);
                    check->setChecked(checked);
                }
            };
            syncCheck(QStringLiteral("SettingsPlaylistVisible"), _p->leftVisible);
            syncCheck(QStringLiteral("SettingsReviewVisible"), _p->rightVisible);
            syncCheck(QStringLiteral("SettingsAnnotationVisible"), _p->annoToolbar && !_p->annoToolbar->isHidden());
            syncCheck(QStringLiteral("SettingsAIVisible"), _p->aiDock && !_p->aiDock->isHidden());
            for (auto* scroll : _p->settingsDialog->findChildren<QScrollArea*>()) {
                if (scroll && scroll->verticalScrollBar()) scroll->verticalScrollBar()->setValue(0);
            }
            _p->settingsDialog->show();
            _p->settingsDialog->raise();
            _p->settingsDialog->activateWindow();
        }
    });

    _p->fullscreenChromeTimer = new QTimer(this);
    _p->fullscreenChromeTimer->setSingleShot(true);
    _p->fullscreenChromeTimer->setInterval(2000);
    connect(_p->fullscreenChromeTimer, &QTimer::timeout, this, &MainWindow::_hideFullScreenChrome);
    qApp->installEventFilter(this);
}

void MainWindow::_setupAIAgentWorkspace()
{
    auto* eventBus = ServiceLocator::getService<IEventBus>();
    auto* providerManager = ServiceLocator::getService<IAIProviderManager>();
    auto* workflowService = ServiceLocator::getService<IAIWorkflowService>();

    _p->aiDock = createThemedDockWidget(QStringLiteral("AI 工作台"), this);
    _p->aiDock->setObjectName(QStringLiteral("AIAgentWorkspaceDock"));
    _p->aiDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    _p->aiDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    _p->aiDock->setMinimumWidth(420);
    _p->aiDock->setBaseSize(460, 0);
    _p->aiDock->setAttribute(Qt::WA_StyledBackground, true);
    _p->aiDock->setProperty("cgplay.surfaceRole", QStringLiteral("dock"));
    const QColor dockSurface = dialogSurfaceColor();
    const QColor dockTitle = compositeOpaque(
        appColorProperty("cgplay.toolbarColor"), dockSurface.darker(110));
    const QColor dockText = compositeOpaque(
        appColorProperty("cgplay.textColor"), qApp ? qApp->palette().color(QPalette::Text) : QColor("#D8DEE7"));
    const QColor dockBorder = compositeOpaque(
        appColorProperty("cgplay.borderColor"), dockText.darker(180));
    _p->aiDock->setStyleSheet(QStringLiteral(
        "QDockWidget{background:%1;color:%2;border:0;}"
        "QDockWidget::title{background:%4;padding:10px 12px;color:%2;font-weight:600;"
             "border:0;}" )
        .arg(dockSurface.name(QColor::HexArgb), dockText.name(QColor::HexArgb),
             dockBorder.name(QColor::HexArgb), dockTitle.name(QColor::HexArgb)));

    const auto ensureWorkspace = [this, providerManager, workflowService, eventBus] {
        if (_p->aiWorkspace) return;
        _p->aiWorkspace = new AIAgentWorkspace(
            providerManager,
            workflowService,
            _p->playbackCtrl.get(),
            ServiceLocator::getService<IAICredentialStore>(),
            _p->userSettings.get(),
            eventBus,
            resolveAnnotationService(_p->annotationService, _annoMgr.get()),
            _p->aiDock);
        _p->aiDock->setWidget(_p->aiWorkspace);
    };

    const bool eagerLegacyWorkspace =
        qApp && qApp->property("cgplay.qwenAsrProviderSmokeMode").toBool();
    if (eagerLegacyWorkspace) {
        ensureWorkspace();
    }
    addDockWidget(Qt::RightDockWidgetArea, _p->aiDock);
    resizeDocks({_p->aiDock}, {460}, Qt::Horizontal);
    // AI tools are opt-in for each session, including builds without Codex.
    _p->aiDock->hide();

    connect(_p->aiDock, &QDockWidget::visibilityChanged, this, [this, ensureWorkspace](bool visible) {
        if (_p->windowSettings) {
            _p->windowSettings->setValue(QStringLiteral("ai/workspaceVisible"), visible);
        }
        if (visible) {
            ensureWorkspace();
        }
        QTimer::singleShot(0, this, [this] { _applyAdaptiveSidePanelLayout(true); });
    });
}

void MainWindow::_setupMenuBar()
{
    auto* mb = menuBar();
    mb->setNativeMenuBar(false);
    mb->hide();

    auto tagCommandAction = [this](QAction* action, const QString& commandId) {
        if (!action || commandId.isEmpty()) {
            return action;
        }
        action->setObjectName(commandId);
        action->setProperty("cgplay.command.id", commandId);
        action->setProperty("commandId", commandId);
        if (_p->userSettings) {
            const QString key = QStringLiteral("shortcuts/%1").arg(commandId);
            if (_p->userSettings->contains(key)) {
                // An explicitly empty override means the user cleared the shortcut.
                action->setShortcut(QKeySequence(_p->userSettings->value(key).toString().trimmed()));
            }
        }
        return action;
    };

    auto* fileMenu = mb->addMenu(QStringLiteral("文件"));
    fileMenu->addAction(QStringLiteral("打开..."), this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("打开媒体"),
            {},
            MediaProbe::mediaFileDialogFilter());
        if (!path.isEmpty()) {
            openFile(path);
        }
    })->setShortcut(QKeySequence::Open);

    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("新建会话"), this, [this] {
        if (_p->sessionMgr->hasUnsavedChanges()) {
            if (QMessageBox::question(
                    this,
                    QStringLiteral("未保存更改"),
                    QStringLiteral("要放弃未保存的更改吗？")) != QMessageBox::Yes) {
                return;
            }
        }
        _p->sessionMgr->newSession();
    })->setShortcut(QKeySequence::New);

    fileMenu->addAction(QStringLiteral("保存会话"), this, [this] {
        _p->sessionMgr->save();
    })->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    fileMenu->addAction(QStringLiteral("会话另存为..."), this, [this] {
        _p->sessionMgr->saveAs();
    });

    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("保存批注..."), this, [this] {
        if (!_p->currentPath.isEmpty()) {
            auto* annotationService = resolveAnnotationService(_p->annotationService, _annoMgr.get());
            AnnotationStorage::save(
                annotationService ? annotationService->annotations() : QVector<AnnotationItem>{},
                AnnotationStorage::getReviewPath(_p->currentPath));
        }
    })->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
    fileMenu->addAction(QStringLiteral("加载批注..."), this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("加载批注"),
            {},
            tr("批注 JSON (*.review.json)"));
        if (!path.isEmpty()) {
            if (auto* annotationService = resolveAnnotationService(_p->annotationService, _annoMgr.get())) {
                annotationService->replaceAnnotations(AnnotationStorage::load(path));
            }
        }
    });

    auto* exportMenu = fileMenu->addMenu(QStringLiteral("导出"));
    exportMenu->addAction(QStringLiteral("导出 JSON..."), this, [this] {
        auto* annotationService = resolveAnnotationService(_p->annotationService, _annoMgr.get());
        if (!annotationService || annotationService->count() == 0) {
            return;
        }
        const QString path = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("导出 JSON"),
            QStringLiteral("review_export.json"),
            tr("JSON (*.json)"));
        if (!path.isEmpty()) {
            ReviewExport::exportJson(annotationService->annotations(), path);
        }
    });
    exportMenu->addAction(QStringLiteral("导出 HTML..."), this, [this] {
        auto* annotationService = resolveAnnotationService(_p->annotationService, _annoMgr.get());
        if (!annotationService || annotationService->count() == 0) {
            return;
        }
        const QString path = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("导出 HTML"),
            QStringLiteral("review_report.html"),
            tr("HTML (*.html)"));
        if (!path.isEmpty()) {
            ReviewExport::exportHtml(annotationService->annotations(), path, QFileInfo(_p->currentPath).fileName());
        }
    });

    exportMenu->addSeparator();
    auto* exportVideoAction = exportMenu->addAction(QStringLiteral("导出视频..."), this, [this] {
        if (_p->currentPath.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("导出视频"), QStringLiteral("请先打开一个媒体文件。"));
            return;
        }

        VideoExportCodec codec = VideoExportCodec::H264;
        QString suggestedName;
        QString fileFilter;
        if (!selectVideoExportCodec(this, false, &codec, &suggestedName, &fileFilter)) {
            return;
        }
        const QString outPath = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("导出视频"),
            suggestedName,
            fileFilter);
        if (outPath.isEmpty()) {
            return;
        }

        bool ok = false;
        const double srcFps = ReviewExport::detectFps(_p->currentPath);
        const double fps = QInputDialog::getDouble(
            this,
            QStringLiteral("输出帧率"),
            QStringLiteral("输出帧率（0 = 自动检测）："),
            srcFps,
            0,
            120,
            2,
            &ok,
            Qt::WindowFlags(),
            0.1);
        if (!ok) {
            return;
        }

        int firstFrame = QInputDialog::getInt(
            this,
            QStringLiteral("起始帧"),
            QStringLiteral("起始帧（从 0 开始）："),
            0,
            0,
            999999,
            1,
            &ok);
        if (!ok) {
            return;
        }

        // Some backends report an unknown/placeholder frame count (0 or 1).
        // Use ffprobe before defaulting to a single-frame export.
        int detectedFrameCount = _p->playbackCtrl ? _p->playbackCtrl->totalFrames() : 0;
        if (detectedFrameCount <= 1) {
            detectedFrameCount = ReviewExport::detectFrameCount(_p->currentPath);
        }
        const int defaultLast = detectedFrameCount > 1
            ? detectedFrameCount - 1
            : 999999;
        int lastFrame = QInputDialog::getInt(
            this,
            QStringLiteral("结束帧"),
            QStringLiteral("结束帧（包含该帧）："),
            defaultLast,
            0,
            999999,
            1,
            &ok);
        if (!ok) {
            return;
        }

        if (detectedFrameCount > 0) {
            firstFrame = qBound(0, firstFrame, detectedFrameCount - 1);
            lastFrame = qBound(0, lastFrame, detectedFrameCount - 1);
        }
        if (firstFrame > lastFrame) {
            std::swap(firstFrame, lastFrame);
        }
        CGPLAY_LOG().exportStarted("clean", outPath, firstFrame, lastFrame);

        auto* progressDlg = new QProgressDialog(
            QStringLiteral("正在导出视频..."),
            QStringLiteral("取消"),
            0,
            100,
            this);
        progressDlg->setWindowModality(Qt::WindowModal);
        progressDlg->setMinimumDuration(0);
        progressDlg->show();

        std::atomic_bool cancelled{false};
        auto future = QtConcurrent::run([&, outPath, firstFrame, lastFrame, fps, codec]() -> bool {
            return ReviewExport::exportVideoClean(
                _p->currentPath,
                outPath,
                { firstFrame, lastFrame },
                fps,
                codec,
                [progressDlg](double pct) {
                    QMetaObject::invokeMethod(
                        progressDlg,
                        "setValue",
                        Qt::QueuedConnection,
                        Q_ARG(int, static_cast<int>(pct * 100)));
                },
                &cancelled);
        });

        QFutureWatcher<bool> watcher;
        QEventLoop exportLoop;
        connect(progressDlg, &QProgressDialog::canceled, this, [&cancelled]() {
            cancelled.store(true, std::memory_order_relaxed);
        });
        connect(&watcher, &QFutureWatcher<bool>::finished, &exportLoop, &QEventLoop::quit);
        watcher.setFuture(future);
        if (!future.isFinished()) {
            exportLoop.exec();
        }

        const bool success = watcher.result();
        progressDlg->close();
        progressDlg->deleteLater();

        if (success) {
            QMessageBox::information(
                this,
                QStringLiteral("导出"),
                QStringLiteral("视频导出成功。\n%1").arg(outPath));
            CGPLAY_LOG().exportFinished(true);
        } else if (!cancelled.load(std::memory_order_relaxed)) {
            QString detail = ReviewExport::lastError();
            if (detail.isEmpty()) {
                detail = QStringLiteral("没有可用的详细错误信息。");
            }
            if (detail.length() > 800) {
                detail = detail.left(800) + QStringLiteral("...");
            }
            QString message = QStringLiteral("视频导出失败。\n\n错误详情：\n%1").arg(detail);
            const bool missingDependency = detail.contains(QStringLiteral("Missing dependencies"), Qt::CaseInsensitive)
                || detail.contains(QStringLiteral("NOT INSTALLED"), Qt::CaseInsensitive)
                || detail.contains(QStringLiteral("No module named"), Qt::CaseInsensitive);
            if (missingDependency) {
                message += QStringLiteral("\n\n依赖：pip install OpenImageIO av numpy pillow");
            }
            message += QStringLiteral("\n\n详细日志请查看输出目录中的 export_error.log。");
            QMessageBox::warning(
                this,
                QStringLiteral("导出"),
                message);
            CGPLAY_LOG().exportFinished(false);
        } else {
            CGPLAY_LOG().exportCancelled();
        }
    });
    exportVideoAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));

    exportMenu->addAction(QStringLiteral("导出带批注视频..."), this, [this] {
        auto* annotationService = resolveAnnotationService(_p->annotationService, _annoMgr.get());
        if (!annotationService || annotationService->count() == 0) {
            QMessageBox::information(this, QStringLiteral("导出"), QStringLiteral("当前没有可导出的批注。"));
            return;
        }
        if (_p->currentPath.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("导出视频"), QStringLiteral("请先打开一个媒体文件。"));
            return;
        }

        VideoExportCodec codec = VideoExportCodec::H264;
        QString suggestedName;
        QString fileFilter;
        if (!selectVideoExportCodec(this, true, &codec, &suggestedName, &fileFilter)) {
            return;
        }
        const QString outPath = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("导出带批注视频"),
            suggestedName,
            fileFilter);
        if (outPath.isEmpty()) {
            return;
        }

        bool ok = false;
        const double srcFps = ReviewExport::detectFps(_p->currentPath);
        const double fps = QInputDialog::getDouble(
            this,
            QStringLiteral("输出帧率"),
            QStringLiteral("输出帧率（0 = 自动检测）："),
            srcFps,
            0,
            120,
            2,
            &ok,
            Qt::WindowFlags(),
            0.1);
        if (!ok) {
            return;
        }

        int firstFrame = QInputDialog::getInt(
            this,
            QStringLiteral("起始帧"),
            QStringLiteral("起始帧（从 0 开始）："),
            0,
            0,
            999999,
            1,
            &ok);
        if (!ok) {
            return;
        }

        int detectedFrameCount = _p->playbackCtrl ? _p->playbackCtrl->totalFrames() : 0;
        if (detectedFrameCount <= 1) {
            detectedFrameCount = ReviewExport::detectFrameCount(_p->currentPath);
        }
        const int defaultLast = detectedFrameCount > 1
            ? detectedFrameCount - 1
            : 999999;
        int lastFrame = QInputDialog::getInt(
            this,
            QStringLiteral("结束帧"),
            QStringLiteral("结束帧（包含该帧）："),
            defaultLast,
            0,
            999999,
            1,
            &ok);
        if (!ok) {
            return;
        }

        if (detectedFrameCount > 0) {
            firstFrame = qBound(0, firstFrame, detectedFrameCount - 1);
            lastFrame = qBound(0, lastFrame, detectedFrameCount - 1);
        }
        if (firstFrame > lastFrame) {
            std::swap(firstFrame, lastFrame);
        }
        const auto ocioOpts = _p->ocioManager ? _p->ocioManager->currentOptions() : tl::OCIOOptions{};
        CGPLAY_LOG().exportStarted("annotated", outPath, firstFrame, lastFrame);

        auto* progressDlg = new QProgressDialog(
            QStringLiteral("正在导出带批注视频..."),
            QStringLiteral("取消"),
            0,
            100,
            this);
        progressDlg->setWindowModality(Qt::WindowModal);
        progressDlg->setMinimumDuration(0);
        progressDlg->show();

        std::atomic_bool cancelled{false};
        auto future = QtConcurrent::run([&, outPath, firstFrame, lastFrame, fps, ocioOpts, codec]() -> bool {
            return ReviewExport::exportVideoAnnotated(
                annotationService ? annotationService->annotations() : QVector<AnnotationItem>{},
                _p->currentPath,
                outPath,
                _p->viewer->mediaW(),
                _p->viewer->mediaH(),
                { firstFrame, lastFrame },
                fps,
                ocioOpts,
                codec,
                [progressDlg](double pct) {
                    QMetaObject::invokeMethod(
                        progressDlg,
                        "setValue",
                        Qt::QueuedConnection,
                        Q_ARG(int, static_cast<int>(pct * 100)));
                },
                &cancelled);
        });

        QFutureWatcher<bool> watcher;
        QEventLoop exportLoop;
        connect(progressDlg, &QProgressDialog::canceled, this, [&cancelled]() {
            cancelled.store(true, std::memory_order_relaxed);
        });
        connect(&watcher, &QFutureWatcher<bool>::finished, &exportLoop, &QEventLoop::quit);
        watcher.setFuture(future);
        if (!future.isFinished()) {
            exportLoop.exec();
        }

        const bool success = watcher.result();
        progressDlg->close();
        progressDlg->deleteLater();

        if (success) {
            QMessageBox::information(
                this,
                QStringLiteral("导出"),
                QStringLiteral("视频导出成功。\n%1").arg(outPath));
            CGPLAY_LOG().exportFinished(true);
        } else if (!cancelled.load(std::memory_order_relaxed)) {
            QString detail = ReviewExport::lastError();
            if (detail.isEmpty()) {
                detail = QStringLiteral("没有可用的详细错误信息。");
            }
            if (detail.length() > 800) {
                detail = detail.left(800) + QStringLiteral("...");
            }
            QString message = QStringLiteral("视频导出失败。\n\n错误详情：\n%1").arg(detail);
            const bool missingDependency = detail.contains(QStringLiteral("Missing dependencies"), Qt::CaseInsensitive)
                || detail.contains(QStringLiteral("NOT INSTALLED"), Qt::CaseInsensitive)
                || detail.contains(QStringLiteral("No module named"), Qt::CaseInsensitive);
            if (missingDependency) {
                message += QStringLiteral("\n\n依赖：pip install OpenImageIO av numpy pillow");
            }
            message += QStringLiteral("\n\n详细日志请查看输出目录中的 export_error.log。");
            QMessageBox::warning(
                this,
                QStringLiteral("导出"),
                message);
            CGPLAY_LOG().exportFinished(false);
        } else {
            CGPLAY_LOG().exportCancelled();
        }
    })->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));

    fileMenu->addSeparator();
    _p->videoExportAction = exportVideoAction;
    _p->renderExportAction = fileMenu->addAction(QStringLiteral("渲染当前范围..."));
    connect(_p->renderExportAction, &QAction::triggered, this, [this] {
        _executeCommandId(QStringLiteral("export.render"));
    });
    _p->renderExportAction->setObjectName(QStringLiteral("export.render"));
    _p->renderExportAction->setProperty("commandId", QStringLiteral("export.render"));
    fileMenu->addAction(QStringLiteral("退出"), this, &MainWindow::close)->setShortcut(QKeySequence::Quit);

    auto* viewMenu = mb->addMenu(QStringLiteral("视图"));
    auto* fitAction = viewMenu->addAction(QStringLiteral("适应窗口"));
    connect(fitAction, &QAction::triggered, this, [this] { _executeCommandId(QStringLiteral("view.fitToWindow")); });
    fitAction->setShortcut(Qt::Key_F); tagCommandAction(fitAction, QStringLiteral("view.fitToWindow"));
    auto* zoomAction = viewMenu->addAction(tr("1:1"));
    connect(zoomAction, &QAction::triggered, this, [this] { _executeCommandId(QStringLiteral("view.zoom1to1")); });
    zoomAction->setShortcut(Qt::Key_1); tagCommandAction(zoomAction, QStringLiteral("view.zoom1to1"));
    auto* zoomInAction = viewMenu->addAction(QStringLiteral("放大"));
    connect(zoomInAction, &QAction::triggered, this, [this] { _executeCommandId(QStringLiteral("view.zoomIn")); });
    zoomInAction->setShortcut(QKeySequence(QStringLiteral("+"))); tagCommandAction(zoomInAction, QStringLiteral("view.zoomIn"));
    auto* zoomOutAction = viewMenu->addAction(QStringLiteral("缩小"));
    connect(zoomOutAction, &QAction::triggered, this, [this] { _executeCommandId(QStringLiteral("view.zoomOut")); });
    zoomOutAction->setShortcut(QKeySequence(QStringLiteral("-"))); tagCommandAction(zoomOutAction, QStringLiteral("view.zoomOut"));
    auto* fullscreenAction = viewMenu->addAction(QStringLiteral("全屏"));
    connect(fullscreenAction, &QAction::triggered, this, [this] { _executeCommandId(QStringLiteral("view.fullscreen")); });
    fullscreenAction->setShortcut(Qt::Key_F11); tagCommandAction(fullscreenAction, QStringLiteral("view.fullscreen"));
    auto* customizeWorkspace = viewMenu->addAction(QStringLiteral("自定义工作区"));
    customizeWorkspace->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_W));
    connect(customizeWorkspace, &QAction::triggered, this, [this] {
        _installSettingsWidgets(_p->reviewPanel);
        if (_p->settingsDialog) {
            qApp->setProperty("cgplay.deferAppearanceRefresh", true);
            for (auto* scroll : _p->settingsDialog->findChildren<QScrollArea*>()) {
                if (scroll && scroll->verticalScrollBar()) scroll->verticalScrollBar()->setValue(0);
            }
            _p->settingsDialog->show();
            _p->settingsDialog->raise();
            _p->settingsDialog->activateWindow();
        }
    });
    _p->workspaceSaveAction = viewMenu->addAction(QStringLiteral("保存工作区"));
    connect(_p->workspaceSaveAction, &QAction::triggered, this, [this] {
        _executeCommandId(QStringLiteral("workspace.save"));
    });
    _p->workspaceSaveAction->setObjectName(QStringLiteral("workspace.save"));
    _p->workspaceSaveAction->setProperty("commandId", QStringLiteral("workspace.save"));
    _p->workspaceResetAction = viewMenu->addAction(QStringLiteral("恢复默认工作区"));
    connect(_p->workspaceResetAction, &QAction::triggered, this, [this] {
        _executeCommandId(QStringLiteral("workspace.reset"));
    });
    _p->workspaceResetAction->setObjectName(QStringLiteral("workspace.reset"));
    _p->workspaceResetAction->setProperty("commandId", QStringLiteral("workspace.reset"));
    viewMenu->addSeparator();

    auto* actLeft = viewMenu->addAction(QStringLiteral("左侧面板"));
    _p->leftPanelAction = actLeft;
    actLeft->setCheckable(true);
    actLeft->setChecked(true);
    connect(actLeft, &QAction::toggled, this, [this](bool v) {
        _p->leftVisible = v;
        if (v) {
            _p->activeSidePanel = 1;
        }
        _applyAdaptiveSidePanelLayout(true);
    });

    auto* actRight = viewMenu->addAction(QStringLiteral("右侧面板"));
    _p->rightPanelAction = actRight;
    actRight->setCheckable(true);
    actRight->setChecked(_p->reviewPanel != nullptr);
    actRight->setEnabled(_p->reviewPanel != nullptr);
    connect(actRight, &QAction::toggled, this, [this](bool v) {
        _p->rightVisible = v;
        if (v) {
            _p->activeSidePanel = 2;
        }
        _applyAdaptiveSidePanelLayout(true);
    });

    viewMenu->addSeparator();
    auto* actAnno = viewMenu->addAction(QStringLiteral("批注工具"));
    actAnno->setCheckable(true);
    actAnno->setChecked(_p->annoToolbar != nullptr);
    actAnno->setEnabled(_p->annoToolbar != nullptr);
    connect(actAnno, &QAction::toggled, this, [this](bool v) {
        _p->annoToolsVisible = v;
        if (!isFullScreen() && _p->annoToolbar) {
            _p->annoToolbar->setVisible(v);
        }
        _onAnnotationModeToggled(v && _p->annoToolbar);
    });

    auto* actCmp = viewMenu->addAction(QStringLiteral("对比工具栏"));
    actCmp->setCheckable(true);
    actCmp->setChecked(false);
    connect(actCmp, &QAction::toggled, _p->compareBar, &QWidget::setVisible);

    if (_p->aiDock) {
        viewMenu->addSeparator();
        QAction* aiWorkspaceAction = _p->aiDock->toggleViewAction();
        aiWorkspaceAction->setText(QStringLiteral("AI 工作台"));
        aiWorkspaceAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
        viewMenu->addAction(aiWorkspaceAction);
    }
    if (auto* codexDock = findChild<QDockWidget*>(QStringLiteral("CodexAgentWorkspaceDock"))) {
        QAction* codexWorkspaceAction = codexDock->toggleViewAction();
        codexWorkspaceAction->setText(QStringLiteral("Codex 工作台"));
        viewMenu->addAction(codexWorkspaceAction);
    }
    _p->translationToggleAction = viewMenu->addAction(QStringLiteral("字幕翻译"));
    tagCommandAction(_p->translationToggleAction, QStringLiteral("translation.toggle"));
    _p->translationToggleAction->setCheckable(true);
    _p->translationToggleAction->setChecked(_p->playbackBar && _p->playbackBar->translationVisible());
    _p->translationToggleAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    connect(_p->translationToggleAction, &QAction::toggled, this, [this](bool visible) {
        _onPlaybackTranslationToggled(visible);
        if (_p->windowSettings) {
            _p->windowSettings->setValue(QStringLiteral("ai/subtitles/translationVisible"), visible);
        }
    });

    auto* windowMenu = mb->addMenu(QStringLiteral("窗口"));
    windowMenu->addAction(QStringLiteral("新建窗口"), this, [this] {
        if (auto* a = qobject_cast<Application*>(qApp)) {
            a->openNewWindow();
        }
    })->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));

    auto* otioMenu = mb->addMenu(QStringLiteral("OTIO"));
    otioMenu->addAction(QStringLiteral("导入 OTIO..."), this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入 OTIO"), {}, tr("OTIO (*.otio)"));
        if (!path.isEmpty()) {
            OtioImporter importer;
            importer.importToModel(path, _p->playlist->model());
        }
    });
    otioMenu->addAction(QStringLiteral("导出 OTIO..."), this, [this] {
        if (_p->playlist->model()->rowCount() == 0) {
            return;
        }
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出 OTIO"), QStringLiteral("timeline.otio"), tr("OTIO (*.otio)"));
        if (!path.isEmpty()) {
            OtioExporter exporter;
            exporter.exportToFile(path, _p->playlist->model());
        }
    });

    auto* ocioMenu = mb->addMenu(QStringLiteral("颜色"));
    if (auto* ocioAction = ocioMenu->menuAction()) {
        ocioAction->setProperty("cgplay.menu.slot", HostExtensionSlots::kMenuColor);
    }
    const bool pluginProvidesOcioMenu = hasMenuContributionForSlot(_p->menuContributions, HostExtensionSlots::kMenuColor);
    if (!pluginProvidesOcioMenu) {
        auto* ocioToggle = ocioMenu->addAction(QStringLiteral("启用 OCIO"));
        ocioToggle->setCheckable(true);
        ocioToggle->setChecked(_p->ocioManager->isEnabled());
        connect(ocioToggle, &QAction::toggled, _p->ocioManager.get(), &OcioManager::setEnabled);
        connect(_p->ocioManager.get(), &OcioManager::enabledChanged, ocioToggle, &QAction::setChecked);

        auto* alphaToggle = ocioMenu->addAction(QStringLiteral("查看 Alpha 通道"));
        alphaToggle->setCheckable(true);
        alphaToggle->setChecked(false);
        connect(alphaToggle, &QAction::toggled, _p->viewer, &ViewerWidget::setAlphaChannelVisible);
        connect(_p->viewer, &ViewerWidget::alphaChannelChanged, alphaToggle, &QAction::setChecked);
        ocioMenu->addAction(QStringLiteral("OCIO 设置..."), this, [this] {
            _p->ocioManager->showSettings(this);
        });
    }
    _applyMenuContributions(mb);

    auto* audioMenu = mb->addMenu(QStringLiteral("音频"));
    auto* muteAction = audioMenu->addAction(QStringLiteral("静音切换"));
    connect(muteAction, &QAction::triggered, this, [this] { _executeCommandId(QStringLiteral("audio.toggleMute")); });
    muteAction->setShortcut(Qt::Key_M); tagCommandAction(muteAction, QStringLiteral("audio.toggleMute"));
    audioMenu->addAction(QStringLiteral("设备..."), this, [this] {
        const auto devs = _p->playbackCtrl->availableAudioDevices();
        if (devs.isEmpty()) {
            return;
        }
        bool ok = false;
        const QString selected = QInputDialog::getItem(
            this,
            QStringLiteral("音频设备"),
            QStringLiteral("设备："),
            devs,
            0,
            false,
            &ok);
        if (ok) {
            _p->playbackCtrl->setAudioDevice(selected);
        }
    });

    auto* pbMenu = mb->addMenu(QStringLiteral("播放"));
    auto* playAction = pbMenu->addAction(QStringLiteral("播放 / 暂停"));
    connect(playAction, &QAction::triggered, this, [this] { _executeCommandId(QStringLiteral("playback.toggle")); });
    tagCommandAction(playAction, QStringLiteral("playback.toggle"));
    auto* prevAction = pbMenu->addAction(QStringLiteral("上一帧"));
    connect(prevAction, &QAction::triggered, this, [this] { _executeCommandId(QStringLiteral("playback.previousFrame")); });
    prevAction->setShortcut(Qt::Key_Left); tagCommandAction(prevAction, QStringLiteral("playback.previousFrame"));
    auto* nextAction = pbMenu->addAction(QStringLiteral("下一帧"));
    connect(nextAction, &QAction::triggered, this, [this] { _executeCommandId(QStringLiteral("playback.nextFrame")); });
    nextAction->setShortcut(Qt::Key_Right); tagCommandAction(nextAction, QStringLiteral("playback.nextFrame"));
    auto* startAction = pbMenu->addAction(QStringLiteral("跳到开头"));
    connect(startAction, &QAction::triggered, this, [this] { _executeCommandId(QStringLiteral("playback.gotoStart")); });
    startAction->setShortcut(Qt::Key_Home); tagCommandAction(startAction, QStringLiteral("playback.gotoStart"));
    auto* endAction = pbMenu->addAction(QStringLiteral("跳到结尾"));
    connect(endAction, &QAction::triggered, this, [this] { _executeCommandId(QStringLiteral("playback.gotoEnd")); });
    endAction->setShortcut(Qt::Key_End); tagCommandAction(endAction, QStringLiteral("playback.gotoEnd"));

    auto* hwMenu = pbMenu->addMenu(QStringLiteral("硬件解码"));
    auto updateHw = [hwMenu] {
        hwMenu->clear();
        auto* app = qobject_cast<Application*>(qApp);
        if (!app) {
            return;
        }
        auto hw = app->hwDecodeManager();
        if (!hw) {
            return;
        }
        for (int i = 0; i <= static_cast<int>(HwAccelType::Auto); ++i) {
            const auto t = static_cast<HwAccelType>(i);
            auto* action = hwMenu->addAction(hwAccelName(t), [hw, t] { hw->setHwAccelType(t); });
            action->setCheckable(true);
            action->setChecked(hw->hwAccelType() == t);
        }
    };
    updateHw();
    if (auto* playbackSignals = _p->playbackCtrl ? _p->playbackCtrl->signalProxy() : nullptr) {
        connect(playbackSignals, &PlaybackServiceSignals::fileOpened, this, [updateHw](const QString&) {
            updateHw();
        });
    }

    auto* helpMenu = mb->addMenu(QStringLiteral("帮助"));
    helpMenu->addAction(QStringLiteral("检查版本更新"), this, [this] {
        _checkForUpdates(true);
    });
    helpMenu->addAction(QStringLiteral("检查缺失组件"), this, [this] {
        _checkVersionAndComponents(true);
    });
    helpMenu->addSeparator();
    helpMenu->addAction(QStringLiteral("使用说明"), this, [this] {
        const QString appDir = QCoreApplication::applicationDirPath();
        const QString manualName = QStringLiteral("CGPlay_User_Guide_1.0.7.html");
        const QStringList candidates = {
            QDir(appDir).filePath(QStringLiteral("docs/") + manualName),
            QDir(appDir).absoluteFilePath(QStringLiteral("../../../docs/") + manualName),
            QDir::current().absoluteFilePath(QStringLiteral("docs/") + manualName),
            QDir(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation))
                .filePath(QStringLiteral("CGPlay_使用说明_1.0.7.html"))
        };

        for (const QString& candidate : candidates) {
            const QFileInfo manual(candidate);
            if (manual.isFile() && QDesktopServices::openUrl(QUrl::fromLocalFile(manual.absoluteFilePath()))) {
                return;
            }
        }

        QMessageBox::warning(
            this,
            QStringLiteral("使用说明不可用"),
            QStringLiteral("未找到离线使用说明。请修复或重新安装 CGPlay。\n\n应存在于：\n%1")
                .arg(QDir(appDir).filePath(QStringLiteral("docs/") + manualName)));
    });
    helpMenu->addAction(QStringLiteral("联系方式 / 反馈 Bug"), this, [this] {
        QMessageBox::information(
            this,
            QStringLiteral("联系方式"),
            QStringLiteral("如有 Bug 或使用问题，请联系：\n2196026568@qq.com"));
    });
    helpMenu->addAction(QStringLiteral("关于 CGPlay"), this, [this] {
        showHelpDocument(this, QStringLiteral("关于 CGPlay"), cgplayAboutHtml());
    });

    auto bindPopup = [this](QPushButton* button, QMenu* menu) {
        if (!button || !menu) {
            return;
        }
        connect(button, &QPushButton::clicked, this, [button, menu] {
            menu->popup(button->mapToGlobal(QPoint(0, button->height() - 2)));
        });
    };

    bindPopup(_p->topBar ? _p->topBar->fileMenuBtn() : nullptr, fileMenu);
    bindPopup(_p->topBar ? _p->topBar->viewMenuBtn() : nullptr, viewMenu);
    bindPopup(_p->topBar ? _p->topBar->windowMenuBtn() : nullptr, windowMenu);
    bindPopup(_p->topBar ? _p->topBar->otioMenuBtn() : nullptr, otioMenu);
    bindPopup(_p->topBar ? _p->topBar->colorMenuBtn() : nullptr, ocioMenu);
    bindPopup(_p->topBar ? _p->topBar->audioMenuBtn() : nullptr, audioMenu);
    bindPopup(_p->topBar ? _p->topBar->playMenuBtn() : nullptr, pbMenu);
    bindPopup(_p->topBar ? _p->topBar->helpMenuBtn() : nullptr, helpMenu);

    if (_p->topBar && _p->topBar->menuButton()) {
        auto* quickMenu = new QMenu(this);
        for (QMenu* menu : {fileMenu, viewMenu, windowMenu, otioMenu, ocioMenu, audioMenu, pbMenu, helpMenu}) {
            if (menu && menu->menuAction()) quickMenu->addAction(menu->menuAction());
        }
        connect(_p->topBar->menuButton(), &QPushButton::clicked, this, [this, quickMenu] {
            quickMenu->popup(
                _p->topBar->menuButton()->mapToGlobal(QPoint(0, _p->topBar->menuButton()->height() - 2)));
        });
    }

    // The command descriptor is the source of truth for both defaults and
    // user overrides. Apply it after every menu action has been created so a
    // cleared or rebound shortcut cannot be replaced by a hardcoded QAction
    // default during startup.
    for (const auto& descriptor : _p->commandDescriptors) {
        for (QAction* action : findChildren<QAction*>(descriptor.id)) {
            if (!action) continue;
            action->setShortcut(QKeySequence(descriptor.shortcut));
            action->setShortcutContext(Qt::WindowShortcut);
        }
    }
    _refreshCommandPresentation();
}

void MainWindow::_loadHostExtensionContributions()
{
    auto* pluginManager = ServiceLocator::getService<PluginManager>();
    if (!_p->commandRegistry) {
        _p->commandRegistry = std::make_unique<CommandRegistry>();
    }
    if (!_p->commandDispatcher) {
        _p->commandDispatcher = std::make_unique<CommandDispatcher>(_p->commandRegistry.get());
    }
    _p->commandDispatcher->setRegistry(_p->commandRegistry.get());
    _p->commandRegistry->clear();
    _p->commandDispatcher->clearHandlers();
    CommandRegistry& registry = *_p->commandRegistry;
    CommandDispatcher& dispatcher = *_p->commandDispatcher;

    // Built-in commands share the same IDs used by shortcut and style settings.
    // Their menu actions are created in _setupMenuBar, while the descriptors make
    // them visible to command management and input configuration.
    auto addBuiltIn = [&registry, &dispatcher](CommandDescriptor descriptor, CommandDispatcher::Handler handler) {
        const QString id = descriptor.id;
        if (!registry.registerCommand(descriptor)) return;
        if (!dispatcher.registerHandler(id, std::move(handler))) {
            qWarning() << "[Commands] Missing or duplicate built-in handler:" << id;
        }
    };
    auto withPlayback = [this](std::function<void(IPlaybackService&)> operation) {
        return [this, operation = std::move(operation)](bool) {
            if (!_p->playbackCtrl) return false;
            operation(*_p->playbackCtrl);
            return true;
        };
    };

    addBuiltIn({QStringLiteral("playback.toggle"), QStringLiteral("播放/暂停"), {}, {}, QStringLiteral("play"), QStringLiteral("Space")},
               withPlayback([](IPlaybackService& playback) { playback.togglePlay(); }));
    addBuiltIn({QStringLiteral("playback.previousFrame"), QStringLiteral("上一帧"), {}, {}, QStringLiteral("skip-back"), QStringLiteral("Left")},
               withPlayback([](IPlaybackService& playback) { playback.prevFrame(); }));
    addBuiltIn({QStringLiteral("playback.nextFrame"), QStringLiteral("下一帧"), {}, {}, QStringLiteral("skip-forward"), QStringLiteral("Right")},
               withPlayback([](IPlaybackService& playback) { playback.nextFrame(); }));
    addBuiltIn({QStringLiteral("playback.gotoStart"), QStringLiteral("跳到开头"), {}, {}, {}, QStringLiteral("Home")},
               withPlayback([](IPlaybackService& playback) { playback.gotoStart(); }));
    addBuiltIn({QStringLiteral("playback.gotoEnd"), QStringLiteral("跳到结尾"), {}, {}, {}, QStringLiteral("End")},
               withPlayback([](IPlaybackService& playback) { playback.gotoEnd(); }));
    addBuiltIn({QStringLiteral("playback.reverse"), QStringLiteral("倒放"), {}, {}, {}, QStringLiteral("J")},
               withPlayback([](IPlaybackService& playback) { playback.reverse(); }));
    addBuiltIn({QStringLiteral("playback.stop"), QStringLiteral("停止"), {}, {}, {}, QStringLiteral("K")},
               withPlayback([](IPlaybackService& playback) { playback.stop(); }));
    addBuiltIn({QStringLiteral("playback.forward"), QStringLiteral("正放"), {}, {}, {}, QStringLiteral("L")},
               withPlayback([](IPlaybackService& playback) { playback.forward(); }));
    addBuiltIn({QStringLiteral("playback.seekBackward10"), QStringLiteral("后退 10 帧"), {}, {}, {}, QStringLiteral("Shift+Left")},
               withPlayback([](IPlaybackService& playback) { playback.seekRelative(-10); }));
    addBuiltIn({QStringLiteral("playback.seekForward10"), QStringLiteral("前进 10 帧"), {}, {}, {}, QStringLiteral("Shift+Right")},
               withPlayback([](IPlaybackService& playback) { playback.seekRelative(10); }));
    addBuiltIn({QStringLiteral("playback.setInPoint"), QStringLiteral("设置入点"), {}, {}, {}, QStringLiteral("I")},
               withPlayback([](IPlaybackService& playback) { playback.setInPoint(playback.currentFrame()); }));
    addBuiltIn({QStringLiteral("playback.setOutPoint"), QStringLiteral("设置出点"), {}, {}, {}, QStringLiteral("O")},
               withPlayback([](IPlaybackService& playback) { playback.setOutPoint(playback.currentFrame()); }));
    addBuiltIn({QStringLiteral("playback.clearInPoint"), QStringLiteral("清除入点"), {}, {}, {}, QStringLiteral("Alt+I")},
               withPlayback([](IPlaybackService& playback) { playback.clearInPoint(); }));
    addBuiltIn({QStringLiteral("playback.clearOutPoint"), QStringLiteral("清除出点"), {}, {}, {}, QStringLiteral("Alt+O")},
               withPlayback([](IPlaybackService& playback) { playback.clearOutPoint(); }));
    addBuiltIn({QStringLiteral("audio.toggleMute"), QStringLiteral("静音切换"), {}, {}, QStringLiteral("volume-x"), QStringLiteral("M")},
               withPlayback([](IPlaybackService& playback) { playback.setMute(!playback.isMuted()); }));
    addBuiltIn({QStringLiteral("audio.openVolume"), QStringLiteral("打开音量控制"), {}, {}, QStringLiteral("volume-2")}, [this](bool) {
        if (auto* volume = findChild<QToolButton*>(QStringLiteral("PlaybackBarMute"))) { volume->click(); return true; }
        return false;
    });
    addBuiltIn({QStringLiteral("view.fitToWindow"), QStringLiteral("适应窗口"), {}, {}, QStringLiteral("maximize"), QStringLiteral("F")}, [this](bool) {
        if (!_p->viewer) return false;
        _p->viewer->fitToWindow();
        return true;
    });
    addBuiltIn({QStringLiteral("view.zoom1to1"), QStringLiteral("1:1"), {}, {}, {}, QStringLiteral("1")}, [this](bool) {
        if (!_p->viewer) return false;
        _p->viewer->zoom1to1();
        return true;
    });
    addBuiltIn({QStringLiteral("view.zoomIn"), QStringLiteral("放大"), {}, {}, QStringLiteral("zoom-in"), QStringLiteral("+")}, [this](bool) {
        if (!_p->viewer) return false;
        _p->viewer->zoomIn();
        return true;
    });
    addBuiltIn({QStringLiteral("view.zoomOut"), QStringLiteral("缩小"), {}, {}, QStringLiteral("zoom-out"), QStringLiteral("-")}, [this](bool) {
        if (!_p->viewer) return false;
        _p->viewer->zoomOut();
        return true;
    });
    addBuiltIn({QStringLiteral("view.fullscreen"), QStringLiteral("全屏"), {}, {}, QStringLiteral("fullscreen"), QStringLiteral("F11")}, [this](bool) {
        _toggleFullScreen();
        return true;
    });
    addBuiltIn({QStringLiteral("translation.toggle"), QStringLiteral("字幕翻译"), {}, {}, {}, QStringLiteral("Ctrl+Shift+T")}, [this](bool) {
        if (!_p->translationToggleAction) return false;
        _p->translationToggleAction->trigger();
        return true;
    });
    addBuiltIn({QStringLiteral("annotation.create"), QStringLiteral("创建批注")}, [this](bool) {
        if (auto* service = resolveAnnotationService(_p->annotationService, _annoMgr.get())) {
            service->createNote(QStringLiteral("新建批注"));
            return true;
        }
        return false;
    });
    addBuiltIn({QStringLiteral("annotation.delete"), QStringLiteral("删除批注"), {}, {}, {}, QStringLiteral("Delete")}, [this](bool) {
        if (auto* service = resolveAnnotationService(_p->annotationService, _annoMgr.get())) return service->removeSelectedAnnotation();
        return false;
    });
    addBuiltIn({QStringLiteral("annotation.undo"), QStringLiteral("撤销批注"), {}, {}, {}, QStringLiteral("Ctrl+Z")}, [this](bool) {
        if (auto* service = resolveAnnotationService(_p->annotationService, _annoMgr.get())) { service->undo(); return true; }
        return false;
    });
    addBuiltIn({QStringLiteral("annotation.redo"), QStringLiteral("重做批注"), {}, {}, {}, QStringLiteral("Ctrl+Y")}, [this](bool) {
        if (auto* service = resolveAnnotationService(_p->annotationService, _annoMgr.get())) { service->redo(); return true; }
        return false;
    });

    const QVector<std::pair<CommandDescriptor, AnnotationToolbar::Tool>> annotationTools = {
        {{QStringLiteral("annotation.tool.select"), QStringLiteral("选择工具"), {}, {}, {}, QStringLiteral("Alt+S")}, AnnotationToolbar::Select},
        {{QStringLiteral("annotation.tool.arrow"), QStringLiteral("箭头工具"), {}, {}, {}, QStringLiteral("Alt+A")}, AnnotationToolbar::Arrow},
        {{QStringLiteral("annotation.tool.rectangle"), QStringLiteral("矩形工具"), {}, {}, {}, QStringLiteral("Alt+R")}, AnnotationToolbar::Rectangle},
        {{QStringLiteral("annotation.tool.circle"), QStringLiteral("圆形工具"), {}, {}, {}, QStringLiteral("Alt+C")}, AnnotationToolbar::Circle},
        {{QStringLiteral("annotation.tool.text"), QStringLiteral("文字工具"), {}, {}, {}, QStringLiteral("Alt+T")}, AnnotationToolbar::Text},
        {{QStringLiteral("annotation.tool.freeDraw"), QStringLiteral("自由绘制"), {}, {}, {}, QStringLiteral("Alt+D")}, AnnotationToolbar::FreeDraw},
    };
    for (const auto& [descriptor, tool] : annotationTools) {
        addBuiltIn(descriptor, [this, tool](bool) {
            if (!_annoMode || !_p->annoToolbar || !_p->annoToolbar->isVisible()) return false;
            if (auto* service = resolveAnnotationService(_p->annotationService, _annoMgr.get())) { service->setTool(tool); return true; }
            return false;
        });
    }

    const QVector<std::pair<CommandDescriptor, int>> compareModes = {
        {{QStringLiteral("compare.modeA"), QStringLiteral("显示镜头 A"), {}, {}, {}, QStringLiteral("A")}, 0},
        {{QStringLiteral("compare.wipe"), QStringLiteral("擦除对比"), {}, {}, {}, QStringLiteral("W")}, 2},
        {{QStringLiteral("compare.overlay"), QStringLiteral("叠加对比"), {}, {}, {}, QStringLiteral("N")}, 3},
        {{QStringLiteral("compare.difference"), QStringLiteral("差异对比"), {}, {}, {}, QStringLiteral("D")}, 4},
        {{QStringLiteral("compare.horizontal"), QStringLiteral("水平分屏"), {}, {}, {}, QStringLiteral("H")}, 5},
        {{QStringLiteral("compare.vertical"), QStringLiteral("垂直分屏"), {}, {}, {}, QStringLiteral("V")}, 6},
        {{QStringLiteral("compare.tile"), QStringLiteral("平铺对比"), {}, {}, {}, QStringLiteral("T")}, 7},
    };
    for (const auto& [descriptor, mode] : compareModes) {
        addBuiltIn(descriptor, [this, mode](bool) {
            if (!_p->compareBar) return false;
            _p->compareBar->setCompareMode(mode);
            return true;
        });
    }
    addBuiltIn({QStringLiteral("compare.toggleB"), QStringLiteral("切换镜头 B"), {}, {}, {}, QStringLiteral("B")}, [this](bool) {
        if (!_p->compareBar) return false;
        _p->compareBar->toggleCompare();
        return true;
    });
    addBuiltIn({QStringLiteral("compare.autoClearB"), QStringLiteral("自动清除镜头 B"), {}, {}, {}, QStringLiteral("Ctrl+Shift+C")}, [this](bool) {
        if (!_p->compareBar) return false;
        _p->compareBar->toggleAutoClearB();
        return true;
    });
    addBuiltIn({QStringLiteral("codex.captureFrame"), QStringLiteral("截图并发送到 Codex")}, [this](bool) {
        ViewerWidget* viewer = _p->viewer.data();
        if (!viewer) return false;
        const QString root = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("captures"));
        QDir().mkpath(root);
        const QString path = QDir(root).filePath(QStringLiteral("frame_%1.png").arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_hhmmss_zzz"))));
        const QPixmap frame = viewer->grab();
        if (frame.isNull() || !frame.save(path, "PNG")) return false;
        if (statusBar()) statusBar()->showMessage(QStringLiteral("当前帧已截图：%1").arg(path), 2500);
        return true;
    });
    addBuiltIn({QStringLiteral("codex.browserSnapshot"), QStringLiteral("浏览器快照")}, [this](bool) {
        auto* workspace = findChild<QWidget*>(QStringLiteral("CodexAgentWorkspace"));
        if (!workspace) return false;
        bool captured = false;
        const bool invoked = QMetaObject::invokeMethod(workspace, "captureBrowserSnapshot", Qt::DirectConnection, Q_RETURN_ARG(bool, captured));
        return invoked && captured;
    });
    addBuiltIn({QStringLiteral("export.render"), QStringLiteral("渲染当前范围")}, [this](bool) {
        if (_p->currentPath.isEmpty() || !_p->playbackCtrl || !_p->playbackCtrl->isValid()) {
            if (statusBar()) statusBar()->showMessage(QStringLiteral("请先打开媒体文件"), 2200);
            return false;
        }
        if (!_p->videoExportAction) return false;
        _p->videoExportAction->trigger();
        return true;
    });
    addBuiltIn({QStringLiteral("workspace.save"), QStringLiteral("保存工作区")}, [this](bool) {
        _saveState();
        if (statusBar()) statusBar()->showMessage(QStringLiteral("当前工作区已保存"), 1800);
        return true;
    });
    addBuiltIn({QStringLiteral("workspace.reset"), QStringLiteral("恢复默认工作区")}, [this](bool) {
        _p->leftVisible = true;
        _p->rightVisible = true;
        _p->activeSidePanel = 2;
        _applyAdaptiveSidePanelLayout(true);
        if (statusBar()) statusBar()->showMessage(QStringLiteral("工作区已恢复默认"), 1800);
        return true;
    });
    addBuiltIn({QStringLiteral("playback.setSpeed"), QStringLiteral("设置播放速度")}, [this](bool) {
        if (auto* speed = findChild<QToolButton*>(QStringLiteral("PlaybackBarSpeed"))) { speed->showMenu(); return true; }
        return false;
    });
    addBuiltIn({QStringLiteral("playback.loop"), QStringLiteral("循环模式")}, [this](bool) {
        if (auto* loop = findChild<QToolButton*>(QStringLiteral("PlaybackBarLoop"))) { loop->click(); return true; }
        return false;
    });
    addBuiltIn({QStringLiteral("translation.mode"), QStringLiteral("翻译模式")}, [this](bool) {
        if (auto* mode = findChild<QToolButton*>(QStringLiteral("PlaybackBarTranslationMode"))) { mode->showMenu(); return true; }
        return false;
    });

    const QVector<CommandDescriptor> contributedCommands = pluginManager
        ? pluginManager->commandDescriptors()
        : QVector<CommandDescriptor>{};
    for (const auto& descriptor : contributedCommands) {
        if (!registry.registerCommand(descriptor) || descriptor.trigger) continue;
        dispatcher.registerHandler(descriptor.id, [this, id = descriptor.id](bool) {
            for (QAction* action : findChildren<QAction*>()) {
                if (!action || action->property("commandId").toString() != id) continue;
                if (!action->isEnabled()) return false;
                action->trigger();
                return true;
            }
            return false;
        });
    }
    _p->commandDescriptors = registry.commands();
    _p->commandDefaultShortcuts.clear();
    for (const auto& descriptor : _p->commandDescriptors) {
        _p->commandDefaultShortcuts.insert(descriptor.id, descriptor.shortcut);
    }
    QSet<QString> knownCommandIds;
    for (const auto& descriptor : _p->commandDescriptors) {
        if (!descriptor.id.trimmed().isEmpty()) knownCommandIds.insert(descriptor.id.trimmed());
    }
    if (_p->toolbarController) _p->toolbarController->setKnownCommands(knownCommandIds);
    if (_p->appearanceController) _p->appearanceController->setKnownStyleTargets(knownCommandIds);
    for (auto& descriptor : _p->commandDescriptors) {
        if (_p->userSettings) {
            const QString key = QStringLiteral("shortcuts/%1").arg(descriptor.id);
            if (_p->userSettings->contains(key)) {
                descriptor.shortcut = _p->userSettings->value(key).toString().trimmed();
            }
        }
    }
    if (!registry.duplicateIds().isEmpty()) {
        qWarning() << "[Commands] Ignored duplicate command IDs:" << registry.duplicateIds();
    }
    _p->menuContributions = pluginManager ? pluginManager->menuContributions() : QVector<MenuContribution>{};
    _p->toolbarContributions = pluginManager ? pluginManager->toolbarContributions() : QVector<ToolbarContribution>{};
    _p->panelContributions = pluginManager ? pluginManager->panelContributions() : QVector<PanelContribution>{};

    _p->menuContributions.erase(std::remove_if(_p->menuContributions.begin(), _p->menuContributions.end(), [&registry](const MenuContribution& contribution) {
        if (contribution.commandId.isEmpty() || registry.contains(contribution.commandId)) return false;
        qWarning() << "[Commands] Ignored menu contribution with unknown command:" << contribution.commandId;
        return true;
    }), _p->menuContributions.end());
    _p->toolbarContributions.erase(std::remove_if(_p->toolbarContributions.begin(), _p->toolbarContributions.end(), [&registry](const ToolbarContribution& contribution) {
        if (contribution.commandId.isEmpty() || registry.contains(contribution.commandId)) return false;
        qWarning() << "[Commands] Ignored toolbar contribution with unknown command:" << contribution.commandId;
        return true;
    }), _p->toolbarContributions.end());

    sortContributions(_p->commandDescriptors);
    sortContributions(_p->menuContributions);
    sortContributions(_p->toolbarContributions);
    std::stable_sort(_p->toolbarContributions.begin(), _p->toolbarContributions.end(), [this](const ToolbarContribution& lhs, const ToolbarContribution& rhs) {
        const int left = _p->userSettings ? _p->userSettings->value(QStringLiteral("toolbar/%1/order").arg(lhs.id), lhs.order).toInt() : lhs.order;
        const int right = _p->userSettings ? _p->userSettings->value(QStringLiteral("toolbar/%1/order").arg(rhs.id), rhs.order).toInt() : rhs.order;
        return left == right ? lhs.id < rhs.id : left < right;
    });
    sortContributions(_p->panelContributions);

}

void MainWindow::_applyMenuContributions(QMenuBar* menuBar)
{
    if (!menuBar || _p->menuContributions.isEmpty()) {
        return;
    }

    auto ensureMenu = [menuBar](const QString& slot, const QString& title) -> QMenu* {
        for (QAction* action : menuBar->actions()) {
            if (!action) {
                continue;
            }
            if (action->property("cgplay.menu.slot").toString() == slot) {
                return action->menu();
            }
        }
        if (title.isEmpty()) {
            return nullptr;
        }
        QMenu* menu = menuBar->addMenu(title);
        if (QAction* action = menu->menuAction()) {
            action->setProperty("cgplay.menu.slot", slot);
        }
        return menu;
    };

    auto findCommand = [this](const QString& commandId) -> const CommandDescriptor* {
        for (const auto& descriptor : _p->commandDescriptors) {
            if (descriptor.id == commandId) {
                return &descriptor;
            }
        }
        return nullptr;
    };

    for (const auto& contribution : _p->menuContributions) {
        QMenu* menu = ensureMenu(contribution.menuSlot, contribution.menuTitle);
        if (!menu) {
            continue;
        }

        if (contribution.separatorBefore && !menu->actions().isEmpty()) {
            menu->addSeparator();
        }

        const CommandDescriptor* descriptor = findCommand(contribution.commandId);
        if (!descriptor) {
            continue;
        }

        QAction* action = descriptor->createAction
            ? descriptor->createAction(menu)
            : new QAction(descriptor->text, menu);
        if (!action) {
            continue;
        }
        if (!action->parent()) {
            action->setParent(menu);
        }
        action->setObjectName(descriptor->id);
        action->setProperty("cgplay.command.id", descriptor->id);
        action->setProperty("commandId", descriptor->id);
        if (!descriptor->text.isEmpty()) {
            action->setText(descriptor->text);
        }
        if (!descriptor->toolTip.isEmpty()) {
            action->setToolTip(descriptor->toolTip);
        }
        if (!descriptor->statusTip.isEmpty()) {
            action->setStatusTip(descriptor->statusTip);
        }
        if (!descriptor->shortcut.isEmpty()) {
            action->setShortcut(QKeySequence(descriptor->shortcut));
        }
        if (!action->isCheckable()) {
            action->setCheckable(descriptor->checkable);
        }
        if (descriptor->isChecked) {
            action->setChecked(descriptor->isChecked());
        }
        if (descriptor->isEnabled) {
            action->setEnabled(descriptor->isEnabled());
        }
        if (descriptor->isVisible) {
            action->setVisible(descriptor->isVisible());
        }
        if (descriptor->trigger) {
            connect(action, &QAction::triggered, this, [this, id = descriptor->id](bool checked) {
                _executeCommandId(id, checked);
            });
        }
        if (!menu->actions().contains(action)) {
            menu->addAction(action);
        }

        if (contribution.separatorAfter) {
            menu->addSeparator();
        }
    }
}

bool MainWindow::_executeCommandId(const QString& commandId, bool checked)
{
    if (!_p->commandDispatcher) return false;
    return _p->commandDispatcher->dispatch(commandId, checked);
}

void MainWindow::_rebuildCustomToolbar()
{
    auto* host = findChild<QWidget*>(QStringLiteral("cgplayCustomToolbar"));
    auto* layout = host ? qobject_cast<QHBoxLayout*>(host->layout()) : nullptr;
    if (!host || !layout) return;

    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }

    QJsonArray configured;
    if (_p->userSettings) {
        QJsonParseError error{};
        const QByteArray raw = _p->userSettings->value(QStringLiteral("toolbar/customButtons")).toByteArray();
        const QJsonDocument document = QJsonDocument::fromJson(raw, &error);
        if (error.error == QJsonParseError::NoError && document.isArray()) configured = document.array();
    }

    ToolbarProfile toolbarProfile;
    bool toolbarProfileValid = false;
    QString toolbarProfileError;
    toolbarProfile = ToolbarProfile::fromJson(
        QJsonObject{{QStringLiteral("version"), ToolbarProfile::SchemaVersion},
                    {QStringLiteral("customButtons"), configured}},
        &toolbarProfileValid,
        &toolbarProfileError);
    if (toolbarProfileValid && _p->toolbarController) {
        toolbarProfileValid = _p->toolbarController->validate(toolbarProfile, &toolbarProfileError);
    }
    if (!toolbarProfileValid) {
        if (!configured.isEmpty()) {
            qWarning() << "[CustomToolbar] Rejected profile:" << toolbarProfileError;
        }
        configured = {};
    } else {
        configured = QJsonArray{};
        for (const auto& button : toolbarProfile.customButtons) configured.append(button.toJson());
    }

    QSet<QString> knownCommands;
    for (const auto& descriptor : _p->commandDescriptors) {
        if (!descriptor.id.trimmed().isEmpty()) knownCommands.insert(descriptor.id);
    }

    int validCount = 0;
    for (const QJsonValue& entry : configured) {
        const QJsonObject object = entry.toObject();
        const QString id = object.value(QStringLiteral("id")).toString().trimmed();
        const QString name = object.value(QStringLiteral("name")).toString().trimmed();
        const QJsonArray commandValues = object.value(QStringLiteral("commands")).toArray();
        QStringList commands;
        bool valid = !id.isEmpty() && !name.isEmpty() && !commandValues.isEmpty();
        for (const QJsonValue& commandValue : commandValues) {
            const QString command = commandValue.toString().trimmed();
            if (command.isEmpty() || !knownCommands.contains(command)) { valid = false; break; }
            commands.push_back(command);
        }
        if (!valid) {
            qWarning() << "[CustomToolbar] Ignored invalid button" << id;
            continue;
        }

        auto* button = new QToolButton(host);
        button->setObjectName(QStringLiteral("CustomToolbarButton_%1").arg(id));
        button->setProperty("commandId", id);
        button->setProperty("cgplay.customButton", true);
        button->setText(name);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setAutoRaise(false);
        button->setCursor(Qt::PointingHandCursor);
        QStringList commandNames;
        for (const auto& command : commands) {
            const auto descriptor = std::find_if(_p->commandDescriptors.cbegin(), _p->commandDescriptors.cend(),
                [&command](const auto& value) { return value.id == command; });
            commandNames.append(descriptor == _p->commandDescriptors.cend() ? command : descriptor->text);
        }
        button->setToolTip(QStringLiteral("%1\n依次执行：%2\n%3").arg(name,
            commandNames.join(QStringLiteral(" → ")), commands.join(QStringLiteral(" → "))));
        button->setAccessibleName(name);
        button->setAccessibleDescription(button->toolTip());
        const QString iconName = object.value(QStringLiteral("icon")).toString().trimmed();
        if (!iconName.isEmpty()) {
            const QIcon icon = QIcon::fromTheme(iconName);
            if (!icon.isNull()) button->setIcon(icon);
        }
        connect(button, &QToolButton::clicked, this, [this, commands, name] {
            for (const QString& command : commands) {
                if (_executeCommandId(command)) continue;
                if (statusBar()) {
                    statusBar()->showMessage(
                        QStringLiteral("%1: \u547d\u4ee4\u4e0d\u53ef\u7528 %2").arg(name, command), 3000);
                }
                break;
            }
        });
        layout->addWidget(button);
        ++validCount;
    }
    layout->addStretch();
    host->setVisible(validCount > 0);
    _refreshCommandPresentation();
}

void MainWindow::_applyToolbarContributions()
{
    if (!_p->reviewPanel) {
        return;
    }

    bool createdAnnotationToolbar = false;
    bool providerAttachedToolbar = false;
    const bool hasProviderToolbar =
        hasToolbarContributionForSlot(_p->toolbarContributions, HostExtensionSlots::kToolbarReviewPrimary);

    for (const auto& contribution : _p->toolbarContributions) {
        if (contribution.toolbarSlot != HostExtensionSlots::kToolbarReviewPrimary) {
            continue;
        }
        if (!contribution.createWidget) {
            continue;
        }

        QWidget* createdWidget = contribution.createWidget(_p->reviewPanel);
        if (!createdWidget) {
            continue;
        }

        if (auto* toolbar = resolveAnnotationToolbarWidget(createdWidget)) {
            _p->annoToolbar = toolbar;
            createdAnnotationToolbar = true;
            providerAttachedToolbar = true;
            const bool visible = !_p->userSettings || _p->userSettings->value(
                QStringLiteral("toolbar/%1/visible").arg(contribution.id), true).toBool();
            toolbar->setVisible(visible);
            _p->annoToolsVisible = visible;
        }
        _p->reviewPanel->setToolsWidget(createdWidget);
        break;
    }

    const bool annotationFallbackAvailable = shouldAllowAnnotationFallback() && _annoMgr;

    if (!providerAttachedToolbar && _p->annoToolbar) {
        _p->reviewPanel->setToolsWidget(_p->annoToolbar);
        createdAnnotationToolbar = true;
    }

    // TODO(Phase13-remove): Remove host-created fallback toolbar after annotation provider coverage reaches 100%.
    if (!_p->annoToolbar && !hasProviderToolbar && annotationFallbackAvailable) {
        _p->annoToolbar = new AnnotationToolbar(_p->reviewPanel);
        _p->annoToolbar->setFixedHeight(98);
        _p->annoToolbar->setObjectName(QStringLiteral("AnnotationToolbar"));
        _p->annoToolbar->show();
        _p->reviewPanel->setToolsWidget(_p->annoToolbar);
        createdAnnotationToolbar = true;
    }

    if (_p->annoToolbar) {
        _connectAnnotationToolbarSignals(_p->annoToolbar);
    }

    if (createdAnnotationToolbar || _p->annoToolbar) {
        _p->annoToolsVisible = _p->annoToolbar && !_p->annoToolbar->isHidden();
        _onAnnotationModeToggled(_p->annoToolsVisible);
    }
}

void MainWindow::_applyPanelContributions()
{
    if (!_p->horzSplitter) {
        return;
    }

    bool createdReviewPanel = false;
    const bool hasProviderPanel =
        hasPanelContributionForSlot(_p->panelContributions, HostExtensionSlots::kPanelRight);

    for (const auto& contribution : _p->panelContributions) {
        if (contribution.panelSlot != HostExtensionSlots::kPanelRight) {
            continue;
        }
        if (!contribution.createWidget) {
            continue;
        }

        QWidget* createdWidget = contribution.createWidget(_p->horzSplitter);
        if (!createdWidget) {
            continue;
        }

        if (auto* panel = resolveReviewPanelWidget(createdWidget)) {
            _p->reviewPanel = panel;
            createdReviewPanel = true;
            break;
        }
    }

    const bool annotationFallbackAvailable = shouldAllowAnnotationFallback() && _annoMgr;

    // TODO(Phase13-remove): Remove host-created fallback review panel after annotation panel provider fully owns the UI.
    if (!createdReviewPanel && !_p->reviewPanel && !hasProviderPanel && annotationFallbackAvailable) {
        _p->reviewPanel = new ReviewPanel(_annoMgr.get(), _p->horzSplitter);
        _p->reviewPanel->setObjectName(QStringLiteral("ReviewPanel"));
        createdReviewPanel = true;
    }

    if (createdReviewPanel && _p->reviewPanel) {
        _p->reviewPanel->setAttribute(Qt::WA_StyledBackground, true);
        _p->reviewPanel->setProperty("cgplay.surfaceRole", QStringLiteral("panel"));
        _p->reviewPanel->setMinimumWidth(0);
        _p->reviewPanel->setMaximumWidth(520);
        _p->reviewPanel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    }
}

void MainWindow::_cleanupDuplicateReviewPanels()
{
    QList<ReviewPanel*> panels;
    for (QWidget* widget : qApp->allWidgets()) {
        if (auto* panel = resolveReviewPanelWidget(widget)) {
            panels.push_back(panel);
        }
    }
    panels.erase(std::remove_if(panels.begin(), panels.end(), [](ReviewPanel* panel) {
        return panel == nullptr;
    }), panels.end());

    if (panels.isEmpty()) {
        _p->reviewPanel = nullptr;
        return;
    }

    ReviewPanel* preferred = _p->reviewPanel;
    if (_p->horzSplitter) {
        for (int i = 0; i < _p->horzSplitter->count(); ++i) {
            if (auto* directPanel = resolveReviewPanelWidget(_p->horzSplitter->widget(i))) {
                preferred = directPanel;
                break;
            }
        }
    }
    if (!preferred) {
        for (ReviewPanel* panel : panels) {
            if (_p->horzSplitter && panel->parentWidget() == _p->horzSplitter) {
                preferred = panel;
                break;
            }
        }
    }
    if (!preferred) {
        preferred = panels.front();
    }

    _p->reviewPanel = preferred;

    if (_p->reviewPanel && _p->horzSplitter && _p->horzSplitter->indexOf(_p->reviewPanel) < 0) {
        _p->reviewPanel->setParent(_p->horzSplitter);
        _p->horzSplitter->addWidget(_p->reviewPanel);
    }

    if (_p->annoToolbar && _p->reviewPanel && !_p->reviewPanel->isAncestorOf(_p->annoToolbar)) {
        _p->reviewPanel->setToolsWidget(_p->annoToolbar);
    }

    if (_p->reviewPanel) {
        _p->reviewPanel->setMinimumWidth(0);
        _p->reviewPanel->setMaximumWidth(520);
        _p->reviewPanel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    }

    for (ReviewPanel* panel : panels) {
        if (!panel || panel == _p->reviewPanel) {
            continue;
        }
        panel->hide();
        panel->setParent(nullptr);
        panel->deleteLater();
    }

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    if (_p->reviewPanel && _p->horzSplitter) {
        const int reviewIndex = _p->horzSplitter->indexOf(_p->reviewPanel);
        if (reviewIndex >= 0) {
            QList<int> sizes = _p->horzSplitter->sizes();
            if (sizes.size() > reviewIndex) {
                if (sizes[reviewIndex] > 0) {
                    sizes[reviewIndex] = std::clamp(sizes[reviewIndex], 1, 320);
                }
                _p->horzSplitter->setSizes(sizes);
            }
        }
    }
}

void MainWindow::_setupStatusBar()
{
    auto* sb = statusBar();
    sb->setSizeGripEnabled(false);
    auto mk = [this](const QString& text, const QString& color=kSec, bool primary=false) -> QLabel* {
        auto* l = new QLabel(text, this);
        l->setAlignment(Qt::AlignCenter);
        // Keep the diagnostics row readable at the 1120px minimum window
        // without letting secondary chips squeeze the playback controls.
        l->setMinimumWidth(primary ? 76 : 58);
        l->setMaximumWidth(primary ? 104 : 92);
        l->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        l->setStyleSheet(QString(
            "QLabel{color:%1;font-size:10px;font-weight:%2;padding:3px 6px;background:%3;"
            "border:1px solid %4;border-radius:6px;}")
            .arg(
                color,
                primary ? QStringLiteral("600") : QStringLiteral("500"),
                primary ? QStringLiteral("#151b22") : QStringLiteral("#111418"),
                primary ? QStringLiteral("rgba(255,140,50,0.16)")
                        : QStringLiteral("rgba(255,255,255,0.055)")));
        return l;
    };

    _p->lblCodec   = mk("--",      kAccent);
    _p->lblRes     = mk("--",      kText);
    _p->lblBitrate = mk("码率: --", kText, true);
    _p->lblBitrate->setMinimumWidth(78);
    _p->lblBitrate->setMaximumWidth(112);
    _p->lblFPS     = mk("-- FPS",  kText, true);
    _p->lblDecoder = mk("--",      kAccent);
    _p->lblDropped = mk("Dropped:0", kText, true);
    _p->lblCache   = mk("Cache:0.0%", kText, true);
    _p->lblCPU     = mk("CPU:0.0%", kSec);
    _p->lblGPU     = mk("GPU:0.0%", kSec);
    _p->lblMem     = mk("Memory:0.0%", kSec);
    _p->lblOcio    = mk("OCIO: Off", kSec);
    _p->lblUpdate  = mk(QStringLiteral("发现新版本"), kAccent);
    _p->lblUpdate->setVisible(false);
    _p->lblUpdate->setCursor(Qt::PointingHandCursor);
    _p->lblUpdate->setTextFormat(Qt::RichText);
    _p->lblUpdate->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    connect(_p->lblUpdate, &QLabel::linkActivated, this, [this]() { _checkForUpdates(true, false); });
    _p->lblView = mk(QStringLiteral("View: %1").arg(
        _p->ocioManager ? _p->ocioManager->currentView() : QStringLiteral("sRGB")), kText);
    auto* lblMeter = mk(QStringLiteral("L |▆▆| R |▆▆|   -20.5 LUFS"), "#7bdc8b");
    lblMeter->setMinimumWidth(145);
    lblMeter->setMaximumWidth(190);

    _p->lblCPU->setToolTip(QStringLiteral("系统总 CPU 使用率"));
    _p->lblGPU->setToolTip(QStringLiteral("系统最繁忙 GPU 引擎使用率"));
    _p->lblMem->setToolTip(QStringLiteral("系统总内存使用率"));
    _p->lblDropped->setToolTip(QStringLiteral("当前播放会话的累计丢帧数"));
    _p->lblCache->setToolTip(QStringLiteral("当前媒体读取缓存占用"));
    _p->lblBitrate->setToolTip(QStringLiteral("当前视频文件的平均总码率；读取不到时显示 --"));

    sb->addWidget(_p->lblCodec);
    sb->addWidget(_p->lblRes);
    sb->addWidget(_p->lblBitrate);
    sb->addWidget(_p->lblFPS);
    sb->addWidget(_p->lblDecoder);
    sb->addWidget(_p->lblDropped);
    sb->addWidget(_p->lblCache);
    sb->addWidget(_p->lblCPU);
    sb->addWidget(_p->lblGPU);
    sb->addWidget(_p->lblMem);
    sb->addWidget(_p->lblOcio);
    sb->addWidget(_p->lblView);
    sb->addWidget(lblMeter, 1);
    sb->addPermanentWidget(_p->lblUpdate, 0);

    // Wire to real data
    connect(_p->viewer, &ViewerWidget::resolutionChanged, this, [this](int w, int h){
        _p->lblRes->setText(QString("%1x%2").arg(w).arg(h));
        if (_p->topBar) _p->topBar->resolutionLabel()->setText(QString("%1x%2").arg(w).arg(h));
    });
    if (_p->performanceService) {
        PerformanceService::Widgets widgets;
        widgets.codec = _p->lblCodec;
        widgets.resolution = _p->lblRes;
        widgets.bitrate = _p->lblBitrate;
        widgets.fps = _p->lblFPS;
        widgets.dropped = _p->lblDropped;
        widgets.cache = _p->lblCache;
        widgets.cpu = _p->lblCPU;
        widgets.gpu = _p->lblGPU;
        widgets.memory = _p->lblMem;
        widgets.ocio = _p->lblOcio;
        widgets.topBar = _p->topBar;
        _p->performanceService->bind(
            _p->playbackCtrl.get(),
            _p->cacheManager.get(),
            _p->ocioManager.get(),
            widgets);
        const QString bitrateUnit = _p->userSettings
            ? _p->userSettings->value(QStringLiteral("playback/bitrateUnit"), QStringLiteral("auto")).toString()
            : QStringLiteral("auto");
        _p->performanceService->setBitrateDisplayUnit(bitrateUnit);
    }
    if (_p->ocioManager) {
        connect(_p->ocioManager.get(), &OcioManager::optionsChanged, this,
                [this](const tl::OCIOOptions&) {
                    if (_p->lblView && _p->ocioManager) {
                        _p->lblView->setText(QStringLiteral("View: %1").arg(
                            _p->ocioManager->currentView()));
                    }
                });
    }
}

void MainWindow::checkVersionAndComponents(bool interactive)
{
    _checkVersionAndComponents(interactive);
}

void MainWindow::_checkVersionAndComponents(bool interactive)
{
    ComponentManager& manager = ComponentManager::instance();
    QString error;
    const bool refreshed = manager.refreshRemoteManifest(&error);
    if (!refreshed && interactive) {
        QMessageBox::warning(
            this,
            QStringLiteral("检查失败"),
            QStringLiteral("无法获取远端版本和组件清单：\n%1").arg(error));
        return;
    }

    _checkForUpdates(interactive, false);
    _showMissingComponentsPrompt(manager.missingRequiredComponents(), interactive);
}

void MainWindow::_showMissingComponentsPrompt(const QStringList& missing, bool interactive)
{
    if (missing.isEmpty()) {
        if (interactive) {
            QMessageBox::information(
                this,
                QStringLiteral("组件检查"),
                QStringLiteral("所有必需组件都已安装。"));
        }
        return;
    }

    const QString listText = missing.join(QStringLiteral("\n- "));
    const auto reply = QMessageBox::question(
        this,
        QStringLiteral("发现缺失组件"),
        QStringLiteral("检测到以下必需组件缺失：\n\n- %1\n\n是否现在自动下载并安装？")
            .arg(listText),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);
    if (reply != QMessageBox::Yes) {
        return;
    }

    QStringList failed;
    for (const QString& id : missing) {
        QString error;
        if (!ComponentManager::instance().ensureComponent(id, this, false, &error)) {
            failed.push_back(error.isEmpty() ? id : QStringLiteral("%1: %2").arg(id, error));
        }
    }

    if (failed.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("组件安装完成"),
            QStringLiteral("缺失组件已安装完成。"));
    } else {
        QMessageBox::warning(
            this,
            QStringLiteral("组件安装失败"),
            QStringLiteral("以下组件未能安装：\n\n- %1").arg(failed.join(QStringLiteral("\n- "))));
    }
}

void MainWindow::_checkForUpdates(bool interactive, bool refreshRemote)
{
    if (_p->updateClosing) return;
    if (_p->updateDownloadInProgress) {
        if (statusBar()) statusBar()->showMessage(QStringLiteral("正在下载更新，可在下载窗口中查看进度或取消。"), 4000);
        return;
    }
    if (!refreshRemote && !_p->latestUpdateResult.isEmpty()) {
        if (interactive) _presentUpdateResult(_p->latestUpdateResult);
        return;
    }
    if (_p->updateCheckInProgress) {
        _p->updateCheckInteractive = _p->updateCheckInteractive || interactive;
        return;
    }
    _p->updateCheckInProgress = true;
    _p->updateCheckInteractive = interactive;
    const QString currentVersion = qApp->applicationVersion();
    const auto state = std::make_shared<UpdateTransferState>();
    _p->updateCheckState = state;
    if (interactive && statusBar()) statusBar()->showMessage(QStringLiteral("正在后台检查 GitHub 最新版本…"));
    auto* watcher = new QFutureWatcher<UpdateCheckResult>(this);
    connect(watcher, &QFutureWatcher<UpdateCheckResult>::finished, this, [this, watcher, state]() {
        const auto result = watcher->result();
        watcher->deleteLater();
        _p->updateCheckInProgress = false;
        const bool interactiveResult = _p->updateCheckInteractive;
        _p->updateCheckInteractive = false;
        if (state->cancelled.load()) {
            if (interactiveResult && !_p->updateClosing) _checkForUpdates(true);
            return;
        }
        _applyBackgroundUpdateResult(result.toJson());
        if (interactiveResult) _presentUpdateResult(result.toJson());
    });
    watcher->setFuture(QtConcurrent::run([currentVersion, state]() {
        return UpdateService::instance().checkForUpdates(currentVersion, state);
    }));
}

void MainWindow::_presentUpdateResult(const QJsonObject& json)
{
    if (_p->updateClosing) return;
    const auto result = UpdateCheckResult::fromJson(json);
    if (!result.error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("检查更新"), result.error);
        return;
    }
    if (!result.updateAvailable) {
        QMessageBox::information(this, QStringLiteral("检查更新"),
            QStringLiteral("当前版本：%1\n最新版本：%2\n\n当前已经是最新版本。")
                .arg(result.currentVersion, result.remoteVersion));
        return;
    }
    QMessageBox prompt(QMessageBox::Question, QStringLiteral("发现新版本"),
        QStringLiteral("当前版本：%1\n最新版本：%2\n\n下载完成后会校验安装包，安装前会再次提醒。")
            .arg(result.currentVersion, result.remoteVersion), QMessageBox::NoButton, this);
    prompt.setTextFormat(Qt::PlainText);
    if (!result.releaseNotes.isEmpty()) prompt.setDetailedText(result.releaseNotes);
    auto* download = prompt.addButton(QStringLiteral("下载更新"), QMessageBox::AcceptRole);
    auto* page = prompt.addButton(QStringLiteral("查看更新说明"), QMessageBox::ActionRole);
    prompt.addButton(QStringLiteral("稍后"), QMessageBox::RejectRole);
    prompt.exec();
    if (_p->updateClosing) return;
    if (prompt.clickedButton() == download) _downloadAndLaunchInstaller(result.remoteVersion);
    else if (prompt.clickedButton() == page) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/xty-luoye/CGPlay/releases/latest")));
    }
}

bool MainWindow::_downloadAndLaunchInstaller(const QString& targetVersion)
{
    if (_p->updateClosing) return false;
    if (_p->updateDownloadInProgress) return true;
    const auto release = UpdateCheckResult::fromJson(_p->latestUpdateResult);
    if (!release.updateAvailable || release.remoteVersion != targetVersion) {
        _checkForUpdates(true);
        return false;
    }
    if (_p->verifiedUpdateInstaller && _p->verifiedUpdateVersion == targetVersion &&
        _p->verifiedUpdateInstaller->downloaded &&
        _p->verifiedUpdateInstaller->sha256.compare(release.sha256, Qt::CaseInsensitive) == 0 &&
        QFileInfo(_p->verifiedUpdateInstaller->installerPath).isFile() &&
        QFileInfo(_p->verifiedUpdateInstaller->installerPath).size() == release.installerSize) {
        _offerDownloadedUpdate(*_p->verifiedUpdateInstaller);
        return true;
    }
    _p->updateDownloadInProgress = true;
    const auto state = std::make_shared<UpdateTransferState>();
    _p->updateDownloadState = state;
    auto* progress = new QProgressDialog(QStringLiteral("正在后台下载安装包，播放器可以继续使用。"),
        QStringLiteral("取消下载"), 0, 1000, this);
    progress->setObjectName(QStringLiteral("UpdateDownloadProgress"));
    progress->setWindowTitle(QStringLiteral("CGPlay 更新"));
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setValue(0);
    connect(progress, &QProgressDialog::canceled, this, [state]() { state->cancelled.store(true); });
    progress->show();
    auto* timer = new QTimer(progress);
    connect(timer, &QTimer::timeout, progress, [state, progress, total = release.installerSize]() {
        const qint64 received = state->received.load();
        progress->setValue(static_cast<int>(std::clamp(received * 1000 / std::max<qint64>(total, 1), 0LL, 1000LL)));
        progress->setLabelText(QStringLiteral("正在后台下载更新，播放器可以继续使用。\n已下载 %1 / %2 MB")
            .arg(received / (1024.0 * 1024.0), 0, 'f', 1).arg(total / (1024.0 * 1024.0), 0, 'f', 1));
    });
    timer->start(250);
    const QPointer<QProgressDialog> progressGuard(progress);
    auto* watcher = new QFutureWatcher<UpdateInstallResult>(this);
    connect(watcher, &QFutureWatcher<UpdateInstallResult>::finished, this,
        [this, watcher, state, progressGuard, version = release.remoteVersion]() {
        const auto result = watcher->result();
        watcher->deleteLater();
        const bool wasCancelled = state->cancelled.load();
        if (progressGuard) {
            progressGuard->disconnect(this);
            progressGuard->close();
        }
        _p->updateDownloadInProgress = false;
        if (wasCancelled || _p->updateClosing) {
            if (statusBar()) statusBar()->showMessage(QStringLiteral("更新下载已取消。"), 4000);
            return;
        }
        if (!result.downloaded) {
            QMessageBox::warning(this, QStringLiteral("更新下载失败"), result.error);
            return;
        }
        _p->verifiedUpdateVersion = version;
        _p->verifiedUpdateInstaller = std::make_shared<UpdateInstallResult>(result);
        _offerDownloadedUpdate(result);
    });
    watcher->setFuture(QtConcurrent::run([release, state]() {
        return UpdateService::instance().downloadInstaller(release, state);
    }));
    return true;
}

void MainWindow::_offerDownloadedUpdate(UpdateInstallResult installer)
{
    if (_p->updateClosing) return;
    QMessageBox prompt(QMessageBox::Information, QStringLiteral("更新已准备好"),
        QStringLiteral("安装包已通过完整性校验。立即安装将关闭 CGPlay，请先保存需要保留的工作。"),
        QMessageBox::NoButton, this);
    auto* install = prompt.addButton(QStringLiteral("关闭并安装"), QMessageBox::AcceptRole);
    prompt.addButton(QStringLiteral("稍后安装"), QMessageBox::RejectRole);
    prompt.setDetailedText(QStringLiteral("安装包位置：%1").arg(installer.installerPath));
    prompt.exec();
    if (prompt.clickedButton() != install || _p->updateClosing) return;
    QString error;
    if (!UpdateService::launchInstaller(installer, &error)) {
        QMessageBox::warning(this, QStringLiteral("启动安装失败"), error);
        return;
    }
    close();
    QCoreApplication::quit();
}

void MainWindow::_setUpdateStatusBadge(const QString& text, const QString& color, const QString& toolTip)
{
    if (!_p->lblUpdate) {
        return;
    }

    const bool visible = !text.trimmed().isEmpty();
    _p->lblUpdate->setVisible(visible);
    if (!visible) {
        _p->lblUpdate->clear();
        _p->lblUpdate->setToolTip(QString());
        return;
    }

    _p->lblUpdate->setText(QStringLiteral("<a href=\"update\" style=\"color:%1\">%2</a>")
        .arg(color.toHtmlEscaped(), text.toHtmlEscaped()));
    _p->lblUpdate->setToolTip(toolTip);
    _p->lblUpdate->setStyleSheet(QString(
        "QLabel{color:%1;font-size:11px;padding:4px 10px;background:#111418;"
        "border:1px solid rgba(255,255,255,0.055);border-radius:6px;}").arg(color));
}

void MainWindow::_applyBackgroundUpdateResult(const QJsonObject& result)
{
    const auto update = UpdateCheckResult::fromJson(result);
    if (!update.error.isEmpty()) {
        qWarning() << "[UpdateCheck]" << update.error;
        return;
    }
    _p->latestUpdateResult = result;
    _p->updateAvailable = update.updateAvailable;
    _p->remoteUpdateVersion = update.remoteVersion;
    if (update.updateAvailable) {
        _setUpdateStatusBadge(QStringLiteral("发现新版本 %1，点击更新").arg(update.remoteVersion),
            QString::fromLatin1(kAccent), QStringLiteral("查看更新说明、下载并安装新版本。"));
        if (statusBar()) statusBar()->showMessage(QStringLiteral("发现新版本 %1").arg(update.remoteVersion), 8000);
    } else {
        _setUpdateStatusBadge({}, QString::fromLatin1(kSec), {});
        if (statusBar()) statusBar()->clearMessage();
    }
}

void MainWindow::_scheduleBackgroundUpdateCheck()
{
    if (const auto* app = qobject_cast<Application*>(qApp); app && app->isAutomationMode()) return;
    if (qApp->property("cgplay.benchmarkMode").toBool() ||
        qApp->property("cgplay.captureUiMode").toBool() ||
        qApp->property("cgplay.playerSmokeMode").toBool() ||
        qApp->property("cgplay.componentCheckMode").toBool()) return;
    const auto check = [this]() {
        if (!_p->userSettings || _p->userSettings->value(QStringLiteral("updates/checkAutomatically"), true).toBool())
            _checkForUpdates(false);
    };
    QTimer::singleShot(5000, this, check);
    auto* dailyCheck = new QTimer(this);
    connect(dailyCheck, &QTimer::timeout, this, check);
    dailyCheck->start(24 * 60 * 60 * 1000);
}

// 鈹€鈹€鈹€ Signal wiring 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€

void MainWindow::_connectSignals()
{
    if (auto* playbackSignals = _p->playbackCtrl ? _p->playbackCtrl->signalProxy() : nullptr) {
        if (PlaybackLogger::frameLoggingEnabled()) {
            connect(playbackSignals, &PlaybackServiceSignals::currentFrameChanged, this, [this](int frame, int) {
                CGPLAY_LOG().frameDisplayed(frame);
            });
        }
        connect(playbackSignals, &PlaybackServiceSignals::playbackStateChanged, this, [this](int s) {
            if (!_p->playbackCtrl) {
                return;
            }
            switch (s) {
            case 0: CGPLAY_LOG().stop(); break;
            case 1: CGPLAY_LOG().forward(_p->playbackCtrl->fps()); break;
            case 2: CGPLAY_LOG().reverse(_p->playbackCtrl->fps()); break;
            }
        });
        connect(playbackSignals, &PlaybackServiceSignals::currentFrameChanged, this, [this](int frame, int) {
            _updateGeneratedSubtitleForFrame(frame);
        });
        connect(playbackSignals, &PlaybackServiceSignals::fileOpened, this, [this](const QString&) {
            if (_p->sessionMgr) {
                _p->sessionMgr->markDirty();
            }
        });
#if CGPLAY_HAS_TLRENDER
        connect(playbackSignals, &PlaybackServiceSignals::playerReady, this, [this](const std::shared_ptr<tl::Player>& player) {
            if (!player || !_p->autoPlayPending || !_p->playbackCtrl) {
                return;
            }
            _p->autoPlayPending = false;
            _p->playbackCtrl->play();
        });
#endif
    }

    if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
        eventBus->subscribe<OcioOptionsChangedEvent>([this](const OcioOptionsChangedEvent& event) {
            if (_p->playlist && _p->playlist->model()) {
                _p->playlist->model()->rebuildThumbnails(event.previewSettings);
            }
            if (_p->timeline) {
                _p->timeline->setPreviewTransformSettings(event.previewSettings);
            }
        });
    }

    // Playlist 鈫?open
    connect(_p->playlist, &PlaylistPanel::shotActivated, this, [this](const QString& p){ openFile(p); });
    connect(_p->playlist, &PlaylistPanel::sequenceImportRequested, this, [this](const QString& p){ openFile(p); });
    auto closeIfCurrentWasRemoved = [this](const QStringList& paths) {
        if (_p->currentPath.isEmpty()) {
            return;
        }
        const QString current = QFileInfo(_p->currentPath).absoluteFilePath();
        for (const QString& path : paths) {
            if (QFileInfo(path).absoluteFilePath() == current) {
                _closeCurrentMedia();
                return;
            }
        }
    };
    connect(_p->playlist, &PlaylistPanel::shotsRemoved, this, closeIfCurrentWasRemoved);
    connect(_p->playlist, &PlaylistPanel::playlistCleared, this, closeIfCurrentWasRemoved);
    connect(_p->playlist, &PlaylistPanel::shotASelected, this, [this](const QString& p){ _p->compareBar->setShotALabel(QFileInfo(p).completeBaseName()); });
    connect(_p->playlist, &PlaylistPanel::shotBSelected, this, [this](const QString& p){
        _p->compareBar->setShotBLabel(QFileInfo(p).completeBaseName());
        _p->playbackCtrl->setCompareFile(p);
        _p->compareBar->setCompareMode(5);
    });

    // Compare 鈫?Viewport
    connect(_p->compareBar, &CompareToolbar::compareOptionsChanged, this, [this](const tl::CompareOptions& o){
        if (auto* vp = _p->viewer->viewport()) vp->setCompareOptions(o);
    });
    connect(_p->compareBar, &CompareToolbar::shotAFileDropped, this, [this](const QString& p){
        QFileInfo fi(p); _p->compareBar->setShotALabel(fi.completeBaseName());
        if (_p->compareBar->isAutoClearB()) { _p->compareBar->setShotBLabel("--"); _p->compareBar->setCompareMode(0); }
        openFile(p);
    });
    connect(_p->compareBar, &CompareToolbar::shotBFileDropped, this, [this](const QString& p){
        QFileInfo fi(p); _p->compareBar->setShotBLabel(fi.completeBaseName());
        _p->playbackCtrl->setCompareFile(p); _p->compareBar->setCompareMode(5);
    });
    connect(_p->viewer, &ViewerWidget::droppedFile, this, [this](const QString& p){
        QFileInfo fi(p);
        if (_p->currentPath.isEmpty()) { _p->compareBar->setShotALabel(fi.completeBaseName()); openFile(p); }
        else { _p->compareBar->setShotBLabel(fi.completeBaseName()); _p->playbackCtrl->setCompareFile(p); _p->compareBar->setCompareMode(5); }
    });
    connect(_p->viewer, &ViewerWidget::compareRequested, this, [this]{
        _p->compareBar->setVisible(!_p->compareBar->isVisible());
    });
    connect(_p->viewer, &ViewerWidget::compareTileRequested, this, [this]{
        _p->compareBar->show();
        _p->compareBar->setCompareMode(7);
    });
    connect(_p->viewer, &ViewerWidget::fullscreenRequested, this, [this]{
        _toggleFullScreen();
    });
    connect(_p->timeline, &TimelineWidget::droppedFile, this, [this](const QString& p){ openFile(p); });

    _connectReviewPanelSignals(_p->reviewPanel);
    // Settings are created by their explicit open actions. Building their
    // editors here blocks startup and repeats the global appearance refresh.

}

void MainWindow::_connectReviewPanelSignals(ReviewPanel* panel)
{
    if (!panel) {
        return;
    }

    QObject::disconnect(panel, nullptr, this, nullptr);
    connect(panel, &ReviewPanel::jumpToFrame, this, [this](int f) {
        _p->playbackCtrl->seekToFrame(f);
    });
    connect(panel, &ReviewPanel::annotationSelected, this, [this](const QString& id) {
        if (auto* annotationService = resolveAnnotationService(_p->annotationService, _annoMgr.get())) {
            annotationService->selectAnnotation(id);
        }
    });
    connect(panel, &ReviewPanel::annotationCommentEdited, this, [this](const QString& id, const QString& text) {
        if (auto* annotationService = resolveAnnotationService(_p->annotationService, _annoMgr.get())) {
            annotationService->updateAnnotationComment(id, text);
            annotationService->selectAnnotation(id);
        }
    });
    connect(panel, &ReviewPanel::createNoteRequested, this, [this](const QString& text) {
        if (auto* annotationService = resolveAnnotationService(_p->annotationService, _annoMgr.get())) {
            const QString id = annotationService->createNote(text);
            if (!id.isEmpty()) {
                annotationService->selectAnnotation(id);
            }
        }
    });
}

void MainWindow::_connectAnnotationToolbarSignals(AnnotationToolbar* toolbar)
{
    if (!toolbar) {
        return;
    }

    QObject::disconnect(toolbar, nullptr, this, nullptr);
    connect(toolbar, &AnnotationToolbar::toolChanged, this, [this](int tool, QColor color) {
        _onAnnotationModeToggled(true);
        if (auto* annotationService = resolveAnnotationService(_p->annotationService, _annoMgr.get())) {
            annotationService->setTool(tool);
            annotationService->setToolColor(color);
        }
    });
    connect(toolbar, &AnnotationToolbar::colorChanged, this, [this](QColor color) {
        if (auto* annotationService = resolveAnnotationService(_p->annotationService, _annoMgr.get())) {
            annotationService->setToolColor(color);
        }
    });
    connect(toolbar, &AnnotationToolbar::deleteRequested, this, [this] {
        if (auto* annotationService = resolveAnnotationService(_p->annotationService, _annoMgr.get())) {
            annotationService->removeSelectedAnnotation();
        }
    });
}

} // namespace cgplay
