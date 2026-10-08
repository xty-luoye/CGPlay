// CGPlay PlaybackController.cpp
// Uses tl::Player directly (like DJV) — no Qt wrappers.

#include "PlaybackController.h"
#include "PlaybackStats.h"
#include "common/core/ServiceLocator.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"
#include "common/jobs/JobSystem.h"
#include "cache/CacheManager.h"
#include "cache/ReadAheadCache.h"
#include "component/ComponentManager.h"
#include "core/ThreadPool.h"
#include "ocio/OcioManager.h"

#include <QTimer>
#include <QTimerEvent>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QDebug>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <array>

#if CGPLAY_HAS_TLRENDER
  #include <tlRender/Timeline/Timeline.h>
  #include <tlRender/Timeline/Player.h>
  #include <tlRender/Timeline/PlayerOptions.h>
  #include <tlRender/Timeline/System.h>
  #include <tlRender/Timeline/Init.h>
#endif

#if CGPLAY_HAS_FTK
  #include <ftk/Core/Context.h>
  #include <ftk/Core/Observable.h>
  #include <ftk/UI/Style.h>
#endif

namespace cgplay {

namespace {

constexpr int kSystemTickIntervalActiveMs = 2;
constexpr int kSystemTickIntervalPausedMs = 16;
constexpr int kSystemTickIntervalIdleMs = 100;

constexpr int kUiTickIntervalActiveMs = 16;
constexpr int kUiTickIntervalPausedMs = 50;
constexpr int kUiTickIntervalIdleMs = 100;

bool isVpFamilyCodec(const QString& value)
{
    static const QStringList codecs{
        QStringLiteral("vp3"), QStringLiteral("vp4"), QStringLiteral("vp5"),
        QStringLiteral("vp6"), QStringLiteral("vp6a"), QStringLiteral("vp6f"),
        QStringLiteral("vp7"), QStringLiteral("vp8"), QStringLiteral("vp9")
    };
    return codecs.contains(value.trimmed().toLower());
}

// AV1 streams are decoded by FFmpeg but some GPU/driver combinations expose
// an incomplete hardware pixel path. Force tlRender's stable software YUV
// conversion for AV1 so MP4/WebM and 8/10-bit sources share one fallback.
bool isAv1Codec(const QString& value)
{
    const QString codec = value.trimmed().toLower();
    return codec == QStringLiteral("av1") || codec == QStringLiteral("av01");
}

#if CGPLAY_HAS_TLRENDER
int playbackStateToInt(tl::Playback playback)
{
    if (playback == tl::Playback::Forward) {
        return 1;
    }
    if (playback == tl::Playback::Reverse) {
        return 2;
    }
    return 0;
}
#endif

QString locateBundledTool(const QString& name)
{
    if (name.compare(QStringLiteral("ffmpeg.exe"), Qt::CaseInsensitive) == 0) {
        const QString componentPath = ComponentManager::instance().componentExecutablePath(
            QStringLiteral("ffmpeg"),
            QStringLiteral("ffmpeg.exe"));
        if (!componentPath.isEmpty()) {
            return componentPath;
        }
    }
    if (name.compare(QStringLiteral("ffprobe.exe"), Qt::CaseInsensitive) == 0) {
        const QString componentPath = ComponentManager::instance().componentExecutablePath(
            QStringLiteral("ffmpeg"),
            QStringLiteral("ffprobe.exe"));
        if (!componentPath.isEmpty()) {
            return componentPath;
        }
    }

    const QString appSibling = QCoreApplication::applicationDirPath() + QLatin1Char('/') + name;
    if (QFileInfo::exists(appSibling)) {
        return appSibling;
    }
    return QStandardPaths::findExecutable(name);
}

bool hasUsableVideoStream(JobContext& context, const QString& ffprobe, const QString& path)
{
    if (ffprobe.isEmpty() || path.isEmpty() || !QFileInfo::exists(path)) {
        return false;
    }

    QProcess proc;
    proc.start(ffprobe, {
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-select_streams"), QStringLiteral("v:0"),
        QStringLiteral("-show_entries"),
        QStringLiteral("stream=width,height,avg_frame_rate,nb_frames,duration"),
        QStringLiteral("-of"), QStringLiteral("json"),
        path
    }, QIODevice::ReadOnly);

    const ProcessOutcome process = context.waitForProcess(proc, 25, 8000);
    if (!process.succeeded()) {
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(process.standardOutput);
    const QJsonArray streams = doc.object().value(QStringLiteral("streams")).toArray();
    if (streams.isEmpty()) {
        return false;
    }

    const QJsonObject stream = streams.first().toObject();
    return stream.value(QStringLiteral("width")).toInt() > 0 &&
           stream.value(QStringLiteral("height")).toInt() > 0;
}

bool isPngEncodedMov(JobContext& context, const QString& path)
{
    const QFileInfo fileInfo(path);
    if (fileInfo.suffix().compare(QStringLiteral("mov"), Qt::CaseInsensitive) != 0) {
        return false;
    }

    const QString ffprobe = locateBundledTool(QStringLiteral("ffprobe.exe"));
    if (ffprobe.isEmpty()) {
        return false;
    }

    QProcess proc;
    proc.start(ffprobe, {
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-select_streams"), QStringLiteral("v:0"),
        QStringLiteral("-show_entries"),
        QStringLiteral("stream=codec_name,pix_fmt"),
        QStringLiteral("-of"), QStringLiteral("json"),
        path
    }, QIODevice::ReadOnly);

    const ProcessOutcome process = context.waitForProcess(proc, 25, 8000);
    if (!process.succeeded()) {
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(process.standardOutput);
    const QJsonArray streams = doc.object().value(QStringLiteral("streams")).toArray();
    if (streams.isEmpty()) {
        return false;
    }

    const QJsonObject stream = streams.first().toObject();
    const QString codec = stream.value(QStringLiteral("codec_name")).toString().toLower();
    // PNG is a valid FFmpeg video codec inside MOV. It can be RGB, RGB48,
    // or RGBA; restricting this check to alpha formats misses common Nuke
    // renders such as rgb48be and leaves tlRender with an invalid time range.
    return codec == QStringLiteral("png");
}

QString pngMovCachePath(const QString& path)
{
    const QFileInfo fileInfo(path);
    const QString key = QStringLiteral("%1|%2|%3")
        .arg(fileInfo.absoluteFilePath())
        .arg(fileInfo.size())
        .arg(fileInfo.lastModified().toMSecsSinceEpoch());
    const QByteArray digest = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex();

    QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (root.isEmpty()) {
        root = QDir::tempPath() + QStringLiteral("/CGPlay");
    }
    QDir dir(root);
    dir.mkpath(QStringLiteral("transcode"));
    dir.cd(QStringLiteral("transcode"));
    return dir.filePath(QStringLiteral("png_rgba_%1_prores4444.mov").arg(QString::fromLatin1(digest.left(16))));
}

QString uniqueTranscodeTempPath(const QString& cachePath, const QString& suffix)
{
    // A canceled JobHandle does not wait for the worker to finish.  Keep each
    // worker's intermediate file private so a late cleanup cannot remove a
    // newer transcode's output.
    const QString token = QUuid::createUuid().toString(QUuid::Id128);
    return cachePath + QLatin1Char('.') + token + suffix;
}

JobOutcome ensurePngMovCache(JobContext& context, const QString& path, bool knownPngMov)
{
    context.reportProgress(5, QStringLiteral("Inspecting media"));
    if (context.shouldStop()) {
        return context.isCancellationRequested() ? JobOutcome::canceled() : JobOutcome::timedOut();
    }
    if (!knownPngMov && !isPngEncodedMov(context, path)) {
        return context.shouldStop()
            ? (context.isCancellationRequested()
                ? JobOutcome::canceled()
                : JobOutcome::timedOut())
            : JobOutcome::success(QString());
    }

    const QString ffmpeg = locateBundledTool(QStringLiteral("ffmpeg.exe"));
    const QString ffprobe = locateBundledTool(QStringLiteral("ffprobe.exe"));
    if (ffmpeg.isEmpty() || ffprobe.isEmpty()) {
        qWarning() << "[PlaybackController] Cannot build PNG MOV cache: ffmpeg or ffprobe not found";
        return JobOutcome::failure(
            QStringLiteral("tool-not-found"),
            QStringLiteral("ffmpeg or ffprobe not found"));
    }

    const QString cachePath = pngMovCachePath(path);
    if (hasUsableVideoStream(context, ffprobe, cachePath)) {
        qInfo() << "[PlaybackController] Using cached PNG MOV transcode:" << cachePath;
        return JobOutcome::success(cachePath);
    }

    const QString tempPath = uniqueTranscodeTempPath(cachePath, QStringLiteral(".tmp.mov"));
    qInfo() << "[PlaybackController] Transcoding PNG MOV for playback:" << path << "->" << cachePath;
    context.reportProgress(10, QStringLiteral("Transcoding media"));

    QProcess proc;
    proc.start(ffmpeg, {
        QStringLiteral("-y"),
        QStringLiteral("-hide_banner"),
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-i"), path,
        QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-map"), QStringLiteral("0:a?"),
        QStringLiteral("-c:v"), QStringLiteral("prores_ks"),
        QStringLiteral("-profile:v"), QStringLiteral("4"),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuva444p10le"),
        QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"),
        tempPath
    }, QIODevice::ReadOnly);

    const ProcessOutcome process = context.waitForProcess(proc);
    if (!process.succeeded()) {
        qWarning() << "[PlaybackController] PNG MOV transcode failed:"
                   << QString::fromUtf8(process.standardError).trimmed();
        QFile::remove(tempPath);
        if (process.state == JobState::Canceled) {
            return JobOutcome::canceled(QStringLiteral("PNG MOV transcode canceled"));
        }
        if (process.state == JobState::TimedOut) {
            return JobOutcome::timedOut(QStringLiteral("PNG/RGBA MOV transcode timed out"));
        }
        return JobOutcome::failure(
            QStringLiteral("transcode-failed"),
            QString::fromUtf8(process.standardError).trimmed());
    }

    context.reportProgress(95, QStringLiteral("Validating media cache"));
    if (!hasUsableVideoStream(context, ffprobe, tempPath)) {
        qWarning() << "[PlaybackController] PNG MOV transcode produced an unusable cache:" << tempPath;
        QFile::remove(tempPath);
        return JobOutcome::failure(
            QStringLiteral("invalid-cache"),
            QStringLiteral("Transcode produced an unusable cache"));
    }

    QFile::remove(cachePath);
    if (!QFile::rename(tempPath, cachePath)) {
        qWarning() << "[PlaybackController] Cannot move PNG/RGBA MOV cache into place:" << cachePath;
        QFile::remove(tempPath);
        return JobOutcome::failure(
            QStringLiteral("cache-commit-failed"),
            QStringLiteral("Cannot move transcode cache into place"));
    }

    context.reportProgress(100, QStringLiteral("Media cache ready"));
    return JobOutcome::success(cachePath);
}

QString av1CachePath(const QString& path)
{
    const QFileInfo fileInfo(path);
    const QString key = QStringLiteral("av1|%1|%2|%3")
        .arg(fileInfo.absoluteFilePath())
        .arg(fileInfo.size())
        .arg(fileInfo.lastModified().toMSecsSinceEpoch());
    const QByteArray digest = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex();
    QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (root.isEmpty()) root = QDir::tempPath() + QStringLiteral("/CGPlay");
    QDir dir(root);
    dir.mkpath(QStringLiteral("transcode"));
    dir.cd(QStringLiteral("transcode"));
    return dir.filePath(QStringLiteral("av1_%1_h264.mp4").arg(QString::fromLatin1(digest.left(16))));
}

JobOutcome ensureAv1Cache(JobContext& context, const QString& path)
{
    const QString ffmpeg = locateBundledTool(QStringLiteral("ffmpeg.exe"));
    const QString ffprobe = locateBundledTool(QStringLiteral("ffprobe.exe"));
    if (ffmpeg.isEmpty() || ffprobe.isEmpty()) {
        return JobOutcome::failure(QStringLiteral("tool-not-found"), QStringLiteral("ffmpeg or ffprobe not found"));
    }
    const QString cachePath = av1CachePath(path);
    if (hasUsableVideoStream(context, ffprobe, cachePath)) return JobOutcome::success(cachePath);
    const QString tempPath = uniqueTranscodeTempPath(cachePath, QStringLiteral(".tmp.mp4"));
    context.reportProgress(10, QStringLiteral("Preparing AV1 playback"));
    QProcess proc;
    proc.start(ffmpeg, {
        QStringLiteral("-y"), QStringLiteral("-hide_banner"), QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-i"), path, QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-map"), QStringLiteral("0:a?"), QStringLiteral("-c:v"), QStringLiteral("libx264"),
        QStringLiteral("-preset"), QStringLiteral("ultrafast"), QStringLiteral("-crf"), QStringLiteral("20"),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-movflags"), QStringLiteral("+faststart"),
        QStringLiteral("-c:a"), QStringLiteral("aac"), QStringLiteral("-b:a"), QStringLiteral("192k"), tempPath
    }, QIODevice::ReadOnly);
    const ProcessOutcome process = context.waitForProcess(proc, 30 * 60 * 1000);
    if (!process.succeeded()) {
        QFile::remove(tempPath);
        return process.state == JobState::Canceled ? JobOutcome::canceled(QStringLiteral("AV1 transcode canceled")) :
            JobOutcome::failure(QStringLiteral("transcode-failed"), QString::fromUtf8(process.standardError).trimmed());
    }
    if (!hasUsableVideoStream(context, ffprobe, tempPath)) {
        QFile::remove(tempPath);
        return JobOutcome::failure(QStringLiteral("invalid-cache"), QStringLiteral("AV1 transcode produced unusable media"));
    }
    QFile::remove(cachePath);
    if (!QFile::rename(tempPath, cachePath)) {
        QFile::remove(tempPath);
        return JobOutcome::failure(QStringLiteral("cache-commit-failed"), QStringLiteral("Cannot commit AV1 cache"));
    }
    context.reportProgress(100, QStringLiteral("AV1 playback ready"));
    return JobOutcome::success(cachePath);
}

} // namespace

// ─── Private ─────────────────────────────────────────────────────────────────
struct PlaybackController::Private
{
    std::shared_ptr<CacheManager> cache;
    std::shared_ptr<OcioManager>  ocio;
    PlaybackStats* stats = nullptr;
    PlaybackServiceSignals* signalProxy = nullptr;

#if CGPLAY_HAS_TLRENDER
    std::shared_ptr<tl::Player> player;
    std::shared_ptr<ftk::Observer<tl::PlayerCacheInfo> > cacheInfoObserver;
#endif

#if CGPLAY_HAS_FTK
    std::shared_ptr<ftk::Context> context;
    std::shared_ptr<ftk::Style>   style;
#endif

    QString currentPath;
    QPointer<JobHandle> transcodeJob;
    quint64 openGeneration = 0;

    // System tick for tlRender player driving
    int     systemTickId = 0;
    int     systemTickIntervalMs = 0;

    // Fallback timer for frame updates when tlRender is not available
    QTimer* tickTimer = nullptr;
    int     uiTickIntervalMs = 0;
    int     frameStub = 0;
    int     totalStub = 1;
    double  fpsStub   = 24.0;
    int     stateStub = 0; // 0=stop, 1=fwd, 2=rev

    // In/Out points (-1 = not set)
    int     inPoint  = -1;
    int     outPoint = -1;

    // v1.1 性能层
    bool    readAheadEnabled = true;
    int     lastReadAheadFrame = -1;
    int     lastReadAheadTotal = -1;

    float   volume = 1.0f;
    bool    muted = false;
};

// ─── Constructor / Destructor ────────────────────────────────────────────────
PlaybackController::PlaybackController(
    std::shared_ptr<CacheManager> cache,
    std::shared_ptr<OcioManager>  ocio,
    QObject* parent)
    : QObject(parent)
    , _p(std::make_unique<Private>())
{
    _p->cache = std::move(cache);
    _p->ocio  = std::move(ocio);
    _p->stats = new PlaybackStats(this);
    _p->signalProxy = new PlaybackServiceSignals(this);

    connect(this, &PlaybackController::fileOpened, _p->signalProxy, &PlaybackServiceSignals::fileOpened);
    connect(this, &PlaybackController::fileClosed, _p->signalProxy, &PlaybackServiceSignals::fileClosed);
    connect(this, &PlaybackController::currentFrameChanged, _p->signalProxy, &PlaybackServiceSignals::currentFrameChanged);
    connect(this, &PlaybackController::fpsChanged, _p->signalProxy, &PlaybackServiceSignals::fpsChanged);
    connect(this, &PlaybackController::cacheUsageChanged, _p->signalProxy, &PlaybackServiceSignals::cacheUsageChanged);
    connect(this, &PlaybackController::playbackStateChanged, _p->signalProxy, &PlaybackServiceSignals::playbackStateChanged);
    connect(this, &PlaybackController::inOutPointsChanged, _p->signalProxy, &PlaybackServiceSignals::inOutPointsChanged);
    connect(this, &PlaybackController::volumeChanged, _p->signalProxy, &PlaybackServiceSignals::volumeChanged);
    connect(this, &PlaybackController::muteChanged, _p->signalProxy, &PlaybackServiceSignals::muteChanged);
    connect(this, &PlaybackController::channelMuteChanged, _p->signalProxy, &PlaybackServiceSignals::channelMuteChanged);
    connect(this, &PlaybackController::audioDeviceChanged, _p->signalProxy, &PlaybackServiceSignals::audioDeviceChanged);
    connect(this, &PlaybackController::audioOffsetChanged, _p->signalProxy, &PlaybackServiceSignals::audioOffsetChanged);
#if CGPLAY_HAS_TLRENDER
    connect(this, &PlaybackController::playerReady, _p->signalProxy, &PlaybackServiceSignals::playerReady);
#endif

    if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
        connect(this, &PlaybackController::fileOpened, this, [this, eventBus](const QString& path) {
            eventBus->publish(MediaOpenedEvent{ path, totalFrames(), fps() });
        });
        connect(this, &PlaybackController::playbackStateChanged, this, [eventBus](int state) {
            if (state == 0) {
                eventBus->publish(PlaybackStoppedEvent{});
            } else {
                eventBus->publish(PlaybackStartedEvent{});
            }
        });
    }

#if CGPLAY_HAS_FTK
    // Create the feather-tk context (required by tlRender)
    _p->context = ftk::Context::create();
    _p->style   = ftk::Style::create(_p->context);
#endif

#if CGPLAY_HAS_TLRENDER
    // Initialize tlRender (registers IO plugins, audio, GL, OTIO types, System)
    tl::init(_p->context);
#endif

    // v1.1: 确保 ThreadPool 已初始化
    ThreadPool::instance();

    // Tick timer for UI refresh
    _p->tickTimer = new QTimer(this);
    connect(_p->tickTimer, &QTimer::timeout, this, [this] { _tickTimer(); });
    _updateTimerCadence(true);
}

PlaybackController::~PlaybackController()
{
    // QObject unregisters basic timers during destruction.  Calling
    // killTimer() after the event dispatcher has already torn them down emits
    // a spurious "timer id is not valid" warning in capture/smoke shutdown.
    _p->systemTickId = 0;
    if (_p->transcodeJob) {
        _p->transcodeJob->cancel();
    }
    _destroyPlayer();
}

PlaybackServiceSignals* PlaybackController::signalProxy() const
{
    return _p->signalProxy;
}

// ─── System Tick ──────────────────────────────────────────────────────────────
void PlaybackController::timerEvent(QTimerEvent* event)
{
    if (event->timerId() == _p->systemTickId) {
        QObject::timerEvent(event);
#if CGPLAY_HAS_TLRENDER
        if (_p->context && _p->player) {
            auto system = _p->context->getSystem<tl::System>();
            if (system) system->tick();
        }
#endif
    } else {
        QObject::timerEvent(event);
    }
}

void PlaybackController::_restartSystemTickTimer(int intervalMs)
{
    intervalMs = std::max(1, intervalMs);
    if (_p->systemTickId && _p->systemTickIntervalMs == intervalMs) {
        return;
    }
    if (_p->systemTickId) {
        killTimer(_p->systemTickId);
        _p->systemTickId = 0;
    }
    _p->systemTickIntervalMs = intervalMs;
    const Qt::TimerType timerType =
        intervalMs <= kSystemTickIntervalPausedMs ? Qt::PreciseTimer : Qt::CoarseTimer;
    _p->systemTickId = startTimer(intervalMs, timerType);
}

int PlaybackController::_desiredSystemTickIntervalMs() const
{
#if CGPLAY_HAS_TLRENDER
    if (!_p->player) {
        return kSystemTickIntervalIdleMs;
    }
#else
    if (_p->currentPath.isEmpty()) {
        return kSystemTickIntervalIdleMs;
    }
#endif
    if (_p->stateStub != 0) {
        return kSystemTickIntervalActiveMs;
    }
    return kSystemTickIntervalPausedMs;
}

int PlaybackController::_desiredUiTickIntervalMs() const
{
#if CGPLAY_HAS_TLRENDER
    if (!_p->player) {
        return kUiTickIntervalIdleMs;
    }
#else
    if (_p->currentPath.isEmpty()) {
        return kUiTickIntervalIdleMs;
    }
#endif
    if (_p->stateStub != 0) {
        return kUiTickIntervalActiveMs;
    }
    return kUiTickIntervalPausedMs;
}

void PlaybackController::_updateTimerCadence(bool force)
{
    const int desiredSystemTickMs = _desiredSystemTickIntervalMs();
    if (force || _p->systemTickIntervalMs != desiredSystemTickMs) {
        _restartSystemTickTimer(desiredSystemTickMs);
    }

    if (_p->tickTimer) {
        const int desiredUiTickMs = _desiredUiTickIntervalMs();
        if (force || _p->uiTickIntervalMs != desiredUiTickMs) {
            _p->uiTickIntervalMs = desiredUiTickMs;
            _p->tickTimer->setTimerType(
                desiredUiTickMs <= kUiTickIntervalActiveMs ? Qt::PreciseTimer : Qt::CoarseTimer);
            _p->tickTimer->setInterval(desiredUiTickMs);
        }
        if (!_p->tickTimer->isActive()) {
            _p->tickTimer->start();
        }
    }
}

// ─── File I/O ────────────────────────────────────────────────────────────────
void PlaybackController::openFile(const QString& path)
{
    openFile(path, 0.0);
}

void PlaybackController::openFile(const QString& path, double sequenceFpsOverride)
{
    openFile(path, sequenceFpsOverride, {});
}

void PlaybackController::openFile(
    const QString& path,
    double sequenceFpsOverride,
    const QString& codecHint)
{
    if (path.isEmpty()) return;

    QFileInfo fi(path);
    if (!fi.exists()) {
        qWarning() << "[PlaybackController] File not found:" << path;
        return;
    }

    if (_p->transcodeJob) {
        _p->transcodeJob->cancel();
        _p->transcodeJob.clear();
    }
    ++_p->openGeneration;
    _destroyPlayer();
    _p->lastReadAheadFrame = -1;
    _p->lastReadAheadTotal = -1;
    if (_p->cache) {
        _p->cache->onCacheInfoChanged(0.0f, 0.0f);
    }
    Q_EMIT cacheUsageChanged(0.0f, 0.0f);
    _createPlayer(path, sequenceFpsOverride, {}, codecHint);
}

void PlaybackController::closeFile()
{
    const bool hadMedia = !_p->currentPath.isEmpty() || isValid();
    if (_p->transcodeJob) {
        _p->transcodeJob->cancel();
        _p->transcodeJob.clear();
    }
    ++_p->openGeneration;
    _destroyPlayer();
    _p->currentPath.clear();
    _p->frameStub = 0;
    _p->totalStub = 1;
    _p->fpsStub = 24.0;
    _p->lastReadAheadFrame = -1;
    _p->lastReadAheadTotal = -1;
    _p->inPoint = -1;
    _p->outPoint = -1;
    if (_p->cache) {
        _p->cache->onCacheInfoChanged(0.0f, 0.0f);
    }
    Q_EMIT cacheUsageChanged(0.0f, 0.0f);
    Q_EMIT playbackStateChanged(0);
    Q_EMIT inOutPointsChanged(-1, -1);
    Q_EMIT currentFrameChanged(0, 1);
    Q_EMIT fpsChanged(24.0);
    if (hadMedia) {
        Q_EMIT fileClosed();
    }
    _updateTimerCadence();
}

QString PlaybackController::currentPath() const { return _p->currentPath; }

void PlaybackController::_createPlayer(
    const QString& path,
    double sequenceFpsOverride,
    const QString& logicalPath,
    const QString& codecHint)
{
    const QString sourcePath = logicalPath.isEmpty() ? path : logicalPath;
    _p->currentPath = sourcePath;

#if CGPLAY_HAS_TLRENDER && CGPLAY_HAS_FTK
    auto openWithTlRender = [&](const QString& mediaPath) -> bool {
        // Build player options
        tl::PlayerOptions pOpts;

        if (_p->cache) {
            size_t ramGB = _p->cache->ramCacheSizeGB();
            pOpts.cache.videoGB = static_cast<float>(ramGB);

            double fpsEstimate = 24.0;
            float readBehindSec = _p->cache->readBehindFrames() / static_cast<float>(fpsEstimate);
            pOpts.cache.readBehind = std::max(0.5F, readBehindSec);

            pOpts.videoRequestMax = std::max<size_t>(16, _p->cache->readAheadFrames());
        }

        // Create timeline (like DJV)
        tl::Options tlOpts;
        if (sequenceFpsOverride > 0.0) {
            tlOpts.ioOptions["SeqIO/DefaultSpeed"] = QString::number(sequenceFpsOverride, 'f', 3).toStdString();
        }
        const bool hintedVpFamily = isVpFamilyCodec(codecHint);
        const bool hintedAv1 = isAv1Codec(codecHint);
        if (hintedVpFamily) {
            tlOpts.ioOptions["FFmpeg/YUVToRGB"] = "1";
            tlOpts.ioOptions["FFmpeg/ThreadCount"] = "4";
        }
        if (hintedAv1) {
            tlOpts.ioOptions["FFmpeg/YUVToRGB"] = "1";
            tlOpts.ioOptions["FFmpeg/ThreadCount"] = "4";
        }
        auto timeline = tl::Timeline::create(
            _p->context, ftk::Path(mediaPath.toStdString()), tlOpts);

        if (!timeline) {
            qCritical() << "[PlaybackController] Timeline::create returned null for:"
                        << mediaPath << "— format may not be supported.";
            return false;
        }

        const auto codecIt = timeline->getIOInfo().tags.find("Video Codec");
        if (codecIt != timeline->getIOInfo().tags.end()) {
            const bool vpFamily = isVpFamilyCodec(QString::fromStdString(codecIt->second));
            const bool av1 = isAv1Codec(QString::fromStdString(codecIt->second));
            if (vpFamily && !hintedVpFamily) {
                // VP codecs use tlRender's slice-threaded compatibility path;
                // keep four workers for 4K decode throughput.
                tlOpts.ioOptions["FFmpeg/YUVToRGB"] = "1";
                tlOpts.ioOptions["FFmpeg/ThreadCount"] = "4";
                timeline = tl::Timeline::create(
                    _p->context, ftk::Path(mediaPath.toStdString()), tlOpts);
                if (!timeline) {
                    qCritical() << "[PlaybackController] VP RGB fallback failed for:" << mediaPath;
                    return false;
                }
                qInfo() << "[PlaybackController] VP-family RGB compatibility path:" << mediaPath;
            }
            if (av1 && !hintedAv1) {
                tlOpts.ioOptions["FFmpeg/YUVToRGB"] = "1";
                tlOpts.ioOptions["FFmpeg/ThreadCount"] = "4";
                timeline = tl::Timeline::create(
                    _p->context, ftk::Path(mediaPath.toStdString()), tlOpts);
                if (!timeline) {
                    qCritical() << "[PlaybackController] AV1 RGB compatibility path failed for:" << mediaPath;
                    return false;
                }
                qInfo() << "[PlaybackController] AV1 RGB compatibility path:" << mediaPath;
            }
        }

        // Create player directly (like DJV — no Qt wrapper)
        auto player = tl::Player::create(_p->context, timeline, pOpts);

        if (!player) {
            qCritical() << "[PlaybackController] Player::create returned null for:" << mediaPath;
            return false;
        }

        auto& tr = player->getTimeRange();
        const double fps = tr.duration().rate();
        const int total = static_cast<int>(tr.duration().value());
        if (fps <= 0.0 || total <= 0) {
            qWarning() << "[PlaybackController] Player opened with invalid time range:"
                       << mediaPath << "fps=" << fps << "frames=" << total;
            player->stop();
            return false;
        }

        // Set loop mode
        player->setLoop(tl::Loop::Loop);

        _p->player = player;
        player->setVolume(_p->volume);
        player->setMute(_p->muted);
        if (_p->cache) {
            _p->cache->onCacheInfoChanged(0.0f, 0.0f);
        }
        _connectPlayer();

        Q_EMIT fileOpened(sourcePath);
        Q_EMIT playerReady(_p->player);

        // Report initial info
        Q_EMIT fpsChanged(fps);
        Q_EMIT currentFrameChanged(0, total);
        _p->fpsStub   = fps;
        _p->totalStub = total;
        _updateTimerCadence();
        return true;
    };

    try {
        if (isAv1Codec(codecHint)) {
            const quint64 generation = _p->openGeneration;
            auto* job = JobRunner::start(QStringLiteral("playback.av1-transcode"), this, 30 * 60 * 1000,
                [path](JobContext& context) { return ensureAv1Cache(context, path); });
            _p->transcodeJob = job;
            connect(job, &JobHandle::finished, this,
                [this, job, path, sourcePath, sequenceFpsOverride, generation](const JobOutcome& outcome) {
                    if (_p->transcodeJob == job) _p->transcodeJob.clear();
                    if (generation != _p->openGeneration || _p->currentPath != sourcePath || !outcome.succeeded()) return;
                    const QString cachePath = outcome.value.toString();
                    if (!cachePath.isEmpty()) _createPlayer(cachePath, sequenceFpsOverride, sourcePath, QStringLiteral("h264"));
                });
            return;
        }
        const bool knownPngMov = codecHint.trimmed().compare(QStringLiteral("png"), Qt::CaseInsensitive) == 0 &&
            QFileInfo(path).suffix().compare(QStringLiteral("mov"), Qt::CaseInsensitive) == 0;
        if (knownPngMov) {
            // MainWindow already probed this stream. Route known PNG MOV to
            // its working cache without a failed native open or a second probe.
            qInfo() << "[PlaybackController] Known PNG MOV compatibility path:" << sourcePath;
        }
        if (!knownPngMov && openWithTlRender(path)) {
            return;
        }

        const quint64 generation = _p->openGeneration;
        auto* job = JobRunner::start(
            QStringLiteral("playback.png-mov-transcode"),
            this,
            30 * 60 * 1000,
            [path, knownPngMov](JobContext& context) {
                return ensurePngMovCache(context, path, knownPngMov);
            });
        _p->transcodeJob = job;
        connect(job, &JobHandle::finished, this,
            [this, job, path, sourcePath, sequenceFpsOverride, generation](const JobOutcome& outcome) {
                if (_p->transcodeJob == job) {
                    _p->transcodeJob.clear();
                }
                if (generation != _p->openGeneration || _p->currentPath != sourcePath) {
                    return;
                }
                if (!outcome.succeeded()) {
                    if (outcome.state != JobState::Canceled) {
                        qWarning() << "[PlaybackController] Background transcode job failed:"
                                   << outcome.errorCode << outcome.errorMessage;
                    }
                    return;
                }
                const QString cachePath = outcome.value.toString();
                if (cachePath.isEmpty()) {
                    return;
                }
                _destroyPlayer();
                _createPlayer(cachePath, sequenceFpsOverride, sourcePath, QStringLiteral("prores"));
            });
        return;
    } catch (const std::exception& e) {
        qCritical() << "[PlaybackController] Failed to open:" << path << "—" << e.what();
    }
#else
    qWarning() << "[PlaybackController] tlRender not available; stub mode for" << path;
    Q_EMIT fileOpened(sourcePath);
    _p->frameStub = 0;
    _p->totalStub = 100;
    Q_EMIT currentFrameChanged(0, 100);
    Q_EMIT fpsChanged(24.0);
    _updateTimerCadence();
#endif
}

void PlaybackController::_destroyPlayer()
{
#if CGPLAY_HAS_TLRENDER
    _p->cacheInfoObserver.reset();
    if (_p->player) {
        _p->player->stop();
        _p->player.reset();
    }
#endif
    _p->stateStub = 0;
    _updateTimerCadence();
}

void PlaybackController::_connectPlayer()
{
#if CGPLAY_HAS_TLRENDER
    if (!_p->player) return;

    // Get initial total frames from time range
    auto& tr = _p->player->getTimeRange();
    _p->totalStub = static_cast<int>(tr.duration().value());
    _p->fpsStub = tr.duration().rate();

    _p->cacheInfoObserver = ftk::Observer<tl::PlayerCacheInfo>::create(
        _p->player->observeCacheInfo(),
        [this](const tl::PlayerCacheInfo& value)
        {
            const float videoPercent = value.videoPercentage;
            const float audioPercent = value.audioPercentage;
            if (_p->cache) {
                _p->cache->onCacheInfoChanged(videoPercent, audioPercent);
            }
            Q_EMIT cacheUsageChanged(videoPercent, audioPercent);
        });
#endif
}

void PlaybackController::_tickTimer()
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        const int actualState = playbackStateToInt(_p->player->getPlayback());
        if (actualState != _p->stateStub) {
            _p->stateStub = actualState;
            Q_EMIT playbackStateChanged(_p->stateStub);
            _updateTimerCadence();
        }

        // Poll player state from UI timer (same pattern as DJV)
        auto& ct = _p->player->getCurrentTime();
        int frame = static_cast<int>(ct.value());
        if (frame != _p->frameStub) {
            _p->frameStub = frame;
            _enforceLoopBounds();
            frame = _p->frameStub; // may have been adjusted
            Q_EMIT currentFrameChanged(frame, _p->totalStub);

            // v1.5: 记录播放统计
            if (_p->stats) {
                FrameStat stat;
                stat.frame      = frame;
                stat.decodeMs   = 0.0;  // 无法从 Player API 获取精确值
                stat.renderMs   = 0.0;
                stat.totalMs    = 0.0;
                stat.dropped    = false;
                stat.hwDecoded  = false;
                _p->stats->recordFrame(stat);
            }

            // v1.1: 触发预读缓存
            if (_p->readAheadEnabled && _p->cache && _p->cache->readAheadCache()) {
                if (_p->lastReadAheadFrame != frame || _p->lastReadAheadTotal != _p->totalStub) {
                    _p->lastReadAheadFrame = frame;
                    _p->lastReadAheadTotal = _p->totalStub;
                    _p->cache->readAheadCache()->onPlayheadMove(frame, _p->totalStub);
                }
            }
        }
    }
#else
    if (_p->stateStub == 1) {
        _p->frameStub = (_p->frameStub + 1) % std::max(1, _p->totalStub);
        _enforceLoopBounds();
        Q_EMIT currentFrameChanged(_p->frameStub, _p->totalStub);

        if (_p->readAheadEnabled && _p->cache && _p->cache->readAheadCache()) {
            if (_p->lastReadAheadFrame != _p->frameStub || _p->lastReadAheadTotal != _p->totalStub) {
                _p->lastReadAheadFrame = _p->frameStub;
                _p->lastReadAheadTotal = _p->totalStub;
                _p->cache->readAheadCache()->onPlayheadMove(_p->frameStub, _p->totalStub);
            }
        }
    } else if (_p->stateStub == 2) {
        _p->frameStub = (_p->frameStub - 1 + _p->totalStub) % std::max(1, _p->totalStub);
        _enforceLoopBounds();
        Q_EMIT currentFrameChanged(_p->frameStub, _p->totalStub);

        if (_p->readAheadEnabled && _p->cache && _p->cache->readAheadCache()) {
            if (_p->lastReadAheadFrame != _p->frameStub || _p->lastReadAheadTotal != _p->totalStub) {
                _p->lastReadAheadFrame = _p->frameStub;
                _p->lastReadAheadTotal = _p->totalStub;
                _p->cache->readAheadCache()->onPlayheadMove(_p->frameStub, _p->totalStub);
            }
        }
    }
#endif
}

// ─── Accessors ────────────────────────────────────────────────────────────────
bool PlaybackController::isValid() const
{
#if CGPLAY_HAS_TLRENDER
    return _p->player != nullptr;
#else
    return !_p->currentPath.isEmpty();
#endif
}

int PlaybackController::currentFrame() const { return _p->frameStub; }
int PlaybackController::totalFrames()  const { return _p->totalStub; }
double PlaybackController::fps()       const { return _p->fpsStub;   }
int PlaybackController::playbackState() const { return _p->stateStub; }

#if CGPLAY_HAS_TLRENDER
std::shared_ptr<tl::Player> PlaybackController::player() const {
    return _p->player;
}
#endif

#if CGPLAY_HAS_FTK
std::shared_ptr<ftk::Context> PlaybackController::context() const { return _p->context; }
std::shared_ptr<ftk::Style>   PlaybackController::style()   const { return _p->style;   }
#endif

// ─── Playback slots ───────────────────────────────────────────────────────────
void PlaybackController::play()    { forward(); }

void PlaybackController::pause()
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) _p->player->setPlayback(tl::Playback::Stop);
    _p->stateStub = 0;
    Q_EMIT playbackStateChanged(0);
    _updateTimerCadence();
#else
    _p->stateStub = 0;
    Q_EMIT playbackStateChanged(0);
    _updateTimerCadence();
#endif
}

void PlaybackController::stop()    { pause(); }

void PlaybackController::forward()
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        if (_p->player->getSpeedMult() != 1.0) {
            _p->player->setSpeedMult(1.0);
        }
        if (_p->player->getPlayback() != tl::Playback::Forward) {
            _p->player->setPlayback(tl::Playback::Forward);
        }
    }
    _p->stateStub = 1;
    Q_EMIT playbackStateChanged(1);
    _updateTimerCadence();
#else
    _p->stateStub = 1;
    Q_EMIT playbackStateChanged(1);
    _updateTimerCadence();
#endif
}

void PlaybackController::reverse()
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        if (_p->player->getSpeedMult() != 1.0) {
            _p->player->setSpeedMult(1.0);
        }
        if (_p->player->getPlayback() != tl::Playback::Reverse) {
            _p->player->setPlayback(tl::Playback::Reverse);
        }
    }
    _p->stateStub = 2;
    Q_EMIT playbackStateChanged(2);
    _updateTimerCadence();
#else
    _p->stateStub = 2;
    Q_EMIT playbackStateChanged(2);
    _updateTimerCadence();
#endif
}

void PlaybackController::togglePlay()
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        _p->player->togglePlayback();
        _p->stateStub = playbackStateToInt(_p->player->getPlayback());
        Q_EMIT playbackStateChanged(_p->stateStub);
        _updateTimerCadence();
    }
#else
    if (_p->stateStub == 0) forward(); else stop();
#endif
}

void PlaybackController::nextFrame()
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) _p->player->frameNext();
#else
    stop();
    _p->frameStub = std::min(_p->frameStub + 1, _p->totalStub - 1);
    Q_EMIT currentFrameChanged(_p->frameStub, _p->totalStub);
#endif
}

void PlaybackController::prevFrame()
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) _p->player->framePrev();
#else
    stop();
    _p->frameStub = std::max(0, _p->frameStub - 1);
    Q_EMIT currentFrameChanged(_p->frameStub, _p->totalStub);
#endif
}

void PlaybackController::seekRelative(int deltaFrames)
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        auto cur = _p->player->getCurrentTime();
        _p->player->seek(cur + OTIO_NS::RationalTime(deltaFrames, cur.rate()));
    }
#else
    stop();
    _p->frameStub = std::clamp(_p->frameStub + deltaFrames, 0, _p->totalStub - 1);
    Q_EMIT currentFrameChanged(_p->frameStub, _p->totalStub);
#endif
}

void PlaybackController::seekToFrame(int frame)
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        auto range = _p->player->getTimeRange();
        double rate = range.start_time().rate();
        _p->player->seek(OTIO_NS::RationalTime(
            range.start_time().value() + frame, rate));
    }
#else
    _p->frameStub = std::clamp(frame, 0, _p->totalStub - 1);
    Q_EMIT currentFrameChanged(_p->frameStub, _p->totalStub);
#endif
}

void PlaybackController::gotoStart()
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) _p->player->gotoStart();
#else
    stop();
    _p->frameStub = 0;
    Q_EMIT currentFrameChanged(0, _p->totalStub);
#endif
}

void PlaybackController::gotoEnd()
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) _p->player->gotoEnd();
#else
    stop();
    _p->frameStub = _p->totalStub - 1;
    Q_EMIT currentFrameChanged(_p->frameStub, _p->totalStub);
#endif
}

void PlaybackController::setSpeed(double spd)
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) _p->player->setSpeed(spd);
#else
    _p->fpsStub = spd;
    Q_EMIT fpsChanged(spd);
#endif
}

void PlaybackController::setLoop(int mode)
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        tl::Loop l = tl::Loop::Loop;
        if (mode == 1) l = tl::Loop::Once;
        else if (mode == 2) l = tl::Loop::PingPong;
        _p->player->setLoop(l);
    }
#endif
}

// ─── In/Out Points (A/B Loop) ──────────────────────────────────────────────

void PlaybackController::setInPoint(int frame)
{
    _p->inPoint = std::max(0, std::min(frame, (int)_p->totalStub - 1));
    Q_EMIT inOutPointsChanged(_p->inPoint, _p->outPoint);
}

void PlaybackController::setOutPoint(int frame)
{
    _p->outPoint = std::max(0, std::min(frame, (int)_p->totalStub - 1));
    Q_EMIT inOutPointsChanged(_p->inPoint, _p->outPoint);
}

void PlaybackController::clearInPoint()
{
    _p->inPoint = -1;
    Q_EMIT inOutPointsChanged(_p->inPoint, _p->outPoint);
}

void PlaybackController::clearOutPoint()
{
    _p->outPoint = -1;
    Q_EMIT inOutPointsChanged(_p->inPoint, _p->outPoint);
}

void PlaybackController::clearInOutPoints()
{
    _p->inPoint = -1;
    _p->outPoint = -1;
    Q_EMIT inOutPointsChanged(_p->inPoint, _p->outPoint);
}

bool PlaybackController::hasInPoint() const { return _p->inPoint >= 0; }
bool PlaybackController::hasOutPoint() const { return _p->outPoint >= 0; }
int  PlaybackController::inPoint()   const { return _p->inPoint; }
int  PlaybackController::outPoint()  const { return _p->outPoint; }
bool PlaybackController::hasInOutRange() const {
    return _p->inPoint >= 0 && _p->outPoint >= 0 && _p->inPoint < _p->outPoint;
}

void PlaybackController::_enforceLoopBounds()
{
    if (!hasInOutRange()) return;

#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        auto pb = _p->player->getPlayback();
        if (pb == tl::Playback::Forward && _p->frameStub >= _p->outPoint) {
            _p->frameStub = _p->inPoint;
            auto range = _p->player->getTimeRange();
            double rate = range.start_time().rate();
            _p->player->seek(OTIO_NS::RationalTime(
                range.start_time().value() + _p->inPoint, rate));
        } else if (pb == tl::Playback::Reverse && _p->frameStub <= _p->inPoint) {
            _p->frameStub = _p->outPoint;
            auto range = _p->player->getTimeRange();
            double rate = range.start_time().rate();
            _p->player->seek(OTIO_NS::RationalTime(
                range.start_time().value() + _p->outPoint, rate));
        }
    }
#else
    // Stub mode: wrap in A-B range
    if (_p->stateStub == 1 && _p->frameStub >= _p->outPoint)
        _p->frameStub = _p->inPoint;
    else if (_p->stateStub == 2 && _p->frameStub <= _p->inPoint)
        _p->frameStub = _p->outPoint;
#endif
}

// ─── Compare ────────────────────────────────────────────────────────────────────
void PlaybackController::setCompareFile(const QString& path)
{
#if CGPLAY_HAS_TLRENDER && CGPLAY_HAS_FTK
    if (!_p->player || path.isEmpty()) return;

    try {
        tl::Options tlOpts;
        auto compareTimeline = tl::Timeline::create(
            _p->context, ftk::Path(path.toStdString()), tlOpts);

        if (compareTimeline) {
            std::vector<std::shared_ptr<tl::Timeline>> compareList;
            compareList.push_back(compareTimeline);
            _p->player->setCompare(compareList);
            _p->player->setCompareTime(tl::CompareTime::Relative);
        }
    } catch (const std::exception& e) {
        qWarning() << "[PlaybackController] Failed to set compare file:" << e.what();
    }
#endif
}

void PlaybackController::clearCompare()
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        _p->player->setCompare({});
    }
#endif
}

bool PlaybackController::hasCompare() const
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        return !_p->player->getCompare().empty();
    }
#endif
    return false;
}

void PlaybackController::setCompareTime(int mode)
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        _p->player->setCompareTime(
            mode == 0 ? tl::CompareTime::Relative : tl::CompareTime::Absolute);
    }
#endif
}

// ─── Audio ────────────────────────────────────────────────────────────────────
float PlaybackController::getVolume() const
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) return _p->player->getVolume();
#endif
    return _p->volume;
}

bool PlaybackController::isMuted() const
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) return _p->player->isMuted();
#endif
    return _p->muted;
}

void PlaybackController::setVolume(float v)
{
    _p->volume = std::clamp(v, 0.0f, 1.0f);
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        _p->player->setVolume(_p->volume);
    }
#endif
    Q_EMIT volumeChanged(_p->volume);
}

void PlaybackController::setMute(bool m)
{
    _p->muted = m;
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        _p->player->setMute(m);
    }
#endif
    Q_EMIT muteChanged(m);
}

void PlaybackController::toggleMute()
{
    setMute(!isMuted());
}

// ─── Channel Mute (L/R independent) ────────────────────────────────────────
bool PlaybackController::isChannelMuted(int channel) const
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        const auto& chMute = _p->player->getChannelMute();
        if (channel >= 0 && channel < static_cast<int>(chMute.size()))
            return chMute[channel];
    }
#endif
    Q_UNUSED(channel);
    return false;
}

void PlaybackController::setChannelMute(int channel, bool mute)
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        auto chMute = _p->player->getChannelMute(); // copy
        if (channel >= 0) {
            if (channel >= static_cast<int>(chMute.size()))
                chMute.resize(channel + 1, false);
            chMute[channel] = mute;
        }
        _p->player->setChannelMute(chMute);
        Q_EMIT channelMuteChanged(channel, mute);
    }
#else
    Q_UNUSED(channel);
    Q_UNUSED(mute);
#endif
}

void PlaybackController::toggleChannelMute(int channel)
{
    setChannelMute(channel, !isChannelMuted(channel));
}

// ─── Audio Device ──────────────────────────────────────────────────────────
QString PlaybackController::getAudioDeviceName() const
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) return QString::fromStdString(_p->player->getAudioDevice().name);
#endif
    return {};
}

void PlaybackController::setAudioDevice(const QString& name)
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        tl::AudioDeviceID id;
        id.name = name.toStdString();
        _p->player->setAudioDevice(id);
        Q_EMIT audioDeviceChanged(name);
    }
#else
    Q_UNUSED(name);
#endif
}

QStringList PlaybackController::availableAudioDevices() const
{
    QStringList list;
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        // Query SDL2 directly for audio output device names
        // tlRender wraps SDL2; device names can be set via SDL2 calls.
        // Return the current device + common defaults.
        list << QString::fromStdString(_p->player->getAudioDevice().name);
        // SDL2: enumerate output devices (requires SDL_Init)
        #ifdef SDL_Init
        SDL_Init(SDL_INIT_AUDIO);
        int ndev = SDL_GetNumAudioDevices(0);
        for (int i = 0; i < ndev; ++i) {
            const char* name = SDL_GetAudioDeviceName(i, 0);
            if (name) list << QString::fromUtf8(name);
        }
        list.removeDuplicates();
        #endif
    }
#endif
    if (list.isEmpty()) list << "Default";
    return list;
}

// ─── Audio Sync Offset ─────────────────────────────────────────────────────
double PlaybackController::getAudioOffset() const
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) return _p->player->getAudioOffset();
#endif
    return 0.0;
}

void PlaybackController::setAudioOffset(double seconds)
{
#if CGPLAY_HAS_TLRENDER
    if (_p->player) {
        _p->player->setAudioOffset(seconds);
        Q_EMIT audioOffsetChanged(seconds);
    }
#else
    Q_UNUSED(seconds);
#endif
}

// ─── v1.1 预读缓存开关 ────────────────────────────────────────────────────────
void PlaybackController::setReadAheadEnabled(bool enabled)
{
    _p->readAheadEnabled = enabled;
    if (!enabled) {
        _p->lastReadAheadFrame = -1;
        _p->lastReadAheadTotal = -1;
    }
    if (!enabled && _p->cache && _p->cache->readAheadCache())
        _p->cache->readAheadCache()->cancelPreload();
}

bool PlaybackController::isReadAheadEnabled() const
{
    return _p->readAheadEnabled;
}

PlaybackStats* PlaybackController::playbackStats() const
{
    return _p->stats;
}

} // namespace cgplay
