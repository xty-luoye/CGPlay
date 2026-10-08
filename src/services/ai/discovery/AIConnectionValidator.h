#pragma once

#include "ai/api/AIProviderTypes.h"

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace cgplay {

struct AIValidationResult
{
    bool success = false;
    QString protocol;
    QString endpoint;
    QString responsesEndpoint;
    QStringList compatibleProtocols;
    QStringList diagnostics;
    QString error;
};

struct AIOpenAIEndpointCandidates
{
    QString canonicalBaseUrl;
    QStringList models;
    QStringList chatCompletions;
    QStringList responses;
};

class AIConnectionValidator
{
public:
    static AIOpenAIEndpointCandidates openAICompatibleEndpointCandidates(
        const QString& baseUrl,
        const QString& model = QString());
    static bool usesAzureApiKeyAuthentication(const QString& endpoint);
    static QList<QPair<QByteArray, QByteArray>> openAICompatibleHeaders(
        const QString& endpoint,
        const QByteArray& apiKey);

    QString defaultBaseUrlForCategory(AIProviderCategory category) const;
    QString canonicalBaseUrl(AIProviderCategory category, const QString& inputBaseUrl) const;
    AIValidationResult validate(
        AIProviderCategory category,
        const QString& baseUrl,
        const QByteArray& apiKey,
        const QString& model) const;
};

} // namespace cgplay
