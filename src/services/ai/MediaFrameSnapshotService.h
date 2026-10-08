#pragma once

#include "ai/api/IMediaFrameSnapshotService.h"

namespace cgplay {

class MediaFrameSnapshotService : public IMediaFrameSnapshotService
{
public:
    bool capture(
        const AIFrameCaptureRequest& request,
        QVector<AIMediaFrameReference>* outFrames,
        QString* error = nullptr) const override;
};

} // namespace cgplay
