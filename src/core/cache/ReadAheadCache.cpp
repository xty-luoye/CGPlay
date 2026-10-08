// CGPlay ReadAheadCache.cpp — v1.1 性能层

#include "ReadAheadCache.h"
#include "core/AsyncImageLoader.h"
#include "core/ThreadPool.h"

#include <QFileInfo>
#include <QDateTime>
#include <QDebug>
#include <QFutureWatcher>
#include <QSet>
#include <algorithm>
#include <limits>

namespace cgplay {

struct ReadAheadCache::Private
{
    // 帧缓存（环形缓冲区风格：用 hash 模拟）
    QHash<int, QImage> frameData;       // frameNum → image
    QQueue<int>        frameOrder;       // LRU 顺序（最早插入的在前面）
    QSet<int>          inFlightFrames;

    // 当前正在预读的帧范围
    int pendingFrom = -1;
    int pendingTo   = -1;
    bool preloadInFlight = false;
    // Keep the newest request while the current batch is still running.  The
    // playhead can move several times before a batch finishes; dropping the
    // newest range leaves the cache behind until another playhead event.
    bool deferredPreload = false;
    int deferredFrom = -1;
    int deferredTo = -1;
    // Monotonic token for invalidating results from cancelled/replaced loads.
    // Guarded by mutex together with the cache state.
    quint64 preloadGeneration = 0;
    int lastPlayheadFrame = -1;
    int lastPlayheadTotal = -1;

    // 序列帧模式
    QString seqDir;
    QString seqPrefix;
    QString seqExt;
    int     seqStartNum = 0;

    // 配置
    size_t maxFrames     = 64;
    size_t readAheadN    = 32;
    size_t readBehindN   = 8;

    // 加载器
    std::unique_ptr<AsyncImageLoader> loader;

    mutable QMutex mutex;
    size_t lastReportedUsedSlots = std::numeric_limits<size_t>::max();
};

// ─── 构造 / 析构 ───────────────────────────────────────────────────────────────
ReadAheadCache::ReadAheadCache(QObject* parent)
    : QObject(parent)
    , _p(std::make_unique<Private>())
{
    _p->loader = std::make_unique<AsyncImageLoader>(this);
    qDebug() << "[ReadAheadCache] Initialized, max frames:" << _p->maxFrames;
}

ReadAheadCache::~ReadAheadCache()
{
    cancelPreload();
    clear();
}

// ─── 配置 ──────────────────────────────────────────────────────────────────────
void ReadAheadCache::setCacheSize(size_t n)
{
    _p->maxFrames = std::max<size_t>(8, std::min<size_t>(512, n));
    _pruneCache(-1);
}

void ReadAheadCache::setReadAheadCount(size_t n)
{
    _p->readAheadN = std::max<size_t>(1, std::min<size_t>(256, n));
}

void ReadAheadCache::setReadBehindCount(size_t n)
{
    _p->readBehindN = std::max<size_t>(0, std::min<size_t>(64, n));
}

size_t ReadAheadCache::cacheSize()       const { return _p->maxFrames; }
size_t ReadAheadCache::readAheadCount()  const { return _p->readAheadN; }
size_t ReadAheadCache::readBehindCount() const { return _p->readBehindN; }

size_t ReadAheadCache::usedSlots() const
{
    QMutexLocker lock(&_p->mutex);
    return _p->frameData.size();
}

bool ReadAheadCache::isFull() const
{
    return usedSlots() >= _p->maxFrames;
}

bool ReadAheadCache::_hasFramePattern() const
{
    QMutexLocker lock(&_p->mutex);
    return !_p->seqDir.isEmpty() && !_p->seqExt.isEmpty();
}

void ReadAheadCache::_emitCacheStatsIfChanged(size_t usedSlots)
{
    if (_p->lastReportedUsedSlots == usedSlots) {
        return;
    }
    _p->lastReportedUsedSlots = usedSlots;
    Q_EMIT cacheStatsChanged(usedSlots, _p->maxFrames);
}

// ─── 帧模式 ────────────────────────────────────────────────────────────────────
void ReadAheadCache::setFramePattern(const QString& dir,
                                     const QString& prefix,
                                     const QString& ext,
                                     int startNum)
{
    QMutexLocker lock(&_p->mutex);
    _p->seqDir      = dir;
    _p->seqPrefix   = prefix;
    _p->seqExt      = ext;
    _p->seqStartNum = startNum;
}

// ─── 获取帧 ────────────────────────────────────────────────────────────────────
QImage ReadAheadCache::getFrame(int frameNumber) const
{
    QMutexLocker lock(&_p->mutex);
    auto it = _p->frameData.find(frameNumber);
    if (it != _p->frameData.end() && !it->isNull())
        return it.value();
    return {};
}

// ─── 请求预读 ──────────────────────────────────────────────────────────────────
void ReadAheadCache::requestPreload(int fromFrame, int toFrame)
{
    if (fromFrame > toFrame) std::swap(fromFrame, toFrame);

    if (!_hasFramePattern()) {
        return;
    }

    const int requestFrom = fromFrame;
    const int requestTo = toFrame;
    quint64 requestGeneration = 0;
    {
        QMutexLocker lock(&_p->mutex);
        if (_p->preloadInFlight) {
            if (_p->pendingFrom != requestFrom || _p->pendingTo != requestTo) {
                _p->deferredFrom = requestFrom;
                _p->deferredTo = requestTo;
                _p->deferredPreload = true;
            }
            return;
        }
        if (_p->pendingFrom == requestFrom && _p->pendingTo == requestTo) {
            return;
        }
        _p->pendingFrom = fromFrame;
        _p->pendingTo   = toFrame;
        _p->preloadInFlight = true;
        requestGeneration = ++_p->preloadGeneration;
    }

    // 构建请求列表（只请求尚未缓存的帧）
    std::vector<ImageLoadRequest> requests;
    {
        QMutexLocker lock(&_p->mutex);
        for (int f = fromFrame; f <= toFrame; ++f) {
            if (_p->frameData.contains(f) || _p->inFlightFrames.contains(f)) continue;

            QString filePath;
            if (!_p->seqDir.isEmpty()) {
                // 序列帧模式：dir/prefix.0001.ext
                int num = _p->seqStartNum + f;
                filePath = QString("%1/%2%3.%4")
                    .arg(_p->seqDir)
                    .arg(_p->seqPrefix)
                    .arg(num, 4, 10, QChar('0'))  // 至少4位补零
                    .arg(_p->seqExt);
            }

            if (!filePath.isEmpty() && QFileInfo::exists(filePath)) {
                ImageLoadRequest req;
                req.filePath = filePath;
                req.frame    = f;
                requests.push_back(req);
                _p->inFlightFrames.insert(f);
            }
        }
    }

    if (requests.empty()) {
        int deferredFrom = -1;
        int deferredTo = -1;
        QMutexLocker lock(&_p->mutex);
        if (_p->preloadGeneration == requestGeneration
            && _p->pendingFrom == requestFrom && _p->pendingTo == requestTo) {
            _p->pendingFrom = -1;
            _p->pendingTo = -1;
            _p->preloadInFlight = false;
            if (_p->deferredPreload) {
                deferredFrom = _p->deferredFrom;
                deferredTo = _p->deferredTo;
                _p->deferredFrom = -1;
                _p->deferredTo = -1;
                _p->deferredPreload = false;
            }
        }
        lock.unlock();
        if (deferredFrom >= 0 && deferredTo >= deferredFrom) {
            requestPreload(deferredFrom, deferredTo);
        }
        return;
    }

    // 使用 AsyncImageLoader 批量预读
    auto future = _p->loader->loadBatchAsync(requests);

    auto* watcher = new QFutureWatcher<std::vector<ImageLoadResult>>(this);
    connect(watcher, &QFutureWatcher<std::vector<ImageLoadResult>>::finished,
            this, [this, watcher, requestFrom, requestTo, requestGeneration]() {
        auto results = watcher->result();
        int loadedCount = 0;
        QVector<int> readyFrames;
        size_t usedSlots = 0;
        int deferredFrom = -1;
        int deferredTo = -1;

        {
            QMutexLocker lock(&_p->mutex);
            // Cancellation/clear or a newer request invalidates this result.
            // In particular, do not remove in-flight markers belonging to the
            // newer request or repopulate a cache that has already been cleared.
            if (_p->preloadGeneration != requestGeneration) {
                watcher->deleteLater();
                return;
            }

            for (const auto& r : results) {
                _p->inFlightFrames.remove(r.frame);
                if (r.success && !r.image.isNull()) {
                    _p->frameData[r.frame] = r.image;
                    if (!_p->frameOrder.contains(r.frame)) {
                        _p->frameOrder.enqueue(r.frame);
                    }
                    ++loadedCount;
                    readyFrames.push_back(r.frame);
                }
            }

            if (_p->pendingFrom == requestFrom && _p->pendingTo == requestTo) {
                _p->pendingFrom = -1;
                _p->pendingTo = -1;
            }
            _p->preloadInFlight = false;
            if (_p->deferredPreload) {
                deferredFrom = _p->deferredFrom;
                deferredTo = _p->deferredTo;
                _p->deferredFrom = -1;
                _p->deferredTo = -1;
                _p->deferredPreload = false;
            }

            // LRU淘汰：超过最大缓存
            while (static_cast<size_t>(_p->frameData.size()) > _p->maxFrames
                   && !_p->frameOrder.isEmpty()) {
                int oldest = _p->frameOrder.dequeue();
                _p->frameData.remove(oldest);
            }
            usedSlots = _p->frameData.size();
        }

        // Start the newest range after the current batch has released its
        // state.  This closes the gap where a playhead move arrived while a
        // batch was in flight and would otherwise be silently discarded.
        if (deferredFrom >= 0 && deferredTo >= deferredFrom) {
            requestPreload(deferredFrom, deferredTo);
        }

        for (int frame : readyFrames) {
            Q_EMIT frameReady(frame);
        }

        _emitCacheStatsIfChanged(usedSlots);
        Q_EMIT preloadProgress(
            static_cast<int>(loadedCount),
            static_cast<int>(results.size()));

        watcher->deleteLater();
    });

    watcher->setFuture(future);
}

// ─── 取消 ──────────────────────────────────────────────────────────────────────
void ReadAheadCache::cancelPreload()
{
    {
        QMutexLocker lock(&_p->mutex);
        ++_p->preloadGeneration;
        _p->pendingFrom = -1;
        _p->pendingTo   = -1;
        _p->preloadInFlight = false;
        _p->deferredFrom = -1;
        _p->deferredTo = -1;
        _p->deferredPreload = false;
        _p->lastPlayheadFrame = -1;
        _p->lastPlayheadTotal = -1;
        _p->inFlightFrames.clear();
    }
    _p->loader->cancelAll();
}

// ─── 清空 ──────────────────────────────────────────────────────────────────────
void ReadAheadCache::clear()
{
    cancelPreload();
    {
        QMutexLocker lock(&_p->mutex);
        _p->frameData.clear();
        _p->frameOrder.clear();
    }
    _emitCacheStatsIfChanged(0);
}

// ─── 播放头移动 ────────────────────────────────────────────────────────────────
void ReadAheadCache::onPlayheadMove(int currentFrame, int totalFrames)
{
    if (!_hasFramePattern()) {
        return;
    }

    {
        QMutexLocker lock(&_p->mutex);
        if (_p->lastPlayheadFrame == currentFrame && _p->lastPlayheadTotal == totalFrames) {
            return;
        }
        _p->lastPlayheadFrame = currentFrame;
        _p->lastPlayheadTotal = totalFrames;
    }

    // 触发预读：当前帧往后 readAheadN 帧
    int from = currentFrame + 1;
    int to   = std::min(currentFrame + static_cast<int>(_p->readAheadN),
                         totalFrames - 1);

    if (from <= to)
        requestPreload(from, to);

    // 清理过期帧（保留 readBehindN 帧的回顾范围）
    int keepFrom = std::max(0, currentFrame - static_cast<int>(_p->readBehindN));
    _pruneCache(keepFrom);
}

// ─── LRU 清理 ──────────────────────────────────────────────────────────────────
void ReadAheadCache::_pruneCache(int keepFrom)
{
    size_t usedSlots = 0;
    {
        QMutexLocker lock(&_p->mutex);

        // 移除 keepFrom 之前的过期帧
        if (keepFrom >= 0) {
            QQueue<int> newOrder;
            while (!_p->frameOrder.isEmpty()) {
                int fn = _p->frameOrder.dequeue();
                if (fn >= keepFrom)
                    newOrder.enqueue(fn);
                else
                    _p->frameData.remove(fn);
            }
            _p->frameOrder = std::move(newOrder);
        }

        // 仍然超过最大容量则从前面移除
        while (static_cast<size_t>(_p->frameData.size()) > _p->maxFrames
               && !_p->frameOrder.isEmpty()) {
            int oldest = _p->frameOrder.dequeue();
            _p->frameData.remove(oldest);
        }
        usedSlots = _p->frameData.size();
    }
    _emitCacheStatsIfChanged(usedSlots);
}

} // namespace cgplay
