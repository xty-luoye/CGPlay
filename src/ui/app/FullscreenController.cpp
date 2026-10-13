#include "ApplicationInternal.h"
#include "FullscreenController.h"
#include <QVariantAnimation>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace cgplay {
namespace {
constexpr auto kFullscreenDockVisibility = "cgplay.fullscreenWasVisible";

void keepFullscreenComposition(QWidget* window)
{
#ifdef Q_OS_WIN
    // Qt documents WS_BORDER as the workaround for Windows fullscreen
    // OpenGL composition. Without it, frames can swap while the desktop
    // keeps presenting the same image. Apply before painting resumes;
    // showNormal() restores the style Qt saved on fullscreen entry.
    // https://doc.qt.io/qt-6.5/windows-issues.html#fullscreen-opengl-based-windows
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    const LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    if (!(style & WS_BORDER)) {
        SetLastError(ERROR_SUCCESS);
        if (!SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_BORDER) &&
            GetLastError() != ERROR_SUCCESS) {
            qWarning() << "Cannot enable fullscreen OpenGL composition:" << GetLastError();
            return;
        }
        if (!SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                          SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                          SWP_NOZORDER | SWP_NOACTIVATE)) {
            qWarning() << "Cannot refresh fullscreen OpenGL composition:" << GetLastError();
        }
    }
#else
    Q_UNUSED(window);
#endif
}

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

FullscreenController::FullscreenController(MainWindow* window) : QObject(window), _window(window)
{
    _state.fullscreenChromeTimer = new QTimer(this);
    _state.fullscreenChromeTimer->setSingleShot(true);
    _state.fullscreenChromeTimer->setInterval(2000);
    connect(_state.fullscreenChromeTimer, &QTimer::timeout, this, &FullscreenController::hideChrome);
}

FullscreenController::~FullscreenController() { shutdown(); }

void FullscreenController::shutdown()
{
    ++_state.fullscreenTransitionGeneration;
    if (_state.fullscreenChromeTimer) _state.fullscreenChromeTimer->stop();
    if (_state.fullscreenMousePollTimer) _state.fullscreenMousePollTimer->stop();
    if (_state.fullscreenOverlayAnimation) _state.fullscreenOverlayAnimation->stop();
    setCursorHidden(false);
    if (_state.fullscreenEntryUpdatesSuspended) {
        _state.fullscreenEntryUpdatesSuspended = false;
        _window->setUpdatesEnabled(true);
    }
}

void FullscreenController::toggle()
{
    const bool fullscreen = _state.fullscreenActive || _window->isFullScreen();
    const quint64 transitionGeneration = ++_state.fullscreenTransitionGeneration;
    if (fullscreen) {
        // Batch exit as well as entry: reattaching each control must not
        // allocate/present an intermediate OpenGL framebuffer.
        _state.fullscreenEntryPending = false;
        if (!_state.fullscreenEntryUpdatesSuspended && _window->updatesEnabled()) {
            _state.fullscreenEntryUpdatesSuspended = true;
            _window->setUpdatesEnabled(false);
        }
        _state.fullscreenActive = false;
        if (_state.fullscreenOverlayAnimation) _state.fullscreenOverlayAnimation->stop();
        if (_state.fullscreenOverlay) {
            _state.fullscreenOverlay->hide();
            // Only raster controls change parent. The OpenGL surface and
            // player remain attached throughout the transition.
            _window->_p->centerSplitter->insertWidget(1, _window->_p->timeline);
            _window->_p->centerSplitter->insertWidget(2, _window->_p->playbackBar);
        }
        _state.fullscreenOverlayProgress = 0.0;
        _state.fullscreenChromeVisible = false;
        _state.fullscreenRestorePending = true;
        if (_state.fullscreenChromeTimer) {
            _state.fullscreenChromeTimer->stop();
        }
        if (_state.fullscreenMousePollTimer) {
            _state.fullscreenMousePollTimer->stop();
        }
        setCursorHidden(false);
        _window->_p->topBar->setVisible(_state.fullscreenTopBar);
        _window->_p->playlist->setVisible(_state.fullscreenLeft);
        if (_window->_p->reviewPanel) _window->_p->reviewPanel->setVisible(_state.fullscreenRight);
        restoreDockWidgetsAfterFullscreen(_window);
        _window->_p->navRail->hide();
        if (_state.fullscreenTimelineHeight > 0) {
            _window->_p->timeline->setFixedHeight(_state.fullscreenTimelineHeight);
        }
        if (_state.fullscreenPlaybackBarHeight > 0) {
            _window->_p->playbackBar->setFixedHeight(_state.fullscreenPlaybackBarHeight);
        }
        _window->_p->timeline->setVisible(_state.fullscreenTimeline);
        _window->_p->playbackBar->setVisible(_state.fullscreenPlaybackBar);
        _window->_p->compareBar->setVisible(_state.fullscreenCompareBar);
        _window->_p->viewer->setChromeVisible(_state.fullscreenViewerChrome);
        if (_window->statusBar()) {
            _window->statusBar()->setVisible(_state.fullscreenStatusBar);
        }
        if (_window->_p->annoToolbar) {
            _window->_p->annoToolbar->setVisible(_window->_p->annoToolsVisible);
            _window->_onAnnotationModeToggled(_window->_p->annoToolsVisible);
        } else {
            _window->_onAnnotationModeToggled(false);
        }
        setContentHostMargins(_window->_p->horzSplitter, false);
        setFullscreenSplitterHandles(_window->_p->horzSplitter, _window->_p->centerSplitter, false);
        if (_window->_p->centerSplitter) {
            _window->_p->centerSplitter->setCollapsible(1, false);
            _window->_p->centerSplitter->setCollapsible(2, false);
        }
        const bool restoreMaximized = _state.fullscreenWindowState.testFlag(Qt::WindowMaximized);
        _window->showNormal();
        const QList<int> centerSizes = _state.fullscreenCenterSizes;
        QTimer::singleShot(0, this, [this, centerSizes, transitionGeneration, restoreMaximized]() {
            if (_state.fullscreenTransitionGeneration != transitionGeneration ||
                _state.fullscreenActive || _window->isFullScreen()) return;
            if (_window->_p->centerSplitter && centerSizes.size() == _window->_p->centerSplitter->count()) {
                _window->_p->centerSplitter->setSizes(centerSizes);
            }
            // Geometry restoration can clear Qt's maximized state. Apply it
            // after restoring normal geometry and splitter layout.
            if (restoreMaximized) _window->showMaximized();
            if (_window->layout()) _window->layout()->activate();
            if (_window->_p->viewer->layout()) _window->_p->viewer->layout()->activate();
            if (auto* vp = _window->_p->viewer->viewport()) vp->setFrameView(true);
            if (_state.fullscreenEntryUpdatesSuspended) {
                _state.fullscreenEntryUpdatesSuspended = false;
                _window->setUpdatesEnabled(true);
            }
        });
        // Windows may deliver the old normal-geometry state after the queued
        // layout pass. Reconcile once after that transition, never polling.
        QTimer::singleShot(150, this, [this, transitionGeneration, restoreMaximized]() {
            if (_state.fullscreenTransitionGeneration != transitionGeneration ||
                _state.fullscreenActive || _window->isFullScreen()) return;
            _state.fullscreenRestorePending = false;
            if (restoreMaximized && !_window->isMaximized()) _window->showMaximized();
        });
    } else {
        // A new entry before the previous exit settles must inherit its
        // intended window state, rather than a temporary normal geometry.
        const bool restoreInFlight = _state.fullscreenRestorePending;
        if (!restoreInFlight) _state.fullscreenWindowState = _window->windowState();
        _state.fullscreenRestorePending = false;
        _state.fullscreenActive = true;
        if (!_state.fullscreenMousePollTimer) {
            _state.fullscreenMousePollTimer = new QTimer(this);
            _state.fullscreenMousePollTimer->setInterval(50);
            connect(_state.fullscreenMousePollTimer, &QTimer::timeout, this, [this]() {
                if (!_window->isFullScreen() || _state.fullscreenEntryPending) {
                    return;
                }
                const QPoint cursorPos = systemCursorPosition();
                const QRect windowRect(_window->mapToGlobal(QPoint(0, 0)), _window->size());
                const int activationHeight = std::clamp(_window->height() / 5, 96, 220);
                const bool cursorNearBottom = windowRect.contains(cursorPos) &&
                    cursorPos.y() >= windowRect.bottom() - activationHeight;
                if (cursorPos != _state.fullscreenLastCursorPos || cursorNearBottom) {
                    _state.fullscreenLastCursorPos = cursorPos;
                    showChromeTemporarily();
                }
            });
        }
        _state.fullscreenLeft  = _window->_p->playlist->isVisible();
        _state.fullscreenRight = _window->_p->reviewPanel ? _window->_p->reviewPanel->isVisible() : false;
        _state.fullscreenNav = false;
        _state.fullscreenTopBar = _window->_p->topBar->isVisible();
        _state.fullscreenTimeline = _window->_p->timeline->isVisible();
        _state.fullscreenPlaybackBar = _window->_p->playbackBar->isVisible();
        _state.fullscreenStatusBar = _window->statusBar() ? _window->statusBar()->isVisible() : true;
        _state.fullscreenCompareBar = _window->_p->compareBar->isVisible();
        _state.fullscreenAIDock = _window->_p->aiDock ? _window->_p->aiDock->isVisible() : false;
        _state.fullscreenViewerChrome = true;
        if (!restoreInFlight)
            _state.fullscreenCenterSizes = _window->_p->centerSplitter ? _window->_p->centerSplitter->sizes() : QList<int>{};
        _state.fullscreenTimelineHeight = _window->_p->timeline->height();
        _state.fullscreenPlaybackBarHeight = _window->_p->playbackBar->height();
        _window->_p->annoToolsVisible = _window->_p->annoToolbar ? _window->_p->annoToolbar->isVisible() : false;
        // Keep intermediate native/chrome layouts out of the compositor.
        // Suspend painting only for this queued layout transaction, not for
        // a wall-clock delay that visibly freezes an already playing movie.
        _state.fullscreenEntryPending = true;
        _state.fullscreenEntryUpdatesSuspended = _state.fullscreenEntryUpdatesSuspended || _window->updatesEnabled();
        if (_state.fullscreenEntryUpdatesSuspended) _window->setUpdatesEnabled(false);
        _state.fullscreenLastCursorPos = systemCursorPosition();
        _state.fullscreenMousePollTimer->stop();
        setContentHostMargins(_window->_p->horzSplitter, true);
        setFullscreenSplitterHandles(_window->_p->horzSplitter, _window->_p->centerSplitter, true);
        setChromeVisible(false, true);
        _window->showFullScreen();
        // Commit one final layout at screen size. Chrome has already left
        // the splitter, so later pointer movement cannot resize the GL FBO.
        QTimer::singleShot(0, this, [this, transitionGeneration]() {
            if (_state.fullscreenTransitionGeneration != transitionGeneration) return;
            const bool entered = _state.fullscreenActive && _window->isFullScreen();
            if (entered) {
                // Native frame refresh emits resize events. A cancelled
                // entry must not queue those events after a newer exit.
                keepFullscreenComposition(_window);
                if (_window->layout()) _window->layout()->activate();
                if (_window->_p->viewer->layout()) _window->_p->viewer->layout()->activate();
                if (auto* vp = _window->_p->viewer->viewport()) vp->setFrameView(true);
                layoutOverlay();
                _window->_p->viewer->viewport()->setFocus(Qt::OtherFocusReason);
            }
            _state.fullscreenEntryPending = false;
            if (_state.fullscreenEntryUpdatesSuspended) {
                _state.fullscreenEntryUpdatesSuspended = false;
                _window->setUpdatesEnabled(true);
            }
            if (entered && _state.fullscreenMousePollTimer) {
                _state.fullscreenLastCursorPos = systemCursorPosition();
                _state.fullscreenMousePollTimer->start();
            }
        });
    }

}

void FullscreenController::showChromeTemporarily()
{
    if (!_window->isFullScreen() || _state.fullscreenEntryPending) {
        return;
    }
    setChromeVisible(true);
    if (_state.fullscreenChromeTimer) {
        _state.fullscreenChromeTimer->start(2000);
    }
}

void FullscreenController::hideChrome()
{
    if (!_window->isFullScreen()) {
        return;
    }
    // Never slide an active seek/volume drag or an open control popup away.
    if (QApplication::mouseButtons() != Qt::NoButton || QApplication::activePopupWidget()) {
        if (_state.fullscreenChromeTimer) _state.fullscreenChromeTimer->start(2000);
        return;
    }
    setChromeVisible(false);
}

void FullscreenController::layoutOverlay()
{
    if (!_state.fullscreenActive || !_state.fullscreenOverlay || !_window->_p->viewer) return;
    auto* viewport = _window->_p->viewer->viewport();
    if (!viewport) return;
    const QPoint origin = viewport->mapTo(_window->centralWidget(), QPoint());
    const int controlsHeight = (_state.fullscreenTimeline ? _state.fullscreenTimelineHeight : 0) +
        (_state.fullscreenPlaybackBar ? _state.fullscreenPlaybackBarHeight : 0);
    // Slide the controls over the bottom edge; the viewport geometry stays fixed.
    const int offset = qRound(controlsHeight * _state.fullscreenOverlayProgress);
    _state.fullscreenOverlay->setGeometry(origin.x(), origin.y() + viewport->height() - offset,
        viewport->width(), controlsHeight);
    _state.fullscreenOverlay->raise();
}

void FullscreenController::setChromeVisible(bool visible, bool forceApply)
{
    if (!forceApply && (!_window->isFullScreen() || !_state.fullscreenOverlay)) return;
    if (!forceApply && _state.fullscreenChromeVisible == visible) return;
    ++_state.fullscreenChromeApplyCount;
    _state.fullscreenChromeVisible = visible;
    if (forceApply) {
        _window->_p->topBar->hide();
        _window->_p->navRail->hide();
        _window->_p->playlist->hide();
        if (_window->_p->reviewPanel) _window->_p->reviewPanel->hide();
        hideDockWidgetsForFullscreen(_window);
        _window->_p->compareBar->hide();
        _window->_p->viewer->setChromeVisible(false);
        if (_window->statusBar()) _window->statusBar()->hide();
        if (_window->_p->annoToolbar) _window->_p->annoToolbar->hide();
        if (!_state.fullscreenOverlay) {
            _state.fullscreenOverlay = new QWidget(_window->centralWidget());
            _state.fullscreenOverlay->setObjectName(QStringLiteral("FullscreenControlsOverlay"));
            _state.fullscreenOverlay->setFocusPolicy(Qt::NoFocus);
            auto* controls = new QVBoxLayout(_state.fullscreenOverlay);
            controls->setContentsMargins(0, 0, 0, 0);
            controls->setSpacing(0);
            _state.fullscreenOverlayAnimation = new QVariantAnimation(this);
            _state.fullscreenOverlayAnimation->setEasingCurve(QEasingCurve::OutCubic);
            connect(_state.fullscreenOverlayAnimation, &QVariantAnimation::valueChanged, this,
                [this](const QVariant& value) {
                    _state.fullscreenOverlayProgress = value.toReal();
                    layoutOverlay();
                });
            connect(_state.fullscreenOverlayAnimation, &QVariantAnimation::finished, this, [this] {
                if (!_state.fullscreenChromeVisible) _state.fullscreenOverlay->hide();
            });
        }
        _state.fullscreenOverlayAnimation->stop();
        _state.fullscreenOverlay->hide();
        _state.fullscreenOverlayProgress = 0.0;
        _state.fullscreenOverlay->layout()->addWidget(_window->_p->timeline);
        _state.fullscreenOverlay->layout()->addWidget(_window->_p->playbackBar);
        _window->_p->timeline->setFixedHeight(_state.fullscreenTimelineHeight);
        _window->_p->playbackBar->setFixedHeight(_state.fullscreenPlaybackBarHeight);
        _window->_p->timeline->setVisible(_state.fullscreenTimeline);
        _window->_p->playbackBar->setVisible(_state.fullscreenPlaybackBar);
        layoutOverlay();
    } else {
        _state.fullscreenOverlayAnimation->stop();
        _state.fullscreenOverlay->show();
        layoutOverlay();
        _state.fullscreenOverlayAnimation->setDuration(140);
        _state.fullscreenOverlayAnimation->setStartValue(_state.fullscreenOverlayProgress);
        _state.fullscreenOverlayAnimation->setEndValue(visible ? 1.0 : 0.0);
        _state.fullscreenOverlayAnimation->start();
    }
    setCursorHidden(!visible);
}

void FullscreenController::setCursorHidden(bool hidden)
{
    if (_state.fullscreenCursorHidden == hidden) {
        return;
    }
    _state.fullscreenCursorHidden = hidden;
    if (hidden) {
        QApplication::setOverrideCursor(Qt::BlankCursor);
    } else {
        QApplication::restoreOverrideCursor();
    }
}

} // namespace cgplay
