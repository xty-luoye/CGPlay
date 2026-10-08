#include "ApplicationInternal.h"

namespace cgplay {

void MainWindow::_restoreState()
{
    const bool restored = _p->windowSettings &&
        _p->windowSettings->contains("geometry") &&
        restoreGeometry(_p->windowSettings->value("geometry").toByteArray());
    if (!restored) {
        if (QScreen* screen = this->screen()) {
            const QRect available = screen->availableGeometry();
            const int width = std::clamp(static_cast<int>(available.width() * 0.76), 1120, 1560);
            const int height = std::clamp(static_cast<int>(available.height() * 0.78), 680, 980);
            resize(width, height);
            const int x = available.x() + (available.width() - width) / 2;
            const int y = available.y() + (available.height() - height) / 2;
            move(x, y);
        } else {
            resize(1360, 820);
        }
    }

    _p->windowStateRestored = false;
    if (_p->windowSettings && _p->windowSettings->contains("windowState")) {
        // The previous build persisted a floating Codex/settings topology.
        // Do not restore that legacy blob; the splitter/workspace JSON below
        // is the authoritative layout for the current player.
        _p->windowSettings->remove(QStringLiteral("windowState"));
        _p->windowSettings->sync();
        _p->windowStateRestored = false;
    }
    // A saved layout must not open an AI workspace on the next launch.
    if (_p->aiDock) {
        _p->aiDock->hide();
    }
    if (_p->windowSettings) {
        _p->windowSettings->setValue(QStringLiteral("ai/workspaceVisible"), false);
        _p->windowSettings->setValue(QStringLiteral("codex/workspaceVisible"), false);
    }
    if (_p->windowSettings) {
        _p->leftVisible = _p->windowSettings
            ->value(QStringLiteral("layout/leftPanelVisible"), true).toBool();
        _p->rightVisible = _p->windowSettings
            ->value(QStringLiteral("layout/rightPanelVisible"), true).toBool();
    }
    if (_p->windowSettings) {
        _p->translationPlaybackMode = TranslationPlaybackStrategy::normalizeModeId(
            _p->windowSettings->value(
                QStringLiteral("ai/subtitles/playbackMode"),
                _p->translationPlaybackMode).toString());
        if (_p->playbackBar) {
            _p->playbackBar->setTranslationMode(_p->translationPlaybackMode);
        }
        _setTranslationStripVisible(false, false);
    }

    // Keep the default审片 layout deterministic after an invalid/legacy state:
    // playlist left, review panel right, viewer in the center. AI docks remain
    // closed until the user opens them in the current session.
    if (!_p->windowStateRestored) {
        _p->leftVisible = true;
        _p->rightVisible = true;
        if (_p->aiDock) {
            _p->aiDock->setFloating(false);
            _p->aiDock->hide();
        }
        if (_p->horzSplitter) _applyAdaptiveSidePanelLayout(true);
    }

    if (width() < 1120 || height() < 680) {
        resize(1360, 820);
    }
}

void MainWindow::_saveState()
{
    if (_p->windowSettings) {
        _p->windowSettings->setValue("geometry", saveGeometry());
        // Do not persist Qt's opaque dock blob.  It can retain removed or
        // floating Codex/settings widgets across layout versions; workspace
        // JSON and the explicit splitter keys are the stable persistence API.
        _p->windowSettings->remove(QStringLiteral("windowState"));
        _p->windowSettings->setValue(QStringLiteral("layout/leftPanelVisible"), _p->leftVisible);
        _p->windowSettings->setValue(QStringLiteral("layout/rightPanelVisible"), _p->rightVisible);
        if (_p->horzSplitter) {
            QVariantList sizes;
            for (const int size : _p->horzSplitter->sizes()) sizes.push_back(size);
            _p->windowSettings->setValue(QStringLiteral("layout/sidePanelSizes"), sizes);
        }
        if (_p->aiDock) {
            _p->windowSettings->setValue(QStringLiteral("ai/workspaceVisible"), _p->aiDock->isVisible());
        }
        _p->windowSettings->setValue(
            QStringLiteral("ai/subtitles/playbackMode"),
            TranslationPlaybackStrategy::normalizeModeId(_p->translationPlaybackMode));
        _p->windowSettings->sync();
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    _p->updateClosing = true;
    if (_p->updateCheckState) {
        _p->updateCheckState->cancelled.store(true);
    }
    if (_p->updateDownloadState) {
        _p->updateDownloadState->cancelled.store(true);
    }
    if (_p->subtitleRefinementCancelRequested) {
        _p->subtitleRefinementCancelRequested->store(true);
    }
    if (_p->highQualityEnhancementCancelRequested) {
        _p->highQualityEnhancementCancelRequested->store(true);
    }
    if (_p->subtitleGenerationBusy) {
        if (_p->subtitleGenerationCancelRequested) {
            _p->subtitleGenerationCancelRequested->store(true);
        }
        if (_p->subtitleGenerationProgress) {
            _p->subtitleGenerationProgress->close();
        }
        if (statusBar()) {
            statusBar()->showMessage(QStringLiteral("已取消后台字幕任务，正在关闭。"), 1500);
        }
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        QTimer::singleShot(1500, []() {
            std::_Exit(0);
        });
    }
    // Close the active media while the main window and its services are
    // still alive.  This releases tlRender and cancels timeline thumbnails
    // even when another window or a background task keeps the process alive.
    _closeCurrentMedia();
    _setFullScreenCursorHidden(false);
    _saveState();
    event->accept();
}
void MainWindow::dragEnterEvent(QDragEnterEvent* event) { if (event->mimeData()->hasUrls()) event->acceptProposedAction(); }
void MainWindow::dropEvent(QDropEvent* event) { const auto u = event->mimeData()->urls(); if (!u.isEmpty()) openFile(u.first().toLocalFile()); }

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    constexpr int kSidePanelHitRadius = 6;
    constexpr int kMinimumViewerWidth = 360;

    auto clearSidePanelCursor = [this]() {
        if (_p->sidePanelCursorWidget) {
            _p->sidePanelCursorWidget->unsetCursor();
            _p->sidePanelCursorWidget = nullptr;
        }
    };

    const auto finishSettingsScrollDrag = [this]() {
        if (!_p->settingsScrollDragging) return;
        const QPointer<QWidget> grabWidget = _p->settingsDragGrabWidget;
        _p->settingsScrollDragging = false;
        _p->settingsDragScrollArea.clear();
        _p->settingsDragGrabWidget.clear();
        if (grabWidget) {
            if (QWidget::mouseGrabber() == grabWidget) {
                grabWidget->releaseMouse();
            }
            grabWidget->unsetCursor();
        }
    };

    const auto finishSidePanelDrag = [this]() {
        if (!_p->sidePanelDragging) return;
        _p->sidePanelDragging = false;
        _p->sidePanelDragBoundary = 0;
        _p->sidePanelDragStartSizes.clear();
        if (QWidget::mouseGrabber() == this) releaseMouse();
        if (_p->sidePanelDragOverrideCursor) {
            QApplication::restoreOverrideCursor();
            _p->sidePanelDragOverrideCursor = false;
        }
    };

    if (event->type() == QEvent::WindowDeactivate ||
        event->type() == QEvent::Hide ||
        event->type() == QEvent::UngrabMouse) {
        finishSettingsScrollDrag();
        finishSidePanelDrag();
        clearSidePanelCursor();
    }

    auto boundaryAt = [this](const QPoint& globalPos) {
        if (isFullScreen() || !_p->horzSplitter || !_p->playlist || !_p->centerSplitter) {
            return 0;
        }

        const QRect splitterRect(
            _p->horzSplitter->mapToGlobal(QPoint(0, 0)),
            _p->horzSplitter->size());
        if (!splitterRect.adjusted(-kSidePanelHitRadius, 0, kSidePanelHitRadius, 0)
                 .contains(globalPos)) {
            return 0;
        }

        {
            const int boundaryX = (_p->leftVisible && _p->playlist->isVisible())
                ? _p->playlist->mapToGlobal(QPoint(_p->playlist->width(), 0)).x()
                : _p->centerSplitter->mapToGlobal(QPoint(0, 0)).x();
            if (std::abs(globalPos.x() - boundaryX) <= kSidePanelHitRadius) {
                return 1;
            }
        }

        if (_p->reviewPanel) {
            const int boundaryX = (_p->rightVisible && _p->reviewPanel->isVisible())
                ? _p->reviewPanel->mapToGlobal(QPoint(0, 0)).x()
                : _p->centerSplitter->mapToGlobal(
                      QPoint(_p->centerSplitter->width(), 0)).x();
            if (std::abs(globalPos.x() - boundaryX) <= kSidePanelHitRadius) {
                return 2;
            }
        }
        return 0;
    };

    auto* eventWidget = qobject_cast<QWidget*>(obj);
    const bool eventBelongsToSettings =
        eventWidget && _p->settingsDialog &&
        eventWidget->window() == _p->settingsDialog.data();
    const auto settingsScrollAt = [this](const QPoint& globalPos) -> QScrollArea* {
        if (!_p->settingsDialog) return nullptr;
        for (auto* scroll : _p->settingsDialog->findChildren<QScrollArea*>()) {
            if (!scroll || !scroll->isVisible() || !scroll->verticalScrollBar()) continue;
            const QRect globalRect(scroll->mapToGlobal(QPoint(0, 0)), scroll->size());
            if (globalRect.contains(globalPos)) return scroll;
        }
        return nullptr;
    };
    const auto settingsScrollForWidget = [&settingsScrollAt](QWidget* widget, const QPoint& globalPos) -> QScrollArea* {
        for (QWidget* current = widget; current; current = current->parentWidget()) {
            if (auto* scroll = qobject_cast<QScrollArea*>(current)) return scroll;
        }
        return settingsScrollAt(globalPos);
    };

    if (eventBelongsToSettings && event->type() == QEvent::Wheel) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        if (auto* scroll = settingsScrollForWidget(eventWidget, wheel->globalPosition().toPoint())) {
            const int pixelDelta = wheel->pixelDelta().y();
            const int angleDelta = wheel->angleDelta().y();
            int delta = pixelDelta != 0 ? pixelDelta : qRound(angleDelta * 0.5);
            if (delta == 0 && angleDelta != 0) delta = angleDelta > 0 ? 1 : -1;
            if (wheel->inverted()) delta = -delta;
            if (delta != 0) {
                auto* bar = scroll->verticalScrollBar();
                bar->setValue(bar->value() - delta);
                wheel->accept();
                return true;
            }
        }
    }

    if (eventBelongsToSettings &&
        (event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::MouseMove ||
         event->type() == QEvent::MouseButtonRelease)) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (event->type() == QEvent::MouseButtonPress &&
            mouse->button() == Qt::MiddleButton && !_p->settingsScrollDragging) {
            if (auto* scroll = settingsScrollForWidget(eventWidget, mouse->globalPosition().toPoint())) {
                _p->settingsScrollDragging = true;
                _p->settingsDragScrollArea = scroll;
                _p->settingsDragGrabWidget = scroll->viewport();
                _p->settingsScrollStartGlobalY = mouse->globalPosition().toPoint().y();
                _p->settingsScrollStartValue = scroll->verticalScrollBar()->value();
                _p->settingsDragGrabWidget->grabMouse();
                _p->settingsDragGrabWidget->setCursor(Qt::ClosedHandCursor);
                mouse->accept();
                return true;
            }
        }
        if (event->type() == QEvent::MouseMove && _p->settingsScrollDragging) {
            if (_p->settingsDragScrollArea && _p->settingsDragScrollArea->verticalScrollBar()) {
                const int deltaY = mouse->globalPosition().toPoint().y() - _p->settingsScrollStartGlobalY;
                _p->settingsDragScrollArea->verticalScrollBar()->setValue(
                    _p->settingsScrollStartValue - deltaY);
            }
            mouse->accept();
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease &&
            mouse->button() == Qt::MiddleButton && _p->settingsScrollDragging) {
            finishSettingsScrollDrag();
            mouse->accept();
            return true;
        }
    }

    // Settings child widgets own their ordinary left-button/key interaction.
    // Passing those events through QMainWindow's filter path made sliders
    // lose drag sequences while the settings dialog was active.
    if (eventBelongsToSettings) return false;

    const bool eventBelongsToMainWindow = eventWidget && eventWidget->window() == this;

    if (false && eventBelongsToMainWindow && (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonRelease)) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        const QPoint globalPos = mouseEvent->globalPosition().toPoint();

        if (event->type() == QEvent::MouseButtonPress &&
            mouseEvent->button() == Qt::LeftButton && !_p->sidePanelDragging) {
            const int boundary = boundaryAt(globalPos);
            if (boundary != 0) {
                _p->sidePanelDragging = true;
                _p->sidePanelDragBoundary = boundary;
                _p->sidePanelDragStartGlobalX = globalPos.x();
                _p->sidePanelDragStartSizes = _p->horzSplitter->sizes();
                if (boundary == 1 && !_p->playlist->isVisible()) {
                    _p->playlist->show();
                    _p->horzSplitter->setSizes(_p->sidePanelDragStartSizes);
                } else if (boundary == 2 && _p->reviewPanel && !_p->reviewPanel->isVisible()) {
                    _p->reviewPanel->show();
                    _p->horzSplitter->setSizes(_p->sidePanelDragStartSizes);
                }
                clearSidePanelCursor();
                grabMouse();
                QApplication::setOverrideCursor(Qt::SplitHCursor);
                _p->sidePanelDragOverrideCursor = true;
                return true;
            }
        }

        if (event->type() == QEvent::MouseMove && _p->sidePanelDragging) {
            QList<int> sizes = _p->sidePanelDragStartSizes;
            if (sizes.size() == _p->horzSplitter->count() && sizes.size() >= 3) {
                const int delta = globalPos.x() - _p->sidePanelDragStartGlobalX;
                const int centerIndex = 2;
                if (_p->sidePanelDragBoundary == 1) {
                    const int leftIndex = 1;
                    const int available = sizes[leftIndex] + sizes[centerIndex];
                    const int maximum = std::max(0, std::min(
                        _p->playlist->maximumWidth(),
                        available - kMinimumViewerWidth));
                    if (maximum > 0) {
                        const int requested = sizes[leftIndex] + delta;
                        const int width = std::clamp(requested, 0, maximum);
                        sizes[centerIndex] = available - width;
                        sizes[leftIndex] = width;
                    }
                } else if (_p->sidePanelDragBoundary == 2 && _p->reviewPanel) {
                    const int rightIndex = sizes.size() - 1;
                    const int available = sizes[centerIndex] + sizes[rightIndex];
                    const int maximum = std::max(0, std::min(
                        _p->reviewPanel->maximumWidth(),
                        available - kMinimumViewerWidth));
                    if (maximum > 0) {
                        const int requested = sizes[rightIndex] - delta;
                        const int width = std::clamp(requested, 0, maximum);
                        sizes[centerIndex] = available - width;
                        sizes[rightIndex] = width;
                    }
                }
                _p->horzSplitter->setSizes(sizes);
            }
            return true;
        }

        if (event->type() == QEvent::MouseButtonRelease && _p->sidePanelDragging) {
            const int releasedBoundary = _p->sidePanelDragBoundary;
            const QList<int> releasedSizes = _p->horzSplitter->sizes();
            _p->sidePanelDragging = false;
            _p->sidePanelDragBoundary = 0;
            _p->sidePanelDragStartSizes.clear();
            releaseMouse();
            if (_p->sidePanelDragOverrideCursor) {
                QApplication::restoreOverrideCursor();
                _p->sidePanelDragOverrideCursor = false;
            }
            if (releasedBoundary == 1 && releasedSizes.size() > 2) {
                _p->leftVisible = releasedSizes[1] > 0;
                if (_p->leftVisible) _p->lastLeftPanelWidth = releasedSizes[1];
                _p->playlist->setVisible(_p->leftVisible);
                if (_p->leftPanelAction) {
                    const QSignalBlocker blocker(_p->leftPanelAction);
                    _p->leftPanelAction->setChecked(_p->leftVisible);
                }
            } else if (releasedBoundary == 2 && _p->reviewPanel && releasedSizes.size() > 3) {
                _p->rightVisible = releasedSizes.constLast() > 0;
                if (_p->rightVisible) _p->lastRightPanelWidth = releasedSizes.constLast();
                _p->reviewPanel->setVisible(_p->rightVisible);
                if (_p->rightPanelAction) {
                    const QSignalBlocker blocker(_p->rightPanelAction);
                    _p->rightPanelAction->setChecked(_p->rightVisible);
                }
            }
            if (_p->windowSettings && _p->horzSplitter) {
                QVariantList sizes;
                for (const int size : _p->horzSplitter->sizes()) {
                    sizes.push_back(size);
                }
                _p->windowSettings->setValue(QStringLiteral("layout/sidePanelSizes"), sizes);
                _p->windowSettings->setValue(QStringLiteral("layout/leftPanelVisible"), _p->leftVisible);
                _p->windowSettings->setValue(QStringLiteral("layout/rightPanelVisible"), _p->rightVisible);
                _p->windowSettings->sync();
            }
            return true;
        }

        if (event->type() == QEvent::MouseMove && !_p->sidePanelDragging) {
            auto* widget = qobject_cast<QWidget*>(obj);
            const bool nearBoundary = boundaryAt(globalPos) != 0;
            if (nearBoundary && widget) {
                if (_p->sidePanelCursorWidget != widget) {
                    clearSidePanelCursor();
                    widget->setCursor(Qt::SplitHCursor);
                    _p->sidePanelCursorWidget = widget;
                }
            } else {
                clearSidePanelCursor();
            }
        }
    } else if (event->type() == QEvent::Leave && obj == _p->sidePanelCursorWidget) {
        clearSidePanelCursor();
    }

    const bool fullscreenActive = _p->fullscreenActive || isFullScreen();
    if (fullscreenActive && event->type() == QEvent::Resize && _p->viewer && obj == _p->viewer->viewport())
        _layoutFullScreenOverlay();
    if (fullscreenActive && eventBelongsToMainWindow &&
        (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress ||
         event->type() == QEvent::KeyRelease)) {
        auto* key = static_cast<QKeyEvent*>(event);
        // Route transport through the command even when a hidden toolbar or
        // a focused button would otherwise consume Space. Honor user rebinding.
        const bool editing = isTextInputFocusWidget(eventWidget);
        if (!editing) {
            const QKeySequence pressed(QKeyCombination(key->modifiers(), Qt::Key(key->key())));
            for (const auto& descriptor : _p->commandDescriptors) {
                if (descriptor.id != QStringLiteral("playback.toggle")) continue;
                if (!descriptor.shortcut.trimmed().isEmpty() &&
                    QKeySequence(descriptor.shortcut).matches(pressed) == QKeySequence::ExactMatch) {
                    if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()) {
                        _executeCommandId(descriptor.id);
                        _showFullScreenChromeTemporarily();
                    }
                    key->accept();
                    return true;
                }
                break;
            }
        }
    }
    if (eventBelongsToMainWindow &&
        (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const bool exitsFullscreen = fullscreenActive && keyEvent->key() == Qt::Key_Escape &&
            keyEvent->modifiers() == Qt::NoModifier;
        const bool textInputKey = isTextInputFocusWidget(eventWidget) &&
            (keyEvent->modifiers() == Qt::NoModifier || keyEvent->modifiers() == Qt::ShiftModifier) &&
            (!keyEvent->text().isEmpty() || keyEvent->key() == Qt::Key_Delete || keyEvent->key() == Qt::Key_Backspace);
        bool togglesFullscreen = false;
        if (!textInputKey) {
            const QKeySequence pressed(QKeyCombination(keyEvent->modifiers(), Qt::Key(keyEvent->key())));
            for (const auto& descriptor : _p->commandDescriptors) {
                if (descriptor.id != QStringLiteral("view.fullscreen")) continue;
                togglesFullscreen = !descriptor.shortcut.trimmed().isEmpty() &&
                    QKeySequence(descriptor.shortcut).matches(pressed) == QKeySequence::ExactMatch;
                break;
            }
        }
        if (exitsFullscreen || togglesFullscreen) {
            // Reserve the key before QAction dispatch, including on native
            // viewer children. Other windows retain their own Escape behavior.
            if (event->type() == QEvent::KeyPress && !keyEvent->isAutoRepeat()) {
                _toggleFullScreen();
            }
            keyEvent->accept();
            return true;
        }
    }
    if (fullscreenActive && eventBelongsToMainWindow) {
        switch (event->type()) {
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::Wheel:
        case QEvent::KeyPress:
            _showFullScreenChromeTemporarily();
            break;
        default:
            break;
        }
    }
    const bool eventBelongsToViewer = eventWidget && _p->viewer &&
        (eventWidget == _p->viewer || _p->viewer->isAncestorOf(eventWidget));
    if (eventBelongsToViewer && event->type() == QEvent::Wheel && _p->userSettings && _p->playbackCtrl) {
        const QString binding = _p->userSettings->value(QStringLiteral("input/mouseWheel"), QStringLiteral("zoom")).toString();
        auto* wheel = static_cast<QWheelEvent*>(event);
        int delta = wheel->angleDelta().y();
        if (delta == 0) delta = wheel->pixelDelta().y();
        if (wheel->inverted()) delta = -delta;
        if (binding == QStringLiteral("none")) {
            wheel->accept();
            return true;
        }
        if (binding == QStringLiteral("frames")) {
            if (delta > 0) _p->playbackCtrl->nextFrame();
            else if (delta < 0) _p->playbackCtrl->prevFrame();
            wheel->accept();
            return true;
        }
        if (binding == QStringLiteral("volume")) {
            if (delta != 0) {
                const float volume = qBound(0.0f, _p->playbackCtrl->getVolume() + (delta > 0 ? 0.05f : -0.05f), 1.0f);
                _p->playbackCtrl->setVolume(volume);
            }
            wheel->accept();
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
#ifdef Q_OS_WIN
    Q_UNUSED(eventType);
    if (!message || !result || isMaximized() || isFullScreen()) {
        return QMainWindow::nativeEvent(eventType, message, result);
    }

    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == WM_XBUTTONDOWN && _p->inputBindings) {
        const QString gesture = GET_XBUTTON_WPARAM(msg->wParam) == XBUTTON1
            ? QStringLiteral("Mouse4") : QStringLiteral("Mouse5");
        const QString command = _p->inputBindings->commandFor(InputBindingStore::Device::Mouse, gesture);
        if (!command.isEmpty()) {
            const bool handled = _executeCommandId(command);
            if (handled) {
                *result = 0;
                return true;
            }
        }
    }
    if (msg->message == WM_NCHITTEST) {
        constexpr LONG kResizeBorder = 8;
        const RECT winRect = [] (HWND hwnd) {
            RECT rect{};
            GetWindowRect(hwnd, &rect);
            return rect;
        }(msg->hwnd);

        const long x = static_cast<short>(LOWORD(msg->lParam));
        const long y = static_cast<short>(HIWORD(msg->lParam));

        const bool resizeLeft = x >= winRect.left && x < winRect.left + kResizeBorder;
        const bool resizeRight = x <= winRect.right && x > winRect.right - kResizeBorder;
        const bool resizeTop = y >= winRect.top && y < winRect.top + kResizeBorder;
        const bool resizeBottom = y <= winRect.bottom && y > winRect.bottom - kResizeBorder;

        if (resizeTop && resizeLeft) { *result = HTTOPLEFT; return true; }
        if (resizeTop && resizeRight) { *result = HTTOPRIGHT; return true; }
        if (resizeBottom && resizeLeft) { *result = HTBOTTOMLEFT; return true; }
        if (resizeBottom && resizeRight) { *result = HTBOTTOMRIGHT; return true; }
        if (resizeLeft) { *result = HTLEFT; return true; }
        if (resizeRight) { *result = HTRIGHT; return true; }
        if (resizeTop) { *result = HTTOP; return true; }
        if (resizeBottom) { *result = HTBOTTOM; return true; }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool textInputFocused = isTextInputFocusWidget(QApplication::focusWidget());
    if (textInputFocused &&
        ((mods == Qt::NoModifier || mods == Qt::ShiftModifier) &&
         (!event->text().isEmpty() || event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace))) {
        QMainWindow::keyPressEvent(event);
        return;
    }

    if (event->key() == Qt::Key_Escape) {
        if (isFullScreen()) { _toggleFullScreen(); return; }
        if (auto* annotationService = resolveAnnotationService(_p->annotationService, _annoMgr.get())) {
            annotationService->setTool(AnnotationToolbar::Select);
        }
        if (_p->compareBar) _p->compareBar->setCompareMode(0);
        return;
    }

    Qt::KeyboardModifiers shortcutModifiers = mods;
    if (event->key() == Qt::Key_Plus) shortcutModifiers &= ~Qt::ShiftModifier;
    const QKeySequence pressed(QKeyCombination(shortcutModifiers, Qt::Key(event->key())));
    for (const auto& descriptor : _p->commandDescriptors) {
        if (descriptor.shortcut.trimmed().isEmpty()) continue;
        if (QKeySequence(descriptor.shortcut).matches(pressed) != QKeySequence::ExactMatch) continue;
        if (_executeCommandId(descriptor.id)) {
            event->accept();
            return;
        }
    }
    QMainWindow::keyPressEvent(event);
}

} // namespace cgplay
