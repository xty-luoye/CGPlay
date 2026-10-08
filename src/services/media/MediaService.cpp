#include "MediaService.h"

namespace cgplay {

MediaInfo MediaService::probe(const QString& path, double fpsOverride) const
{
    return MediaProbe::probe(path, fpsOverride);
}

bool MediaService::isStillImagePath(const QString& path) const
{
    return MediaProbe::isStillImagePath(path);
}

bool MediaService::isVideoPath(const QString& path) const
{
    return MediaProbe::isVideoPath(path);
}

QString MediaService::formatLabelForPath(const QString& path) const
{
    return MediaProbe::formatLabelForPath(path);
}

QString MediaService::displayNameForPath(const QString& path) const
{
    return MediaProbe::displayNameForPath(path);
}

QVector<SequenceFrame> MediaService::collectSequenceFiles(const QString& path) const
{
    return MediaProbe::collectSequenceFiles(path);
}

QString MediaService::locateFfmpeg() const
{
    return MediaProbe::locateFfmpeg();
}

QString MediaService::locateFfprobe() const
{
    return MediaProbe::locateFfprobe();
}

double MediaService::parseFpsRatio(const QString& value) const
{
    return MediaProbe::parseFpsRatio(value);
}

QImage MediaService::makePreviewImage(
    const QImage& source,
    bool sourceIsLinear,
    const QSize& targetSize,
    const OcioManager::PreviewTransformSettings& previewSettings) const
{
    return ThumbnailService::makePreviewImage(source, sourceIsLinear, targetSize, previewSettings);
}

QIcon MediaService::makePlaylistIcon(
    const QString& path,
    const OcioManager::PreviewTransformSettings& previewSettings) const
{
    return ThumbnailService::makePlaylistIcon(path, previewSettings);
}

QVector<ThumbnailFrame> MediaService::buildTimelineThumbnails(
    const QString& path,
    int totalFrames,
    double fps,
    int count,
    const OcioManager::PreviewTransformSettings& previewSettings) const
{
    return ThumbnailService::buildTimelineThumbnails(path, totalFrames, fps, count, previewSettings);
}

} // namespace cgplay
