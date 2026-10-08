#include "Application.h"
#include "ApplicationPrivate.h"
#include "playback/PlaybackController.h"
#include "media/MediaProbe.h"
#include "viewer/ViewerWidget.h"
#include "viewer/TlViewport.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QTimer>
#include <QWindow>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace cgplay {
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
