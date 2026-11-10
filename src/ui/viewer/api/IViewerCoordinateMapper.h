#pragma once

#include <QPointF>
#include <QRectF>
#include <QtPlugin>

namespace cgplay {

class IViewerCoordinateMapper
{
public:
    virtual ~IViewerCoordinateMapper() = default;

    // Widget positions, extents and pixels-per-image-pixel zoom are Qt logical
    // units. Implementations handle the native renderer's device pixel ratio.
    virtual QPointF imageToWidget(const QPointF& point, int imageWidth, int imageHeight) const = 0;
    virtual QPointF widgetToImage(const QPointF& point, int imageWidth, int imageHeight) const = 0;
    virtual QRectF visibleVideoRect() const = 0;
    virtual double zoom() const = 0;
    virtual QPointF viewPos() const = 0;
};

} // namespace cgplay

#define CGPLAY_IVIEWERCOORDINATEMAPPER_IID "com.cgplay.IViewerCoordinateMapper"
Q_DECLARE_INTERFACE(cgplay::IViewerCoordinateMapper, CGPLAY_IVIEWERCOORDINATEMAPPER_IID)
