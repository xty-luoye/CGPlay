#pragma once

#include <QObject>
#include <QList>
#include <QPoint>
#include <Qt>

class QTimer;
class QWidget;
class QVariantAnimation;

namespace cgplay {
class MainWindow;

// Owns fullscreen transitions, deferred layout generation, overlay animation
// and idle/mouse timers. The host retains ownership of its existing widgets.
class FullscreenController : public QObject
{
public:
    explicit FullscreenController(MainWindow* window);
    ~FullscreenController() override;
    struct State {
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
        QWidget* fullscreenOverlay = nullptr;
        QVariantAnimation* fullscreenOverlayAnimation = nullptr;
        qreal fullscreenOverlayProgress = 0.0;
        bool fullscreenCursorHidden = false;
        QList<int> fullscreenCenterSizes;
        int fullscreenTimelineHeight = 0;
        int fullscreenPlaybackBarHeight = 0;
        QTimer* fullscreenChromeTimer = nullptr;
        QTimer* fullscreenMousePollTimer = nullptr;
        QPoint fullscreenLastCursorPos;
    };
    const State& state() const { return _state; }
    void shutdown();
    void toggle();
    void showChromeTemporarily();
    void hideChrome();
    void layoutOverlay();
    void setChromeVisible(bool visible, bool forceApply = false);
    void setCursorHidden(bool hidden);

private:
    MainWindow* _window; // QObject parent; controller is destroyed before host widgets.
    State _state;
};
} // namespace cgplay
