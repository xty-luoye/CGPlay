#pragma once

#include "ai/api/IAIProvider.h"

#include <QtPlugin>

#include <memory>

namespace cgplay {

class IAIProviderManager
{
public:
    virtual ~IAIProviderManager() = default;

    virtual QString defaultProviderId() const = 0;
    virtual void setDefaultProviderId(const QString& providerId) = 0;

    virtual QVector<AIProviderInfo> providers() const = 0;
    virtual bool hasProvider(const QString& providerId) const = 0;
    virtual bool registerProvider(std::shared_ptr<IAIProvider> provider) = 0;
    virtual bool unregisterProvider(const QString& providerId) = 0;

    virtual QString submit(const AIRequest& request) = 0;
    virtual AIResponse chatSync(const AIRequest& request) = 0;
    virtual AIAudioTranscriptionResult transcribe(const AIAudioTranscriptionRequest& request) = 0;
    virtual bool cancel(const QString& jobId) = 0;
    virtual AIJobSnapshot jobSnapshot(const QString& jobId) const = 0;
};

} // namespace cgplay

#define CGPLAY_IAIPROVIDERMANAGER_IID "com.cgplay.IAIProviderManager"
Q_DECLARE_INTERFACE(cgplay::IAIProviderManager, CGPLAY_IAIPROVIDERMANAGER_IID)
