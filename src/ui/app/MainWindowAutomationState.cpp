#include "ApplicationInternal.h"
#include <QEventLoop>
#include <QMenu>
#if CGPLAY_HAS_TLRENDER
#include <tlRender/Timeline/Player.h>
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace cgplay {
QJsonArray MainWindow::_runMediaStateSmokeChecks(const QString& mediaPath)
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
            {"message", "Media state ownership and delayed callback regression"}, {"details", details}});
    };
#if CGPLAY_HAS_TLRENDER
    const QString other = qEnvironmentVariable("CGPLAY_SMOKE_OTHER_SEQUENCE");
    const QString video = qEnvironmentVariable("CGPLAY_SMOKE_VIDEO");
    const QString evidence = qEnvironmentVariable("CGPLAY_SMOKE_EVIDENCE_DIR");
    if (!QFileInfo::exists(mediaPath) || !QFileInfo::exists(other) || !QFileInfo::exists(video) || evidence.isEmpty()) {
        add("State fixture", false);
        return results;
    }
    auto* playback = dynamic_cast<PlaybackController*>(_p->playbackCtrl.get());
    if (!playback) { add("State real playback service", false); return results; }
    const auto exrInfo = MediaProbe::probe(mediaPath, 24.0);
    const auto otherInfo = MediaProbe::probe(other, 30.0);
    const auto videoInfo = MediaProbe::probe(video);
    _openFileWithMediaInfo(mediaPath, 24.0, exrInfo);
    settle();
    if (!playback->player()) { add("State real player ready", false); return results; }
    playback->setSpeed(24.0);
    playback->setLoop(0);
    playback->setInPoint(3);
    playback->setOutPoint(8);
    _openFileWithMediaInfo(other, 30.0, otherInfo);
    settle();
    add("Media switch clears A-B range", !playback->hasInOutRange(),
        {{"in", playback->inPoint()}, {"out", playback->outPoint()}});
    int maxFrame = 0;
    QObject receiver;
    connect(playback->signalProxy(), &PlaybackServiceSignals::currentFrameChanged, &receiver,
        [&](int frame, int) { maxFrame = std::max(maxFrame, frame); });
    playback->forward();
    settle(650);
    playback->pause();
    disconnect(playback->signalProxy(), nullptr, &receiver, nullptr);
    add("New media plays beyond old out point", maxFrame > 8, {{"maxFrame", maxFrame}});

    auto* speed = _p->playbackBar->findChild<QToolButton*>("PlaybackBarSpeed");
    auto* loop = _p->playbackBar->findChild<QToolButton*>("PlaybackBarLoop");
    if (!speed || !speed->menu() || !loop) { add("State controls found", false); return results; }
    for (auto* action : speed->menu()->actions())
        if (action->text() == QStringLiteral("2.0x")) { action->trigger(); break; }
    loop->click();
    _openFileWithMediaInfo(mediaPath, 24.0, exrInfo);
    settle();
    add("Speed preference survives media switch", speed->text() == "2.0x" &&
        playback->speedMultiplier() == 2.0 && std::abs(playback->player()->getSpeed() - 48.0) < .001,
        {{"label", speed->text()}, {"actualSpeed", playback->player()->getSpeed()}});
    add("Loop preference survives media switch", loop->text() == "1" &&
        playback->loopMode() == 1 && playback->player()->getLoop() == tl::Loop::Once);
    _p->playbackBar->grab().save(evidence + "/playback_options.png");
    {
        PlaybackBar second(_p->playbackCtrl, this);
        auto* secondLoop = second.findChild<QToolButton*>("PlaybackBarLoop");
        auto* secondSpeed = second.findChild<QToolButton*>("PlaybackBarSpeed");
        add("New control binds current playback options", secondLoop && secondSpeed &&
            secondLoop->text() == "1" && secondSpeed->text() == "2.0x");
        if (secondLoop) secondLoop->click();
        add("Controls share service loop state", playback->loopMode() == 2 && loop->text() == "AB" &&
            playback->player()->getLoop() == tl::Loop::PingPong);
    }
    _openFileWithMediaInfo(other, 30.0, otherInfo);
    settle();
    add("Speed multiplier follows new media rate", std::abs(playback->player()->getSpeed() - 60.0) < .001 &&
        speed->text() == "2.0x");
    _closeCurrentMedia();
    _openFileWithMediaInfo(mediaPath, 24.0, exrInfo);
    add("Close and reopen preserve playback preferences", playback->speedMultiplier() == 2.0 && playback->loopMode() == 2 &&
        playback->player() && std::abs(playback->player()->getSpeed() - 48.0) < .001);
    playback->setLoop(0);
    playback->setSpeed(playback->fps());

    auto& subtitles = _p->subtitleSession.state();
    auto generationToken = std::make_shared<std::atomic_bool>(false);
    auto refinementToken = std::make_shared<std::atomic_bool>(false);
    auto enhancementToken = std::make_shared<std::atomic_bool>(false);
    subtitles.subtitleGenerationCancelRequested = generationToken;
    subtitles.subtitleRefinementCancelRequested = refinementToken;
    subtitles.highQualityEnhancementCancelRequested = enhancementToken;
    subtitles.subtitleGenerationBusy = subtitles.subtitleRefinementBusy = subtitles.highQualityEnhancementBusy = true;
    subtitles.generatedSubtitlePath = "stale.vtt";
    const auto oldGeneration = _p->mediaGenerationId;
    _openFileWithMediaInfo(other, 30.0, otherInfo);
    add("Media switch cancels all subtitle tasks", generationToken->load() && refinementToken->load() && enhancementToken->load());
    add("Media switch clears subtitle session", _p->mediaGenerationId > oldGeneration &&
        !subtitles.subtitleGenerationBusy && !subtitles.subtitleRefinementBusy && !subtitles.highQualityEnhancementBusy &&
        subtitles.generatedSubtitlePath.isEmpty());

    _p->ocioManager->clearExrSceneLinearDefaults();
    _p->ocioManager->setEnabled(false);
    _openFileWithMediaInfo(mediaPath, 24.0, exrInfo);
    const bool exrEnabled = _p->ocioManager->isEnabled();
    QElapsedTimer transition;
    transition.start();
    _openFileWithMediaInfo(video, 0.0, videoInfo);
    const bool afterSwitch = _p->ocioManager->isEnabled();
    const auto switchMs = transition.elapsed();
    settle(500);
    add("EXR delayed callback respects media switch", exrEnabled && !afterSwitch && !_p->ocioManager->isEnabled(),
        {{"switchMs", switchMs}, {"afterTimer", _p->ocioManager->isEnabled()}});
    playback->pause();
    playback->seekToFrame(20);
    settle(250);
    const auto before = _p->viewer->viewport()->grabFramebuffer();
    _p->ocioManager->clearExrSceneLinearDefaults();
    _p->ocioManager->setEnabled(false);
    settle(250);
    const auto after = _p->viewer->viewport()->grabFramebuffer();
    int changedPixels = 0, samples = 0;
    if (before.size() == after.size()) {
        for (int y = 0; y < before.height(); y += 8)
            for (int x = 0; x < before.width(); x += 8) {
                const QColor a = before.pixelColor(x, y), b = after.pixelColor(x, y);
                const int delta = std::abs(a.red()-b.red()) + std::abs(a.green()-b.green()) + std::abs(a.blue()-b.blue());
                if (delta > 12) ++changedPixels;
                ++samples;
            }
    }
    const bool imagesSaved = before.save(evidence + "/video_after_switch.png") && after.save(evidence + "/video_reference.png");
    add("Video color unaffected by stale EXR state", imagesSaved && samples > 0 && changedPixels == 0,
        {{"sampleCount", samples}, {"changedPixels", changedPixels}, {"frame", playback->currentFrame()}});
    // Reopen the same path to prove identity is a session generation, not
    // only a filename. Never invoke an external subtitle provider in smoke.
    _p->playbackBar->setTranslationVisible(true);
    _scheduleGeneratedSubtitleContinuation(0.0, 200);
    add("Old subtitle continuation scheduled", subtitles.subtitleContinuationScheduled);
    _p->playbackBar->setTranslationVisible(false);
    _closeCurrentMedia();
    _openFileWithMediaInfo(video, 0.0, videoInfo);
    playback->pause();
    _p->playbackBar->setTranslationVisible(true);
    _scheduleGeneratedSubtitleContinuation(0.5, 800);
    _p->playbackBar->setTranslationVisible(false);
    settle(350);
    add("Old subtitle timer cannot consume new session request", subtitles.subtitleContinuationScheduled &&
        subtitles.subtitleContinuationStartSeconds == 0.5);
    settle(650);
    add("Current subtitle timer still completes", !subtitles.subtitleContinuationScheduled &&
        subtitles.subtitleContinuationStartSeconds == -1.0 && !subtitles.subtitleGenerationBusy);
    _openFileWithMediaInfo(mediaPath, 24.0, exrInfo);
    _closeCurrentMedia();
    settle(400);
    add("EXR close invalidates delayed callback", !playback->isValid() && !_p->ocioManager->isEnabled());
    _openFileWithMediaInfo(mediaPath, 24.0, exrInfo);
    _p->ocioManager->setEnabled(false);
    _openFileWithMediaInfo(other, 30.0, otherInfo);
    settle(400);
    add("New EXR owns its color defaults", playback->currentPath() == other && _p->ocioManager->isEnabled());
    _closeCurrentMedia();
#else
    Q_UNUSED(mediaPath);
    add("State requires tlRender backend", false);
#endif
    return results;
}
} // namespace cgplay
