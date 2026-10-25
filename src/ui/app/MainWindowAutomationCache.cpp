#include "Application.h"
#include "ApplicationPrivate.h"
#include "cache/CacheManager.h"
#include "playback/PlaybackController.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QThread>
#include <cmath>
#include <functional>
#include <memory>
#if CGPLAY_HAS_TLRENDER
#include <tlRender/Timeline/Player.h>
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace cgplay {
QJsonArray MainWindow::_runCacheRuntimeSmokeChecks(const QString& mediaPath)
{
    QJsonArray results;
    const auto add = [&](const QString& name, bool passed, QJsonObject details = {}) {
        int visible = 0;
#ifdef Q_OS_WIN
        EnumWindows([](HWND window, LPARAM data) -> BOOL {
            DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(data);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&visible));
#endif
        const bool background = qApp->property("cgplay.automationBackground").toBool();
        details.insert("background", background); details.insert("nativeVisibleWindows", visible);
        results.append(QJsonObject{{"name", name}, {"status", passed && background && !visible ? "PASS" : "FAIL"},
            {"message", "Real native Player cache options and multiwindow signal lifecycle"}, {"details", details}});
    };
#if CGPLAY_HAS_TLRENDER
    const auto wait = [](const std::function<bool()>& predicate, int timeout = 8000) {
        QElapsedTimer timer; timer.start();
        while (!predicate() && timer.elapsed() < timeout) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(5);
        }
        return predicate();
    };
    const auto opened = _runMediaOpenSmokeCheck(mediaPath);
    results.append(opened);
    auto* playback = _p->playbackCtrl.get();
    const auto cache = _p->cacheManager;
    if (!playback || !playback->player() || !cache) { add("Native cache ready", false); return results; }
    playback->pause(); playback->setMute(true);
    const QString secondPath = qEnvironmentVariable("CGPLAY_CACHE_SECOND_MEDIA");
    auto second = std::make_unique<PlaybackController>(cache, _p->ocioManager);
    second->openFile(secondPath);
    if (!wait([&] { return second->isValid() && second->player(); })) {
        add("Second cache player ready", false, {{"path", secondPath}}); return results;
    }
    second->pause(); second->setMute(true);
    playback->setInPoint(2); playback->setOutPoint(12);
    playback->seekToFrame(5);
    wait([&] { return playback->currentFrame() == 5; });
    const auto a = playback->player();
    auto b = second->player();
    const auto time = a->getCurrentTime();
    const auto inOut = a->getInOutRange();
    const auto compare = a->getCompare();
    const auto originalA = a->getCacheOptions(), originalB = b->getCacheOptions();
    const size_t originalGB = cache->ramCacheSizeGB();
    const size_t originalAhead = cache->readAheadFrames(), originalBehind = cache->readBehindFrames();
    struct Restore {
        std::function<void()> action;
        ~Restore() { action(); }
    } restore{[=] { cache->setRamCacheSizeGB(originalGB); cache->setReadAheadFrames(originalAhead); cache->setReadBehindFrames(originalBehind); }};
    add("Native cache media rates", std::abs(playback->fps()-30.0)<.001 && std::abs(second->fps()-24.0)<.001,
        {{"aFPS", playback->fps()}, {"bFPS", second->fps()}});
    cache->setRamCacheSizeGB(3); cache->setReadBehindFrames(60);
    const auto changedA = a->getCacheOptions(), changedB = b->getCacheOptions();
    add("Native cache RAM applies to both players", changedA.videoGB == 3.F && changedB.videoGB == 3.F,
        {{"aGB", changedA.videoGB}, {"bGB", changedB.videoGB}});
    add("Native cache uses each media rate", std::abs(changedA.readBehind-2.F)<.001F && std::abs(changedB.readBehind-2.5F)<.001F,
        {{"aSeconds", changedA.readBehind}, {"bSeconds", changedB.readBehind}});
    add("Native cache preserves audio budget", changedA.audioGB == originalA.audioGB && changedB.audioGB == originalB.audioGB);
    cache->setReadAheadFrames(4);
    add("Native request limit does not rebuild active players", playback->player() == a && second->player() == b);
    add("Native cache preserves playhead and in-out", a->getCurrentTime() == time && a->getInOutRange() == inOut && a->getCompare() == compare
        && playback->inPoint() == 2 && playback->outPoint() == 12);
    cache->setReadBehindFrames(0);
    add("Native cache zero behind", a->getCacheOptions().readBehind == 0.F && b->getCacheOptions().readBehind == 0.F);
    int explicitRequests = 0;
    QObject observation;
    QObject::connect(cache.get(), &CacheManager::playbackCacheClearRequested, &observation, [&] { ++explicitRequests; });
    cache->clear();
    add("Source/stat clear is isolated from native players", explicitRequests == 0);
    cache->clearPlaybackCaches();
    add("Explicit native clear request is emitted once", explicitRequests == 1 && playback->player() == a && second->player() == b);
    b.reset();
    second.reset();
    cache->clearPlaybackCaches();
    add("Closed controller disconnects cache callbacks", explicitRequests == 2 && playback->player() == a);
#else
    add("Native cache requires tlRender", false);
#endif
    return results;
}

} // namespace cgplay
