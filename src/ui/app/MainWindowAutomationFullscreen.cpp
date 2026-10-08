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
    const auto normal = [&] {
        return !_p->fullscreenActive && !isFullScreen() && !_p->fullscreenCursorHidden &&
            !_p->fullscreenEntryPending && !_p->fullscreenEntryUpdatesSuspended &&
            updatesEnabled() && target->updatesEnabled() &&
            (!_p->fullscreenMousePollTimer || !_p->fullscreenMousePollTimer->isActive()) &&
            (!_p->fullscreenChromeTimer || !_p->fullscreenChromeTimer->isActive()) &&
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
        QTimer::singleShot(40, &paintProbe, injectEntryInput);
        QTimer::singleShot(90, &paintProbe, injectEntryInput);
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
    bind(originalShortcut);
    for (const auto& action : actions) action.first->setShortcut(action.second);
    return results;
}

} // namespace cgplay
