#include "Application.h"
#include "ApplicationPrivate.h"
#include "playback/PlaybackController.h"
#include "media/MediaProbe.h"
#include "viewer/ViewerWidget.h"
#include "viewer/TlViewport.h"
#include "PlaybackBar.h"
#include "timeline/TimelineWidget.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QTimer>
#include <QWindow>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QRegularExpression>
#include <cmath>
#if CGPLAY_HAS_TLRENDER
#include <tlRender/Timeline/Player.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace cgplay {
QJsonArray MainWindow::_runSequenceTimeSmokeChecks(const QString& mediaPath)
{
    QJsonArray results;
    const auto settle = [](int ms = 180) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    const auto add = [&](const QString& name, bool passed, QJsonObject details = {}) {
        int visible = 0;
#ifdef Q_OS_WIN
        EnumWindows([](HWND window, LPARAM data) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(data);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&visible));
#else
        for (auto* window : QApplication::topLevelWidgets())
            if (window->isVisible() && !window->testAttribute(Qt::WA_DontShowOnScreen)) ++visible;
#endif
        const bool background = qApp->property("cgplay.automationBackground").toBool();
        details.insert("background", background);
        details.insert("nativeVisibleWindows", visible);
        results.append(QJsonObject{{"name", name}, {"status", passed && background && visible == 0 ? "PASS" : "FAIL"},
            {"message", "Sequence relative-frame and UI time contract"}, {"details", details}});
    };
#if CGPLAY_HAS_TLRENDER
    const auto files = MediaProbe::collectSequenceFiles(mediaPath);
    const double fps = qEnvironmentVariable("CGPLAY_SMOKE_SEQUENCE_FPS", "24").toDouble();
    if (files.size() < 12 || fps <= 0.0) {
        add("Sequence fixture", false, {{"media", mediaPath}, {"files", files.size()}, {"fps", fps}});
        return results;
    }
    _openFileWithMediaInfo(mediaPath, fps, MediaProbe::probe(mediaPath, fps));
    settle();
    auto* playback = _p->playbackCtrl.get();
    const auto player = playback->player();
    if (!player) {
        add("Sequence player ready", false);
        return results;
    }
    playback->pause();
    playback->clearInOutPoints();
    const int total = files.size();
    const int first = files.front().frameNumber;
    add("Sequence source range", playback->totalFrames() == total &&
        std::abs(playback->fps() - fps) < 0.0001 &&
        std::abs(player->getTimeRange().start_time().value_rescaled_to(fps) - first) < 0.0001,
        {{"sourceFirst", first}, {"total", playback->totalFrames()}, {"fps", playback->fps()}});
    const auto labelText = [](QWidget* parent, const QString& pattern) {
        const QRegularExpression expression(pattern);
        for (auto* label : parent->findChildren<QLabel*>())
            if (expression.match(label->text()).hasMatch()) return label->text();
        return QString();
    };
    const auto checkFrame = [&](const QString& name, int expected) {
        settle();
        const int nominalFps = qRound(fps);
        const int seconds = expected / nominalFps;
        const QString expectedTime = QStringLiteral("%1:%2:%3:%4")
            .arg(seconds / 3600, 2, 10, QChar('0')).arg(seconds / 60 % 60, 2, 10, QChar('0'))
            .arg(seconds % 60, 2, 10, QChar('0')).arg(expected % nominalFps, 2, 10, QChar('0'));
        const QString time = labelText(_p->playbackBar, QStringLiteral("^\\d{2}:\\d{2}:\\d{2}:\\d{2}$"));
        const QString count = labelText(_p->timeline, QStringLiteral("^\\d+ / \\d+$"));
        const double source = player->getCurrentTime().value_rescaled_to(fps);
        add(name, playback->currentFrame() == expected && time == expectedTime &&
            count == QStringLiteral("%1 / %2").arg(expected + 1).arg(total) &&
            std::abs(source - (first + expected)) < 0.0001,
            {{"expectedFrame", expected}, {"frame", playback->currentFrame()}, {"sourceFrame", source},
             {"timecode", time}, {"expectedTimecode", expectedTime}, {"timelineCounter", count}});
    };
    checkFrame("Sequence initial time", 0);
    playback->seekToFrame(10);
    checkFrame("Sequence relative seek", 10);
    playback->nextFrame();
    checkFrame("Sequence next frame", 11);
    playback->prevFrame();
    checkFrame("Sequence previous frame", 10);
    playback->seekRelative(-5);
    checkFrame("Sequence relative backward seek", 5);
    playback->gotoEnd();
    checkFrame("Sequence end time", total - 1);
    playback->gotoStart();
    checkFrame("Sequence start time", 0);
    auto* bar = _p->timeline->layout()->itemAt(0)->widget();
    const QPointF local(bar->width() / 2, bar->height() / 2);
    const QPointF global(bar->mapToGlobal(local.toPoint()));
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &press);
    QApplication::sendEvent(bar, &release);
    settle();
    const int sought = playback->currentFrame();
    add("Sequence timeline seek range", sought >= total / 2 - 1 && sought <= total / 2 + 1,
        {{"frame", sought}});
    // The exact frame selected by a mouse coordinate is rounded by the
    // timeline widget.  Validate the time display against that reported
    // relative frame rather than assuming one particular rounding direction.
    checkFrame("Sequence timeline seek time", sought);
    playback->gotoStart();
    playback->forward();
    settle(std::max(350, qRound(1500.0 / fps)));
    playback->pause();
    settle();
    const int played = playback->currentFrame();
    add("Sequence playback advances relative frames", played > 0 && played < total,
        {{"frame", played}, {"total", total}});
    checkFrame("Sequence playback time", played);
    playback->setInPoint(3);
    playback->setOutPoint(8);
    playback->seekToFrame(7);
    settle();
    QList<int> loopFrames;
    QObject receiver;
    connect(playback->signalProxy(), &PlaybackServiceSignals::currentFrameChanged, &receiver,
        [&](int frame, int) { loopFrames.append(frame); });
    playback->forward();
    settle(std::max(600, qRound(2500.0 / fps)));
    playback->pause();
    disconnect(playback->signalProxy(), nullptr, &receiver, nullptr);
    bool wrapped = false;
    bool bounded = !loopFrames.isEmpty();
    int previous = 7;
    for (int frame : loopFrames) {
        bounded = bounded && frame >= 3 && frame < 8;
        wrapped = wrapped || frame < previous;
        previous = frame;
    }
    QJsonArray observed;
    for (int frame : loopFrames) observed.append(frame);
    add("Sequence in-out relative loop", bounded && wrapped, {{"frames", observed}});
    playback->clearInOutPoints();
    playback->gotoStart();
    checkFrame("Sequence restored start", 0);
#else
    Q_UNUSED(mediaPath);
    add("Sequence tlRender backend required", false);
#endif
    return results;
}

QJsonObject MainWindow::_runMediaOpenSmokeCheck(const QString& mediaPath)
{
    const bool timecodeVisible = _p->viewer && _p->viewer->isTimecodeVisible();
    if (_p->viewer) _p->viewer->setTimecodeVisible(false);
    QElapsedTimer timer;
    timer.start();
    openFile(mediaPath);
    const qint64 dispatchMs = timer.elapsed();
    qint64 readyMs = -1, pixelMs = -1;
    bool autoPlayPass = true;
    QImage firstFrame;
    const bool requirePixels = qEnvironmentVariableIntValue("CGPLAY_SMOKE_REQUIRE_NONBLACK") == 1;
    while (timer.elapsed() < 30000) {
        if (_p->playbackCtrl && _p->playbackCtrl->isValid()) {
            if (readyMs < 0) readyMs = timer.elapsed();
            if (readyMs >= 0 && MediaProbe::isVideoPath(mediaPath)) {
                autoPlayPass = _p->playbackCtrl->playbackState() != 0;
            }
            if (auto* viewport = _p->viewer ? _p->viewer->viewport() : nullptr) {
                const QImage frame = viewport->grabFramebuffer();
                // Sample the image surface, never surrounding UI chrome. Known
                // nonblack fixtures opt in; genuinely black movies remain valid.
                int brightPixels = 0;
                for (int y = frame.height() / 4; y < frame.height() * 3 / 4; y += 9) {
                    for (int x = frame.width() / 4; x < frame.width() * 3 / 4; x += 9) {
                        const QColor c = frame.pixelColor(x, y);
                        const int hi = qMax(c.red(), qMax(c.green(), c.blue()));
                        if (hi > 45) ++brightPixels;
                    }
                }
                if (brightPixels >= 8) {
                    firstFrame = frame;
                    pixelMs = timer.elapsed();
                    break;
                }
                if (!requirePixels && timer.elapsed() - readyMs >= 250) {
                    firstFrame = frame;
                    break;
                }
            }
        }
        QEventLoop loop;
        QTimer::singleShot(10, &loop, &QEventLoop::quit);
        loop.exec();
    }
    const QStringList args = QCoreApplication::arguments();
    const int outputArg = args.indexOf(QStringLiteral("--smoke-output"));
    const QDir dir(outputArg >= 0 && outputArg + 1 < args.size()
        ? QFileInfo(args[outputArg + 1]).absolutePath()
        : QDir::current().filePath(QStringLiteral("tests/artifacts/media_open_smoke")));
    QDir().mkpath(dir.absolutePath());
    const QString prefix = outputArg >= 0 && outputArg + 1 < args.size()
        ? QFileInfo(args[outputArg + 1]).completeBaseName() : QStringLiteral("media_open");
    const QString capture = dir.filePath(prefix + QStringLiteral("_first_video_frame.png"));
    const bool saved = !firstFrame.isNull() && firstFrame.save(capture);
    const QString bitrateLabel = _p->lblBitrate ? _p->lblBitrate->text() : QString();
    const bool bitrateLabelPresent = bitrateLabel.startsWith(QStringLiteral("码率:"));
    int visible = 0;
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (widget->isVisible() && !widget->testAttribute(Qt::WA_DontShowOnScreen)) ++visible;
    }
    int nativeVisible = 0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId == GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(param);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&nativeVisible));
#endif
    const bool background = qApp->property("cgplay.automationBackground").toBool();
    const bool passed = dispatchMs <= 500 && readyMs >= 0 && autoPlayPass && bitrateLabelPresent
        && (!requirePixels || (pixelMs >= 0 && saved))
        && background && visible == 0 && nativeVisible == 0;
    const QJsonObject details{
        {"media", mediaPath}, {"openDispatchMs", dispatchMs}, {"playerReadyMs", readyMs},
        {"autoPlayPass", autoPlayPass}, {"playbackState", _p->playbackCtrl ? _p->playbackCtrl->playbackState() : 0},
        {"bitrateLabel", bitrateLabel}, {"bitrateLabelPresent", bitrateLabelPresent},
        {"firstNonBlackFrameMs", pixelMs}, {"firstFramePixelPass", pixelMs >= 0},
        {"firstFrameCapture", saved ? capture : QString()}, {"nonblackFixtureRequired", requirePixels},
        {"captureMethod", "QOpenGLWidget::grabFramebuffer (background surface)"},
        {"background", background}, {"visiblePlatformWindows", visible}, {"nativeVisibleWindows", nativeVisible},
        {"frame", _p->playbackCtrl->currentFrame()}, {"total", _p->playbackCtrl->totalFrames()}, {"fps", _p->playbackCtrl->fps()}
    };
    const QString failure = dispatchMs > 500 ? QStringLiteral("synchronous open exceeded 500ms")
        : readyMs < 0 ? QStringLiteral("player invalid")
        : !autoPlayPass ? QStringLiteral("video opened in paused state")
        : QStringLiteral("First-frame pixels or background gate failed");
    if (_p->viewer) _p->viewer->setTimecodeVisible(timecodeVisible);
    return {{"name", "open media"}, {"status", passed ? "PASS" : "FAIL"},
            {"message", passed ? QFileInfo(mediaPath).fileName() : failure},
            {"details", details}};
}
}
