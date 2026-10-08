#include "ApplicationInternal.h"
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QWindow>
#include <cmath>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace cgplay {
namespace {

// Wait for real elapsed time, not processEvents' maximum processing budget.
void settleFullscreen(int milliseconds = 220)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

void fullscreenKey(QWidget* target, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                   bool repeat = false)
{
    QKeyEvent press(QEvent::KeyPress, key, modifiers, QString(), repeat);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QApplication::sendEvent(target, &release);
}

class FullscreenKeySink : public QWidget
{
public:
    using QWidget::QWidget;
    int keyCount = 0;
protected:
    void keyPressEvent(QKeyEvent* event) override { ++keyCount; event->accept(); }
};

class FullscreenPaintProbe : public QObject
{
public:
    std::function<void()> onPaint;
protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::Paint && onPaint) onPaint();
        return false;
    }
};

} // namespace

QJsonArray MainWindow::_runFullscreenSmokeChecks()
{
    QJsonArray results;
    const auto add = [&](const QString& name, bool passed, QJsonObject detail = {}) {
        int visibleWindows = 0;
        for (auto* widget : QApplication::topLevelWidgets())
            if (widget->isVisible() && !widget->testAttribute(Qt::WA_DontShowOnScreen)) ++visibleWindows;
        int nativeVisibleWindows = 0;
#ifdef Q_OS_WIN
        EnumWindows([](HWND window, LPARAM data) -> BOOL {
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId == GetCurrentProcessId() && IsWindowVisible(window))
                ++*reinterpret_cast<int*>(data);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&nativeVisibleWindows));
#endif
        const bool background = qApp->property("cgplay.automationBackground").toBool();
        detail.insert("background", background);
        detail.insert("visiblePlatformWindows", visibleWindows);
        detail.insert("nativeVisibleWindows", nativeVisibleWindows);
        detail.insert("nativeFullscreen", isFullScreen());
        detail.insert("requestedFullscreen", _p->fullscreenActive);
        detail.insert("mousePollActive", _p->fullscreenMousePollTimer && _p->fullscreenMousePollTimer->isActive());
        detail.insert("entryPending", _p->fullscreenEntryPending);
        detail.insert("entryUpdatesSuspended", _p->fullscreenEntryUpdatesSuspended);
        detail.insert("updatesEnabled", updatesEnabled());
        results.append(QJsonObject{{"name", name}, {"status", passed && background && visibleWindows == 0 && nativeVisibleWindows == 0 ? "PASS" : "FAIL"},
                                   {"message", "fullscreen interaction regression"}, {"details", detail}});
    };
    if (!_p->viewer || !_p->viewer->viewport()) {
        add(QStringLiteral("Fullscreen viewer available"), false);
        return results;
    }
    auto* target = _p->viewer->viewport();
    if (_p->fullscreenActive || isFullScreen()) _toggleFullScreen();
    showNormal();
    settleFullscreen();
    const QRect originalGeometry = geometry();
    const auto normalSizes = _p->centerSplitter->sizes();
    const bool originalTopBar = _p->topBar->isVisible();
    const int originalTimelineHeight = _p->timeline->height();
    const bool originalAIDock = _p->aiDock && _p->aiDock->isVisible();
    const auto* codexDock = findChild<QDockWidget*>(QStringLiteral("CodexAgentWorkspaceDock"));
    const bool originalCodexDock = codexDock && codexDock->isVisible();
    const auto normal = [&] {
        return !_p->fullscreenActive && !isFullScreen() && !_p->fullscreenCursorHidden &&
            !_p->fullscreenEntryPending && !_p->fullscreenEntryUpdatesSuspended &&
            updatesEnabled() && target->updatesEnabled() &&
            (!_p->fullscreenMousePollTimer || !_p->fullscreenMousePollTimer->isActive()) &&
            (!_p->fullscreenChromeTimer || !_p->fullscreenChromeTimer->isActive()) &&
            (_p->aiDock && _p->aiDock->isVisible()) == originalAIDock &&
            (codexDock && codexDock->isVisible()) == originalCodexDock &&
            _p->topBar->isVisible() == originalTopBar && _p->timeline->height() == originalTimelineHeight;
    };
    // Use the same live descriptor/action pair as the shortcut settings UI.
    auto descriptor = std::find_if(_p->commandDescriptors.begin(), _p->commandDescriptors.end(),
        [](const auto& item) { return item.id == QStringLiteral("view.fullscreen"); });
    if (descriptor == _p->commandDescriptors.end()) {
        add(QStringLiteral("Fullscreen command available"), false);
        return results;
    }
    const QString originalShortcut = descriptor->shortcut;
    QList<QPair<QAction*, QKeySequence>> actions;
    for (auto* action : findChildren<QAction*>())
        if (action->property("cgplay.command.id").toString() == QStringLiteral("view.fullscreen"))
            actions.append(qMakePair(action, action->shortcut()));
    const auto bind = [&](const QString& shortcut) {
        descriptor->shortcut = shortcut;
        for (const auto& action : actions) action.first->setShortcut(QKeySequence(shortcut));
    };
    bind(QStringLiteral("F11"));
    fullscreenKey(target, Qt::Key_F11);
    settleFullscreen();
    const bool entered = isFullScreen();
    fullscreenKey(target, Qt::Key_Escape);
    settleFullscreen();
    add(QStringLiteral("Esc exits fullscreen"), entered && normal(), {{"receiver", "TlViewport"}});

    {
        // Ignore physical cursor movement without changing production timers.
        // Only injected input can request chrome in this deterministic case.
        const QSignalBlocker cursorPollBlocker(_p->fullscreenMousePollTimer);
        FullscreenPaintProbe paintProbe;
        target->installEventFilter(&paintProbe);
        QElapsedTimer entryClock;
        QJsonObject firstPaint;
        QJsonObject firstPresented;
        QJsonArray samples;
        int paints = 0;
        int disabledUpdatePaintEvents = 0;
        int injectedInputEvents = 0;
        int entryInputEvents = 0;
        const auto snapshot = [&] {
            const QPoint origin = target->mapTo(this, QPoint(0, 0));
            return QJsonObject{
                {"elapsedMs", static_cast<double>(entryClock.elapsed())},
                {"viewport", QJsonArray{origin.x(), origin.y(), target->width(), target->height()}},
                {"zoom", target->zoom()},
                {"renderFrameCount", static_cast<double>(target->renderFrameCount())},
                {"timelineHeight", _p->timeline->height()},
                {"playbackBarHeight", _p->playbackBar->height()},
                {"chromeVisible", _p->fullscreenChromeVisible},
                {"entryPending", _p->fullscreenEntryPending},
                {"updatesEnabled", updatesEnabled() && target->updatesEnabled()}};
        };
        paintProbe.onPaint = [&] {
            if (!isFullScreen()) return;
            // Qt can deliver a queued Paint while updates are disabled. It
            // does not invoke paintGL or present that layout; keep evidence
            // of these events, but measure the first enabled paint instead.
            if (!updatesEnabled() || !target->updatesEnabled()) {
                ++disabledUpdatePaintEvents;
                return;
            }
            ++paints;
            if (!firstPaint.isEmpty()) return;
            firstPaint = snapshot();
            // Paint-event filters run before paintGL. Read zoom after that
            // paint completes, because tlRender computes frame-fit in draw.
            QTimer::singleShot(0, &paintProbe, [&] { firstPresented = snapshot(); });
        };
        const auto injectEntryInput = [&] {
            const QPointF local(target->rect().center());
            const QPointF global(target->mapToGlobal(local.toPoint()));
            for (const auto type : {QEvent::MouseMove, QEvent::MouseButtonRelease}) {
                if (_p->fullscreenEntryPending) ++entryInputEvents;
                ++injectedInputEvents;
                QMouseEvent event(type, local, global,
                    type == QEvent::MouseButtonRelease ? Qt::LeftButton : Qt::NoButton,
                    Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(target, &event);
            }
        };
        entryClock.start();
        fullscreenKey(target, Qt::Key_F11);
        injectEntryInput();
        QTimer::singleShot(0, &paintProbe, [&] {
            if (_p->fullscreenEntryPending) injectEntryInput();
        });
        settleFullscreen(220);
        samples.append(snapshot());
        while (entryClock.elapsed() < 2250) {
            settleFullscreen(50);
            samples.append(snapshot());
        }
        target->removeEventFilter(&paintProbe);
        paintProbe.onPaint = {};
        const auto hiddenLayout = [](const QJsonObject& state) {
            return !state.isEmpty() && state.value("timelineHeight").toInt() == 0 &&
                state.value("playbackBarHeight").toInt() == 0 &&
                !state.value("chromeVisible").toBool() && !state.value("entryPending").toBool() &&
                state.value("updatesEnabled").toBool();
        };
        const double presentedZoom = firstPresented.value("zoom").toDouble();
        const auto samePresentedLayout = [&](const QJsonObject& state) {
            return hiddenLayout(state) && state.value("viewport") == firstPaint.value("viewport") &&
                std::abs(state.value("zoom").toDouble() - presentedZoom) < 1e-6;
        };
        bool stable = hiddenLayout(firstPaint) && samePresentedLayout(firstPresented) &&
            presentedZoom > 0.0 && paints > 0 && entryInputEvents >= 2 &&
            firstPresented.value("renderFrameCount").toDouble() > firstPaint.value("renderFrameCount").toDouble();
        for (const auto& value : samples) stable = stable && samePresentedLayout(value.toObject());
        add(QStringLiteral("Fullscreen first frame and delayed zoom stability"), stable,
            {{"firstPaint", firstPaint}, {"firstPresented", firstPresented}, {"samples", samples},
             {"paintEvents", paints}, {"injectedInputEvents", injectedInputEvents},
             {"disabledUpdatePaintEvents", disabledUpdatePaintEvents},
             {"inputEventsDuringEntry", entryInputEvents}, {"physicalCursorIgnored", true},
             {"observationMs", static_cast<double>(entryClock.elapsed())}});

        const double manualZoom = presentedZoom * 1.25;
        target->setFrameView(false);
        target->setZoom(manualZoom);
        settleFullscreen(350);
        add(QStringLiteral("Fullscreen explicit zoom after entry"),
            !target->hasFrameView() && std::abs(target->zoom() - manualZoom) < 1e-6 &&
                updatesEnabled() && target->updatesEnabled() && !_p->fullscreenEntryPending,
            {{"requestedZoom", manualZoom}, {"actualZoom", target->zoom()}});
        fullscreenKey(target, Qt::Key_Escape);
        settleFullscreen();
        add(QStringLiteral("Fullscreen entry restores repainting"), normal());
    }

    fullscreenKey(target, Qt::Key_F11);
    settleFullscreen();
    const bool enteredByF11 = isFullScreen();
    fullscreenKey(target, Qt::Key_F11);
    settleFullscreen();
    add(QStringLiteral("F11 exits fullscreen"), enteredByF11 && normal(), {{"receiver", "TlViewport"}});

    fullscreenKey(target, Qt::Key_F11, Qt::NoModifier, true);
    const bool repeatDidNotEnter = !isFullScreen();
    fullscreenKey(target, Qt::Key_F11);
    fullscreenKey(target, Qt::Key_F11, Qt::NoModifier, true);
    fullscreenKey(target, Qt::Key_Escape, Qt::NoModifier, true);
    const bool repeatDidNotExit = isFullScreen();
    fullscreenKey(target, Qt::Key_Escape);
    settleFullscreen();
    add(QStringLiteral("Fullscreen repeat key guard"), repeatDidNotEnter && repeatDidNotExit && normal());

    fullscreenKey(target, Qt::Key_F11);
    settleFullscreen();
    {
        FullscreenKeySink otherWindow;
        otherWindow.setAttribute(Qt::WA_DontShowOnScreen);
        otherWindow.show();
        fullscreenKey(&otherWindow, Qt::Key_Escape);
        fullscreenKey(&otherWindow, Qt::Key_F11);
        QDialog dialog(this);
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        fullscreenKey(&dialog, Qt::Key_Escape);
        add(QStringLiteral("Fullscreen foreign-window isolation"),
            isFullScreen() && otherWindow.keyCount == 2 && !dialog.isVisible(), {{"deliveredKeys", otherWindow.keyCount}});
    }
    fullscreenKey(target, Qt::Key_Escape);
    settleFullscreen();

    bind(QStringLiteral("Alt+F10"));
    fullscreenKey(target, Qt::Key_F10, Qt::AltModifier);
    settleFullscreen();
    const bool customEntered = isFullScreen();
    fullscreenKey(target, Qt::Key_F11);
    const bool oldKeyIgnored = isFullScreen();
    fullscreenKey(target, Qt::Key_F10, Qt::AltModifier);
    settleFullscreen();
    add(QStringLiteral("Fullscreen custom shortcut"), customEntered && oldKeyIgnored && normal());
    bind(QString());
    _toggleFullScreen();
    fullscreenKey(target, Qt::Key_F11);
    const bool clearedKeyIgnored = isFullScreen();
    fullscreenKey(target, Qt::Key_Escape);
    settleFullscreen();
    add(QStringLiteral("Fullscreen cleared shortcut"), clearedKeyIgnored && normal());
    bind(QStringLiteral("F11"));

    bool rapidPassed = true;
    for (int i = 0; i < 5; ++i) {
        fullscreenKey(target, Qt::Key_F11);
        fullscreenKey(target, Qt::Key_Escape);
        fullscreenKey(target, Qt::Key_F11);
        settleFullscreen();
        rapidPassed = rapidPassed && isFullScreen() && !_p->topBar->isVisible() &&
            !_p->fullscreenEntryPending && !_p->fullscreenEntryUpdatesSuspended &&
            updatesEnabled() && target->updatesEnabled();
        fullscreenKey(target, Qt::Key_Escape);
        settleFullscreen();
        rapidPassed = rapidPassed && normal();
    }
    add(QStringLiteral("Fullscreen rapid transitions"), rapidPassed && geometry() == originalGeometry &&
        _p->centerSplitter->sizes() == normalSizes,
        {{"cycles", 5}, {"settleMs", 220}, {"normalGeometryRestored", geometry() == originalGeometry},
         {"splitterRestored", _p->centerSplitter->sizes() == normalSizes}});

    QJsonObject maximizedDetail;
    showMaximized();
    maximizedDetail.insert("maximizeImmediate", static_cast<int>(windowState()));
    settleFullscreen();
    maximizedDetail.insert("maximizeSettled", static_cast<int>(windowState()));
    fullscreenKey(target, Qt::Key_F11);
    maximizedDetail.insert("savedWindowState", static_cast<int>(_p->fullscreenWindowState));
    settleFullscreen();
    maximizedDetail.insert("entered", static_cast<int>(windowState()));
    fullscreenKey(target, Qt::Key_Escape);
    maximizedDetail.insert("exitImmediate", static_cast<int>(windowState()));
    settleFullscreen();
    maximizedDetail.insert("exitSettled", static_cast<int>(windowState()));
    maximizedDetail.insert("platformState", windowHandle() ? static_cast<int>(windowHandle()->windowState()) : -1);
    add(QStringLiteral("Fullscreen restores maximized state"), isMaximized() && normal(), maximizedDetail);
    fullscreenKey(target, Qt::Key_F11);
    fullscreenKey(target, Qt::Key_Escape);
    fullscreenKey(target, Qt::Key_F11);
    settleFullscreen();
    fullscreenKey(target, Qt::Key_Escape);
    settleFullscreen();
    add(QStringLiteral("Fullscreen rapid maximized transitions"), isMaximized() && normal() && !_p->fullscreenRestorePending);
    showNormal();
    settleFullscreen();

    fullscreenKey(target, Qt::Key_F11);
    settleFullscreen();
    // Stop physical-cursor input only for this deterministic request burst.
    _p->fullscreenMousePollTimer->stop();
    _hideFullScreenChrome();
    settleFullscreen(30);
    const quint64 countBefore = _p->fullscreenChromeApplyCount;
    QElapsedTimer requestTimer;
    requestTimer.start();
    for (int i = 0; i < 200; ++i) _showFullScreenChromeTemporarily();
    const double requestMs = requestTimer.nsecsElapsed() / 1e6;
    settleFullscreen(30);
    const quint64 applies = _p->fullscreenChromeApplyCount - countBefore;
    add(QStringLiteral("Fullscreen chrome coalescing"), applies == 1 && _p->fullscreenChromeVisible &&
        _p->playbackBar->height() > 0 && _p->fullscreenChromeTimer->isActive(),
        {{"requests", 200}, {"layoutApplications", static_cast<double>(applies)}, {"requestMs", requestMs}});

    const QStringList args = QCoreApplication::arguments();
    const int outArg = args.indexOf(QStringLiteral("--smoke-output"));
    const QDir outputDir(outArg >= 0 && outArg + 1 < args.size()
        ? QFileInfo(args[outArg + 1]).absolutePath()
        : QDir::current().absoluteFilePath(QStringLiteral("tests/artifacts/fullscreen_smoke")));
    QDir().mkpath(outputDir.absolutePath());
    const QString controlsCapture = outputDir.filePath(QStringLiteral("fullscreen_controls.png"));
    // Hidden automation surfaces cannot be read reliably by native PrintWindow.
    const bool controlsSaved = grab().save(controlsCapture);
    _hideFullScreenChrome();
    settleFullscreen(30);
    const bool collapsed = _p->timeline->height() == 0 && _p->playbackBar->height() == 0 && _p->fullscreenCursorHidden;
    fullscreenKey(target, Qt::Key_Escape);
    settleFullscreen();
    const QString restoredCapture = outputDir.filePath(QStringLiteral("fullscreen_restored.png"));
    const bool restoredSaved = grab().save(restoredCapture);
    add(QStringLiteral("Fullscreen layout evidence"), controlsSaved && restoredSaved && collapsed && normal(),
        {{"controlsCapture", controlsCapture}, {"restoredCapture", restoredCapture}, {"captureMethod", "QWidget::grab (background surface)"}});

    // The geometry checks above also run while paused. Exercise actual
    // playback separately: a correct final layout can still conceal a long
    // interval with no painted video during entry/exit.
    {
        const QSignalBlocker cursorPollBlocker(_p->fullscreenMousePollTimer);
        const int savedFrame = _p->playbackCtrl->currentFrame();
        const int savedPlayback = _p->playbackCtrl->playbackState();
        const bool savedMute = _p->playbackCtrl->isMuted();
        _p->playbackCtrl->setMute(true);
        QJsonArray trials;
        bool responsive = _p->playbackCtrl->isValid() && _p->playbackCtrl->totalFrames() > 1;
        constexpr qint64 stallLimitMs = 1000;
        for (const bool maximized : {false, true, false}) {
            if (maximized) showMaximized(); else showNormal();
            _p->playbackCtrl->pause();
            _p->playbackCtrl->seekToFrame(0);
            _p->playbackCtrl->play();
            settleFullscreen(300);

            QElapsedTimer clock;
            clock.start();
            QVector<qint64> paintIntervals;
            QJsonArray paintedFrames;
            qint64 lastPaintMs = -1;
            qint64 lastRendered = target->renderFrameCount();
            qint64 firstFullscreenPaintMs = -1;
            qint64 entryStartMs = -1;
            qint64 maxPaintGapMs = 0;
            int mediaFrameEvents = 0;
            int swapEvents = 0;
            FullscreenPaintProbe playbackProbe;
            target->installEventFilter(&playbackProbe);
            const auto frameConnection = connect(_p->playbackCtrl->signalProxy(), &PlaybackServiceSignals::currentFrameChanged,
                &playbackProbe, [&](int, int) { ++mediaFrameEvents; });
            const auto swapConnection = connect(target, &QOpenGLWidget::frameSwapped,
                &playbackProbe, [&] { ++swapEvents; });
            playbackProbe.onPaint = [&] {
                if (!updatesEnabled() || !target->updatesEnabled()) return;
                const qint64 before = target->renderFrameCount();
                // The filter runs before paintGL. Count only a completed
                // renderer invocation, not repeated/disabled Paint events.
                QTimer::singleShot(0, &playbackProbe, [&, before] {
                    const qint64 rendered = target->renderFrameCount();
                    if (rendered <= before || rendered <= lastRendered) return;
                    const qint64 now = clock.elapsed();
                    if (lastPaintMs >= 0) {
                        const qint64 gap = now - lastPaintMs;
                        paintIntervals.append(gap);
                        maxPaintGapMs = std::max(maxPaintGapMs, gap);
                    }
                    lastPaintMs = now;
                    lastRendered = rendered;
                    if (entryStartMs >= 0 && isFullScreen() && !_p->fullscreenEntryPending &&
                        firstFullscreenPaintMs < 0) firstFullscreenPaintMs = now;
                    paintedFrames.append(QJsonObject{{"elapsedMs", double(now)},
                        {"renderFrameCount", double(rendered)},
                        {"mediaFrame", _p->playbackCtrl->currentFrame()},
                        {"fullscreen", isFullScreen()}, {"entryPending", _p->fullscreenEntryPending}});
                });
            };
            settleFullscreen(250);
            const int frameBefore = _p->playbackCtrl->currentFrame();
            entryStartMs = clock.elapsed();
            QElapsedTimer callClock;
            callClock.start();
            fullscreenKey(target, Qt::Key_F11);
            const double entryCallMs = callClock.nsecsElapsed() / 1e6;
            settleFullscreen(650);
            const int frameInFullscreen = _p->playbackCtrl->currentFrame();
            const bool keptPlaying = _p->playbackCtrl->playbackState() == 1;
            callClock.restart();
            fullscreenKey(target, Qt::Key_Escape);
            const double exitCallMs = callClock.nsecsElapsed() / 1e6;
            settleFullscreen(350);
            target->removeEventFilter(&playbackProbe);
            playbackProbe.onPaint = {};
            disconnect(frameConnection);
            disconnect(swapConnection);
            if (lastPaintMs >= 0) maxPaintGapMs = std::max(maxPaintGapMs, clock.elapsed() - lastPaintMs);
            std::sort(paintIntervals.begin(), paintIntervals.end());
            const qint64 p95PaintGapMs = paintIntervals.isEmpty() ? -1 : paintIntervals[
                std::min(paintIntervals.size() - 1, qsizetype(std::ceil(paintIntervals.size() * 0.95)) - 1)];
            const qint64 firstPaintLatencyMs = firstFullscreenPaintMs >= 0
                ? firstFullscreenPaintMs - entryStartMs : -1;
            const bool passed = normal() && isMaximized() == maximized && keptPlaying &&
                _p->playbackCtrl->playbackState() == 1 && mediaFrameEvents > 2 &&
                frameInFullscreen != frameBefore && paintIntervals.size() >= 4 &&
                firstPaintLatencyMs >= 0 && firstPaintLatencyMs < stallLimitMs &&
                maxPaintGapMs < stallLimitMs && entryCallMs < stallLimitMs && exitCallMs < stallLimitMs;
            responsive = responsive && passed;
            trials.append(QJsonObject{{"passed", passed}, {"fromMaximized", maximized},
                {"entryCallMs", entryCallMs}, {"exitCallMs", exitCallMs},
                {"firstFullscreenPaintMs", double(firstPaintLatencyMs)},
                {"maxPaintGapMs", double(maxPaintGapMs)}, {"p95PaintGapMs", double(p95PaintGapMs)},
                {"paintedFrames", paintedFrames}, {"frameSwappedEvents", swapEvents},
                {"mediaFrameEvents", mediaFrameEvents}, {"frameBefore", frameBefore},
                {"frameInFullscreen", frameInFullscreen}, {"keptPlaying", keptPlaying}});
        }
        _p->playbackCtrl->pause();
        _p->playbackCtrl->seekToFrame(savedFrame);
        _p->playbackCtrl->setMute(savedMute);
        if (savedPlayback == 1) _p->playbackCtrl->play();
        else if (savedPlayback == 2) _p->playbackCtrl->reverse();
        showNormal();
        settleFullscreen();
        add(QStringLiteral("Fullscreen playing transition responsiveness"), responsive,
            {{"trials", trials}, {"stallLimitMs", double(stallLimitMs)},
             {"measurement", "Completed paintGL invocations and media-frame events on hidden Qt surfaces"}});
    }
    bind(originalShortcut);
    for (const auto& action : actions) action.first->setShortcut(action.second);
    return results;
}

} // namespace cgplay
