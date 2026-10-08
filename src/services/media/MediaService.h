#pragma once

#include "media/api/IMediaService.h"

namespace cgplay {

class MediaService : public IMediaService
{
public:
    ~MediaService() override = default;

    MediaInfo probe(const QString& path, double fpsOverride = 0.0) const override;
    bool isStillImagePath(const QString& path) const override;
    bool isVideoPath(const QString& path) const override;
    QString formatLabelForPath(const QString& path) const override;
    QString displayNameForPath(const QString& path) const override;
    QVector<SequenceFrame> collectSequenceFiles(const QString& path) const override;
    QString locateFfmpeg() const override;
    QString locateFfprobe() const override;
    double parseFpsRatio(const QString& value) const override;

    QImage makePreviewImage(
        const QImage& source,
        bool sourceIsLinear,
        const QSize& targetSize,
        const OcioManager::PreviewTransformSettings& previewSettings) const override;

    QIcon makePlaylistIcon(
        const QString& path,
        const OcioManager::PreviewTransformSettings& previewSettings) const override;

    QVector<ThumbnailFrame> buildTimelineThumbnails(
        const QString& path,
        int totalFrames,
        double fps,
        int count,
        const OcioManager::PreviewTransformSettings& previewSettings) const override;
};

} // namespace cgplay
