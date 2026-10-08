#pragma once
// CGPlay GPUUploadQueue.h — v1.1 性能层
// GPU 上传队列：使用 PBO (Pixel Buffer Object) 实现异步纹理上传，
// 通过双缓冲/三缓冲避免 CPU-GPU 同步等待，减少 stutter。

#include <QObject>
#include <QOpenGLFunctions_4_1_Core>
#include <QImage>
#include <QQueue>
#include <memory>
#include <atomic>
#include <cstddef>
#include <functional>

namespace cgplay {

struct UploadEntry
{
    QImage    image;          // 源图像数据（CPU 端）
    int       frame    = 0;   // 帧号
    bool      isEXR    = false;
};

// ─── GPUUploadQueue ────────────────────────────────────────────────────────────
// 在 paintGL 之前异步将纹理数据上传到 GPU，
// 减少 glTexImage2D 的主线程阻塞时间。
// ────────────────────────────────────────────────────────────────────────────────
class GPUUploadQueue : public QObject
{
    Q_OBJECT
public:
    explicit GPUUploadQueue(QObject* parent = nullptr);
    ~GPUUploadQueue() override;

    // ── OpenGL 初始化 ─────────────────────────────────────────────────────
    // 必须在有效的 OpenGL 上下文中调用（initializeGL 时）
    void initialize(QOpenGLFunctions_4_1_Core* gl);
    void shutdown();

    // ── 上传 ──────────────────────────────────────────────────────────────
    // enqueue: 提交一个帧到上传队列（调用方可从任何线程调用）
    void enqueue(const UploadEntry& entry);

    // processUploads: 在 paintGL 之前调用，处理已完成的 PBO 上传
    // 返回本次完成的帧数
    int  processUploads();

    // ── 状态 ──────────────────────────────────────────────────────────────
    int  pendingCount() const;     // 排队等待上传的帧数
    int  activeCount()  const;     // 正在 PBO 传输中的帧数
    bool isInitialized() const;

    // ── 回调 ──────────────────────────────────────────────────────────────
    using FrameReadyCallback = std::function<void(int frame, GLuint textureID)>;
    void setFrameReadyCallback(FrameReadyCallback cb);

Q_SIGNALS:
    void uploadComplete(int frame);
    void uploadFailed(int frame, const QString& error);

private:
    struct Private;
    std::unique_ptr<Private> _p;
};

} // namespace cgplay
