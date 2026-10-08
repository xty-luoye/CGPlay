// CGPlay AsyncImageLoader.cpp — v1.1 性能层

#include "AsyncImageLoader.h"
#include "ThreadPool.h"

#include <QFileInfo>
#include <QImageReader>
#include <QtConcurrent>
#include <QDebug>
#include <QAtomicInteger>
#include <QPointer>

// ─── OpenEXR ──────────────────────────────────────────────────────────────────
#if __has_include(<OpenEXR/ImfRgbaFile.h>)
  #define CGPLAY_EXR_ENABLED 1
  #include <OpenEXR/ImfRgbaFile.h>
  #include <OpenEXR/ImfArray.h>
  #include <OpenEXR/ImfHeader.h>
  #include <OpenEXR/ImfFrameBuffer.h>
  #include <OpenEXR/ImfChannelList.h>
  #include <OpenEXR/ImfInputFile.h>
  #include <OpenEXR/ImfMultiPartInputFile.h>
  #include <OpenEXR/ImfOutputFile.h>
  #include <Imath/ImathBox.h>
#else
  #define CGPLAY_EXR_ENABLED 0
#endif

namespace cgplay {

struct AsyncImageLoader::Private
{
    // Keep cancellation state alive independently of the QObject.  Batch
    // futures may outlive the loader itself (for example when a cache is
    // destroyed while its read-ahead task is still queued).
    std::shared_ptr<QAtomicInteger<bool>> cancelled =
        std::make_shared<QAtomicInteger<bool>>(false);
    std::shared_ptr<QAtomicInteger<int>> activeLoads =
        std::make_shared<QAtomicInteger<int>>(0);
};

// ─── 构造 / 析构 ───────────────────────────────────────────────────────────────
AsyncImageLoader::AsyncImageLoader(QObject* parent)
    : QObject(parent)
    , _p(std::make_unique<Private>())
{
    qDebug() << "[AsyncImageLoader] Initialized, EXR support:"
             << (CGPLAY_EXR_ENABLED ? "enabled" : "disabled");
}

AsyncImageLoader::~AsyncImageLoader()
{
    cancelAll();
    // Wait briefly for in-flight loads to complete
    int waited = 0;
    while (_p->activeLoads->loadRelaxed() > 0 && waited < 100) {
        QThread::msleep(50);
        waited++;
    }
}

void AsyncImageLoader::cancelAll()
{
    _p->cancelled->storeRelaxed(true);
}

// ─── 同步 EXR 头探测 ──────────────────────────────────────────────────────────
bool AsyncImageLoader::probeEXR(const QString& path, int& width, int& height,
                                std::vector<QString>* channelNames)
{
#if CGPLAY_EXR_ENABLED
    try {
        Imf::MultiPartInputFile file(path.toStdString().c_str());
        int numParts = file.parts();
        if (numParts < 1) return false;

        const Imf::Header& header = file.header(0);
        const Imath::Box2i& dw = header.dataWindow();
        width  = dw.max.x - dw.min.x + 1;
        height = dw.max.y - dw.min.y + 1;

        if (channelNames) {
            channelNames->clear();
            const Imf::ChannelList& ch = header.channels();
            for (auto it = ch.begin(); it != ch.end(); ++it)
                channelNames->push_back(QString::fromStdString(it.name()));
        }
        return true;
    } catch (const std::exception& e) {
        qWarning() << "[AsyncImageLoader] probeEXR failed:" << e.what();
    }
#endif
    Q_UNUSED(path);
    width = height = 0;
    return false;
}

// ─── 内部：单帧加载 ───────────────────────────────────────────────────────────
static ImageLoadResult _loadOne(const ImageLoadRequest& req,
                                const QAtomicInteger<bool>* cancelled)
{
    ImageLoadResult result;
    result.filePath = req.filePath;
    result.frame    = req.frame;

    QFileInfo fi(req.filePath);
    if (!fi.exists()) {
        result.errorMsg = QString("File not found: %1").arg(req.filePath);
        return result;
    }

    QString ext = fi.suffix().toLower();

#if CGPLAY_EXR_ENABLED
    // ── EXR: 使用 OpenEXR 原生读取 ─────────────────────────────────────────
    if (ext == "exr") {
        if (cancelled && cancelled->loadRelaxed()) {
            result.errorMsg = "Cancelled";
            return result;
        }
        try {
            Imf::RgbaInputFile file(req.filePath.toStdString().c_str());
            const Imath::Box2i& dw = file.dataWindow();
            result.origWidth  = dw.max.x - dw.min.x + 1;
            result.origHeight = dw.max.y - dw.min.y + 1;

            if (cancelled && cancelled->loadRelaxed()) {
                result.errorMsg = "Cancelled";
                return result;
            }

            Imf::Array2D<Imf::Rgba> pixels(result.origHeight, result.origWidth);
            file.setFrameBuffer(&pixels[0][0] - dw.min.x - dw.min.y * result.origWidth,
                                1, result.origWidth);
            file.readPixels(dw.min.y, dw.max.y);

            if (cancelled && cancelled->loadRelaxed()) {
                result.errorMsg = "Cancelled";
                return result;
            }

            // 转换为 QImage (RGBA64 获取最高精度)
            QImage img(result.origWidth, result.origHeight, QImage::Format_RGBA64);
            for (int y = 0; y < result.origHeight; ++y) {
                quint16* scanline = reinterpret_cast<quint16*>(img.scanLine(y));
                for (int x = 0; x < result.origWidth; ++x) {
                    const auto& p = pixels[y][x];
                    // half → float → uint16 (PQ-safe range)
                    float rf = static_cast<float>(p.r);
                    float gf = static_cast<float>(p.g);
                    float bf = static_cast<float>(p.b);
                    float af = static_cast<float>(p.a);
                    scanline[x * 4 + 0] = static_cast<quint16>(
                        std::max(0.f, std::min(65535.f, rf * 65535.f)));
                    scanline[x * 4 + 1] = static_cast<quint16>(
                        std::max(0.f, std::min(65535.f, gf * 65535.f)));
                    scanline[x * 4 + 2] = static_cast<quint16>(
                        std::max(0.f, std::min(65535.f, bf * 65535.f)));
                    scanline[x * 4 + 3] = static_cast<quint16>(
                        std::max(0.f, std::min(65535.f, af * 65535.f)));
                }
            }
            result.image   = img;
            result.success = true;
        } catch (const std::exception& e) {
            result.errorMsg = QString("EXR read error: %1").arg(e.what());
            qWarning() << "[AsyncImageLoader]" << result.errorMsg;
        }
        return result;
    }
#endif

    // ── 其他格式：使用 QImageReader ────────────────────────────────────────
    if (cancelled && cancelled->loadRelaxed()) {
        result.errorMsg = "Cancelled";
        return result;
    }

    QImageReader reader(req.filePath);
    reader.setAutoTransform(true);
    if (req.mipLevel > 0) {
        QSize orig = reader.size();
        int divisor = 1 << req.mipLevel;
        reader.setScaledSize(QSize(orig.width() / divisor, orig.height() / divisor));
    }

    if (reader.canRead()) {
        result.image = reader.read();
        if (!result.image.isNull()) {
            result.origWidth  = reader.size().width();
            result.origHeight = reader.size().height();
            result.success = true;
        } else {
            result.errorMsg = reader.errorString();
        }
    } else {
        result.errorMsg = QString("Cannot read: %1").arg(req.filePath);
    }

    return result;
}

// ─── 异步加载 ──────────────────────────────────────────────────────────────────
ImageLoadResult AsyncImageLoader::loadSync(const ImageLoadRequest& req)
{
    return _loadOne(req, nullptr);
}

QFuture<ImageLoadResult> AsyncImageLoader::loadAsync(const ImageLoadRequest& req)
{
    _p->cancelled->storeRelaxed(false);

    const auto cancelled = _p->cancelled;
    const auto activeLoads = _p->activeLoads;
    activeLoads->fetchAndAddRelaxed(1);
    const QPointer<AsyncImageLoader> self(this);

    return QtConcurrent::run([req, cancelled, activeLoads, self]() -> ImageLoadResult {
        auto result = _loadOne(req, cancelled.get());
        activeLoads->fetchAndSubRelaxed(1);
        if (result.success || !result.errorMsg.isEmpty()) {
            if (self) {
                QMetaObject::invokeMethod(self, [self, r = result]() {
                    if (self) {
                        Q_EMIT self->imageLoaded(r);
                    }
                }, Qt::QueuedConnection);
            }
        }
        return result;
    });
}

QFuture<std::vector<ImageLoadResult>> AsyncImageLoader::loadBatchAsync(
    const std::vector<ImageLoadRequest>& requests)
{
    _p->cancelled->storeRelaxed(false);

    // Capture the shared cancellation state instead of this/_p.  The
    // returned future can still be running after AsyncImageLoader is
    // destroyed, so no batch worker may dereference the QObject lifetime.
    const auto cancelled = _p->cancelled;

    return QtConcurrent::run([requests, cancelled]() -> std::vector<ImageLoadResult> {
        std::vector<ImageLoadResult> results;
        results.resize(requests.size());

        // 并行处理批次
        results = QtConcurrent::blockingMapped(
            requests,
            [cancelled](const ImageLoadRequest& req) -> ImageLoadResult {
                return _loadOne(req, cancelled.get());
            });

        return results;
    });
}

} // namespace cgplay
