#pragma once

#include <QString>
#include <QtGlobal>
#include <QtPlugin>

namespace cgplay {

class IOverlayProvider
{
public:
    virtual ~IOverlayProvider() = default;

    virtual void bind(const QString& viewId) = 0;
    virtual void unbind(const QString& viewId) = 0;
    virtual void handleActiveViewChanged(const QString& viewId) = 0;
    virtual void handleActiveViewInvalidated(const QString& viewId) = 0;
    virtual void handleOverlayHostChanged(const QString& viewId) = 0;
    virtual void handleOverlayHostInvalidated(const QString& viewId) = 0;
    virtual void handleCoordinateMapperChanged(const QString& viewId) = 0;
    virtual void handleCoordinateMapperInvalidated(const QString& viewId) = 0;

    virtual QString boundViewId() const = 0;
    virtual bool hasOverlayBinding() const = 0;
    virtual bool hasOverlayHostBinding() const = 0;
    virtual bool hasCoordinateMapperBinding() const = 0;
    virtual int overlayCount() const = 0;
    virtual quint64 overlayInstanceId() const = 0;
};

} // namespace cgplay

#define CGPLAY_IOVERLAYPROVIDER_IID "com.cgplay.IOverlayProvider"
Q_DECLARE_INTERFACE(cgplay::IOverlayProvider, CGPLAY_IOVERLAYPROVIDER_IID)
