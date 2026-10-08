#pragma once

#include "ai/api/AIProviderTypes.h"
#include "annotation/AnnotationItem.h"
#include "ocio/OcioManager.h"

#include <QString>

namespace cgplay {

struct MediaOpenedEvent
{
    QString path;
    int totalFrames = 0;
    double fps = 0.0;

    static QString eventName() { return QStringLiteral("MediaOpenedEvent"); }
};

struct FrameChangedEvent
{
    int frame = 0;
    int total = 0;

    static QString eventName() { return QStringLiteral("FrameChangedEvent"); }
};

struct PlaybackStartedEvent
{
    static QString eventName() { return QStringLiteral("PlaybackStartedEvent"); }
};

struct PlaybackStoppedEvent
{
    static QString eventName() { return QStringLiteral("PlaybackStoppedEvent"); }
};

struct SessionLoadedEvent
{
    QString path;

    static QString eventName() { return QStringLiteral("SessionLoadedEvent"); }
};

struct AnnotationChangedEvent
{
    QVector<AnnotationItem> annotations;

    static QString eventName() { return QStringLiteral("AnnotationChangedEvent"); }
};

struct AnnotationSelectionRequestedEvent
{
    QString annotationId;

    static QString eventName() { return QStringLiteral("AnnotationSelectionRequestedEvent"); }
};

struct OcioOptionsChangedEvent
{
    tl::OCIOOptions options;
    OcioManager::PreviewTransformSettings previewSettings;

    static QString eventName() { return QStringLiteral("OcioOptionsChangedEvent"); }
};

struct OcioDisplayStateChangedEvent
{
    bool enabled = false;
    QString display;

    static QString eventName() { return QStringLiteral("OcioDisplayStateChangedEvent"); }
};

struct CacheUsageChangedEvent
{
    float videoPercent = 0.0f;
    float audioPercent = 0.0f;

    static QString eventName() { return QStringLiteral("CacheUsageChangedEvent"); }
};

struct PlaybackStatsUpdatedEvent
{
    int droppedFrames = 0;
    double currentFps = 0.0;
    double averageFps = 0.0;

    static QString eventName() { return QStringLiteral("PlaybackStatsUpdatedEvent"); }
};

struct ActiveViewChangedEvent
{
    QString viewId;

    static QString eventName() { return QStringLiteral("ActiveViewChangedEvent"); }
};

struct ActiveViewInvalidatedEvent
{
    QString viewId;

    static QString eventName() { return QStringLiteral("ActiveViewInvalidatedEvent"); }
};

struct OverlayHostChangedEvent
{
    QString viewId;

    static QString eventName() { return QStringLiteral("OverlayHostChangedEvent"); }
};

struct OverlayHostInvalidatedEvent
{
    QString viewId;

    static QString eventName() { return QStringLiteral("OverlayHostInvalidatedEvent"); }
};

struct CoordinateMapperChangedEvent
{
    QString viewId;

    static QString eventName() { return QStringLiteral("CoordinateMapperChangedEvent"); }
};

struct CoordinateMapperInvalidatedEvent
{
    QString viewId;

    static QString eventName() { return QStringLiteral("CoordinateMapperInvalidatedEvent"); }
};

struct ViewTransformChangedEvent
{
    QString viewId;

    static QString eventName() { return QStringLiteral("ViewTransformChangedEvent"); }
};

struct ViewportResizedEvent
{
    QString viewId;

    static QString eventName() { return QStringLiteral("ViewportResizedEvent"); }
};

struct AIAnalysisRequestedEvent
{
    QString jobId;
    QString providerId;
    AIRequest request;

    static QString eventName() { return QStringLiteral("AIAnalysisRequestedEvent"); }
};

struct AIAnalysisCompletedEvent
{
    QString jobId;
    QString providerId;
    AIResponse response;

    static QString eventName() { return QStringLiteral("AIAnalysisCompletedEvent"); }
};

struct AIAnalysisFailedEvent
{
    QString jobId;
    QString providerId;
    QString errorMessage;
    AIRequest request;

    static QString eventName() { return QStringLiteral("AIAnalysisFailedEvent"); }
};

struct AIProviderAvailabilityChangedEvent
{
    QString providerId;
    bool available = false;

    static QString eventName() { return QStringLiteral("AIProviderAvailabilityChangedEvent"); }
};

struct AIProviderDetectedEvent
{
    AIDetectionResult result;

    static QString eventName() { return QStringLiteral("AIProviderDetectedEvent"); }
};

struct AIModelListUpdatedEvent
{
    AIDetectionResult result;

    static QString eventName() { return QStringLiteral("AIModelListUpdatedEvent"); }
};

struct AIConnectionValidatedEvent
{
    AIDetectionResult result;

    static QString eventName() { return QStringLiteral("AIConnectionValidatedEvent"); }
};

struct AIConnectionFailedEvent
{
    AIDetectionResult result;

    static QString eventName() { return QStringLiteral("AIConnectionFailedEvent"); }
};

struct AIWorkflowStartedEvent
{
    QString jobId;
    AIWorkflowRequest request;

    static QString eventName() { return QStringLiteral("AIWorkflowStartedEvent"); }
};

} // namespace cgplay
