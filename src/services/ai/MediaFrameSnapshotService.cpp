#include "MediaFrameSnapshotService.h"

#include "common/core/ServiceLocator.h"
#include "core/playback/api/IPlaybackService.h"
#include "services/media/api/IMediaService.h"
#include "ui/viewer/CompareToolbar.h"
#include "ui/viewer/api/IActivePlaybackView.h"
#include "ui/viewer/api/IOverlayHost.h"
#include "ui/viewer/api/IViewerCoordinateMapper.h"

#include <QBuffer>
#include <QImage>
#include <QMetaObject>
#include <QOpenGLWidget>
#include <QPixmap>
#include <QRectF>
#include <QThread>
#include <QWidget>

namespace cgplay {

namespace {

QString normalizedEncoding(const QString& encoding)
{
    return encoding.trimmed().isEmpty()
        ? QStringLiteral("PNG")
        : encoding.trimmed().toUpper();
}

QString mimeTypeForEncoding(const QString& encoding)
{
    QString mimeType = QStringLiteral("image/%1").arg(encoding.toLower());
    if (mimeType == QStringLiteral("image/jpg")) {
        mimeType = QStringLiteral("image/jpeg");
    }
    return mimeType;
}

QString compareModeName(int mode)
{
    switch (mode) {
    case 0: return QStringLiteral("a-only");
    case 1: return QStringLiteral("b-only");
    case 2: return QStringLiteral("wipe");
    case 3: return QStringLiteral("overlay");
    case 4: return QStringLiteral("difference");
    case 5: return QStringLiteral("horizontal-split");
    case 6: return QStringLiteral("vertical-split");
    case 7: return QStringLiteral("tile");
    default: return QStringLiteral("unknown");
    }
}

QString compareLayoutSummary(int mode)
{
    switch (mode) {
    case 5: return QStringLiteral("A is on the left, B is on the right");
    case 6: return QStringLiteral("A is on the top, B is on the bottom");
    case 7: return QStringLiteral("A and B are tiled side-by-side or top-bottom depending on aspect ratio");
    case 3: return QStringLiteral("A and B are overlaid in the same area");
    case 4: return QStringLiteral("A and B are shown as a difference composite");
    case 2: return QStringLiteral("A and B are shown by a wipe control");
    case 1: return QStringLiteral("Only B is currently visible");
    case 0: return QStringLiteral("Only A is currently visible");
    default: return QStringLiteral("Compare layout unknown");
    }
}

bool encodeFrameImage(
    const QImage& image,
    const QString& mediaPath,
    int frame,
    const QString& encoding,
    const QJsonObject& metadata,
    QVector<AIMediaFrameReference>* outFrames,
    QString* error)
{
    if (!outFrames) {
        if (error) {
            *error = QStringLiteral("Snapshot output buffer is null");
        }
        return false;
    }
    if (image.isNull()) {
        if (error) {
            *error = QStringLiteral("Captured compare image is empty");
        }
        return false;
    }

    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, encoding.toUtf8().constData())) {
        if (error) {
            *error = QStringLiteral("Unable to encode AI frame snapshot as %1").arg(encoding);
        }
        return false;
    }

    AIMediaFrameReference reference;
    reference.mediaPath = mediaPath;
    reference.frame = frame;
    reference.encodedBytes = bytes;
    reference.mimeType = mimeTypeForEncoding(encoding);
    reference.size = image.size();
    reference.metadata = metadata;
    reference.metadata.insert(QStringLiteral("encoding"), encoding);
    outFrames->push_back(reference);
    return true;
}

CompareToolbar* locateCompareToolbar(QWidget* widget)
{
    if (!widget) {
        return nullptr;
    }
    QWidget* window = widget->window();
    return window ? window->findChild<CompareToolbar*>() : nullptr;
}

bool captureWidgetImage(QWidget* widget, QImage* outImage, QString* error)
{
    if (!widget || !outImage) {
        if (error) {
            *error = QStringLiteral("Active playback widget is unavailable");
        }
        return false;
    }

    QImage capturedImage;
    QString captureError;
    const auto capture = [&]() {
        if (widget->size().isEmpty()) {
            captureError = QStringLiteral("Active playback widget has no size");
            return;
        }

        if (auto* glWidget = qobject_cast<QOpenGLWidget*>(widget)) {
            capturedImage = glWidget->grabFramebuffer();
        } else {
            capturedImage = widget->grab().toImage();
        }

        if (capturedImage.isNull()) {
            captureError = QStringLiteral("Unable to capture the active playback view");
        }
    };

    if (QThread::currentThread() == widget->thread()) {
        capture();
    } else {
        QMetaObject::invokeMethod(widget, capture, Qt::BlockingQueuedConnection);
    }

    if (capturedImage.isNull()) {
        if (error) {
            *error = captureError.isEmpty()
                ? QStringLiteral("Unable to capture the active playback view")
                : captureError;
        }
        return false;
    }

    *outImage = capturedImage;
    return true;
}

QRect scaledCropRect(const QRect& logicalRect, const QSize& widgetSize, const QSize& imageSize)
{
    if (logicalRect.isEmpty() || !widgetSize.isValid() || !imageSize.isValid()) {
        return QRect();
    }

    const qreal scaleX = widgetSize.width() > 0
        ? static_cast<qreal>(imageSize.width()) / static_cast<qreal>(widgetSize.width())
        : 1.0;
    const qreal scaleY = widgetSize.height() > 0
        ? static_cast<qreal>(imageSize.height()) / static_cast<qreal>(widgetSize.height())
        : 1.0;

    return QRect(
        qRound(logicalRect.x() * scaleX),
        qRound(logicalRect.y() * scaleY),
        qRound(logicalRect.width() * scaleX),
        qRound(logicalRect.height() * scaleY));
}

bool captureActiveViewImage(
    const AIFrameCaptureRequest& request,
    QImage* outImage,
    QJsonObject* outMetadata,
    QString* error)
{
    auto* overlayHost = ServiceLocator::getService<IOverlayHost>();
    if (!overlayHost) {
        if (error) {
            *error = QStringLiteral("Active overlay host is unavailable");
        }
        return false;
    }

    QWidget* widget = overlayHost->overlayParentWidget();
    if (!widget) {
        if (error) {
            *error = QStringLiteral("Active playback widget is unavailable");
        }
        return false;
    }

    QImage capturedImage;
    if (!captureWidgetImage(widget, &capturedImage, error)) {
        return false;
    }

    QJsonObject metadata;
    metadata.insert(QStringLiteral("captureMode"), QStringLiteral("active-view"));
    metadata.insert(QStringLiteral("captureSource"), QStringLiteral("viewport"));
    metadata.insert(QStringLiteral("widgetWidth"), widget->width());
    metadata.insert(QStringLiteral("widgetHeight"), widget->height());
    metadata.insert(QStringLiteral("sampleCountRequested"), request.sampleCount);

    if (auto* activeView = ServiceLocator::getService<IActivePlaybackView>()) {
        metadata.insert(QStringLiteral("activeViewId"), activeView->activeViewId());
    }

    QRect cropRect = capturedImage.rect();
    if (auto* mapper = ServiceLocator::getService<IViewerCoordinateMapper>()) {
        const QRect widgetRect(QPoint(0, 0), widget->size());
        const QRect logicalCropRect =
            mapper->visibleVideoRect().toAlignedRect().intersected(widgetRect);
        if (!logicalCropRect.isEmpty()) {
            const QRect scaledRect =
                scaledCropRect(logicalCropRect, widget->size(), capturedImage.size())
                    .intersected(capturedImage.rect());
            if (!scaledRect.isEmpty()) {
                cropRect = scaledRect;
                metadata.insert(QStringLiteral("croppedToVisibleVideo"), true);
                metadata.insert(QStringLiteral("logicalCropX"), logicalCropRect.x());
                metadata.insert(QStringLiteral("logicalCropY"), logicalCropRect.y());
                metadata.insert(QStringLiteral("logicalCropWidth"), logicalCropRect.width());
                metadata.insert(QStringLiteral("logicalCropHeight"), logicalCropRect.height());
            }
        }
        metadata.insert(QStringLiteral("zoom"), mapper->zoom());
        metadata.insert(QStringLiteral("viewPosX"), mapper->viewPos().x());
        metadata.insert(QStringLiteral("viewPosY"), mapper->viewPos().y());
    }

    if (cropRect != capturedImage.rect()) {
        capturedImage = capturedImage.copy(cropRect);
    }

    if (capturedImage.isNull()) {
        if (error) {
            *error = QStringLiteral("Captured frame became empty after cropping");
        }
        return false;
    }

    if (outImage) {
        *outImage = capturedImage;
    }
    if (outMetadata) {
        *outMetadata = metadata;
    }
    return true;
}

} // namespace

bool MediaFrameSnapshotService::capture(
    const AIFrameCaptureRequest& request,
    QVector<AIMediaFrameReference>* outFrames,
    QString* error) const
{
    if (!outFrames) {
        if (error) {
            *error = QStringLiteral("Snapshot output buffer is null");
        }
        return false;
    }

    outFrames->clear();

    QString mediaPath = request.mediaPath.trimmed();
    int frame = request.currentFrame;
    if (mediaPath.isEmpty()) {
        if (auto* playback = ServiceLocator::getService<IPlaybackService>()) {
            mediaPath = playback->currentPath();
            frame = playback->currentFrame();
        }
    }

    if (mediaPath.isEmpty()) {
        if (error) {
            *error = QStringLiteral("No media path is available for AI frame capture");
        }
        return false;
    }

    auto* mediaService = ServiceLocator::getService<IMediaService>();
    if (!mediaService) {
        if (error) {
            *error = QStringLiteral("IMediaService is unavailable");
        }
        return false;
    }

    QImage image;
    QJsonObject metadata;
    QString activeViewCaptureError;
    if (captureActiveViewImage(request, &image, &metadata, &activeViewCaptureError)) {
        metadata.insert(QStringLiteral("mediaPathResolved"), mediaPath);
        metadata.insert(QStringLiteral("frameResolved"), frame);
    } else if (mediaService->isStillImagePath(mediaPath)) {
        image = QImage(mediaPath);
        if (image.isNull()) {
            if (error) {
                *error = QStringLiteral("Unable to load still image for AI frame capture: %1").arg(mediaPath);
            }
            return false;
        }
        metadata.insert(QStringLiteral("captureMode"), QStringLiteral("still-image"));
        metadata.insert(QStringLiteral("captureSource"), QStringLiteral("file"));
        metadata.insert(QStringLiteral("mediaPathResolved"), mediaPath);
        metadata.insert(QStringLiteral("frameResolved"), frame);
        if (!activeViewCaptureError.isEmpty()) {
            metadata.insert(QStringLiteral("activeViewCaptureFallback"), activeViewCaptureError);
        }
    } else {
        if (error) {
            *error = activeViewCaptureError.isEmpty()
                ? QStringLiteral("Video or sequence frame capture requires an active playback view")
                : activeViewCaptureError;
        }
        return false;
    }

    if (request.targetSize.isValid()) {
        image = image.scaled(
            request.targetSize,
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation);
        metadata.insert(QStringLiteral("targetWidth"), request.targetSize.width());
        metadata.insert(QStringLiteral("targetHeight"), request.targetSize.height());
    }

    const QString encoding = normalizedEncoding(request.encoding);
    const bool compareRequest = request.scope == AIRequestScope::CompareAB;
    auto* playbackService = ServiceLocator::getService<IPlaybackService>();
    const bool hasCompare =
        compareRequest &&
        playbackService &&
        playbackService->hasCompare();

    if (!compareRequest || !hasCompare) {
        return encodeFrameImage(image, mediaPath, frame, encoding, metadata, outFrames, error);
    }

    QWidget* overlayWidget = nullptr;
    if (auto* overlayHost = ServiceLocator::getService<IOverlayHost>()) {
        overlayWidget = overlayHost->overlayParentWidget();
    }
    CompareToolbar* compareToolbar = locateCompareToolbar(overlayWidget);
    const int compareMode = compareToolbar ? compareToolbar->compareMode() : -1;
    const QString compareALabel =
        compareToolbar ? compareToolbar->shotALabel().trimmed() : QString();
    const QString compareBLabel =
        compareToolbar ? compareToolbar->shotBLabel().trimmed() : QString();

    QJsonObject fullMetadata = metadata;
    fullMetadata.insert(QStringLiteral("compareMode"), compareMode);
    fullMetadata.insert(QStringLiteral("compareModeName"), compareModeName(compareMode));
    fullMetadata.insert(QStringLiteral("compareLayoutSummary"), compareLayoutSummary(compareMode));
    if (!compareALabel.isEmpty()) {
        fullMetadata.insert(QStringLiteral("compareALabel"), compareALabel);
    }
    if (!compareBLabel.isEmpty()) {
        fullMetadata.insert(QStringLiteral("compareBLabel"), compareBLabel);
    }
    fullMetadata.insert(QStringLiteral("compareRole"), QStringLiteral("compare-full"));
    if (!encodeFrameImage(image, mediaPath, frame, encoding, fullMetadata, outFrames, error)) {
        return false;
    }

    QRect aRect;
    QRect bRect;
    QString aSegment;
    QString bSegment;

    switch (compareMode) {
    case 5:
        aRect = QRect(0, 0, image.width() / 2, image.height());
        bRect = QRect(image.width() / 2, 0, image.width() - image.width() / 2, image.height());
        aSegment = QStringLiteral("left");
        bSegment = QStringLiteral("right");
        break;
    case 6:
        aRect = QRect(0, 0, image.width(), image.height() / 2);
        bRect = QRect(0, image.height() / 2, image.width(), image.height() - image.height() / 2);
        aSegment = QStringLiteral("top");
        bSegment = QStringLiteral("bottom");
        break;
    case 7:
        if (image.width() >= image.height()) {
            aRect = QRect(0, 0, image.width() / 2, image.height());
            bRect = QRect(image.width() / 2, 0, image.width() - image.width() / 2, image.height());
            aSegment = QStringLiteral("left");
            bSegment = QStringLiteral("right");
        } else {
            aRect = QRect(0, 0, image.width(), image.height() / 2);
            bRect = QRect(0, image.height() / 2, image.width(), image.height() - image.height() / 2);
            aSegment = QStringLiteral("top");
            bSegment = QStringLiteral("bottom");
        }
        break;
    default:
        return true;
    }

    aRect = aRect.intersected(image.rect());
    bRect = bRect.intersected(image.rect());
    if (aRect.isEmpty() || bRect.isEmpty()) {
        return true;
    }

    QJsonObject aMetadata = fullMetadata;
    aMetadata.insert(QStringLiteral("compareRole"), QStringLiteral("compare-a"));
    aMetadata.insert(QStringLiteral("compareSegment"), aSegment);
    aMetadata.insert(QStringLiteral("compareCropX"), aRect.x());
    aMetadata.insert(QStringLiteral("compareCropY"), aRect.y());
    aMetadata.insert(QStringLiteral("compareCropWidth"), aRect.width());
    aMetadata.insert(QStringLiteral("compareCropHeight"), aRect.height());

    QJsonObject bMetadata = fullMetadata;
    bMetadata.insert(QStringLiteral("compareRole"), QStringLiteral("compare-b"));
    bMetadata.insert(QStringLiteral("compareSegment"), bSegment);
    bMetadata.insert(QStringLiteral("compareCropX"), bRect.x());
    bMetadata.insert(QStringLiteral("compareCropY"), bRect.y());
    bMetadata.insert(QStringLiteral("compareCropWidth"), bRect.width());
    bMetadata.insert(QStringLiteral("compareCropHeight"), bRect.height());

    if (!encodeFrameImage(image.copy(aRect), mediaPath, frame, encoding, aMetadata, outFrames, error)) {
        return false;
    }
    if (!encodeFrameImage(image.copy(bRect), mediaPath, frame, encoding, bMetadata, outFrames, error)) {
        return false;
    }
    return true;
}

} // namespace cgplay
