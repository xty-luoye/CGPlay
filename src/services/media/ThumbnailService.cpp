#include "ThumbnailService.h"
#include "ThumbnailDiskCache.h"

#include "core/AsyncImageLoader.h"
#include "common/jobs/JobSystem.h"
#include "media/MediaProbe.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRect>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QSemaphore>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QHash>
#include <memory>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <vector>

namespace cgplay {

namespace {

QImage loadStillImage(const QString& path, JobContext* job)
{
    if (job && job->shouldStop()) return {};
    if (MediaProbe::isStillImagePath(path) &&
        QFileInfo(path).suffix().compare(QStringLiteral("exr"), Qt::CaseInsensitive) == 0) {
        ImageLoadRequest req;
        req.filePath = path;
        // The caller already owns a worker; waiting on another task in the
        // same pool can starve it. Native decoding is cancelable at boundaries.
        const auto result = AsyncImageLoader::loadSync(req);
        if (job && job->shouldStop()) return {};
        if (result.success) {
            return result.image;
        }
    }

    QImageReader reader(path);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    const QImage image = reader.read();
    return job && job->shouldStop() ? QImage{} : image;
}

QString cachePathForVideoFrame(const QString& path, int frame, const QSize& size,
    double fps, const ThumbnailDiskCache::Policy& policy)
{
    const QFileInfo fileInfo(path);
    const QByteArray key = QCryptographicHash::hash(
        QFileInfo(path).absoluteFilePath().toUtf8() +
            QByteArray::number(fileInfo.size()) + ':' +
            QByteArray::number(fileInfo.lastModified().toMSecsSinceEpoch()) + ':' +
            QByteArray::number(size.width()) + 'x' +
            QByteArray::number(size.height()) + ':' +
            QByteArray::number(fps, 'g', 17),
        QCryptographicHash::Sha1).toHex();
    return ThumbnailDiskCache::path(policy, key, frame);
}

QSemaphore thumbnailProcessSlots(2);

struct ThumbnailFlight
{
    QMutex mutex;
    QWaitCondition finished;
    bool running = true;
};
QMutex thumbnailFlightsMutex;
QHash<QString, std::shared_ptr<ThumbnailFlight>> thumbnailFlights;

struct ThumbnailFlightCompletion
{
    QString key;
    std::shared_ptr<ThumbnailFlight> flight;
    ~ThumbnailFlightCompletion()
    {
        {
            QMutexLocker lock(&flight->mutex);
            flight->running = false;
            flight->finished.wakeAll();
        }
        QMutexLocker lock(&thumbnailFlightsMutex);
        if (thumbnailFlights.value(key) == flight) thumbnailFlights.remove(key);
    }
};

struct ThumbnailProcessSlot
{
    bool acquired = false;
    ~ThumbnailProcessSlot() { if (acquired) thumbnailProcessSlots.release(); }
};

bool canBatchVideoFrames(const QString& path, double fps, JobContext* job)
{
    QString ffprobe = qEnvironmentVariable("CGPLAY_TEST_FFPROBE");
    if (ffprobe.isEmpty()) {
        const QString ffmpeg = MediaProbe::locateFfmpeg();
        if (!ffmpeg.isEmpty()) {
            ffprobe = QFileInfo(ffmpeg).dir().filePath(
                QFileInfo(ffmpeg).suffix().compare(QStringLiteral("exe"), Qt::CaseInsensitive) == 0
                    ? QStringLiteral("ffprobe.exe")
                    : QStringLiteral("ffprobe"));
        }
    }
    if (ffprobe.isEmpty()) {
        ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    }
    if (ffprobe.isEmpty() || (job && job->shouldStop())) {
        return false;
    }

    QProcess probe;
    probe.setProcessChannelMode(QProcess::MergedChannels);
    probe.start(ffprobe, {
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-select_streams"), QStringLiteral("v:0"),
        QStringLiteral("-show_entries"), QStringLiteral("stream=avg_frame_rate,r_frame_rate,nb_frames"),
        QStringLiteral("-of"), QStringLiteral("json"), path
    }, QIODevice::ReadOnly);
    JobContext localJob(5000);
    JobContext& context = job ? *job : localJob;
    const ProcessOutcome outcome = context.waitForProcess(probe, 25, 5000);
    if (!outcome.succeeded() || (job && job->shouldStop())) {
        return false;
    }

    const QJsonArray streams = QJsonDocument::fromJson(outcome.standardOutput)
        .object().value(QStringLiteral("streams")).toArray();
    if (streams.isEmpty()) {
        return false;
    }
    const QJsonObject stream = streams.first().toObject();
    bool okFrames = false;
    const int frameCount = stream.value(QStringLiteral("nb_frames")).toString().toInt(&okFrames);
    const auto parseRatio = [](const QString& value) {
        const QStringList parts = value.split(QLatin1Char('/'));
        bool okA = false;
        bool okB = false;
        const double a = parts.value(0).toDouble(&okA);
        const double b = parts.size() > 1 ? parts.value(1).toDouble(&okB) : 1.0;
        return okA && (parts.size() == 1 || okB) && b > 0.0 ? a / b : 0.0;
    };
    const double averageFps = parseRatio(stream.value(QStringLiteral("avg_frame_rate")).toString());
    const double nominalFps = parseRatio(stream.value(QStringLiteral("r_frame_rate")).toString());
    if (!okFrames || frameCount <= 0 || averageFps <= 0.0 || nominalFps <= 0.0 || fps <= 0.0) {
        return false;
    }
    const auto closeEnough = [](double a, double b) {
        return std::abs(a - b) <= std::max(0.01, std::max(a, b) * 0.001);
    };
    // The batch filter selects decoded frame numbers. Only use it when the
    // source is confirmed CFR and the caller's timeline FPS matches it;
    // otherwise retain the timestamp-based seek fallback for VFR/overrides.
    return closeEnough(averageFps, nominalFps) && closeEnough(averageFps, fps);
}

bool buildVideoThumbnailBatch(
    const QString& path,
    const QVector<int>& frames,
    const QSize& size,
    double fps,
    JobContext* job,
    const ThumbnailDiskCache::Policy& diskPolicy)
{
    if (frames.size() < 2 || (job && job->shouldStop())) {
        return false;
    }

    const QString ffmpeg = MediaProbe::locateFfmpeg();
    if (ffmpeg.isEmpty()) {
        return false;
    }

    std::vector<std::shared_ptr<ThumbnailDiskCache::Lease>> leases;
    QVector<QString> destinations;
    for (const int frame : frames) {
        const QString destination = cachePathForVideoFrame(path, frame, size, fps, diskPolicy);
        auto lease = ThumbnailDiskCache::acquire(destination, [job] { return job && job->shouldStop(); });
        if (!lease) return false;
        destinations.push_back(destination);
        leases.push_back(std::move(lease));
    }

    bool acquired = false;
    while (!acquired) {
        if (job && job->shouldStop()) return false;
        acquired = thumbnailProcessSlots.tryAcquire(1, 50);
    }

    QTemporaryDir tempDir(QDir::tempPath() + QStringLiteral("/cgplay_media_thumb_batch_XXXXXX"));
    if (!tempDir.isValid()) {
        thumbnailProcessSlots.release();
        return false;
    }

    QStringList selection;
    selection.reserve(frames.size());
    for (const int frame : frames) {
        // QProcess passes this as one argument; the escaped comma is for the
        // ffmpeg expression parser, not for a shell.
        selection.push_back(QStringLiteral("eq(n\\,%1)").arg(std::max(0, frame)));
    }
    const QString filter = QStringLiteral("select=%1,scale=%2:%3:force_original_aspect_ratio=increase,crop=%2:%3")
        .arg(selection.join(QLatin1Char('+')))
        .arg(size.width())
        .arg(size.height());
    const QString outputPattern = QDir(tempDir.path()).filePath(QStringLiteral("thumb_%08d.jpg"));

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(ffmpeg, {
        QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-y"), QStringLiteral("-i"), path,
        QStringLiteral("-vf"), filter,
        QStringLiteral("-vsync"), QStringLiteral("0"),
        QStringLiteral("-frames:v"), QString::number(frames.size()),
        QStringLiteral("-q:v"), QStringLiteral("2"), outputPattern
    }, QIODevice::ReadOnly);
    JobContext localJob(30000);
    JobContext& context = job ? *job : localJob;
    const ProcessOutcome outcome = context.waitForProcess(process, 25, 30000);
    thumbnailProcessSlots.release();
    if (!outcome.succeeded() || (job && job->shouldStop())) {
        return false;
    }

    QVector<QString> generated;
    generated.reserve(frames.size());
    for (int i = 0; i < frames.size(); ++i) {
        if (job && job->shouldStop()) {
            return false;
        }
        const QString generatedPath = QDir(tempDir.path()).filePath(
            QStringLiteral("thumb_%1.jpg").arg(i + 1, 8, 10, QChar('0')));
        QImage image;
        if (!QFileInfo::exists(generatedPath) || !image.load(generatedPath)) {
            return false;
        }
        generated.push_back(generatedPath);
    }

    // Move only complete JPEGs into the regular per-frame cache. A race with
    // another worker is harmless: the first complete cache wins.
    for (int i = 0; i < frames.size(); ++i) {
        if (job && job->shouldStop()) {
            return false;
        }
        const QString& destination = destinations[i];
        if (QFileInfo::exists(destination)) {
            continue;
        }
        if (!QFile::rename(generated[i], destination) && !QFileInfo::exists(destination)) {
            return false;
        }
    }
    ThumbnailDiskCache::schedulePrune(diskPolicy);
    return true;
}

QImage loadVideoFrame(
    const QString& path,
    int frame,
    double fps,
    const QSize& size,
    JobContext* job,
    const ThumbnailDiskCache::Policy& diskPolicy)
{
    const QString ffmpeg = MediaProbe::locateFfmpeg();
    if (ffmpeg.isEmpty()) return {};
    JobContext localJob(9000);
    JobContext& context = job ? *job : localJob;
    const int safeFrame = std::max(0, frame);
    const double safeFps = fps > 0.0 ? fps : 24.0;
    const QString jpg = cachePathForVideoFrame(path, safeFrame, size, safeFps, diskPolicy);
    auto lease = ThumbnailDiskCache::acquire(jpg, [&context] { return context.shouldStop(); });
    if (!lease) return {};

    for (;;) {
        if (context.shouldStop()) return {};
        QImage cached;
        if (cached.load(jpg) && !cached.isNull()) { ThumbnailDiskCache::touch(jpg); return cached; }

        std::shared_ptr<ThumbnailFlight> flight;
        bool producer = false;
        {
            QMutexLocker lock(&thumbnailFlightsMutex);
            flight = thumbnailFlights.value(jpg);
            if (!flight) {
                flight = std::make_shared<ThumbnailFlight>();
                thumbnailFlights.insert(jpg, flight);
                producer = true;
            }
        }
        if (!producer) {
            QMutexLocker lock(&flight->mutex);
            while (flight->running) {
                if (context.shouldStop()) return {};
                flight->finished.wait(&flight->mutex, 50);
            }
            // A canceled/failed producer must not cancel a live consumer.
            // Recheck the cache and let one surviving request retry.
            continue;
        }

        ThumbnailFlightCompletion completion{jpg, flight};
        // Another process may have committed a complete thumbnail meanwhile.
        if (cached.load(jpg) && !cached.isNull()) { ThumbnailDiskCache::touch(jpg); return cached; }
        ThumbnailProcessSlot slot;
        while (!slot.acquired) {
            if (context.shouldStop()) return {};
            slot.acquired = thumbnailProcessSlots.tryAcquire(1, 50);
        }

        QTemporaryFile temporary(QFileInfo(jpg).dir().filePath(QStringLiteral(".cgplay-thumb-XXXXXX.jpg")));
        if (!temporary.open()) return {};
        const QString temporaryPath = temporary.fileName();
        temporary.close();
        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        const QString ss = QString::number(static_cast<double>(safeFrame) / safeFps, 'f', 3);
        process.start(ffmpeg, {
            QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-y"), QStringLiteral("-ss"), ss, QStringLiteral("-i"), path,
            QStringLiteral("-frames:v"), QStringLiteral("1"),
            QStringLiteral("-vf"),
            QStringLiteral("scale=%1:%2:force_original_aspect_ratio=increase,crop=%1:%2")
                .arg(size.width()).arg(size.height()),
            QStringLiteral("-q:v"), QStringLiteral("2"), temporaryPath
        }, QIODevice::ReadOnly);
        const ProcessOutcome outcome = context.waitForProcess(process, 25, 9000);
        if (!outcome.succeeded() || context.shouldStop()) return {};
        QImage image;
        if (!image.load(temporaryPath) || image.isNull()) return {};

        QFile input(temporaryPath);
        QSaveFile output(jpg);
        output.setDirectWriteFallback(false);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) return {};
        while (!input.atEnd()) {
            if (context.shouldStop()) return {};
            const QByteArray block = input.read(64 * 1024);
            if (block.isEmpty() && input.error() != QFileDevice::NoError) return {};
            if (output.write(block) != block.size()) return {};
        }
        if (context.shouldStop()) return {};
        if (!output.commit()) return {};
        ThumbnailDiskCache::schedulePrune(diskPolicy);
        return image;
    }
}

int frameAt(int index, int count, int totalFrames)
{
    if (count <= 1 || totalFrames <= 1) {
        return 0;
    }
    return static_cast<int>(std::round(
        static_cast<double>(index) * static_cast<double>(totalFrames - 1) /
        static_cast<double>(count - 1)));
}

int remapDisplayFrameToSequenceIndex(int displayFrame, int displayTotal, int sequenceCount)
{
    if (sequenceCount <= 1 || displayTotal <= 1) {
        return 0;
    }
    const int clampedFrame = std::clamp(displayFrame, 0, displayTotal - 1);
    const double normalized = static_cast<double>(clampedFrame) / static_cast<double>(displayTotal - 1);
    return std::clamp(
        static_cast<int>(std::round(normalized * static_cast<double>(sequenceCount - 1))),
        0,
        sequenceCount - 1);
}

} // namespace

QImage ThumbnailService::makePreviewImage(
    const QImage& source,
    bool sourceIsLinear,
    const QSize& targetSize,
    const OcioManager::PreviewTransformSettings& previewSettings)
{
    if (source.isNull() || !targetSize.isValid()) {
        return {};
    }

    QImage working = sourceIsLinear
        ? source.convertToFormat(QImage::Format_RGBA32FPx4)
        : source.convertToFormat(QImage::Format_RGB32);

    if (working.isNull()) {
        return {};
    }

    if (previewSettings.enabled && !OcioManager::applyPreviewTransform(working, previewSettings)) {
        qWarning() << "[ThumbnailService] Preview color transform failed";
        return {};
    }
    // Disabled OCIO keeps the existing identity/clamp preview policy. Quantize
    // only here, after any configured display mapping has processed float RGB.
    working = working.convertToFormat(QImage::Format_RGB32);

    working = working.scaled(targetSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QRect crop(
        std::max(0, (working.width() - targetSize.width()) / 2),
        std::max(0, (working.height() - targetSize.height()) / 2),
        targetSize.width(),
        targetSize.height());
    return working.copy(crop);
}

QIcon ThumbnailService::makePlaylistIcon(
    const QString& path,
    const OcioManager::PreviewTransformSettings& previewSettings,
    const MediaInfo* mediaInfo,
    JobContext* job)
{
    const QImage preview = makePlaylistImage(path, previewSettings, mediaInfo, job);
    return preview.isNull() ? QIcon() : QIcon(QPixmap::fromImage(preview));
}

QImage ThumbnailService::makePlaylistImage(
    const QString& path,
    const OcioManager::PreviewTransformSettings& previewSettings,
    const MediaInfo* mediaInfo,
    JobContext* job)
{
    static const QSize playlistSize(160, 96);
    if (path.startsWith(QStringLiteral(":/cgplay/"))) {
        QImage image(path);
        return image.isNull()
            ? QImage{}
            : image.scaled(playlistSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }

    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return {};
    }

    QImage image;
    bool linear = false;
    if (MediaProbe::isStillImagePath(path)) {
        const auto sequence = MediaProbe::collectSequenceFiles(path);
        const QString stillPath = sequence.isEmpty()
            ? path
            : sequence[sequence.size() / 2].path;
        image = loadStillImage(stillPath, job);
        linear = QFileInfo(stillPath).suffix().compare(QStringLiteral("exr"), Qt::CaseInsensitive) == 0;
    } else {
        const MediaInfo probedInfo = mediaInfo ? MediaInfo{} : MediaProbe::probe(path, 0.0, job);
        const MediaInfo& info = mediaInfo ? *mediaInfo : probedInfo;
        const int sampleFrame = std::max(0, std::min(info.effectiveFrameCount() / 4, info.effectiveFrameCount() - 1));
        image = loadVideoFrame(path, sampleFrame, info.fps, playlistSize, job, ThumbnailDiskCache::policy());
    }

    return makePreviewImage(image, linear, playlistSize, previewSettings);
}

QVector<ThumbnailFrame> ThumbnailService::buildTimelineThumbnails(
    const QString& path,
    int totalFrames,
    double fps,
    int count,
    const OcioManager::PreviewTransformSettings& previewSettings,
    JobContext* job)
{
    QVector<ThumbnailFrame> out;
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return out;
    }

    static const QSize timelineSize(112, 64);
    totalFrames = std::max(1, totalFrames);
    fps = fps > 0.0 ? fps : 24.0;
    count = std::max(1, std::min({count, 24, totalFrames}));
    out.reserve(count);

    if (MediaProbe::isStillImagePath(path)) {
        const auto sequence = MediaProbe::collectSequenceFiles(path);
        if (!sequence.isEmpty()) {
            std::vector<ImageLoadRequest> requests;
            QVector<int> displayFrames;
            requests.reserve(count);
            displayFrames.reserve(count);
            for (int i = 0; i < count; ++i) {
                const int displayFrame = frameAt(i, count, totalFrames);
                const int sampleIndex = remapDisplayFrameToSequenceIndex(
                    displayFrame, totalFrames, sequence.size());
                ImageLoadRequest req;
                req.filePath = sequence[sampleIndex].path;
                req.frame = sequence[sampleIndex].frameNumber;
                requests.push_back(req);
                displayFrames.push_back(displayFrame);
            }

            // This function already runs on the thumbnail worker.  Queuing a
            // batch back onto the same bounded pool and then waiting can
            // deadlock when all workers are occupied by timeline requests.
            // Decode directly on the current worker instead.
            for (int i = 0; i < requests.size(); ++i) {
                if (job && job->shouldStop()) {
                    return out;
                }
                const ImageLoadResult result = AsyncImageLoader::loadSync(requests[static_cast<size_t>(i)]);
                const QString framePath = requests[static_cast<size_t>(i)].filePath;
                const QImage preview = makePreviewImage(
                    result.image,
                    QFileInfo(framePath).suffix().compare(QStringLiteral("exr"), Qt::CaseInsensitive) == 0,
                    timelineSize,
                    previewSettings);
                if (!preview.isNull()) {
                    out.push_back({displayFrames[i], preview});
                }
            }
        } else {
            const QImage still = loadStillImage(path, job);
            const QImage preview = makePreviewImage(
                still,
                QFileInfo(path).suffix().compare(QStringLiteral("exr"), Qt::CaseInsensitive) == 0,
                timelineSize,
                previewSettings);
            if (!preview.isNull()) {
                for (int i = 0; i < count; ++i) {
                    out.push_back({frameAt(i, count, totalFrames), preview});
                }
            }
        }
        return out;
    }

    QVector<int> displayFrames;
    displayFrames.reserve(count);
    for (int i = 0; i < count; ++i) {
        displayFrames.push_back(frameAt(i, count, totalFrames));
    }
    const auto diskPolicy = ThumbnailDiskCache::policy();

    // For short clips, one bounded ffmpeg decode is substantially cheaper than
    // starting a process for every seek. Long clips keep the seek-per-frame
    // path so a 2-hour file is never decoded from frame zero just for thumbs.
    if (totalFrames <= 1800 && displayFrames.size() >= 2 && canBatchVideoFrames(path, fps, job)) {
        QVector<int> missingFrames;
        missingFrames.reserve(displayFrames.size());
        for (const int displayFrame : displayFrames) {
            if (!QFileInfo::exists(cachePathForVideoFrame(path, displayFrame, timelineSize, fps, diskPolicy))) {
                missingFrames.push_back(displayFrame);
            }
        }
        if (missingFrames.size() >= 2) {
            const bool batched = buildVideoThumbnailBatch(path, missingFrames, timelineSize, fps, job, diskPolicy);
            if (job && job->shouldStop()) {
                return out;
            }
            Q_UNUSED(batched);
        }
    }

    for (const int displayFrame : displayFrames) {
        if (job && job->shouldStop()) {
            break;
        }
        const QImage frame = loadVideoFrame(path, displayFrame, fps, timelineSize, job, diskPolicy);
        const QImage preview = makePreviewImage(frame, false, timelineSize, previewSettings);
        if (!preview.isNull()) {
            out.push_back({displayFrame, preview});
        }
    }

    return out;
}

} // namespace cgplay
