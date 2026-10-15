// CGPlay SessionManager.cpp
// v1.4 — .cgsession 工程文件序列化/反序列化

#include "SessionManager.h"

#include "common/core/ServiceLocator.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"
#include "app/Application.h"
#include "annotation/AnnotationManager.h"
#include "annotation/AnnotationStorage.h"
#include "session/api/ISessionContributor.h"
#include "playback/PlaybackController.h"
#include "playlist/PlaylistPanel.h"
#include "playlist/PlaylistModel.h"
#include "viewer/CompareToolbar.h"
#include "ocio/OcioManager.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QMessageBox>
#include <QFileDialog>
#include <QSettings>
#include <QStandardPaths>
#include <QApplication>
#include <QTimer>
#include <QStatusBar>
#include <QDebug>
#include <algorithm>

namespace cgplay {

namespace {

ISessionContributor* resolveSessionContributor()
{
    return ServiceLocator::getService<ISessionContributor>();
}

} // namespace

// ─── Constants ──────────────────────────────────────────────────────────────
static constexpr int    kSessionVersion = 1;
static const QString    kSessionExt     = ".cgsession";
static const QString    kRecoveryFile   = "_autosave.cgsession";

SessionManager::SessionManager(MainWindow* mainWindow,
                               std::shared_ptr<AnnotationManager> annoMgr,
                               std::shared_ptr<OcioManager>       ocioMgr,
                               QObject* parent)
    : QObject(parent)
    , _mainWindow(mainWindow)
    , _annoMgr(std::move(annoMgr))
    , _ocioMgr(std::move(ocioMgr))
{
    connect(_mainWindow, &MainWindow::mediaOpenRequested, this, [this](const QString&) {
        if (!_openingRestoreMedia) _cancelRestore();
    });
    connect(_mainWindow, &MainWindow::mediaOpenFailed, this,
        [this](const QString& path, const QString& error) {
            if (_pendingRestore && _pendingRestore->mediaPath == path) _failRestore(error);
        });
    connect(_mainWindow, &MainWindow::mediaPlayerOpening, this, [this](const QString& path) {
        if (!_pendingRestore) return;
        if (_pendingRestore->mediaPath != path) {
            _cancelRestore();
            return;
        }
        _pendingRestore->playerOpening = true;
    });
    if (auto* playback = _mainWindow->playbackController()) {
        connect(playback->signalProxy(), &PlaybackServiceSignals::fileOpened, this,
            [this](const QString& path) {
                if (!_pendingRestore || !_pendingRestore->playerOpening || _pendingRestore->mediaPath != path) return;
                const quint64 generation = _pendingRestore->generation;
                // Finish after the media-open stack has applied source defaults,
                // installed the viewer, and decided whether to auto-play.
                QTimer::singleShot(0, this, [this, generation] { _finishRestore(generation); });
            });
        connect(playback->signalProxy(), &PlaybackServiceSignals::fileOpenFailed, this,
            [this](const QString& path, const QString& error) {
                if (_pendingRestore && _pendingRestore->playerOpening && _pendingRestore->mediaPath == path) _failRestore(error);
            });
    }
    if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
        connect(this, &SessionManager::sessionLoaded, this, [eventBus](const QString& path) {
            eventBus->publish(SessionLoadedEvent{ path });
        });
    }
}

SessionManager::~SessionManager()
{
    disableAutoSave();
}

// ─── 保存 ──────────────────────────────────────────────────────────────────
bool SessionManager::save(const QString& path)
{
    if (_pendingRestore) return false;
    QString filePath = path.isEmpty() ? _sessionPath : path;
    if (filePath.isEmpty()) return saveAs();

    QJsonObject root = _serialize();

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(_mainWindow, tr("Save Failed"),
            tr("Could not write file:\n%1").arg(filePath));
        return false;
    }

    QJsonDocument doc(root);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    _sessionPath = filePath;
    _setDirty(false);
    _preserveRecovery = false;
    clearRecovery();  // 手动保存后清除恢复文件

    Q_EMIT sessionSaved(filePath);
    return true;
}

bool SessionManager::saveAs()
{
    QString path = QFileDialog::getSaveFileName(
        _mainWindow,
        tr("Save Project"),
        _sessionPath.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
              + "/untitled" + kSessionExt
            : _sessionPath,
        tr("CGPlay Project (*%1)").arg(kSessionExt));

    if (path.isEmpty()) return false;
    if (!path.endsWith(kSessionExt)) path += kSessionExt;
    return save(path);
}

// ─── 加载 ──────────────────────────────────────────────────────────────────
bool SessionManager::load(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(_mainWindow, tr("Load Failed"),
            tr("Could not read file:\n%1").arg(path));
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError) {
        QMessageBox::warning(_mainWindow, tr("Load Failed"),
            tr("JSON parse error:\n%1").arg(err.errorString()));
        return false;
    }

    if (!_beginRestore(doc.object(), path, false)) {
        QMessageBox::warning(_mainWindow, tr("Load Failed"),
            tr("The project file version is incompatible or the format is invalid."));
        return false;
    }

    return true;
}

// ─── 自动保存 ──────────────────────────────────────────────────────────────
void SessionManager::enableAutoSave(int intervalMs)
{
    if (_autoSaveTimer) return;
    _autoSaveTimer = new QTimer(this);
    _autoSaveTimer->setInterval(intervalMs);
    connect(_autoSaveTimer, &QTimer::timeout, this, [this] {
        if (_pendingRestore || _preserveRecovery) return;
        if (_dirty && hasSession()) {
            save();  // 保存到当前工程文件 → 同时清除恢复文件
        } else if (_dirty) {
            saveRecovery();  // 无工程文件但有未保存更改时保留恢复快照
        }
    });
    _autoSaveTimer->start();
}

void SessionManager::disableAutoSave()
{
    if (_autoSaveTimer) {
        _autoSaveTimer->stop();
        delete _autoSaveTimer;
        _autoSaveTimer = nullptr;
    }
}

// ─── 崩溃恢复 ──────────────────────────────────────────────────────────────
QString SessionManager::recoveryPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + "/" + kRecoveryFile;
}

void SessionManager::saveRecovery()
{
    if (_pendingRestore || _preserveRecovery) return;
    QJsonObject root = _serialize();

    const QString path = recoveryPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.close();
    }
}

bool SessionManager::hasRecoverySession() const
{
    return QFile::exists(recoveryPath());
}

bool SessionManager::loadRecovery()
{
    const QString path = recoveryPath();
    if (!QFile::exists(path)) return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    file.close();
    // A failed or interrupted recovery must survive both auto-save and the
    // window's unconditional clearRecovery() during shutdown.
    _preserveRecovery = true;
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
    return _beginRestore(doc.object(), {}, true);
}

void SessionManager::clearRecovery()
{
    if (_preserveRecovery || _pendingRestore) return;
    const QString path = recoveryPath();
    if (QFile::exists(path)) QFile::remove(path);
}

// ─── 新建会话 ──────────────────────────────────────────────────────────────
void SessionManager::newSession()
{
    _cancelRestore();
    _preserveRecovery = false;
    _sessionPath.clear();
    _setDirty(false);
    clearRecovery();  // 新建工程后清除旧恢复文件
}

// ─── 标记已修改 ────────────────────────────────────────────────────────────
void SessionManager::markDirty()
{
    _setDirty(true);
}

// ─── Private: 序列化 ───────────────────────────────────────────────────────
QJsonObject SessionManager::_serialize() const
{
    QJsonObject root;
    root["version"]         = kSessionVersion;
    root["cgplay_version"]  = "1.35";
    root["timestamp"]       = QDateTime::currentDateTime().toString(Qt::ISODate);

    // ── Media ────────────────────────────────────────────────────────────
    QJsonObject media;
    {
        auto* pb = _mainWindow->playbackController();
        if (pb) {
            media["current_path"]  = pb->currentPath();
            media["current_frame"] = pb->currentFrame();
        }
    }
    root["media"] = media;

    // ── Playlist ─────────────────────────────────────────────────────────
    {
        auto* pl = _mainWindow->playlistPanel();
        if (pl && pl->model()) {
            root["playlist"] = pl->model()->toJson();
        }
    }

    // ── Playback ─────────────────────────────────────────────────────────
    QJsonObject playback;
    {
        auto* pb = _mainWindow->playbackController();
        if (pb) {
            playback["in_point"]  = pb->hasInPoint()  ? pb->inPoint()  : -1;
            playback["out_point"] = pb->hasOutPoint() ? pb->outPoint() : -1;
        }
    }
    root["playback"] = playback;

    // ── Compare ──────────────────────────────────────────────────────────
    QJsonObject compare;
    auto* compareBar = _mainWindow->compareToolbar();
    if (compareBar) {
        compare["mode"]          = compareBar->compareMode();
        compare["wipe_center_x"] = compareBar->wipeCenterX();
        compare["wipe_center_y"] = compareBar->wipeCenterY();
        compare["wipe_rotation"] = compareBar->wipeRotation();
        compare["overlay"]       = compareBar->overlayAmount();
        compare["auto_clear_b"]  = compareBar->isAutoClearB();
    }
    root["compare"] = compare;

    // ── OCIO ─────────────────────────────────────────────────────────────
    QJsonObject ocio;
    if (_ocioMgr) {
        ocio["config_path"] = _ocioMgr->currentConfigPath();
        ocio["input"]       = _ocioMgr->currentInput();
        ocio["display"]     = _ocioMgr->currentDisplay();
        ocio["view"]        = _ocioMgr->currentView();
        ocio["exposure"]    = _ocioMgr->exposure();
        ocio["gamma"]       = _ocioMgr->gamma();
        ocio["enabled"]     = _ocioMgr->isEnabled();
    }
    root["ocio"] = ocio;

    // ── Annotations ──────────────────────────────────────────────────────
    if (auto* contributor = resolveSessionContributor()) {
        contributor->serializeInto(root);
    } else if (_annoMgr && _annoMgr->count() > 0) {
        // TODO(Phase13-remove): Transitional annotation session fallback until plugin session contribution is the only supported path.
        _annoMgr->serializeInto(root);
    }

    // ── Layout ───────────────────────────────────────────────────────────
    QJsonObject layout;
    layout["geometry_b64"]     = QString::fromLatin1(_mainWindow->saveGeometry().toBase64());
    layout["window_state_b64"] = QString::fromLatin1(_mainWindow->saveState().toBase64());
    root["layout"] = layout;

    return root;
}

bool SessionManager::_beginRestore(const QJsonObject& root, const QString& path, bool recovery)
{
    if (root["version"].toInt(-1) < 1) return false;
    _cancelRestore();
    const QString mediaPath = root["media"].toObject()["current_path"].toString();
    _pendingRestore = PendingRestore{root, mediaPath, path, ++_restoreGeneration, recovery};
    if (recovery) _preserveRecovery = true;
    // Install stored sequence FPS before openFile() chooses its override.
    auto* playlist = _mainWindow->playlistPanel();
    if (playlist && playlist->model() && root.contains("playlist")) {
        playlist->model()->fromJson(root["playlist"].toObject());
    }
    if (mediaPath.isEmpty()) {
        _finishRestore(_pendingRestore->generation);
    } else {
        _openingRestoreMedia = true;
        _mainWindow->openFile(mediaPath);
        _openingRestoreMedia = false;
    }
    return true; // Accepted; sessionLoaded is emitted only after media readiness.
}

void SessionManager::_cancelRestore()
{
    ++_restoreGeneration;
    _pendingRestore.reset();
}

void SessionManager::_failRestore(const QString& error)
{
    if (!_pendingRestore) return;
    const QString path = _pendingRestore->sessionPath;
    _cancelRestore();
    qWarning() << "[Session] Restore failed; recovery snapshot retained:" << error;
    if (_mainWindow->statusBar()) {
        _mainWindow->statusBar()->showMessage(tr("Session restore failed: %1").arg(error), 10000);
    }
    Q_EMIT sessionLoadFailed(path, error);
}

void SessionManager::_finishRestore(quint64 generation)
{
    if (!_pendingRestore || _pendingRestore->generation != generation) return;
    auto* playback = _mainWindow->playbackController();
    if (!_pendingRestore->mediaPath.isEmpty() && (!playback || !playback->isValid() ||
        playback->currentPath() != _pendingRestore->mediaPath)) {
        _failRestore(tr("The requested media is not ready."));
        return;
    }
    const PendingRestore restored = *_pendingRestore;
    if (!_deserialize(restored.root)) {
        _failRestore(tr("Invalid session data."));
        return;
    }
    if (!_pendingRestore || _pendingRestore->generation != generation) return;
    _pendingRestore.reset();
    _sessionPath = restored.sessionPath;
    _setDirty(restored.recovery);
    _preserveRecovery = false;
    if (restored.recovery) clearRecovery();
    Q_EMIT sessionLoaded(restored.sessionPath);
}

// ─── Private: apply state only after the requested media is ready ─────────
bool SessionManager::_deserialize(const QJsonObject& root)
{
    if (root["version"].toInt(-1) < 1) return false;
    const int currentFrame = root["media"].toObject()["current_frame"].toInt(0);

    // ── Playback ─────────────────────────────────────────────────────────
    auto* pb = _mainWindow->playbackController();
    if (pb && !root["media"].toObject()["current_path"].toString().isEmpty()) {
        pb->pause();
        pb->clearInOutPoints();
        QJsonObject pbo = root["playback"].toObject();
        int inPt  = pbo["in_point"].toInt(-1);
        int outPt = pbo["out_point"].toInt(-1);
        if (inPt >= 0)  pb->setInPoint(inPt);
        if (outPt >= 0) pb->setOutPoint(outPt);
        pb->seekToFrame(std::clamp(currentFrame, 0, std::max(0, pb->totalFrames() - 1)));
    }

    // ── Compare ──────────────────────────────────────────────────────────
    auto* compareBar = _mainWindow->compareToolbar();
    if (compareBar && root.contains("compare")) {
        QJsonObject co = root["compare"].toObject();
        compareBar->setCompareMode(co["mode"].toInt(0));
        compareBar->setWipeCenter(
            static_cast<float>(co["wipe_center_x"].toDouble(0.5)),
            static_cast<float>(co["wipe_center_y"].toDouble(0.5)));
        compareBar->setWipeRotation(static_cast<float>(co["wipe_rotation"].toDouble(0.0)));
        compareBar->setOverlay(static_cast<float>(co["overlay"].toDouble(0.5)));
        compareBar->setAutoClearB(co["auto_clear_b"].toBool(true));
    }

    // ── OCIO ─────────────────────────────────────────────────────────────
    if (_ocioMgr && root.contains("ocio")) {
        QJsonObject oo = root["ocio"].toObject();
        QString configPath = oo["config_path"].toString();
        if (!configPath.isEmpty() && configPath.startsWith("ocio://")) {
            _ocioMgr->loadBuiltinConfig("ACES");   // 恢复为内置 ACES 配置
        } else if (!configPath.isEmpty()) {
            _ocioMgr->loadConfig(configPath);
        }
        if (!oo["input"].toString().isEmpty())
            _ocioMgr->setInput(oo["input"].toString());
        if (!oo["display"].toString().isEmpty())
            _ocioMgr->setDisplay(oo["display"].toString());
        if (!oo["view"].toString().isEmpty())
            _ocioMgr->setView(oo["view"].toString());
        _ocioMgr->setExposure(static_cast<float>(oo["exposure"].toDouble(0.0)));
        _ocioMgr->setGamma(static_cast<float>(oo["gamma"].toDouble(1.0)));
        _ocioMgr->setEnabled(oo["enabled"].toBool(true));
    }

    // ── Annotations ──────────────────────────────────────────────────────
    if (auto* contributor = resolveSessionContributor()) {
        contributor->deserializeFrom(root);
    } else if (_annoMgr && root.contains(QStringLiteral("annotations"))) {
        // TODO(Phase13-remove): Transitional annotation session fallback until plugin session contribution is the only supported path.
        _annoMgr->deserializeFrom(root);
    }

    // ── Layout ───────────────────────────────────────────────────────────
    if (root.contains("layout")) {
        QJsonObject lo = root["layout"].toObject();
        QString geomB64 = lo["geometry_b64"].toString();
        QString stateB64 = lo["window_state_b64"].toString();
        if (!geomB64.isEmpty()) {
            _mainWindow->restoreGeometry(
                QByteArray::fromBase64(geomB64.toLatin1()));
        }
        if (!stateB64.isEmpty()) {
            _mainWindow->restoreState(
                QByteArray::fromBase64(stateB64.toLatin1()));
        }
    }

    return true;
}

// ─── Private: 标记已修改 ──────────────────────────────────────────────────
void SessionManager::_setDirty(bool dirty)
{
    if (_dirty != dirty) {
        _dirty = dirty;
        Q_EMIT dirtyChanged(_dirty);
    }
}

} // namespace cgplay
