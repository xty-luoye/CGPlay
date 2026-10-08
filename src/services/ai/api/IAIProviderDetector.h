#pragma once

#include "ai/api/AIProviderTypes.h"

#include <QtPlugin>

namespace cgplay {

class IAIProviderDetector
{
public:
    virtual ~IAIProviderDetector() = default;

    virtual AIDetectionResult detect(const AIDetectionRequest& request) = 0;
    virtual AIDetectionResult discoverCurrentModels() = 0;
    virtual bool saveConfiguration(
        const AIDetectionRequest& request,
        const AIDetectionResult& result,
        QString* error = nullptr) = 0;
    virtual AIDetectionResult currentConfiguration() const = 0;
};

} // namespace cgplay

#define CGPLAY_IAIPROVIDERDETECTOR_IID "com.cgplay.IAIProviderDetector"
Q_DECLARE_INTERFACE(cgplay::IAIProviderDetector, CGPLAY_IAIPROVIDERDETECTOR_IID)
