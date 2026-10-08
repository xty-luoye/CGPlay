#pragma once

#include <QRect>
#include <QtPlugin>

class QWidget;

namespace cgplay {

class IOverlayHost
{
public:
    virtual ~IOverlayHost() = default;

    virtual QWidget* overlayParentWidget() const = 0;
    virtual QRect overlayGeometry() const = 0;
    virtual void attachOverlay(QWidget* overlay) = 0;
    virtual void detachOverlay(QWidget* overlay) = 0;
    virtual void requestOverlayRefresh() = 0;
};

} // namespace cgplay

#define CGPLAY_IOVERLAYHOST_IID "com.cgplay.IOverlayHost"
Q_DECLARE_INTERFACE(cgplay::IOverlayHost, CGPLAY_IOVERLAYHOST_IID)
