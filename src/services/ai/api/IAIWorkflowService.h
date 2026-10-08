#pragma once

#include "ai/api/AIProviderTypes.h"

#include <QtPlugin>

namespace cgplay {

class IAIWorkflowService
{
public:
    virtual ~IAIWorkflowService() = default;

    virtual QString submit(const AIWorkflowRequest& request) = 0;
    virtual bool cancel(const QString& jobId) = 0;
    virtual AIJobSnapshot jobSnapshot(const QString& jobId) const = 0;
    virtual QVector<AISearchResult> searchAnnotations(const QString& query) const = 0;
};

} // namespace cgplay

#define CGPLAY_IAIWORKFLOWSERVICE_IID "com.cgplay.IAIWorkflowService"
Q_DECLARE_INTERFACE(cgplay::IAIWorkflowService, CGPLAY_IAIWORKFLOWSERVICE_IID)
