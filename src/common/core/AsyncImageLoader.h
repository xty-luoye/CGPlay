#pragma once
// CGPlay AsyncImageLoader.h — v1.1 性能层
// 异步图像加载器：在后台线程中读取 EXR/DPX 帧数据，
// 支持多部分 EXR、通道选择和降采样。

#include <QObject>
#include <QImage>
#include <QString>
#include <QFuture>
#include <memory>
#include <vector>
#include <functional>

namespace cgplay {

// ─── ImageLoadRequest ──────────────────────────────────────────────────────────
struct ImageLoadRequest {
    QString   filePath;          // 图像文件路径
    int       frame      = 0;    // 帧号（用于序列文件命名）
    int       mipLevel   = 0;    // MIP 级别（0=全分辨率）
    QString   channelName;       // 通道名（空=全部/默认通道）
    bool      halfFloat  = false;// 半浮点精度（节省带宽）
};

// ─── ImageLoadResult ───────────────────────────────────────────────────────────
struct ImageLoadResult {
    bool      success   = false;
    QImage    image;             // 解码后的图像（RGBA8 或 RGBA16）
    QString   filePath;
    int       frame     = 0;
    int       origWidth = 0;
    int       origHeight= 0;
    QString   errorMsg;
};

// ─── AsyncImageLoader ──────────────────────────────────────────────────────────
// 在后台线程中使用 OpenEXR / Qt 图像插件异步读取帧数据。
// 使用 QFuture 返回结果，可通过 QFutureWatcher 监听完成。
// ────────────────────────────────────────────────────────────────────────────────
class AsyncImageLoader : public QObject
{
    Q_OBJECT
public:
    explicit AsyncImageLoader(QObject* parent = nullptr);
    ~AsyncImageLoader() override;

    // ── 加载 ──────────────────────────────────────────────────────────────
    // loadAsync: 提交异步加载请求，返回 QFuture 用于获取结果
    QFuture<ImageLoadResult> loadAsync(const ImageLoadRequest& req);

    // Decode directly when the caller already owns a background worker.
    // Avoid queuing another task onto that same worker pool and waiting for it.
    static ImageLoadResult loadSync(const ImageLoadRequest& req);

    // loadBatchAsync: 批量加载（利用多线程并行读取）
    QFuture<std::vector<ImageLoadResult>> loadBatchAsync(
        const std::vector<ImageLoadRequest>& requests);

    // ── 信息 ──────────────────────────────────────────────────────────────
    // 同步读取 EXR 头信息（快速，不加载像素数据）
    static bool probeEXR(const QString& path, int& width, int& height,
                         std::vector<QString>* channelNames = nullptr);

    // ── 取消 ──────────────────────────────────────────────────────────────
    void cancelAll();

Q_SIGNALS:
    void imageLoaded(const ImageLoadResult& result);
    void batchProgress(int completed, int total);

private:
    struct Private;
    std::unique_ptr<Private> _p;
};

} // namespace cgplay
