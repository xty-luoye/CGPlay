#include "ApplicationInternal.h"
#include "session/SessionManager.h"
#include <QCryptographicHash>
#include <QEventLoop>
#include <QSaveFile>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace cgplay {
QJsonArray MainWindow::_runSessionRestoreSmokeChecks(const QString& mediaPath)
{
    QJsonArray results;
    const QString evidence = qEnvironmentVariable("CGPLAY_SMOKE_EVIDENCE_DIR");
    const QString other = qEnvironmentVariable("CGPLAY_SMOKE_OTHER_VIDEO");
    const QString exr = qEnvironmentVariable("CGPLAY_SMOKE_EXR");
    const auto add = [&](const QString& name, bool passed, QJsonObject details = {}) {
        int visible = 0;
#ifdef Q_OS_WIN
        EnumWindows([](HWND window, LPARAM data) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(data);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&visible));
#endif
        const bool background = qApp->property("cgplay.automationBackground").toBool();
        details.insert("background", background);
        details.insert("nativeVisibleWindows", visible);
        results.append(QJsonObject{{"name", name}, {"status", passed && background && visible == 0 ? "PASS" : "FAIL"},
            {"message", "Real session restoration with asynchronous media readiness"}, {"details", details}});
    };
    if (evidence.isEmpty() || !QFileInfo::exists(mediaPath) || !QFileInfo::exists(other) || !QFileInfo::exists(exr)) {
        add("Session fixture", false);
        return results;
    }
    const auto settle = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    const auto waitFor = [&](const std::function<bool()>& ready) {
        QElapsedTimer timer;
        timer.start();
        while (!ready() && timer.elapsed() < 15000) settle(20);
        return ready();
    };
    // Use the production restore implementation and real player. Only the
    // snapshot destination is overridden so tests cannot touch user recovery.
    class TestSession final : public SessionManager {
    public:
        TestSession(MainWindow* window, std::shared_ptr<AnnotationManager> annotations,
            std::shared_ptr<OcioManager> ocio, QString path)
            : SessionManager(window, std::move(annotations), std::move(ocio)), _path(std::move(path)) {}
        QString recoveryPath() const override { return _path; }
    private:
        QString _path;
    } session(this, _annoMgr, _p->ocioManager, QDir(evidence).filePath("_autosave.cgsession"));
    auto* playback = _p->playbackCtrl.get();
    int loaded = 0;
    int failed = 0;
    QObject receiver;
    connect(&session, &SessionManager::sessionLoaded, &receiver, [&](const QString&) { ++loaded; });
    connect(&session, &SessionManager::sessionLoadFailed, &receiver, [&](const QString&, const QString&) { ++failed; });
    const auto snapshot = [](const QString& path, int frame, int in, int out) {
        return QJsonObject{{"version", 1}, {"media", QJsonObject{{"current_path", path}, {"current_frame", frame}}},
            {"playback", QJsonObject{{"in_point", in}, {"out_point", out}}}};
    };
    const auto write = [&](const QJsonObject& data, const QString& path = QString()) {
        QSaveFile file(path.isEmpty() ? session.recoveryPath() : path);
        if (!file.open(QIODevice::WriteOnly)) return false;
        const auto bytes = QJsonDocument(data).toJson();
        return file.write(bytes) == bytes.size() && file.commit();
    };
    const auto digest = [&] {
        QFile file(session.recoveryPath());
        if (!file.open(QIODevice::ReadOnly)) return QByteArray{};
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    };
    const auto state = [&] {
        return QJsonObject{{"path", playback->currentPath()}, {"in", playback->inPoint()},
            {"out", playback->outPoint()}, {"frame", playback->currentFrame()}, {"loaded", loaded}, {"failed", failed}};
    };
    const auto restore = [&](const QJsonObject& root) {
        if (!write(root) || !session.loadRecovery()) return false;
        return waitFor([&] { return !session.isRestoring(); });
    };

    _closeCurrentMedia();
    const auto saved = snapshot(mediaPath, 48, 24, 72);
    const bool accepted = write(saved) && session.loadRecovery();
    add("Session recovery waits for media readiness", accepted && session.isRestoring() &&
        QFileInfo::exists(session.recoveryPath()) && loaded == 0);
    const bool completed = waitFor([&] { return !session.isRestoring(); });
    settle(160);
    add("Session A-B and frame restored after open", completed && loaded == 1 && playback->inPoint() == 24 &&
        playback->outPoint() == 72 && playback->currentFrame() == 48 && playback->playbackState() == 0, state());
    add("Session successful recovery removes snapshot", !QFileInfo::exists(session.recoveryPath()) && session.hasUnsavedChanges());
    add("Session restored range screenshot", _p->timeline->grab().save(QDir(evidence).filePath("restored_range.png")));

    const int beforeStaleReady = loaded;
    const int beforeStaleFailure = failed;
    const bool staleAccepted = write(saved) && session.loadRecovery();
    // Model the completion signals of an older same-path transcode while
    // the current request is still probing; keep the real existing player.
    Q_EMIT playback->signalProxy()->fileOpened(mediaPath);
    Q_EMIT playback->signalProxy()->fileOpenFailed(mediaPath, QStringLiteral("older request failed"));
    add("Session ignores old same-path ready and failure during probe", staleAccepted && session.isRestoring() &&
        loaded == beforeStaleReady && failed == beforeStaleFailure && QFileInfo::exists(session.recoveryPath()));
    const bool freshReady = waitFor([&] { return !session.isRestoring(); });
    settle(160);
    add("Session accepts current readiness after stale signals", freshReady && loaded == beforeStaleReady + 1 &&
        playback->inPoint() == 24 && playback->outPoint() == 72 && playback->currentFrame() == 48, state());

    // A new same-path request must cancel an older restore, not merely match
    // its filename when either asynchronous open finishes.
    const int beforeSamePath = loaded;
    const quint64 beforeSameGeneration = _p->mediaGenerationId;
    const bool sameAccepted = write(saved) && session.loadRecovery();
    openFile(mediaPath);
    const bool sameOpened = waitFor([&] { return _p->mediaGenerationId > beforeSameGeneration && playback->isValid(); });
    settle(180);
    add("Session same-path reopen cancels stale restoration", sameAccepted && sameOpened && !session.isRestoring() &&
        loaded == beforeSamePath && !playback->hasInPoint() && !playback->hasOutPoint(), state());
    add("Session canceled recovery keeps snapshot", QFileInfo::exists(session.recoveryPath()));

    // Cancel after fileOpened has already queued restore completion, before
    // that completion runs. This exercises the generation guard itself.
    bool switchOnReady = true;
    QObject switchReceiver;
    connect(playback->signalProxy(), &PlaybackServiceSignals::fileOpened, &switchReceiver, [&](const QString& path) {
        if (switchOnReady && path == mediaPath) {
            switchOnReady = false;
            openFile(other);
        }
    });
    const int beforeSwitch = loaded;
    const bool switchAccepted = write(saved) && session.loadRecovery();
    const bool otherOpened = waitFor([&] { return playback->currentPath() == other && playback->isValid() && !session.isRestoring(); });
    settle(180);
    disconnect(playback->signalProxy(), nullptr, &switchReceiver, nullptr);
    add("Session queued completion cannot affect replacement", switchAccepted && otherOpened && loaded == beforeSwitch &&
        !playback->hasInPoint() && !playback->hasOutPoint(), state());

    const bool closeAccepted = write(saved) && session.loadRecovery();
    _closeCurrentMedia();
    settle(350);
    const auto preservedAfterClose = digest();
    session.clearRecovery(); // Same API called by MainWindow destruction.
    session.saveRecovery();
    add("Session close cancels pending media and preserves recovery", closeAccepted && !session.isRestoring() &&
        !playback->isValid() && !preservedAfterClose.isEmpty() && digest() == preservedAfterClose, state());

    const QString missing = QDir(evidence).filePath("missing_media.mp4");
    const auto missingSaved = snapshot(missing, 48, 24, 72);
    const bool missingWritten = write(missingSaved);
    const auto missingDigest = digest();
    const int beforeFailure = failed;
    const int beforeFailedLoad = loaded;
    const bool missingAccepted = missingWritten && session.loadRecovery();
    const bool failedReady = waitFor([&] { return !session.isRestoring(); });
    session.saveRecovery();
    session.clearRecovery();
    add("Session load failure ends pending state", missingAccepted && failedReady && failed == beforeFailure + 1 &&
        loaded == beforeFailedLoad, state());
    add("Session failed recovery cannot be overwritten or removed", !missingDigest.isEmpty() && digest() == missingDigest);

    const bool retried = restore(saved);
    settle(160);
    add("Session failure can be retried successfully", retried && playback->inPoint() == 24 && playback->outPoint() == 72 &&
        playback->currentFrame() == 48 && !QFileInfo::exists(session.recoveryPath()), state());
    const auto noPoints = snapshot(mediaPath, 0, -1, -1);
    const bool cleared = restore(noPoints);
    settle(160);
    add("Session zero frame and empty A-B restored", cleared && playback->currentFrame() == 0 &&
        !playback->hasInPoint() && !playback->hasOutPoint(), state());

    const QString project = QDir(evidence).filePath("saved.cgsession");
    const int beforeProject = loaded;
    const bool projectAccepted = write(saved, project) && session.load(project);
    add("Session normal load signals only after ready", projectAccepted && session.isRestoring() && loaded == beforeProject);
    const bool projectReady = waitFor([&] { return !session.isRestoring(); });
    settle(160);
    add("Session saved project uses readiness contract", projectReady && loaded == beforeProject + 1 &&
        session.sessionPath() == project && !session.hasUnsavedChanges() && playback->inPoint() == 24 &&
        playback->outPoint() == 72 && playback->currentFrame() == 48, state());

    // EXR defaults are rebound on a delayed timer. Restored view selections
    // must remain authoritative after that timer's deadline.
    openFile(exr);
    const bool exrReady = waitFor([&] { return playback->isValid() && playback->currentPath() == exr; });
    settle(400);
    const QString display = _p->ocioManager->currentDisplay();
    QString alternateView;
    for (const QString& view : _p->ocioManager->availableViews(display)) {
        if (view != _p->ocioManager->currentView()) { alternateView = view; break; }
    }
    QJsonObject exrSaved = snapshot(exr, 12, 4, 20);
    exrSaved.insert("ocio", QJsonObject{{"config_path", _p->ocioManager->currentConfigPath()},
        {"input", _p->ocioManager->currentInput()}, {"display", display}, {"view", alternateView},
        {"enabled", true}, {"exposure", 0.75}, {"gamma", 1.2}});
    const bool exrRestored = exrReady && !alternateView.isEmpty() && restore(exrSaved);
    settle(450);
    add("Session EXR range uses relative frames", exrRestored && playback->inPoint() == 4 && playback->outPoint() == 20 &&
        playback->currentFrame() == 12, state());
    add("Session restored EXR view survives delayed defaults", exrRestored && _p->ocioManager->currentView() == alternateView &&
        _p->ocioManager->isEnabled() && std::abs(_p->ocioManager->exposure() - .75f) < .001f &&
        std::abs(_p->ocioManager->gamma() - 1.2f) < .001f,
        {{"expectedView", alternateView}, {"actualView", _p->ocioManager->currentView()}});

    const quint64 beforeOrdinary = _p->mediaGenerationId;
    openFile(other);
    const bool ordinaryReady = waitFor([&] { return _p->mediaGenerationId > beforeOrdinary && playback->isValid(); });
    add("Session ordinary media replacement clears range", ordinaryReady && !playback->hasInPoint() && !playback->hasOutPoint());
    _closeCurrentMedia();
    session.newSession();
    return results;
}
} // namespace cgplay
