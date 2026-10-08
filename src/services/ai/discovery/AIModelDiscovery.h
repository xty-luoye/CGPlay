#pragma once

#include "ai/api/AIProviderTypes.h"

#include <QString>

namespace cgplay {

struct AIModelDiscoveryResult
{
    bool success = false;
    QString endpoint;
    QStringList models;
    QStringList diagnostics;
    QString error;
};

class AIModelDiscovery
{
public:
    AIModelDiscoveryResult discover(
        AIProviderCategory category,
        const QString& baseUrl,
        const QByteArray& apiKey) const;
};

} // namespace cgplay
