#pragma once

#include "ai/api/AIProviderTypes.h"

#include <QtPlugin>

namespace cgplay {

class IAIContextBuilder
{
public:
    virtual ~IAIContextBuilder() = default;

    virtual AIRequestContext buildContext(
        AIRequestScope scope,
        const QJsonObject& options = {}) const = 0;
};

} // namespace cgplay

#define CGPLAY_IAICONTEXTBUILDER_IID "com.cgplay.IAIContextBuilder"
Q_DECLARE_INTERFACE(cgplay::IAIContextBuilder, CGPLAY_IAICONTEXTBUILDER_IID)
