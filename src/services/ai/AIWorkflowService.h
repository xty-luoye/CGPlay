#pragma once

#include "ai/api/IAIWorkflowService.h"

namespace cgplay {

class IAIContextBuilder;
class IAIProviderManager;
class IMediaFrameSnapshotService;
class IEventBus;

class AIWorkflowService : public IAIWorkflowService
{
public:
    AIWorkflowService(
        IAIProviderManager* providerManager,
        IAIContextBuilder* contextBuilder,
        IMediaFrameSnapshotService* frameSnapshotService,
        IEventBus* eventBus = nullptr);

    QString submit(const AIWorkflowRequest& request) override;
    bool cancel(const QString& jobId) override;
    AIJobSnapshot jobSnapshot(const QString& jobId) const override;
    QVector<AISearchResult> searchAnnotations(const QString& query) const override;

private:
    IAIProviderManager* _providerManager = nullptr;
    IAIContextBuilder* _contextBuilder = nullptr;
    IMediaFrameSnapshotService* _frameSnapshotService = nullptr;
    IEventBus* _eventBus = nullptr;
};

} // namespace cgplay
