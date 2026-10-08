#pragma once

#include "ai/api/AIProviderTypes.h"

#include <QtPlugin>

namespace cgplay {

class IAIProvider
{
public:
    virtual ~IAIProvider() = default;

    virtual QString providerId() const = 0;
    virtual QString providerName() const = 0;
    virtual bool isAvailable() const = 0;
    virtual AIProviderCapabilities capabilities() const = 0;
    virtual AIResponse chat(const AIRequest& request) = 0;
    virtual AIAudioTranscriptionResult transcribe(const AIAudioTranscriptionRequest& request) = 0;
};

} // namespace cgplay

#define CGPLAY_IAIPROVIDER_IID "com.cgplay.IAIProvider"
Q_DECLARE_INTERFACE(cgplay::IAIProvider, CGPLAY_IAIPROVIDER_IID)
