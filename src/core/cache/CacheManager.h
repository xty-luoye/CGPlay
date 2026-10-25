#pragma once
// CGPlay CacheManager.h
// Frame & thumbnail prefetch cache — v1.1 性能层升级

#include <QObject>
#include <QString>
#include <memory>
#include <cstddef>

namespace cgplay {

class ReadAheadCache;

// ─── CacheManager ─────────────────────────────────────────────────────────────
// Wraps tlRender's PlayerCacheOptions and exposes cache configuration.
// v1.1: 集成了 ReadAheadCache 用于智能帧预读。
// ─────────────────────────────────────────────────────────────────────────────
class CacheManager : public QObject
{
    Q_OBJECT
public:
    explicit CacheManager(QObject* parent = nullptr);
    ~CacheManager() override;

    // ── Cache size ────────────────────────────────────────────────────────────
    size_t ramCacheSizeGB()    const;
    void   setRamCacheSizeGB(size_t gb);

    size_t readAheadFrames()   const;
    void   setReadAheadFrames(size_t frames);

    size_t readBehindFrames()  const;
    void   setReadBehindFrames(size_t frames);

    // ── v1.1 ReadAheadCache ───────────────────────────────────────────────────
    ReadAheadCache* readAheadCache() const;
    void setSequencePattern(const QString& dir, const QString& prefix,
                            const QString& ext, int startNum);

    // ── Cache control ─────────────────────────────────────────────────────────
    void clear();
    // Explicit user action: also clear the native caches of attached players.
    // Normal source switching continues to call clear() only.
    void clearPlaybackCaches();

    // ── Stats ─────────────────────────────────────────────────────────────────
    float videoUsagePercent()  const;
    float audioUsagePercent()  const;

public Q_SLOTS:
    void onCacheInfoChanged(float videoPercent, float audioPercent);

Q_SIGNALS:
    void cacheUpdated(float videoPercent, float audioPercent);
    void optionsChanged();
    void playbackCacheClearRequested();

private:
    void _notifyUsageIfChanged(bool force = false);

    struct Private;
    std::unique_ptr<Private> _p;
};

} // namespace cgplay
