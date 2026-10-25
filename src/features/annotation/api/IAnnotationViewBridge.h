#pragma once

#include "annotation/AnnotationItem.h"

#include <QtPlugin>

#include <functional>

namespace cgplay {

class IAnnotationViewBridge
{
public:
    virtual ~IAnnotationViewBridge() = default;

    virtual int currentFrame() const = 0;
    virtual double mediaFps() const = 0; // 0 = no known media rate
    virtual int mediaWidth() const = 0;
    virtual int mediaHeight() const = 0;

    virtual QString selectedAnnotationId() const = 0;
    virtual void setInteractionHandlers(
        std::function<void(const AnnotationItem&)> onCreated,
        std::function<void(const QString&)> onSelected,
        std::function<void(const QString&)> onDeleteRequested) = 0;
};

} // namespace cgplay

#define CGPLAY_IANNOTATIONVIEWBRIDGE_IID "com.cgplay.IAnnotationViewBridge/2.0"
Q_DECLARE_INTERFACE(cgplay::IAnnotationViewBridge, CGPLAY_IANNOTATIONVIEWBRIDGE_IID)
