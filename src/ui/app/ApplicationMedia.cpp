#include "ApplicationInternal.h"

#include "common/jobs/JobSystem.h"

#include <numeric>


namespace cgplay {



void MainWindow::openFile(const QString& path)
{
    if (path.isEmpty()) return;

    Q_EMIT mediaOpenRequested(path);

    _p->autoPlayPending = false;

    const QFileInfo incomingInfo(path);
    const QString incomingSuffix = incomingInfo.suffix().toLower();
    const bool isSequenceLikeStill = ReviewExport::isStillImage(path) &&
        (incomingSuffix == "exr" || incomingSuffix == "dpx");
    double sequenceFpsOverride = 0.0;
    if (isSequenceLikeStill && _p->playlist && _p->playlist->model()) {
        auto* playlistModel = _p->playlist->model();
        for (int i = 0; i < playlistModel->shotCount(); ++i) {
            const auto& shot = playlistModel->shotAt(i);
            if (shot.path == path && shot.fps > 0.0) {
                sequenceFpsOverride = shot.fps;
                break;
            }
        }
        if (sequenceFpsOverride <= 0.0) {
            sequenceFpsOverride = _promptSequenceFps(path);
        }
    }

    if (_p->mediaProbeJob) {
        _p->mediaProbeJob->cancel();
        _p->mediaProbeJob.clear();
    }
    const quint64 generation = ++_p->mediaProbeGeneration;
    const auto mediaInfo = std::make_shared<MediaInfo>();
    auto* job = JobRunner::start(
        QStringLiteral("media.probe"),
        this,
        8000,
        [path, sequenceFpsOverride, mediaInfo](JobContext& context) {
            *mediaInfo = MediaProbe::probe(path, sequenceFpsOverride, &context);
            return JobOutcome::success();
        });
    _p->mediaProbeJob = job;
    connect(job, &JobHandle::finished, this,
        [this, job, path, sequenceFpsOverride, generation, mediaInfo](const JobOutcome& outcome) {
            if (_p->mediaProbeJob == job) {
                _p->mediaProbeJob.clear();
            }
            if (generation != _p->mediaProbeGeneration || mediaInfo->error == QStringLiteral("Media probe canceled")) {
                return;
            }
            if (!outcome.succeeded()) {
                Q_EMIT mediaOpenFailed(path, outcome.errorMessage.isEmpty()
                    ? QStringLiteral("Media probing failed or was canceled.") : outcome.errorMessage);
                return;
            }
            _openFileWithMediaInfo(path, sequenceFpsOverride, *mediaInfo);
        });
}

void MainWindow::_openFileWithMediaInfo(
    const QString& path,
    double sequenceFpsOverride,
    const MediaInfo& mediaInfo)
{
    const QFileInfo incomingInfo(path);
    const QString incomingSuffix = incomingInfo.suffix().toLower();

    _resetMediaSession(path, SubtitleGenerationService::mediaFingerprint(path, mediaInfo.durationSeconds));
    _p->cacheManager->clear();
    if (auto* annotationService = resolveAnnotationService(_p->annotationService, _annoMgr.get())) {
        annotationService->clearAnnotations();
        annotationService->selectAnnotation(QString());
    }
    if (_p->playlist && _p->playlist->model()) {
        auto* playlistModel = _p->playlist->model();
        int existingIndex = -1;
        for (int i = 0; i < playlistModel->shotCount(); ++i) {
            if (playlistModel->shotAt(i).path == path) {
                existingIndex = i;
                break;
            }
        }
        if (existingIndex < 0) {
            playlistModel->addPath(path, mediaInfo, sequenceFpsOverride);
            existingIndex = playlistModel->shotCount() - 1;
        } else if (existingIndex >= 0) {
            // Confirmed open metadata supersedes any older playlist probe for
            // this row, including a sequence FPS chosen by the user.
            playlistModel->updateMetadata(existingIndex, mediaInfo, sequenceFpsOverride);
        }
        if (existingIndex >= 0) {
            playlistModel->setCurrentIndex(existingIndex);
        }
    }
    if (auto* s = _p->playbackCtrl->playbackStats()) s->reset();
    if (_p->timeline) _p->timeline->setMediaPath(path);
    _p->autoPlayPending = mediaInfo.isVideo;
    Q_EMIT mediaPlayerOpening(path);
    if (auto* playback = dynamic_cast<PlaybackController*>(_p->playbackCtrl.get())) {
        playback->openFile(path, sequenceFpsOverride, mediaInfo.codecName);
    } else {
        _p->playbackCtrl->openFile(path, sequenceFpsOverride);
    }
    // Apply after the timeline/player exists so tlRender's GPU viewport
    // receives the ACES options for the newly opened EXR item.
    if (incomingSuffix == QStringLiteral("exr") && _p->ocioManager && _p->playbackCtrl->isValid()) {
        _p->ocioManager->applyExrSceneLinearDefaults();
        // Playback creates the tlRender video item asynchronously. Rebind
        // once after that item exists so its GPU display shader receives the
        // exact ACES 1.2 options instead of retaining the pre-open defaults.
        const quint64 generation = _p->mediaGenerationId;
        const auto initialColorOptions = _p->ocioManager->currentOptions();
        QTimer::singleShot(250, this, [this, generation, path, initialColorOptions] {
            if (generation != _p->mediaGenerationId || _p->currentPath != path ||
                !_p->playbackCtrl->isValid()) return;
            // A loaded session or a user adjustment owns its selected transform;
            // this delayed GPU rebind must not replace it with EXR defaults.
            if (_p->ocioManager->currentOptions() != initialColorOptions) return;
            _p->ocioManager->applyExrSceneLinearDefaults();
        });
    }
    const QString codecText = formatCodecText(mediaInfo);
    const QString resText = mediaInfo.resolutionText();
    const QString bitrateUnit = _p->performanceService
        ? _p->performanceService->bitrateDisplayUnit()
        : QStringLiteral("auto");
    const QString bitrateText = QStringLiteral("码率: %1").arg(mediaInfo.bitrateText(bitrateUnit));
    const QString fpsText = PerformanceService::formatFpsText(_p->playbackCtrl->fps() > 0.0 ? _p->playbackCtrl->fps() : mediaInfo.fps);
    _p->lblCodec->setText(codecText);
    _p->lblRes->setText(resText);
    _p->lblBitrate->setText(bitrateText);
    _p->lblFPS->setText(fpsText);
    if (_p->performanceService) {
        _p->performanceService->applyMediaInfo(mediaInfo, _p->playbackCtrl.get());
    } else if (_p->topBar) {
        _p->topBar->codecLabel()->setText(codecText);
        _p->topBar->resolutionLabel()->setText(resText);
        _p->topBar->fpsLabel()->setText(PerformanceService::formatFpsText(_p->playbackCtrl->fps() > 0.0 ? _p->playbackCtrl->fps() : mediaInfo.fps, 0));
    }
    CGPLAY_LOG().fileOpened(path, _p->playbackCtrl->totalFrames(), _p->playbackCtrl->fps(), 0, 0, codecText);
}

void MainWindow::_resetMediaSession(const QString& path, const QString& fingerprint)
{
    // Commit a new media identity before touching callbacks or progress UI.
    ++_p->mediaGenerationId;
    _p->currentPath = path;
    _p->currentMediaFingerprint = fingerprint;
    _p->subtitleSession.cancelTasks();
    if (_p->generatedSubtitleRefreshTimer) _p->generatedSubtitleRefreshTimer->stop();
    if (_p->translationPrePlaybackWaitTimer) _p->translationPrePlaybackWaitTimer->stop();
    for (const auto& progress : {_p->subtitleGenerationProgress, _p->translationPrePlaybackWaitProgress}) {
        if (progress) {
            const QSignalBlocker blocker(progress);
            progress->close();
        }
    }
    _p->subtitleGenerationProgress.clear();
    _p->translationPrePlaybackWaitProgress.clear();
    _p->subtitleSession.reset();
    if (!path.isEmpty()) {
        _p->subtitleSession.state().highQualityEnhancementLastDiagnostics = QJsonObject{
            {QStringLiteral("clearedForMediaSwitch"), true},
            {QStringLiteral("mediaFingerprint"), fingerprint},
            {QStringLiteral("mediaPath"), QFileInfo(path).absoluteFilePath()}};
    }
    if (_p->generatedSubtitleLabel) {
        _p->generatedSubtitleLabel->hide();
        _p->generatedSubtitleLabel->clear();
    }
    if (qApp) qApp->setProperty("cgplay.currentMediaHighQualityEnhancement", QString());
    // Also resets a previous EXR's manual-override suppression when switching
    // to another EXR, and restores color state on close, not only video open.
    if (_p->ocioManager) _p->ocioManager->clearExrSceneLinearDefaults();
}

void MainWindow::_closeCurrentMedia()
{
    Q_EMIT mediaOpenRequested({});
    _p->autoPlayPending = false;
    ++_p->mediaProbeGeneration;
    if (_p->mediaProbeJob) {
        _p->mediaProbeJob->cancel();
        _p->mediaProbeJob.clear();
    }
    _resetMediaSession({}, {});
    if (auto* annotationService = resolveAnnotationService(_p->annotationService, _annoMgr.get())) {
        annotationService->clearAnnotations();
        annotationService->selectAnnotation(QString());
    }
    _p->cacheManager->clear();
    if (auto* stats = _p->playbackCtrl->playbackStats()) {
        stats->reset();
    }
    _p->playbackCtrl->closeFile();
    if (_p->timeline) {
        _p->timeline->setMediaPath({});
        _p->timeline->setInOutPoints(-1, -1);
    }
    if (_p->reviewPanel) {
        _p->reviewPanel->refresh({}, 0);
        _p->reviewPanel->selectById({});
    }
    if (_p->compareBar) {
        _p->compareBar->setShotALabel(QStringLiteral("--"));
        _p->compareBar->setShotBLabel(QStringLiteral("--"));
        _p->compareBar->setCompareMode(0);
    }
    if (_p->performanceService) {
        _p->performanceService->resetMediaInfo();
    } else {
        if (_p->lblCodec) {
            _p->lblCodec->setText("--");
        }
        if (_p->lblRes) {
            _p->lblRes->setText("--");
        }
        if (_p->lblBitrate) {
            _p->lblBitrate->setText("码率: --");
        }
        if (_p->lblFPS) {
            _p->lblFPS->setText("-- FPS");
        }
        if (_p->lblDropped) {
            _p->lblDropped->setText("Dropped:0");
        }
        if (_p->lblCache) {
            _p->lblCache->setText("Cache:0.0%");
        }
        if (_p->topBar) {
            _p->topBar->codecLabel()->setText("--");
            _p->topBar->resolutionLabel()->setText("--");
            _p->topBar->fpsLabel()->setText("-- FPS");
        }
    }
    if (_p->sessionMgr) {
        _p->sessionMgr->markDirty();
    }
}

double MainWindow::_promptSequenceFps(const QString& path) const
{
    static const QList<double> presets = {
        23.976, 24.0, 25.0, 29.97, 30.0, 48.0, 50.0, 59.94, 60.0
    };

    double defaultFps = 24.0;
    if (_p->playlist && _p->playlist->model()) {
        auto* playlistModel = _p->playlist->model();
        for (int i = 0; i < playlistModel->shotCount(); ++i) {
            const auto& shot = playlistModel->shotAt(i);
            if (shot.path == path && shot.fps > 0.0) {
                defaultFps = shot.fps;
                break;
            }
        }
    }

    // Background automation must never block on a modal FPS picker. The
    // deterministic default is also the same value used when a user cancels.
    if (qApp && qApp->property("cgplay.automationBackground").toBool()) {
        return defaultFps;
    }

    QStringList items;
    int defaultIndex = 0;
    for (int i = 0; i < presets.size(); ++i) {
        const QString text = QString::number(presets[i], 'f', presets[i] == std::floor(presets[i]) ? 0 : 3);
        items << text;
        if (std::abs(presets[i] - defaultFps) < 0.0005) {
            defaultIndex = i;
        }
    }

    bool ok = false;
    const QString selected = QInputDialog::getItem(
        const_cast<MainWindow*>(this),
        QStringLiteral("选择帧速率"),
        QStringLiteral("为当前 EXR/序列选择播放帧速率："),
        items,
        defaultIndex,
        true,
        &ok);
    if (!ok) {
        return defaultFps;
    }

    bool valueOk = false;
    const double value = selected.toDouble(&valueOk);
    return valueOk && value > 0.0 ? value : defaultFps;
}

void MainWindow::_autoLoadReview()
{
    if (_p->currentPath.isEmpty()) return;
    QString rp = AnnotationStorage::getReviewPath(_p->currentPath);
    if (!QFileInfo::exists(rp)) return;
    auto anns = AnnotationStorage::load(rp);
    if (anns.isEmpty()) return;
    if (auto* annotationService = resolveAnnotationService(_p->annotationService, _annoMgr.get())) {
        annotationService->replaceAnnotations(anns);
    }
    if (_p->reviewPanel) {
        _p->reviewPanel->refresh(anns, _p->playbackCtrl->currentFrame());
    }
}

void MainWindow::_onAnnotationModeToggled(bool active) { _annoMode = active; }

void MainWindow::_toggleFullScreen()
{
    if (_p->fullscreen) _p->fullscreen->toggle();
}

void MainWindow::_showFullScreenChromeTemporarily()
{
    if (_p->fullscreen) _p->fullscreen->showChromeTemporarily();
}

void MainWindow::_hideFullScreenChrome()
{
    if (_p->fullscreen) _p->fullscreen->hideChrome();
}

void MainWindow::_layoutFullScreenOverlay()
{
    if (_p->fullscreen) _p->fullscreen->layoutOverlay();
}

void MainWindow::_setFullScreenChromeVisible(bool visible, bool forceApply)
{
    if (_p->fullscreen) _p->fullscreen->setChromeVisible(visible, forceApply);
}

void MainWindow::_setFullScreenCursorHidden(bool hidden)
{
    if (_p->fullscreen) _p->fullscreen->setCursorHidden(hidden);
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    _layoutGeneratedSubtitleOverlay();
    _restoreAuxDocksAfterShow();
    if (!_p->splitterLayoutRestoredAfterShow) {
        _p->splitterLayoutRestoredAfterShow = true;
        QTimer::singleShot(0, this, [this]() {
            _restoreSplitterLayoutIfNeeded();
        });
    }
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    _queueGeneratedSubtitleOverlayLayout();
    _layoutFullScreenOverlay();
    if (!_p->sidePanelLayoutQueued) {
        _p->sidePanelLayoutQueued = true;
        QTimer::singleShot(0, this, [this] {
            _p->sidePanelLayoutQueued = false;
            _applyAdaptiveSidePanelLayout(false);
        });
    }
}

void MainWindow::_restoreAuxDocksAfterShow()
{
    if (_p->auxDocksRestoredAfterShow) {
        return;
    }
    _p->auxDocksRestoredAfterShow = true;
    // AI docks start closed. Their explicit menu actions own visibility;
    // neither persisted state nor a delayed show may reopen them at startup.
}

void MainWindow::_restoreSplitterLayoutIfNeeded()
{
    if (!_p->horzSplitter || !_p->playlist || !_p->centerSplitter) {
        return;
    }
    const auto constrainedSizes = [this](QList<int> sizes) {
        if (sizes.size() != _p->horzSplitter->count() || sizes.size() < 3) return sizes;
        sizes[0] = 0;
        const int leftIndex = 1;
        const int centerIndex = 2;
        const int rightIndex = sizes.size() > 3 ? sizes.size() - 1 : -1;
        const bool leftVisible = _p->leftVisible;
        const bool rightVisible = rightIndex >= 0 && _p->reviewPanel && _p->rightVisible;
        int left = leftVisible ? qBound(1, sizes[leftIndex], 520) : 0;
        int right = rightVisible ? qBound(1, sizes[rightIndex], 520) : 0;
        const int total = qMax(_p->horzSplitter->width(), 0);
        constexpr int minimumViewerWidth = 280;
        int sideBudget = qMax(0, total - minimumViewerWidth);
        while (left + right > sideBudget && (left > 1 || right > 1)) {
            if (left >= right && left > 1) --left;
            else if (right > 1) --right;
            else break;
        }
        sizes[leftIndex] = left;
        if (rightIndex >= 0) sizes[rightIndex] = right;
        sizes[centerIndex] = qMax(minimumViewerWidth, total - left - right);
        return sizes;
    };
    _applyAdaptiveSidePanelLayout(true);
    if (_p->userSettings) {
        const QString preset = safeWorkspaceName(_p->userSettings->value(QStringLiteral("workspace/preset"), QStringLiteral("默认审片")).toString());
        const QString workspacePath = _p->settingsProfileService
            ? _p->settingsProfileService->workspacePath(preset)
            : QString{};
        const bool hasSavedWorkspace = QFileInfo::exists(workspacePath) || QFileInfo::exists(workspacePath + QStringLiteral(".bak"));
        bool recovered = false;
        QString workspaceError;
        const bool workspaceLoaded = hasSavedWorkspace && _p->workspaceController &&
            _p->workspaceController->load(preset, &recovered, &workspaceError);
        if (!workspaceError.isEmpty()) {
            qWarning() << "[Workspace]" << workspaceError;
        }
        if (workspaceLoaded) {
                const WorkspaceProfile& workspace = _p->workspaceController->currentProfile();
                const auto playlist = workspace.panels.value(QStringLiteral("playlist"));
                const auto review = workspace.panels.value(QStringLiteral("review"));
                _p->leftVisible = workspace.panels.contains(QStringLiteral("playlist")) ? playlist.visible : true;
                _p->rightVisible = workspace.panels.contains(QStringLiteral("review")) ? review.visible : true;
                if (playlist.size > 0) _p->lastLeftPanelWidth = playlist.size;
                if (review.size > 0) _p->lastRightPanelWidth = review.size;
                // Startup workspace restoration must not open the AI dock.
                // Preserve any explicit open/close made in this session.
                if (_p->translationToggleAction) {
                    _p->translationToggleAction->setChecked(workspace.translationEnabled);
                }
                {
                    const bool requested = workspace.secondaryWindow;
                    const bool automationMode = qobject_cast<Application*>(qApp) && qobject_cast<Application*>(qApp)->isAutomationMode();
                    if (!automationMode) QTimer::singleShot(0, this, [requested] {
                        QList<QPointer<SecondaryWindow>> windows;
                        for (QWidget* window : qApp->topLevelWidgets()) {
                            if (auto* secondary = qobject_cast<SecondaryWindow*>(window)) windows.push_back(secondary);
                        }
                        if (requested && windows.isEmpty()) {
                            if (auto* app = qobject_cast<Application*>(qApp)) app->openNewWindow(true);
                        } else if (!requested) {
                            for (const auto& secondary : windows) if (secondary) secondary->close();
                        }
                    });
                }
                // The built-in default审片 preset is a recovery baseline, not a
                // user-customizable hidden-panel preset.  Keep both side panels
                // available when an older acceptance run wrote a partial file.
                if (preset == QStringLiteral("默认审片")) {
                    _p->leftVisible = true;
                    _p->rightVisible = true;
                }
                _applyAdaptiveSidePanelLayout(true);
                if (workspace.splitterSizes.size() == _p->horzSplitter->count()) {
                    QList<int> restored;
                    for (const int value : workspace.splitterSizes) restored.push_back(std::max(0, value));
                    _p->horzSplitter->setSizes(constrainedSizes(restored));
                }
                return;
        }
    }
    if (_p->windowSettings && _p->windowSettings->contains(QStringLiteral("layout/sidePanelSizes"))) {
        const QVariantList stored = _p->windowSettings
            ->value(QStringLiteral("layout/sidePanelSizes"))
            .toList();
        if (stored.size() == _p->horzSplitter->count()) {
            QList<int> sizes;
            sizes.reserve(stored.size());
            for (const QVariant& value : stored) {
                sizes.push_back(std::max(0, value.toInt()));
            }
            sizes[0] = 0;
            if (!_p->leftVisible) sizes[1] = 0;
            if (_p->reviewPanel && !_p->rightVisible) sizes[sizes.size() - 1] = 0;
            _p->horzSplitter->setSizes(constrainedSizes(sizes));
        }
    }
}

void MainWindow::_applyAdaptiveSidePanelLayout(bool force)
{
    if (!_p->horzSplitter || !_p->playlist || !_p->centerSplitter || isFullScreen()) {
        return;
    }

    const int totalWidth = _p->horzSplitter->width();
    if (totalWidth <= 0) {
        return;
    }

    const int mode = totalWidth < 760 ? 0 : (totalWidth < 1220 ? 1 : 2);
    if (!force && mode == _p->sidePanelLayoutMode) {
        return;
    }
    _p->sidePanelLayoutMode = mode;

    const QList<int> currentSizes = _p->horzSplitter->sizes();
    const int currentLeft = currentSizes.size() > 1 ? currentSizes[1] : 0;
    const int currentRight = currentSizes.size() > 3 ? currentSizes.constLast() : 0;
    if (_p->playlist->isVisible() && currentLeft > 0) {
        _p->lastLeftPanelWidth = qBound(1, currentLeft, 520);
    }
    if (_p->reviewPanel && _p->reviewPanel->isVisible() && currentRight > 0) {
        _p->lastRightPanelWidth = qBound(1, currentRight, 520);
    }

    bool showLeft = _p->leftVisible;
    bool showRight = _p->rightVisible && _p->reviewPanel;
    if (mode == 0 && showLeft && showRight) {
        showLeft = _p->activeSidePanel == 1;
        showRight = !showLeft;
    }
    // Keep side widgets alive at width 0 so the native splitter handle remains
    // available for dragging them back out after a full collapse.
    _p->playlist->setVisible(true);
    if (_p->reviewPanel) {
        _p->reviewPanel->setVisible(true);
    }

    if (_p->leftPanelAction) {
        const QSignalBlocker blocker(_p->leftPanelAction);
        _p->leftPanelAction->setChecked(_p->leftVisible);
    }
    if (_p->rightPanelAction) {
        const QSignalBlocker blocker(_p->rightPanelAction);
        _p->rightPanelAction->setChecked(_p->rightVisible);
    }

    const bool dualPanels = showLeft && showRight;
    // Match the pre-customization reference proportions: the in-player
    // playlist/review rails stay compact so the viewer remains dominant.
    const int defaultSideWidth = mode == 0 ? 220 : (dualPanels ? 160 : 240);
    const int minimumRestoredSideWidth = dualPanels ? (mode == 0 ? defaultSideWidth : 1) : 220;
    const int leftWidth = showLeft
        ? (_p->lastLeftPanelWidth > 0 ? qBound(minimumRestoredSideWidth, _p->lastLeftPanelWidth, 520) : defaultSideWidth)
        : 0;
    const int rightWidth = showRight
        ? (_p->lastRightPanelWidth > 0 ? qBound(minimumRestoredSideWidth, _p->lastRightPanelWidth, 520) : defaultSideWidth)
        : 0;
    const int centerWidth = std::max(360, totalWidth - leftWidth - rightWidth);
    _p->sidePanelLayoutApplying = true;
    if (_p->reviewPanel) {
        _p->horzSplitter->setSizes({0, leftWidth, centerWidth, rightWidth});
    } else {
        _p->horzSplitter->setSizes({0, leftWidth, centerWidth});
    }
    _p->sidePanelLayoutApplying = false;
}

void MainWindow::_layoutTranslationStrip()
{
    // Legacy realtime translation strip was removed; generated subtitle overlay is the active UI.
}

void MainWindow::_setTranslationStripVisible(bool visible, bool refreshRuntime)
{
    Q_UNUSED(refreshRuntime);
    _p->translationStripVisible = visible;
    if (_p->generatedSubtitleLabel) {
        if (visible) {
            _p->generatedSubtitleLabel->hide();
        } else if (!_p->subtitleSession.state().generatedSubtitleCues.isEmpty()) {
            _updateGeneratedSubtitleForFrame(_p->playbackCtrl ? _p->playbackCtrl->currentFrame() : 0);
        }
    }
    if (_p->viewerShellLayout) {
        _p->viewerShellLayout->invalidate();
        _p->viewerShellLayout->activate();
    }
    if (_p->viewerShell) {
        _p->viewerShell->updateGeometry();
        _p->viewerShell->update();
    }
    if (_p->viewer) {
        _p->viewer->updateGeometry();
        _p->viewer->update();
    }
    if (_p->translationToggleAction) {
        const QSignalBlocker blocker(_p->translationToggleAction);
        _p->translationToggleAction->setChecked(visible);
    }
    if (_p->playbackBar) {
        _p->playbackBar->setTranslationVisible(visible);
    }
}

void MainWindow::_setupGeneratedSubtitleOverlay()
{
    if (_p->generatedSubtitleLabel || !_p->viewer) {
        return;
    }

    QWidget* host = _p->viewer->overlayParentWidget();
    if (!host) {
        host = _p->viewer;
    }

    auto* label = new QLabel(host);
    label->setObjectName(QStringLiteral("GeneratedSubtitleOverlayLabel"));
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    label->setStyleSheet(QStringLiteral(
        "QLabel#GeneratedSubtitleOverlayLabel{"
        "color:#FFFFFF;background:transparent;"
        "border:none;"
        "font-size:22px;font-weight:800;padding:0px 8px;}"));
    auto* shadow = new QGraphicsDropShadowEffect(label);
    shadow->setBlurRadius(16);
    shadow->setOffset(0, 2);
    shadow->setColor(QColor(0, 0, 0, 210));
    label->setGraphicsEffect(shadow);
    label->hide();
    _p->generatedSubtitleLabel = label;
    _layoutGeneratedSubtitleOverlay();
}

void MainWindow::_queueGeneratedSubtitleOverlayLayout()
{
    if (_p->generatedSubtitleLayoutQueued) {
        return;
    }
    _p->generatedSubtitleLayoutQueued = true;
    QTimer::singleShot(16, this, [this]() {
        _p->generatedSubtitleLayoutQueued = false;
        _layoutGeneratedSubtitleOverlay();
    });
}

void MainWindow::_layoutGeneratedSubtitleOverlay()
{
    if (!_p->generatedSubtitleLabel || !_p->viewer) {
        return;
    }

    QWidget* host = _p->viewer->overlayParentWidget();
    if (!host) {
        host = _p->viewer;
    }
    if (_p->generatedSubtitleLabel->parentWidget() != host) {
        _p->generatedSubtitleLabel->setParent(host);
    }

    const QRect viewerRect = host->rect();
    const int margin = 22;
    const int maxWidth = std::max(80, viewerRect.width() - margin * 2);
    int width = maxWidth;
    int height = 82;
    const QString labelText = _p->generatedSubtitleLabel->text().trimmed();
    if (!labelText.isEmpty()) {
        const QFontMetrics metrics(_p->generatedSubtitleLabel->font());
        const int maxHeight = std::max(96, viewerRect.height() / 3);
        const QRect measured = metrics.boundingRect(
            QRect(0, 0, maxWidth, maxHeight),
            Qt::AlignCenter | Qt::TextWordWrap,
            labelText);
        width = std::clamp(measured.width() + 20, 80, maxWidth);
        height = std::clamp(measured.height() + 18, 36, maxHeight);
    }
    const int x = std::max(margin, (viewerRect.width() - width) / 2);
    // Keep generated translations above the common burned-in subtitle band so
    // hard-subbed videos do not make the translated overlay look invisible.
    const int bottomOffset = 124;
    const int y = std::clamp(
        viewerRect.height() - height - bottomOffset,
        margin,
        std::max(margin, viewerRect.height() - height - margin));
    _p->generatedSubtitleLabel->setGeometry(x, y, width, height);
    _p->generatedSubtitleLabel->raise();
}

} // namespace cgplay
