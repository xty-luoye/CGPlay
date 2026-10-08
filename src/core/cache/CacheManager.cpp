// CGPlay CacheManager.cpp — v1.1 性能层升级

#include "CacheManager.h"
#include "ReadAheadCache.h"
#include "common/core/ServiceLocator.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"

#include <QDebug>
#include <algorithm>
#include <cmath>

namespace cgplay {

struct CacheManager::Private
{
    size_t ramCacheSizeGB    = 4;    // default 4 GB RAM cache
    size_t readAheadFrames   = 32;   // pre-read 32 frames ahead
    size_t readBehindFrames  = 8;    // keep 8 frames behind

    float videoUsagePercent  = 0.f;
    float audioUsagePercent  = 0.f;
    float playerVideoUsagePercent = 0.f;
    float readAheadUsagePercent = 0.f;
    float lastNotifiedVideoUsagePercent = -1.f;
    float lastNotifiedAudioUsagePercent = -1.f;

    // v1.1: 帧预读缓存
    std::unique_ptr<ReadAheadCache> readAheadCache;
};

CacheManager::CacheManager(QObject* parent)
    : QObject(parent)
    , _p(std::make_unique<Private>())
{
    _p->readAheadCache = std::make_unique<ReadAheadCache>(this);
    _p->readAheadCache->setCacheSize(64);
    _p->readAheadCache->setReadAheadCount(_p->readAheadFrames);
    _p->readAheadCache->setReadBehindCount(_p->readBehindFrames);
    connect(_p->readAheadCache.get(), &ReadAheadCache::cacheStatsChanged,
            this, [this](size_t used, size_t max) {
        _p->readAheadUsagePercent = max
            ? std::clamp(static_cast<float>(used) * 100.0f / static_cast<float>(max), 0.0f, 100.0f)
            : 0.0f;
        _p->videoUsagePercent = std::max(_p->playerVideoUsagePercent, _p->readAheadUsagePercent);
        _notifyUsageIfChanged();
    });
    qDebug() << "[CacheManager] Initialized with ReadAheadCache (v1.1)";
}

CacheManager::~CacheManager() = default;

size_t CacheManager::ramCacheSizeGB()    const { return _p->ramCacheSizeGB;    }
size_t CacheManager::readAheadFrames()   const { return _p->readAheadFrames;   }
size_t CacheManager::readBehindFrames()  const { return _p->readBehindFrames;  }
float  CacheManager::videoUsagePercent() const { return _p->videoUsagePercent; }
float  CacheManager::audioUsagePercent() const { return _p->audioUsagePercent; }

void CacheManager::setRamCacheSizeGB(size_t gb)
{
    _p->ramCacheSizeGB = std::max<size_t>(1, gb);
    Q_EMIT optionsChanged();
}

void CacheManager::setReadAheadFrames(size_t frames)
{
    _p->readAheadFrames = std::max<size_t>(1, frames);
    if (_p->readAheadCache)
        _p->readAheadCache->setReadAheadCount(frames);
    Q_EMIT optionsChanged();
}

void CacheManager::setReadBehindFrames(size_t frames)
{
    _p->readBehindFrames = frames;
    if (_p->readAheadCache)
        _p->readAheadCache->setReadBehindCount(frames);
    Q_EMIT optionsChanged();
}

// ─── v1.1 ReadAheadCache ───────────────────────────────────────────────────────
ReadAheadCache* CacheManager::readAheadCache() const
{
    return _p->readAheadCache.get();
}

void CacheManager::setSequencePattern(const QString& dir,
                                      const QString& prefix,
                                      const QString& ext,
                                      int startNum)
{
    if (_p->readAheadCache)
        _p->readAheadCache->setFramePattern(dir, prefix, ext, startNum);
}

void CacheManager::clear()
{
    _p->videoUsagePercent = 0.f;
    _p->audioUsagePercent = 0.f;
    _p->playerVideoUsagePercent = 0.f;
    _p->readAheadUsagePercent = 0.f;
    if (_p->readAheadCache)
        _p->readAheadCache->clear();
    _notifyUsageIfChanged();
    qDebug() << "[CacheManager] Cache cleared";
}

void CacheManager::onCacheInfoChanged(float videoPercent, float audioPercent)
{
    _p->playerVideoUsagePercent = std::clamp(videoPercent, 0.0f, 100.0f);
    _p->audioUsagePercent = std::clamp(audioPercent, 0.0f, 100.0f);
    _p->videoUsagePercent = std::max(_p->playerVideoUsagePercent, _p->readAheadUsagePercent);
    _notifyUsageIfChanged();
}

void CacheManager::_notifyUsageIfChanged(bool force)
{
    const bool changed =
        force ||
        std::fabs(_p->videoUsagePercent - _p->lastNotifiedVideoUsagePercent) > 0.01f ||
        std::fabs(_p->audioUsagePercent - _p->lastNotifiedAudioUsagePercent) > 0.01f;
    if (!changed) {
        return;
    }

    _p->lastNotifiedVideoUsagePercent = _p->videoUsagePercent;
    _p->lastNotifiedAudioUsagePercent = _p->audioUsagePercent;
    Q_EMIT cacheUpdated(_p->videoUsagePercent, _p->audioUsagePercent);
    if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
        eventBus->publish(CacheUsageChangedEvent{ _p->videoUsagePercent, _p->audioUsagePercent });
    }
}

} // namespace cgplay
