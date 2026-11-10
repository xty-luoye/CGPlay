#pragma once

#include "ai/api/AIProviderTypes.h"

#include <QtPlugin>

namespace cgplay {

// Optional capability kept separate from the existing snapshot interface ABI.
class IMediaFrameRangePreparation
{
public:
    virtual ~IMediaFrameRangePreparation() = default;
    virtual bool prepareRangeCapture(const AIFrameCaptureRequest&, AIRequest*, QString*) const = 0;
};

class IMediaFrameSnapshotService
{
public:
    virtual ~IMediaFrameSnapshotService() = default;

    virtual bool capture(
        const AIFrameCaptureRequest& request,
        QVector<AIMediaFrameReference>* outFrames,
        QString* error = nullptr) const = 0;
};

} // namespace cgplay

#define CGPLAY_IMEDIAFRAMESNAPSHOTSERVICE_IID "com.cgplay.IMediaFrameSnapshotService"
Q_DECLARE_INTERFACE(cgplay::IMediaFrameSnapshotService, CGPLAY_IMEDIAFRAMESNAPSHOTSERVICE_IID)
