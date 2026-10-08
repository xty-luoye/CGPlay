#include "ApplicationInternal.h"

#include "common/jobs/JobSystem.h"

#include <numeric>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace cgplay {

namespace {

constexpr auto kFullscreenDockVisibility = "cgplay.fullscreenWasVisible";

void setContentHostMargins(QSplitter* splitter, bool fullscreen)
{
    QWidget* contentHost = splitter ? splitter->parentWidget() : nullptr;
    QLayout* contentLayout = contentHost ? contentHost->layout() : nullptr;
    if (contentLayout) {
        contentLayout->setContentsMargins(fullscreen ? QMargins{} : QMargins{16, 0, 16, 8});
    }
}

void hideDockWidgetsForFullscreen(QWidget* owner)
{
    if (!owner) return;
    for (QDockWidget* dock : owner->findChildren<QDockWidget*>()) {
        if (!dock->property(kFullscreenDockVisibility).isValid()) {
            dock->setProperty(kFullscreenDockVisibility, dock->isVisible());
        }
        const QSignalBlocker blocker(dock);
        dock->hide();
    }
}

void restoreDockWidgetsAfterFullscreen(QWidget* owner)
{
    if (!owner) return;
    for (QDockWidget* dock : owner->findChildren<QDockWidget*>()) {
        const QVariant wasVisible = dock->property(kFullscreenDockVisibility);
        if (!wasVisible.isValid()) continue;
        dock->setProperty(kFullscreenDockVisibility, QVariant{});
        const QSignalBlocker blocker(dock);
        dock->setVisible(wasVisible.toBool());
    }
}

void setFullscreenSplitterHandles(QSplitter* horizontal, QSplitter* vertical, bool fullscreen)
{
    if (horizontal) horizontal->setHandleWidth(fullscreen ? 0 : 8);
    if (vertical) vertical->setHandleWidth(fullscreen ? 0 : 1);
}

QPoint systemCursorPosition()
{
#ifdef Q_OS_WIN
    POINT point{};
    if (::GetCursorPos(&point)) {
        return QPoint(point.x, point.y);
    }
#endif
    return QCursor::pos();
}

} // namespace

void MainWindow::openFile(const QString& path)
{
    if (path.isEmpty()) return;

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
        [this, job, path, sequenceFpsOverride, generation, mediaInfo](const JobOutcome&) {
            if (_p->mediaProbeJob == job) {
                _p->mediaProbeJob.clear();
            }
            if (generation != _p->mediaProbeGeneration || mediaInfo->error == QStringLiteral("Media probe canceled")) {
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

    if (incomingSuffix != QStringLiteral("exr") && _p->ocioManager) {
        _p->ocioManager->clearExrSceneLinearDefaults();
    }

    _p->currentPath = path;
    _p->currentMediaFingerprint = SubtitleGenerationService::mediaFingerprint(path, mediaInfo.durationSeconds);
    _p->mediaGenerationId += 1;
    if (_p->subtitleGenerationCancelRequested) {
        _p->subtitleGenerationCancelRequested->store(true);
    }
    if (_p->highQualityEnhancementCancelRequested) {
        _p->highQualityEnhancementCancelRequested->store(true);
    }
    _p->subtitleGenerationBusy = false;
    _p->subtitleGenerationWatcher.clear();
    _p->subtitleGenerationActiveTranslatedVttPath.clear();
    _p->highQualityEnhancementBusy = false;
    _p->highQualityEnhancementWatcher.clear();
    _p->highQualityEnhancementCancelRequested.reset();
    _p->highQualityEnhancementMediaPath.clear();
    _p->highQualityEnhancementLastDiagnostics = QJsonObject{
        { QStringLiteral("clearedForMediaSwitch"), true },
        { QStringLiteral("mediaFingerprint"), _p->currentMediaFingerprint },
        { QStringLiteral("mediaPath"), QFileInfo(path).absoluteFilePath() }
    };
    if (qApp) {
        qApp->setProperty("cgplay.currentMediaHighQualityEnhancement", QString());
    }
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
        } else if (sequenceFpsOverride > 0.0) {
            auto& shot = const_cast<ShotItem&>(playlistModel->shotAt(existingIndex));
            shot.fps = sequenceFpsOverride;
            shot.width = mediaInfo.width;
            shot.height = mediaInfo.height;
            shot.firstFrame = mediaInfo.firstFrame;
            shot.lastFrame = mediaInfo.lastFrame >= mediaInfo.firstFrame
                ? mediaInfo.lastFrame
                : mediaInfo.firstFrame + std::max(1, mediaInfo.effectiveFrameCount()) - 1;
            Q_EMIT playlistModel->dataChanged(
                playlistModel->index(existingIndex),
                playlistModel->index(existingIndex));
        } else if (existingIndex >= 0) {
            auto& shot = const_cast<ShotItem&>(playlistModel->shotAt(existingIndex));
            shot.width = mediaInfo.width;
            shot.height = mediaInfo.height;
            shot.fps = mediaInfo.fps > 0.0 ? mediaInfo.fps : shot.fps;
            shot.firstFrame = mediaInfo.firstFrame;
            shot.lastFrame = mediaInfo.lastFrame >= mediaInfo.firstFrame
                ? mediaInfo.lastFrame
                : mediaInfo.firstFrame + std::max(1, mediaInfo.effectiveFrameCount()) - 1;
            shot.format = mediaInfo.formatLabel.isEmpty() ? shot.format : mediaInfo.formatLabel;
            shot.name = mediaInfo.name.isEmpty() ? shot.name : mediaInfo.name;
            Q_EMIT playlistModel->dataChanged(
                playlistModel->index(existingIndex),
                playlistModel->index(existingIndex));
        }
        if (existingIndex >= 0) {
            playlistModel->setCurrentIndex(existingIndex);
        }
    }
    if (auto* s = _p->playbackCtrl->playbackStats()) s->reset();
    if (_p->timeline) _p->timeline->setMediaPath(path);
    _p->autoPlayPending = mediaInfo.isVideo;
    if (auto* playback = dynamic_cast<PlaybackController*>(_p->playbackCtrl.get())) {
        playback->openFile(path, sequenceFpsOverride, mediaInfo.codecName);
    } else {
        _p->playbackCtrl->openFile(path, sequenceFpsOverride);
    }
    // Apply after the timeline/player exists so tlRender's GPU viewport
    // receives the ACES options for the newly opened EXR item.
    if (incomingSuffix == QStringLiteral("exr") && _p->ocioManager) {
        _p->ocioManager->applyExrSceneLinearDefaults();
        // Playback creates the tlRender video item asynchronously. Rebind
        // once after that item exists so its GPU display shader receives the
        // exact ACES 1.2 options instead of retaining the pre-open defaults.
        QTimer::singleShot(250, this, [manager = _p->ocioManager] {
            if (manager) manager->applyExrSceneLinearDefaults();
        });
    }
    _p->generatedSubtitleCues.clear();
    _p->generatedSubtitlePath.clear();
    _p->generatedSubtitleMediaFingerprint.clear();
    _p->generatedSubtitleCoverageEndSeconds = 0.0;
    _p->generatedSubtitleProcessedEndSeconds = 0.0;
    _p->generatedSubtitleLastText.clear();
    _p->generatedSubtitleLastCueStartSeconds = -1.0;
    _p->generatedSubtitleLastCueEndSeconds = -1.0;
    _p->generatedSubtitleLastShownAtSeconds = -1.0;
    _p->subtitleContinuationScheduled = false;
    _p->subtitleContinuationStartSeconds = -1.0;
    _p->subtitleContinuationQueuedWhileBusy = false;
    _p->subtitleContinuationQueuedStartSeconds = -1.0;
    _p->subtitleContinuationRetryCount = 0;
    _p->highQualityVisualPrefetchLastCheckMs = 0;
    if (_p->generatedSubtitleLabel) {
        _p->generatedSubtitleLabel->hide();
        _p->generatedSubtitleLabel->clear();
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

void MainWindow::_closeCurrentMedia()
{
    _p->autoPlayPending = false;
    ++_p->mediaProbeGeneration;
    if (_p->mediaProbeJob) {
        _p->mediaProbeJob->cancel();
        _p->mediaProbeJob.clear();
    }
    _p->currentPath.clear();
    _p->currentMediaFingerprint.clear();
    _p->mediaGenerationId += 1;
    _p->generatedSubtitleCues.clear();
    _p->generatedSubtitlePath.clear();
    _p->generatedSubtitleMediaFingerprint.clear();
    _p->generatedSubtitleMediaFingerprint.clear();
    _p->generatedSubtitleCoverageEndSeconds = 0.0;
    _p->generatedSubtitleProcessedEndSeconds = 0.0;
    _p->generatedSubtitleLastText.clear();
    _p->generatedSubtitleLastCueStartSeconds = -1.0;
    _p->generatedSubtitleLastCueEndSeconds = -1.0;
    _p->generatedSubtitleLastShownAtSeconds = -1.0;
    _p->subtitleContinuationScheduled = false;
    _p->subtitleContinuationStartSeconds = -1.0;
    _p->subtitleContinuationQueuedWhileBusy = false;
    _p->subtitleContinuationQueuedStartSeconds = -1.0;
    _p->subtitleContinuationRetryCount = 0;
    _p->highQualityVisualPrefetchLastCheckMs = 0;
    if (_p->highQualityEnhancementCancelRequested) {
        _p->highQualityEnhancementCancelRequested->store(true);
    }
    _p->highQualityEnhancementBusy = false;
    _p->highQualityEnhancementWatcher.clear();
    _p->highQualityEnhancementCancelRequested.reset();
    _p->highQualityEnhancementMediaPath.clear();
    _p->highQualityEnhancementLastDiagnostics = QJsonObject();
    if (_p->generatedSubtitleLabel) {
        _p->generatedSubtitleLabel->hide();
        _p->generatedSubtitleLabel->clear();
    }
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
    const bool fullscreen = _p->fullscreenActive || isFullScreen();
    const quint64 transitionGeneration = ++_p->fullscreenTransitionGeneration;
    if (fullscreen) {
        // An exit can cancel entry before its deferred layout has completed.
        // The cancelled callback must never leave the window unable to paint.
        _p->fullscreenEntryPending = false;
        if (_p->fullscreenEntryUpdatesSuspended) {
            _p->fullscreenEntryUpdatesSuspended = false;
            setUpdatesEnabled(true);
        }
        _p->fullscreenActive = false;
        _p->fullscreenRestorePending = true;
        if (_p->fullscreenChromeTimer) {
            _p->fullscreenChromeTimer->stop();
        }
        if (_p->fullscreenMousePollTimer) {
            _p->fullscreenMousePollTimer->stop();
        }
        _setFullScreenCursorHidden(false);
        _p->topBar->setVisible(_p->fullscreenTopBar);
        _p->playlist->setVisible(_p->fullscreenLeft);
        if (_p->reviewPanel) _p->reviewPanel->setVisible(_p->fullscreenRight);
        restoreDockWidgetsAfterFullscreen(this);
        _p->navRail->hide();
        if (_p->fullscreenTimelineHeight > 0) {
            _p->timeline->setFixedHeight(_p->fullscreenTimelineHeight);
        }
        if (_p->fullscreenPlaybackBarHeight > 0) {
            _p->playbackBar->setFixedHeight(_p->fullscreenPlaybackBarHeight);
        }
        _p->timeline->setVisible(_p->fullscreenTimeline);
        _p->playbackBar->setVisible(_p->fullscreenPlaybackBar);
        _p->compareBar->setVisible(_p->fullscreenCompareBar);
        _p->viewer->setChromeVisible(_p->fullscreenViewerChrome);
        if (statusBar()) {
            statusBar()->setVisible(_p->fullscreenStatusBar);
        }
        if (_p->annoToolbar) {
            _p->annoToolbar->setVisible(_p->annoToolsVisible);
            _onAnnotationModeToggled(_p->annoToolsVisible);
        } else {
            _onAnnotationModeToggled(false);
        }
        if (auto* vp = _p->viewer->viewport()) vp->setFrameView(true);
        setContentHostMargins(_p->horzSplitter, false);
        setFullscreenSplitterHandles(_p->horzSplitter, _p->centerSplitter, false);
        if (_p->centerSplitter) {
            _p->centerSplitter->setCollapsible(1, false);
            _p->centerSplitter->setCollapsible(2, false);
        }
        const bool restoreMaximized = _p->fullscreenWindowState.testFlag(Qt::WindowMaximized);
        showNormal();
        const QList<int> centerSizes = _p->fullscreenCenterSizes;
        QTimer::singleShot(0, this, [this, centerSizes, transitionGeneration, restoreMaximized]() {
            if (_p->fullscreenTransitionGeneration != transitionGeneration ||
                _p->fullscreenActive || isFullScreen()) return;
            if (_p->centerSplitter && centerSizes.size() == _p->centerSplitter->count()) {
                _p->centerSplitter->setSizes(centerSizes);
            }
            // Geometry restoration can clear Qt's maximized state. Apply it
            // after restoring normal geometry and splitter layout.
            if (restoreMaximized) showMaximized();
        });
        // Windows may deliver the old normal-geometry state after the queued
        // layout pass. Reconcile once after that transition, never polling.
        QTimer::singleShot(150, this, [this, transitionGeneration, restoreMaximized]() {
            if (_p->fullscreenTransitionGeneration != transitionGeneration ||
                _p->fullscreenActive || isFullScreen()) return;
            _p->fullscreenRestorePending = false;
            if (restoreMaximized && !isMaximized()) showMaximized();
        });
    } else {
        // A new entry before the previous exit settles must inherit its
        // intended window state, rather than a temporary normal geometry.
        if (!_p->fullscreenRestorePending) _p->fullscreenWindowState = windowState();
        _p->fullscreenRestorePending = false;
        _p->fullscreenActive = true;
        if (!_p->fullscreenMousePollTimer) {
            _p->fullscreenMousePollTimer = new QTimer(this);
            _p->fullscreenMousePollTimer->setInterval(50);
            connect(_p->fullscreenMousePollTimer, &QTimer::timeout, this, [this]() {
                if (!isFullScreen() || _p->fullscreenEntryPending) {
                    return;
                }
                const QPoint cursorPos = systemCursorPosition();
                const QRect windowRect(mapToGlobal(QPoint(0, 0)), size());
                const int activationHeight = std::clamp(height() / 5, 96, 220);
                const bool cursorNearBottom = windowRect.contains(cursorPos) &&
                    cursorPos.y() >= windowRect.bottom() - activationHeight;
                if (cursorPos != _p->fullscreenLastCursorPos || cursorNearBottom) {
                    _p->fullscreenLastCursorPos = cursorPos;
                    _showFullScreenChromeTemporarily();
                }
            });
        }
        _p->fullscreenLeft  = _p->playlist->isVisible();
        _p->fullscreenRight = _p->reviewPanel ? _p->reviewPanel->isVisible() : false;
        _p->fullscreenNav = false;
        _p->fullscreenTopBar = _p->topBar->isVisible();
        _p->fullscreenTimeline = _p->timeline->isVisible();
        _p->fullscreenPlaybackBar = _p->playbackBar->isVisible();
        _p->fullscreenStatusBar = statusBar() ? statusBar()->isVisible() : true;
        _p->fullscreenCompareBar = _p->compareBar->isVisible();
        _p->fullscreenAIDock = _p->aiDock ? _p->aiDock->isVisible() : false;
        _p->fullscreenViewerChrome = true;
        _p->fullscreenCenterSizes = _p->centerSplitter ? _p->centerSplitter->sizes() : QList<int>{};
        _p->fullscreenTimelineHeight = _p->timeline->height();
        _p->fullscreenPlaybackBarHeight = _p->playbackBar->height();
        _p->annoToolsVisible = _p->annoToolbar ? _p->annoToolbar->isVisible() : false;
        // Keep intermediate native/chrome layouts out of the compositor.
        // Suspend painting only for this queued layout transaction, not for
        // a wall-clock delay that visibly freezes an already playing movie.
        _p->fullscreenEntryPending = true;
        _p->fullscreenEntryUpdatesSuspended = updatesEnabled();
        if (_p->fullscreenEntryUpdatesSuspended) setUpdatesEnabled(false);
        _p->fullscreenLastCursorPos = systemCursorPosition();
        _p->fullscreenMousePollTimer->stop();
        showFullScreen();
        // Let Windows/Qt complete the native fullscreen transition before
        // collapsing docks and splitter rows. Synchronous layout churn here
        // can stall the OpenGL surface and leave a black frame for seconds.
        QTimer::singleShot(0, this, [this, transitionGeneration]() {
            if (_p->fullscreenTransitionGeneration != transitionGeneration) return;
            if (_p->fullscreenActive && isFullScreen()) {
                setContentHostMargins(_p->horzSplitter, true);
                setFullscreenSplitterHandles(_p->horzSplitter, _p->centerSplitter, true);
                _setFullScreenChromeVisible(false, true);
            }
            // The chrome helper queues one final splitter reconciliation.
            // Commit after it, then let Qt coalesce one paint at final size.
            // Always release our suspension even if native entry is refused.
            QTimer::singleShot(0, this, [this, transitionGeneration]() {
                if (_p->fullscreenTransitionGeneration != transitionGeneration) return;
                const bool entered = _p->fullscreenActive && isFullScreen();
                if (entered) {
                    if (layout()) layout()->activate();
                    if (_p->viewer->layout()) _p->viewer->layout()->activate();
                    if (auto* vp = _p->viewer->viewport()) vp->setFrameView(true);
                }
                _p->fullscreenEntryPending = false;
                if (_p->fullscreenEntryUpdatesSuspended) {
                    _p->fullscreenEntryUpdatesSuspended = false;
                    setUpdatesEnabled(true);
                }
                if (entered && _p->fullscreenMousePollTimer) {
                    _p->fullscreenLastCursorPos = systemCursorPosition();
                    _p->fullscreenMousePollTimer->start();
                }
            });
        });
    }

}

void MainWindow::_showFullScreenChromeTemporarily()
{
    if (!isFullScreen() || _p->fullscreenEntryPending) {
        return;
    }
    _setFullScreenChromeVisible(true);
    if (_p->fullscreenChromeTimer) {
        _p->fullscreenChromeTimer->start(2000);
    }
}

void MainWindow::_hideFullScreenChrome()
{
    if (!isFullScreen()) {
        return;
    }
    _setFullScreenChromeVisible(false);
}

void MainWindow::_setFullScreenChromeVisible(bool visible, bool forceApply)
{
    if (!forceApply && !isFullScreen()) {
        return;
    }

    // Input and the 50 ms cursor poll can request the same state hundreds of
    // times. Only a visibility transition needs dock/layout and paint work.
    if (!forceApply && _p->fullscreenChromeVisible == visible) return;
    const quint64 transitionGeneration = _p->fullscreenTransitionGeneration;
    ++_p->fullscreenChromeApplyCount;
    _p->fullscreenChromeVisible = visible;
    _p->topBar->hide();
    _p->navRail->hide();
    _p->playlist->hide();
    if (_p->reviewPanel) _p->reviewPanel->hide();
    hideDockWidgetsForFullscreen(this);
    // Keep raster controls in the fullscreen composition and collapse their
    // splitter rows instead of hide/show. QOpenGLWidget can otherwise retain
    // or cover stale sibling surfaces after the first fullscreen frame.
    _p->timeline->setVisible(visible && _p->fullscreenTimeline);
    _p->playbackBar->setVisible(visible && _p->fullscreenPlaybackBar);
    _p->timeline->setFixedHeight(visible && _p->fullscreenTimeline
        ? std::max(1, _p->fullscreenTimelineHeight)
        : 0);
    _p->playbackBar->setFixedHeight(visible && _p->fullscreenPlaybackBar
        ? std::max(1, _p->fullscreenPlaybackBarHeight)
        : 0);
    if (_p->centerSplitter) {
        _p->centerSplitter->setHandleWidth(visible ? 1 : 0);
        _p->centerSplitter->setCollapsible(1, true);
        _p->centerSplitter->setCollapsible(2, true);
        if (visible && _p->fullscreenCenterSizes.size() == _p->centerSplitter->count()) {
            _p->centerSplitter->setSizes(_p->fullscreenCenterSizes);
            QTimer::singleShot(0, this, [this, transitionGeneration]() {
                if (_p->fullscreenTransitionGeneration == transitionGeneration &&
                    _p->fullscreenActive && isFullScreen() && _p->fullscreenChromeVisible && _p->centerSplitter &&
                    _p->fullscreenCenterSizes.size() == _p->centerSplitter->count()) {
                    _p->centerSplitter->setSizes(_p->fullscreenCenterSizes);
                    _p->timeline->raise();
                    _p->playbackBar->raise();
                    _p->timeline->update();
                    _p->playbackBar->update();
                    _p->centerSplitter->update();
                }
            });
        } else if (!visible && _p->centerSplitter->count() == 3) {
            const QList<int> sizes = _p->centerSplitter->sizes();
            const int total = std::accumulate(sizes.cbegin(), sizes.cend(), 0);
            _p->centerSplitter->setSizes({std::max(total, _p->centerSplitter->height()), 0, 0});
            QTimer::singleShot(0, this, [this, transitionGeneration]() {
                if (_p->fullscreenTransitionGeneration != transitionGeneration ||
                    !_p->fullscreenActive || !isFullScreen() || _p->fullscreenChromeVisible) return;
                _p->timeline->hide();
                _p->playbackBar->hide();
                _p->timeline->setFixedHeight(0);
                _p->playbackBar->setFixedHeight(0);
                if (_p->centerSplitter && _p->centerSplitter->count() == 3) {
                    const int total = std::max(_p->centerSplitter->height(), 1);
                    _p->centerSplitter->setSizes({total, 0, 0});
                }
                if (_p->viewerShell) _p->viewerShell->raise();
                if (_p->viewer) _p->viewer->raise();
                // A synchronous repaint at every ancestor can render the
                // OpenGL surface repeatedly during a single layout change.
                // update() lets Qt combine these into its next composition.
                if (_p->viewerShell) _p->viewerShell->update();
                if (_p->viewer && _p->viewer->viewport()) _p->viewer->viewport()->update();
                if (_p->centerSplitter) _p->centerSplitter->update();
                update();
            });
        }
    }
    _p->compareBar->hide();
    _p->viewer->setChromeVisible(false);
    if (statusBar()) {
        statusBar()->hide();
    }
    if (_p->annoToolbar) {
        _p->annoToolbar->hide();
    }
    _setFullScreenCursorHidden(!visible);
}

void MainWindow::_setFullScreenCursorHidden(bool hidden)
{
    if (_p->fullscreenCursorHidden == hidden) {
        return;
    }
    _p->fullscreenCursorHidden = hidden;
    if (hidden) {
        QApplication::setOverrideCursor(Qt::BlankCursor);
    } else {
        QApplication::restoreOverrideCursor();
    }
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
        } else if (!_p->generatedSubtitleCues.isEmpty()) {
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
