// CGPlay GPUUploadQueue.cpp — v1.1 性能层
// PBO-based async texture upload pipeline.
//
// 管线：
//   1. CPU 端：将 QImage 数据写入 PBO（glBufferData / glBufferSubData）
//   2. GPU 端：从 PBO 异步传输到纹理对象（glTexSubImage2D，延迟更低）
//   3. 主线程 processUploads：检查完成并返回纹理 ID
//
// 双缓冲：使用 2 个 PBO，一个写入，一个传输，流水线重叠。

#include "GPUUploadQueue.h"

#include <QDebug>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

namespace cgplay {

struct GPUUploadQueue::Private
{
    QOpenGLFunctions_4_1_Core* gl = nullptr;

    // PBO 双缓冲
    struct PBO {
        GLuint   id     = 0;
        GLsizeiptr size = 0;
        bool     inUse  = false;
    };
    PBO pboA;
    PBO pboB;

    // 纹理缓存
    struct TexEntry {
        GLuint textureID = 0;
        int    frame     = 0;
        bool   ready     = false;
    };
    QHash<int, TexEntry> textures;  // frame → GL texture
    QQueue<int>          texOrder;   // LRU 顺序

    // 上传队列
    QQueue<UploadEntry>  pendingQueue;
    mutable QMutex       queueMutex;

    // 统计
    std::atomic<int> activeUploads{0};

    // 回调
    FrameReadyCallback frameReadyCb;

    // 状态
    bool initialized = false;

    // 纹理池
    static constexpr int kMaxTextures = 32;
};

// ─── 构造 / 析构 ───────────────────────────────────────────────────────────────
GPUUploadQueue::GPUUploadQueue(QObject* parent)
    : QObject(parent)
    , _p(std::make_unique<Private>())
{
    qDebug() << "[GPUUploadQueue] Created";
}

GPUUploadQueue::~GPUUploadQueue()
{
    shutdown();
}

// ─── OpenGL 初始化 / 清理 ─────────────────────────────────────────────────────
void GPUUploadQueue::initialize(QOpenGLFunctions_4_1_Core* gl)
{
    if (_p->initialized) return;
    _p->gl = gl;

    // 创建 PBO 双缓冲（初始大小 16MB，按需扩展）
    GLsizeiptr initialSize = 16 * 1024 * 1024; // 16 MB
    gl->glGenBuffers(1, &_p->pboA.id);
    gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _p->pboA.id);
    gl->glBufferData(GL_PIXEL_UNPACK_BUFFER, initialSize, nullptr, GL_STREAM_DRAW);
    _p->pboA.size = initialSize;

    gl->glGenBuffers(1, &_p->pboB.id);
    gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _p->pboB.id);
    gl->glBufferData(GL_PIXEL_UNPACK_BUFFER, initialSize, nullptr, GL_STREAM_DRAW);
    _p->pboB.size = initialSize;

    gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    _p->initialized = true;
    qDebug() << "[GPUUploadQueue] Initialized with PBO double-buffer ("
             << initialSize / (1024*1024) << "MB each)";
}

void GPUUploadQueue::shutdown()
{
    if (!_p->initialized || !_p->gl) return;

    // 删除所有纹理
    for (auto& tex : _p->textures) {
        if (tex.textureID)
            _p->gl->glDeleteTextures(1, &tex.textureID);
    }
    _p->textures.clear();
    _p->texOrder.clear();

    // 删除 PBO
    if (_p->pboA.id) { _p->gl->glDeleteBuffers(1, &_p->pboA.id); _p->pboA.id = 0; }
    if (_p->pboB.id) { _p->gl->glDeleteBuffers(1, &_p->pboB.id); _p->pboB.id = 0; }

    _p->gl = nullptr;
    _p->initialized = false;
}

// ─── 入队 ──────────────────────────────────────────────────────────────────────
void GPUUploadQueue::enqueue(const UploadEntry& entry)
{
    if (entry.image.isNull()) return;

    {
        QMutexLocker lock(&_p->queueMutex);
        _p->pendingQueue.enqueue(entry);
    }
    _p->activeUploads.fetch_add(1);
}

// ─── 处理上传 ──────────────────────────────────────────────────────────────────
int GPUUploadQueue::processUploads()
{
    if (!_p->initialized || !_p->gl) return 0;

    int uploaded = 0;
    UploadEntry entry;
    bool had = false;

    {
        QMutexLocker lock(&_p->queueMutex);
        if (!_p->pendingQueue.isEmpty()) {
            entry = _p->pendingQueue.dequeue();
            had = true;
        }
    }

    while (had) {
        // 选择空闲的 PBO
        Private::PBO* pbo = nullptr;
        if (!_p->pboA.inUse) pbo = &_p->pboA;
        else if (!_p->pboB.inUse) pbo = &_p->pboB;
        else {
            // 两个 PBO 都在忙，跳过本帧（下一帧再处理）
            QMutexLocker lock(&_p->queueMutex);
            _p->pendingQueue.push_front(entry); // 放回队首
            break;
        }

        pbo->inUse = true;
        auto* gl = _p->gl;

        // 确保 PBO 容量足够
        GLsizeiptr needed = static_cast<GLsizeiptr>(entry.image.sizeInBytes());
        gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo->id);
        if (needed > pbo->size) {
            gl->glBufferData(GL_PIXEL_UNPACK_BUFFER, needed, nullptr, GL_STREAM_DRAW);
            pbo->size = needed;
        }

        // 映射 PBO 并写入数据
        void* ptr = gl->glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
        if (ptr) {
            memcpy(ptr, entry.image.constBits(), needed);
            gl->glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

            // 创建或获取纹理
            GLuint texID = 0;
            auto texIt = _p->textures.find(entry.frame);
            if (texIt != _p->textures.end()) {
                texID = texIt->textureID;
            } else {
                gl->glGenTextures(1, &texID);
                gl->glBindTexture(GL_TEXTURE_2D, texID);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                // 分配纹理存储
                GLenum internalFormat = entry.isEXR ? GL_RGBA16 : GL_RGBA8;
                GLenum srcFormat = entry.image.hasAlphaChannel() ?
                    (entry.image.format() == QImage::Format_RGBA64 ?
                        GL_RGBA : GL_RGBA) : GL_RGB;
                GLenum srcType = (entry.image.format() == QImage::Format_RGBA64)
                    ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;

                gl->glTexImage2D(GL_TEXTURE_2D, 0,
                    static_cast<GLint>(internalFormat),
                    entry.image.width(), entry.image.height(),
                    0, srcFormat, srcType, nullptr);

                Private::TexEntry te;
                te.textureID = texID;
                te.frame     = entry.frame;
                te.ready     = false;
                _p->textures[entry.frame] = te;
                _p->texOrder.enqueue(entry.frame);
            }

            // 从 PBO 传输到纹理
            gl->glBindTexture(GL_TEXTURE_2D, texID);
            GLenum format = entry.image.hasAlphaChannel() ?
                (entry.image.format() == QImage::Format_RGBA64 ? GL_RGBA : GL_RGBA)
                : GL_RGB;
            GLenum type = (entry.image.format() == QImage::Format_RGBA64)
                ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;

            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                entry.image.width(), entry.image.height(),
                format, type, nullptr); // nullptr → 从 PBO 读取

            // 标记完成
            if (_p->textures.contains(entry.frame))
                _p->textures[entry.frame].ready = true;

            // 回调
            if (_p->frameReadyCb)
                _p->frameReadyCb(entry.frame, texID);

            Q_EMIT uploadComplete(entry.frame);
            uploaded++;
        } else {
            Q_EMIT uploadFailed(entry.frame, "PBO map failed");
        }

        gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        pbo->inUse = false;
        _p->activeUploads.fetch_sub(1);

        // LRU 清理
        while (_p->texOrder.size() > Private::kMaxTextures) {
            int oldest = _p->texOrder.dequeue();
            auto it = _p->textures.find(oldest);
            if (it != _p->textures.end()) {
                gl->glDeleteTextures(1, &it->textureID);
                _p->textures.remove(oldest);
            }
        }

        // 取下一个
        {
            QMutexLocker lock(&_p->queueMutex);
            if (!_p->pendingQueue.isEmpty()) {
                entry = _p->pendingQueue.dequeue();
                had = true;
            } else {
                had = false;
            }
        }
    }

    return uploaded;
}

// ─── 状态 ──────────────────────────────────────────────────────────────────────
int GPUUploadQueue::pendingCount() const
{
    QMutexLocker lock(&_p->queueMutex);
    return _p->pendingQueue.size();
}

int GPUUploadQueue::activeCount() const
{
    return _p->activeUploads.load();
}

bool GPUUploadQueue::isInitialized() const
{
    return _p->initialized;
}

// ─── 回调 ──────────────────────────────────────────────────────────────────────
void GPUUploadQueue::setFrameReadyCallback(FrameReadyCallback cb)
{
    _p->frameReadyCb = std::move(cb);
}

} // namespace cgplay
