#include "ApplicationInternal.h"
#include "common/jobs/JobSystem.h"
#include "playback/api/Timecode.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QUrl>
#include <cmath>
#include <limits>
#if CGPLAY_HAS_TLRENDER
#include <tlRender/Timeline/Player.h>
#include <tlRender/Timeline/Timeline.h>
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace cgplay {
namespace {

void settlePlaybackInteraction(int milliseconds = 100)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

void playbackInteractionKey(QWidget* target, int key,
                            Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                            bool repeat = false)
{
    if (!target) return;
    const QString text = key == Qt::Key_Space ? QStringLiteral(" ") : QString();
    QKeyEvent overrideEvent(QEvent::ShortcutOverride, key, modifiers, text);
    QApplication::sendEvent(target, &overrideEvent);
    QKeyEvent press(QEvent::KeyPress, key, modifiers, text, repeat);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
    QApplication::sendEvent(target, &release);
}

class PlaybackInteractionKeySink : public QWidget
{
public:
    using QWidget::QWidget;
    int keyCount = 0;
protected:
    void keyPressEvent(QKeyEvent* event) override { ++keyCount; event->accept(); }
};

} // namespace

QJsonArray MainWindow::_runPlaybackInteractionSmokeChecks(const QString& mediaPath)
{
    QJsonArray results;
    const auto add = [&](const QString& name, bool passed, QJsonObject details = {}) {
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
        details.insert("background", background);
        details.insert("nativeVisibleWindows", nativeVisibleWindows);
        results.append(QJsonObject{{"name", name},
            {"status", passed && background && nativeVisibleWindows == 0 ? "PASS" : "FAIL"},
            {"message", "Playback keyboard, navigation, display and drop regression"}, {"details", details}});
    };
    results.append(_runMediaOpenSmokeCheck(mediaPath));
    auto* playback = _p->playbackCtrl.get();
    auto* bar = _p->playbackBar;
    auto* timeline = _p->timeline;
    auto* viewer = _p->viewer.data();
    if (!playback || !playback->isValid() || !bar || !timeline || !viewer) {
        add("Playback interaction ready", false);
        return results;
    }
    auto* initialFocus = focusWidget();
    add("Opened media focuses viewer", initialFocus &&
        (initialFocus == viewer || viewer->isAncestorOf(initialFocus)),
        {{"focusedWidget", initialFocus ? initialFocus->objectName() : QString()},
         {"focusedClass", initialFocus ? QString::fromLatin1(initialFocus->metaObject()->className()) : QString()}});
    const bool savedNavigationSeconds = bar->navigationInSeconds();
    const int savedNavigationAmount = bar->navigationStepAmount();
    const bool savedDisplaySeconds = bar->displaySeconds();
    const float savedVolume = playback->getVolume();
    const bool savedMute = playback->isMuted();
    const int savedLoop = playback->loopMode();
    const double savedSpeed = playback->speedMultiplier();
    const auto sourceFrame = [&]() {
#if CGPLAY_HAS_TLRENDER
        if (const auto player = playback->player()) {
            const double fps = playback->fps();
            return qRound(player->getCurrentTime().value_rescaled_to(fps) -
                player->getTimeRange().start_time().value_rescaled_to(fps));
        }
#endif
        return playback->currentFrame();
    };
    playback->setMute(true);
    playback->clearInOutPoints();
    playback->setLoop(0);

    auto* playlistView = _p->playlist->findChild<QListView*>();
    auto* transport = bar->playBtn();
    bar->setNavigationStep(true, 1);
    const int total = playback->totalFrames();
    const auto wrapped = [total](int frame) { return (frame % total + total) % total; };
    const int beforePlayingSeek = sourceFrame();
    const int secondsStep = bar->navigationStepFrames();
    playbackInteractionKey(playlistView, Qt::Key_Right);
    add("First-open Right seeks without pausing", playlistView && playback->playbackState() == 1 &&
        sourceFrame() == wrapped(beforePlayingSeek + secondsStep),
        {{"before", beforePlayingSeek}, {"after", sourceFrame()}, {"step", secondsStep},
         {"playbackState", playback->playbackState()}});
    playbackInteractionKey(transport, Qt::Key_Space);
    const bool firstSpacePaused = playback->playbackState() == 0;
    playbackInteractionKey(transport, Qt::Key_Space);
    add("First-open Space works on transport", firstSpacePaused && playback->playbackState() == 1,
        {{"receiver", "PlaybackBarPlayToggle"}, {"playbackState", playback->playbackState()}});

    playback->setSpeed(playback->fps() * 1.5);
    playback->reverse();
    const int beforeReverseSeek = sourceFrame();
    playbackInteractionKey(transport, Qt::Key_Left);
    add("Left seek preserves reverse and speed", playback->playbackState() == 2 &&
        sourceFrame() == wrapped(beforeReverseSeek - secondsStep) &&
        std::abs(playback->speedMultiplier() - 1.5) < 0.0001);
    playback->pause();
    playback->setSpeed(playback->fps());

    bool navigationMenuPassed = true;
    QJsonArray navigationTrials;
    for (const bool seconds : {false, true}) {
        for (const int amount : {1, 5, 10}) {
            const QString name = QStringLiteral("PlaybackBarNavigation%1%2")
                .arg(seconds ? QStringLiteral("Seconds") : QStringLiteral("Frames")).arg(amount);
            auto* action = bar->findChild<QAction*>(name);
            if (action) action->trigger();
            playback->seekToFrame(total / 3);
            const int before = sourceFrame();
            const int step = seconds ? qMax(1, qRound(amount * playback->fps())) : amount;
            playbackInteractionKey(playlistView, Qt::Key_Right);
            const bool passed = action && bar->navigationInSeconds() == seconds &&
                bar->navigationStepAmount() == amount && bar->navigationStepFrames() == step &&
                sourceFrame() == wrapped(before + step) && playback->playbackState() == 0;
            navigationMenuPassed = navigationMenuPassed && passed;
            navigationTrials.append(QJsonObject{{"seconds", seconds}, {"amount", amount},
                {"step", step}, {"before", before}, {"after", sourceFrame()}, {"passed", passed}});
        }
    }
    add("Navigation frame and second menu choices", navigationMenuPassed, {{"trials", navigationTrials}});
    bar->setNavigationStep(false, 1);
    playback->seekToFrame(total / 3);
    const int beforeRepeat = sourceFrame();
    playbackInteractionKey(playlistView, Qt::Key_Right, Qt::NoModifier, true);
    const bool repeatNavigated = sourceFrame() == wrapped(beforeRepeat + 1);
    playbackInteractionKey(transport, Qt::Key_Space, Qt::NoModifier, true);
    add("Held navigation repeats and Space does not", repeatNavigated && playback->playbackState() == 0);

    const auto loopBoundarySeeks = [&]() {
        playback->pause();
        playback->setLoop(0);
        const int count = playback->totalFrames();
        const int multipleLoops = static_cast<int>(std::min<qint64>(
            std::numeric_limits<int>::max(), static_cast<qint64>(count) * 3 + 7));
        const QList<int> deltas{-1, 1, -10, 10, -multipleLoops, multipleLoops,
            std::numeric_limits<int>::min(), std::numeric_limits<int>::max()};
        QJsonArray trials;
        bool passed = count > 0;
        if (count <= 0) return QJsonObject{{"passed", false}, {"total", count}};
        for (const int start : {0, count / 2, count - 1}) {
            for (const int delta : deltas) {
                playback->seekToFrame(start);
                const qint64 offset = static_cast<qint64>(start) + delta;
                const int expected = static_cast<int>((offset % count + count) % count);
                playback->seekRelative(delta);
                const int actual = sourceFrame();
                const bool trialPassed = actual == expected && playback->playbackState() == 0;
                passed = passed && trialPassed;
                trials.append(QJsonObject{{"start", start}, {"delta", delta},
                    {"expected", expected}, {"actual", actual}, {"passed", trialPassed}});
            }
        }
        return QJsonObject{{"passed", passed}, {"total", count}, {"fps", playback->fps()}, {"trials", trials}};
    };
    const QJsonObject boundaryTrials = loopBoundarySeeks();
    add("Loop relative seek preserves overshoot and large deltas", boundaryTrials.value("passed").toBool(), boundaryTrials);
#if CGPLAY_HAS_TLRENDER
    {
        bool nativeModesPassed = true;
        QJsonArray nativeModeTrials;
        const auto player = playback->player();
        const int beyondRange = static_cast<int>(std::min<qint64>(
            std::numeric_limits<int>::max(), static_cast<qint64>(total) + 7));
        for (const int mode : {1, 2}) {
            playback->setLoop(mode);
            for (const int state : {0, 1, 2}) {
                for (const int start : {0, total - 1}) {
                    for (const int delta : {-beyondRange, -1, 1, beyondRange}) {
                        playback->pause();
                        playback->seekToFrame(total / 2);
                        if (state == 1) playback->forward();
                        if (state == 2) playback->reverse();
                        playback->seekToFrame(start);
                        const auto before = player->getCurrentTime();
                        player->seek(before + OTIO_NS::RationalTime(delta, before.rate()));
                        const auto expected = player->getCurrentTime();
                        const auto expectedState = player->getPlayback();
                        player->seek(before);
                        playback->seekRelative(delta);
                        const bool passed = player->getCurrentTime() == expected && player->getPlayback() == expectedState;
                        nativeModesPassed = nativeModesPassed && passed;
                        nativeModeTrials.append(QJsonObject{{"mode", mode}, {"state", state},
                            {"start", start}, {"delta", delta}, {"passed", passed},
                            {"expectedTime", expected.value()}, {"actualTime", player->getCurrentTime().value()},
                            {"expectedState", static_cast<int>(expectedState)},
                            {"actualState", static_cast<int>(player->getPlayback())}});
                    }
                }
            }
        }
        playback->pause();
        playback->setLoop(0);
        add("Once and PingPong relative seeks preserve native behavior", nativeModesPassed, {{"trials", nativeModeTrials}});
    }
#endif

    playback->setVolume(0.5f);
    playbackInteractionKey(playlistView, Qt::Key_Up);
    const bool volumeUp = std::abs(playback->getVolume() - 0.55f) < 0.0001;
    playbackInteractionKey(transport, Qt::Key_Down);
    const bool volumeDown = std::abs(playback->getVolume() - 0.5f) < 0.0001;
    playback->setVolume(0.98f);
    playbackInteractionKey(playlistView, Qt::Key_Up, Qt::NoModifier, true);
    const bool upperBound = playback->getVolume() == 1.0f;
    playback->setVolume(0.02f);
    playbackInteractionKey(playlistView, Qt::Key_Down);
    add("Up Down volume shortcuts and bounds", volumeUp && volumeDown && upperBound && playback->getVolume() == 0.0f);

    {
        QLineEdit editor(this);
        editor.setText(QStringLiteral("edit"));
        editor.show();
        editor.setFocus();
        const int before = sourceFrame();
        playback->setVolume(0.5f);
        const float volume = playback->getVolume();
        playbackInteractionKey(&editor, Qt::Key_Space);
        playbackInteractionKey(&editor, Qt::Key_Right);
        playbackInteractionKey(&editor, Qt::Key_Up);
        const bool upPreservedVolume = playback->getVolume() == volume;
        playbackInteractionKey(&editor, Qt::Key_Down);
        add("Transport shortcuts preserve text input", editor.text().contains(QLatin1Char(' ')) &&
            playback->playbackState() == 0 && sourceFrame() == before && upPreservedVolume && playback->getVolume() == volume);
        playback->setVolume(0.0f);
    }
    {
        PlaybackInteractionKeySink otherWindow(this, Qt::Dialog);
        otherWindow.setAttribute(Qt::WA_DontShowOnScreen);
        otherWindow.show();
        const int before = sourceFrame();
        playbackInteractionKey(&otherWindow, Qt::Key_Space);
        playbackInteractionKey(&otherWindow, Qt::Key_Right);
        playbackInteractionKey(&otherWindow, Qt::Key_Up);
        add("Transport shortcuts isolate other windows", otherWindow.keyCount == 3 &&
            playback->playbackState() == 0 && sourceFrame() == before && playback->getVolume() == 0.0f);
    }
    {
        QMenu popup(this);
        popup.setAttribute(Qt::WA_DontShowOnScreen);
        popup.addAction(QStringLiteral("Menu item"));
        const int before = sourceFrame();
        popup.popup(QPoint(0, 0));
        playbackInteractionKey(&popup, Qt::Key_Right);
        playbackInteractionKey(&popup, Qt::Key_Up);
        popup.hide();
        add("Transport shortcuts preserve popup navigation", playback->playbackState() == 0 &&
            sourceFrame() == before && playback->getVolume() == 0.0f);
    }
    for (auto& descriptor : _p->commandDescriptors) {
        if (descriptor.id != QStringLiteral("playback.nextFrame")) continue;
        const QString savedShortcut = descriptor.shortcut;
        QList<QPair<QAction*, QKeySequence>> actions;
        for (auto* action : findChildren<QAction*>(descriptor.id)) actions.append({action, action->shortcut()});
        const auto bind = [&](const QString& shortcut) {
            descriptor.shortcut = shortcut;
            for (const auto& action : actions) action.first->setShortcut(QKeySequence(shortcut));
        };
        playback->seekToFrame(total / 3);
        const int before = sourceFrame();
        bind(QStringLiteral("Alt+G"));
        playbackInteractionKey(viewer, Qt::Key_Right);
        const bool oldIgnored = sourceFrame() == before;
        playbackInteractionKey(viewer, Qt::Key_G, Qt::AltModifier);
        const bool customWorked = sourceFrame() == wrapped(before + 1);
        bind(QString());
        const int beforeCleared = sourceFrame();
        playbackInteractionKey(viewer, Qt::Key_Right);
        playbackInteractionKey(viewer, Qt::Key_G, Qt::AltModifier);
        add("Navigation custom and cleared shortcuts", oldIgnored && customWorked && sourceFrame() == beforeCleared);
        descriptor.shortcut = savedShortcut;
        for (const auto& action : actions) action.first->setShortcut(action.second);
        break;
    }

    auto* progress = timeline->findChild<QToolButton*>(QStringLiteral("TimelineProgressDisplay"));
    auto* timeDisplay = bar->findChild<QToolButton*>(QStringLiteral("PlaybackBarTimeDisplay"));
    bar->setDisplaySeconds(false);
    timeline->setDisplaySeconds(false);
    playback->seekToFrame(qMin(37, total - 1));
    settlePlaybackInteraction();
    const bool frameDisplay = progress && progress->text().contains(QString::number(playback->currentFrame() + 1));
    if (progress) progress->click();
    const bool secondsDisplay = bar->displaySeconds() && timeline->displaySeconds() && progress && timeDisplay &&
        progress->text().contains(QStringLiteral("秒")) && timeDisplay->text().contains(QStringLiteral("秒"));
    if (timeDisplay) timeDisplay->click();
    add("Timeline and transport display units stay synchronized", frameDisplay && secondsDisplay &&
        !bar->displaySeconds() && !timeline->displaySeconds());

    const QString evidence = qEnvironmentVariable("CGPLAY_SMOKE_EVIDENCE_DIR");
    {
        TimelineWidget narrowTimeline(_p->playbackCtrl);
        narrowTimeline.setAttribute(Qt::WA_DontShowOnScreen);
        narrowTimeline.resize(360, 110);
        narrowTimeline.setMediaPath(playback->currentPath());
        narrowTimeline.setDisplaySeconds(true);
        narrowTimeline.show();
        playback->seekToFrame(total - 1);
        settlePlaybackInteraction();
        auto* narrowProgress = narrowTimeline.findChild<QToolButton*>(QStringLiteral("TimelineProgressDisplay"));
        const QString expectedProgress = QStringLiteral("%1 / %2 秒")
            .arg(formatFrameSeconds(total - 1, playback->fps()), formatFrameSeconds(total, playback->fps()));
        const bool complete = narrowProgress && narrowProgress->text() == expectedProgress &&
            narrowProgress->width() >= narrowProgress->sizeHint().width() &&
            narrowTimeline.rect().contains(QRect(narrowProgress->mapTo(&narrowTimeline, QPoint()), narrowProgress->size()));
        const QString capture = QDir(evidence).filePath(QStringLiteral("timeline_narrow_seconds.png"));
        const bool captured = !evidence.isEmpty() && narrowTimeline.grab().save(capture);
        add("Narrow timeline displays complete seconds progress", complete && captured,
            {{"text", narrowProgress ? narrowProgress->text() : QString()},
             {"width", narrowProgress ? narrowProgress->width() : 0},
             {"requiredWidth", narrowProgress ? narrowProgress->sizeHint().width() : 0},
             {"expectedText", expectedProgress}, {"capture", capture}});
    }
    bar->setDisplaySeconds(true);
    timeline->setDisplaySeconds(true);
    settlePlaybackInteraction();
    const QString timelineCapture = QDir(evidence).filePath(QStringLiteral("timeline_seconds.png"));
    const QString controlsCapture = QDir(evidence).filePath(QStringLiteral("playback_controls_seconds.png"));
    add("Playback timeline and controls capture", !evidence.isEmpty() &&
        timeline->grab().save(timelineCapture) && bar->grab().save(controlsCapture),
        {{"timelineCapture", timelineCapture}, {"controlsCapture", controlsCapture}});

    const QString sequenceInput = qEnvironmentVariable("CGPLAY_SMOKE_SEEK_SEQUENCE");
    const QString sequencePath = sequenceInput.isEmpty() ? QString() : QFileInfo(sequenceInput).absoluteFilePath();
    const double sequenceFps = qEnvironmentVariable("CGPLAY_SMOKE_SEEK_SEQUENCE_FPS", "23.976").toDouble();
    if (!sequencePath.isEmpty() && QFileInfo(sequencePath).isFile() && sequenceFps > 0.0) {
        _openFileWithMediaInfo(sequencePath, sequenceFps, MediaProbe::probe(sequencePath, sequenceFps));
        settlePlaybackInteraction();
        QJsonObject sequenceTrials = loopBoundarySeeks();
        double sourceStart = 0.0;
#if CGPLAY_HAS_TLRENDER
        if (const auto player = playback->player())
            sourceStart = player->getTimeRange().start_time().value_rescaled_to(playback->fps());
#endif
        const bool fractionalRate = std::abs(playback->fps() - sequenceFps) < 0.00001 &&
            std::abs(playback->fps() - std::round(playback->fps())) > 0.00001;
        sequenceTrials.insert("sourceStart", sourceStart);
        sequenceTrials.insert("expectedFps", sequenceFps);
        add("Fractional-rate sequence relative seeks respect source start", playback->isValid() &&
            sourceStart > 0.0 && fractionalRate && sequenceTrials.value("passed").toBool(), sequenceTrials);
        _openFileWithMediaInfo(mediaPath, 0.0, MediaProbe::probe(mediaPath));
        settlePlaybackInteraction();
    } else {
        add("Fractional-rate sequence seek fixture", false, {{"path", sequencePath}, {"fps", sequenceFps}});
    }

    const QString otherVideoInput = qEnvironmentVariable("CGPLAY_SMOKE_OTHER_VIDEO");
    const QString otherVideo = otherVideoInput.isEmpty() ? QString() : QFileInfo(otherVideoInput).absoluteFilePath();
    if (!otherVideo.isEmpty() && QFileInfo(otherVideo).isFile() && _p->playlist->model()) {
        auto* model = _p->playlist->model();
        const int beforeCount = model->shotCount();
        bool dragAccepted = false;
        bool dropAccepted = false;
        qint64 openWaitMs = 0;
        const auto currentVideoMatches = [&]() {
            return QFileInfo(playback->currentPath()).absoluteFilePath() == otherVideo;
        };
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(otherVideo)});
        const auto dropVideo = [&]() {
            QDragEnterEvent enter(viewer->rect().center(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer, &enter);
            dragAccepted = enter.isAccepted();
            QDropEvent drop(QPointF(viewer->rect().center()), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer, &drop);
            dropAccepted = drop.isAccepted();
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 10000 && (!currentVideoMatches() || !playback->isValid() ||
                   _p->mediaProbeJob || _p->autoPlayPending)) settlePlaybackInteraction(20);
            openWaitMs = timer.elapsed();
            return dragAccepted && dropAccepted;
        };
        const bool accepted = dropVideo();
        add("Viewer drop plays new media and appends playlist", accepted && currentVideoMatches() &&
            playback->isValid() && playback->playbackState() == 1 && model->shotCount() == beforeCount + 1 &&
            _p->compareBar->compareMode() == 0 && !playback->hasCompare(),
            {{"currentPath", playback->currentPath()}, {"expectedPath", otherVideo},
             {"dragAccepted", dragAccepted}, {"dropAccepted", dropAccepted},
             {"isValid", playback->isValid()}, {"playbackState", playback->playbackState()},
             {"autoPlayPending", _p->autoPlayPending}, {"mediaProbePending", !_p->mediaProbeJob.isNull()},
             {"hasCompare", playback->hasCompare()}, {"openWaitMs", double(openWaitMs)},
             {"playlistBefore", beforeCount}, {"playlistAfter", model->shotCount()},
             {"compareMode", _p->compareBar->compareMode()}});
        dropVideo();
        add("Repeated viewer drop avoids duplicate playlist row", model->shotCount() == beforeCount + 1);
        QMetaObject::invokeMethod(_p->playlist, "shotBSelected", Q_ARG(QString, mediaPath));
        settlePlaybackInteraction(250);
        const bool explicitCompare = playback->hasCompare() && _p->compareBar->compareMode() == 5;
        QMetaObject::invokeMethod(viewer, "compareRequested");
        add("Comparison opens explicitly and closes to single view", explicitCompare && _p->compareBar->compareMode() == 0);

        QMetaObject::invokeMethod(_p->playlist, "shotASelected", Q_ARG(QString, mediaPath));
        const bool openingA = !_p->mediaProbeJob.isNull();
        QMetaObject::invokeMethod(_p->playlist, "shotBSelected", Q_ARG(QString, mediaPath));
        const bool firstBQueued = _p->pendingComparePath == mediaPath;
        QMetaObject::invokeMethod(_p->compareBar, "shotBFileDropped", Q_ARG(QString, otherVideo));
        const bool latestBQueued = _p->pendingComparePath == otherVideo;
        QElapsedTimer comparisonTimer;
        comparisonTimer.start();
        while (comparisonTimer.elapsed() < 10000 && (_p->mediaProbeJob || _p->autoPlayPending ||
               QFileInfo(playback->currentPath()).absoluteFilePath() != QFileInfo(mediaPath).absoluteFilePath()))
            settlePlaybackInteraction(20);
        QString comparedPath;
#if CGPLAY_HAS_TLRENDER
        if (const auto player = playback->player(); player && !player->getCompare().empty()) {
            comparedPath = QString::fromStdString(player->getCompare().front()->getPath().get());
        }
#endif
        add("Async A selection applies latest requested B", openingA && firstBQueued && latestBQueued &&
            _p->pendingComparePath.isEmpty() && playback->hasCompare() && _p->compareBar->compareMode() == 5 &&
            QFileInfo(comparedPath).absoluteFilePath() == otherVideo,
            {{"openingA", openingA}, {"firstBQueued", firstBQueued}, {"latestBQueued", latestBQueued},
             {"comparedPath", comparedPath}, {"expectedB", otherVideo},
             {"pendingComparePath", _p->pendingComparePath}, {"waitMs", double(comparisonTimer.elapsed())}});

        QMetaObject::invokeMethod(_p->playlist, "shotASelected", Q_ARG(QString, mediaPath));
        QMetaObject::invokeMethod(_p->playlist, "shotBSelected", Q_ARG(QString, otherVideo));
        const bool comparisonPending = _p->pendingComparePath == otherVideo;
        const bool replacementAccepted = dropVideo();
        add("New viewer drop cancels pending comparison", comparisonPending && replacementAccepted &&
            currentVideoMatches() && _p->pendingComparePath.isEmpty() && !playback->hasCompare() &&
            _p->compareBar->compareMode() == 0 && model->shotCount() == beforeCount + 1,
            {{"comparisonWasPending", comparisonPending}, {"pendingComparePath", _p->pendingComparePath},
             {"hasCompare", playback->hasCompare()}, {"compareMode", _p->compareBar->compareMode()}});
    } else {
        add("Viewer drop second-video fixture", false, {{"path", otherVideo}});
    }

    playback->pause();
    playback->setVolume(savedVolume);
    playback->setMute(savedMute);
    playback->setLoop(savedLoop);
    playback->setSpeed(playback->fps() * savedSpeed);
    bar->setNavigationStep(savedNavigationSeconds, savedNavigationAmount);
    bar->setDisplaySeconds(savedDisplaySeconds);
    timeline->setDisplaySeconds(savedDisplaySeconds);
    return results;
}

} // namespace cgplay
