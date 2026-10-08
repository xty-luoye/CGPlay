#pragma once
// CGPlay ReadAheadCache.h — v1.1 性能层
// 预读帧缓存：在播放时提前将后续帧加载到 RAM，
// 利用线程池并行读取，减少磁盘 I/O 延迟。

#include <QObject>
#include <QImage>
#include <QString>
#include <QHash>
#include <QQueue>
#include <QMutex>
#include <memory>
#include <atomic>
#include <cstddef>

namespace cgplay {

// ─── CachedFrame ───────────────────────────────────────────────────────────────
struct CachedFrame {
    QImage    image;
    QString   filePath;     // 序列帧文件的完整路径
    int       frame    = 0;
    bool      ready    = false;
};

// ─── ReadAheadCache ────────────────────────────────────────────────────────────
// 环形缓冲区，存储预读的帧数据。
// 播放时，根据当前帧位置自动触发后续帧的预读。
// ────────────────────────────────────────────────────────────────────────────────
class ReadAheadCache : public QObject
{
    Q_OBJECT
public:
    explicit ReadAheadCache(QObject* parent = nullptr);
    ~ReadAheadCache() override;

    // ── 配置 ──────────────────────────────────────────────────────────────
    void setCacheSize(size_t maxFrames);     // 最大缓存帧数（默认 64）
    void setReadAheadCount(size_t n);         // 预读帧数（默认 32）
    void setReadBehindCount(size_t n);        // 后读保留帧数（默认 8）

    size_t cacheSize()       const;
    size_t readAheadCount()  const;
    size_t readBehindCount() const;
    size_t usedSlots()       const;           // 当前已占用的槽位
    bool   isFull()          const;

    // ── 帧管理 ────────────────────────────────────────────────────────────
    // setFramePattern: 设置序列帧文件命名模式
    //   dir: 序列目录, prefix: 前缀, ext: 扩展名, startNum: 起始编号
    void setFramePattern(const QString& dir,
                         const QString& prefix,
                         const QString& ext,
                         int startNum);

    // getFrame: 获取已缓存的帧数据（同步，O(1) 查找）。
    // 返回空 QImage 表示该帧未缓存；值返回避免暴露内部容器地址。
    QImage getFrame(int frameNumber) const;

    // requestPreload: 请求预读指定范围的帧
    void requestPreload(int fromFrame, int toFrame);

    // cancelPreload: 取消所有进行中的预读
    void cancelPreload();

    // clear: 清空所有缓存
    void clear();

    // ── 回放集成 ──────────────────────────────────────────────────────────
    // onPlayheadMove: 播放头移动时调用，触发自动预读
    void onPlayheadMove(int currentFrame, int totalFrames);

Q_SIGNALS:
    void frameReady(int frameNumber);    // 某帧数据已准备好
    void cacheStatsChanged(size_t used, size_t max);
    void preloadProgress(int done, int total);

private:
    bool _hasFramePattern() const;
    void _emitCacheStatsIfChanged(size_t usedSlots);
    void _pruneCache(int currentFrame);  // 清理过期帧

    struct Private;
    std::unique_ptr<Private> _p;
};

} // namespace cgplay
