#pragma once

#include "ocio/OcioManager.h"

#include <QIcon>
#include <QImage>
#include <QSize>
#include <QVector>
#include <QString>

namespace cgplay {

class JobContext;
struct MediaInfo;

struct ThumbnailFrame
{
    int frame = 0;
    QImage image;
};

class ThumbnailService
{
public:
    static QImage makePreviewImage(
        const QImage& source,
        bool sourceIsLinear,
        const QSize& targetSize,
        const OcioManager::PreviewTransformSettings& previewSettings);

    static QIcon makePlaylistIcon(
        const QString& path,
        const OcioManager::PreviewTransformSettings& previewSettings,
        const MediaInfo* mediaInfo = nullptr,
        JobContext* job = nullptr);

    static QImage makePlaylistImage(
        const QString& path,
        const OcioManager::PreviewTransformSettings& previewSettings,
        const MediaInfo* mediaInfo = nullptr,
        JobContext* job = nullptr);

    static QVector<ThumbnailFrame> buildTimelineThumbnails(
        const QString& path,
        int totalFrames,
        double fps,
        int count,
        const OcioManager::PreviewTransformSettings& previewSettings,
        JobContext* job = nullptr);
};

} // namespace cgplay
