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
    virtual int mediaWidth() const = 0;
    virtual int mediaHeight() const = 0;

    virtual QString selectedAnnotationId() const = 0;
    virtual void setInteractionHandlers(
        std::function<void(const AnnotationItem&)> onCreated,
        std::function<void(const QString&)> onSelected,
        std::function<void(const QString&)> onDeleteRequested) = 0;
};

} // namespace cgplay

#define CGPLAY_IANNOTATIONVIEWBRIDGE_IID "com.cgplay.IAnnotationViewBridge"
Q_DECLARE_INTERFACE(cgplay::IAnnotationViewBridge, CGPLAY_IANNOTATIONVIEWBRIDGE_IID)
