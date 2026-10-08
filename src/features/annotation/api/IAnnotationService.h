#pragma once

#include "annotation/AnnotationItem.h"

#include <QColor>
#include <QVector>
#include <QtPlugin>

namespace cgplay {

class IAnnotationService
{
public:
    virtual ~IAnnotationService() = default;

    virtual int count() const = 0;
    virtual QVector<AnnotationItem> annotations() const = 0;
    virtual void replaceAnnotations(const QVector<AnnotationItem>& annotations) = 0;
    virtual void clearAnnotations() = 0;

    virtual bool undo() = 0;
    virtual bool redo() = 0;

    virtual QString addAnnotation(const AnnotationItem& annotation) = 0;
    virtual bool removeAnnotation(const QString& annotationId) = 0;
    virtual bool removeSelectedAnnotation() = 0;
    virtual bool updateAnnotationComment(const QString& annotationId, const QString& text) = 0;
    virtual QString createNote(const QString& text) = 0;

    virtual void selectAnnotation(const QString& annotationId) = 0;
    virtual QString selectedAnnotationId() const = 0;

    virtual void setTool(int tool) = 0;
    virtual int currentTool() const = 0;
    virtual void setToolColor(const QColor& color) = 0;
    virtual QColor currentToolColor() const = 0;
};

} // namespace cgplay

#define CGPLAY_IANNOTATIONSERVICE_IID "com.cgplay.IAnnotationService"
Q_DECLARE_INTERFACE(cgplay::IAnnotationService, CGPLAY_IANNOTATIONSERVICE_IID)
