#pragma once

#include "ai/api/IAIContextBuilder.h"

namespace cgplay {

class AIContextBuilder : public IAIContextBuilder
{
public:
    AIRequestContext buildContext(
        AIRequestScope scope,
        const QJsonObject& options = {}) const override;
};

} // namespace cgplay
