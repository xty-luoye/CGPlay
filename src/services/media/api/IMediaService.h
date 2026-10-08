#pragma once

#include "media/MediaProbe.h"
#include "media/ThumbnailService.h"

namespace cgplay {

class IMediaService
{
public:
    virtual ~IMediaService() = default;

    virtual MediaInfo probe(const QString& path, double fpsOverride = 0.0) const = 0;
    virtual bool isStillImagePath(const QString& path) const = 0;
    virtual bool isVideoPath(const QString& path) const = 0;
    virtual QString formatLabelForPath(const QString& path) const = 0;
    virtual QString displayNameForPath(const QString& path) const = 0;
    virtual QVector<SequenceFrame> collectSequenceFiles(const QString& path) const = 0;
    virtual QString locateFfmpeg() const = 0;
    virtual QString locateFfprobe() const = 0;
    virtual double parseFpsRatio(const QString& value) const = 0;

    virtual QImage makePreviewImage(
        const QImage& source,
        bool sourceIsLinear,
        const QSize& targetSize,
        const OcioManager::PreviewTransformSettings& previewSettings) const = 0;

    virtual QIcon makePlaylistIcon(
        const QString& path,
        const OcioManager::PreviewTransformSettings& previewSettings) const = 0;

    virtual QVector<ThumbnailFrame> buildTimelineThumbnails(
        const QString& path,
        int totalFrames,
        double fps,
        int count,
        const OcioManager::PreviewTransformSettings& previewSettings) const = 0;
};

} // namespace cgplay
