#pragma once

#include "ai/api/IAIProvider.h"

namespace cgplay {

class IAICredentialStore;
class ISettingsService;

class OpenAIResponsesProvider final : public IAIProvider
{
public:
    OpenAIResponsesProvider(
        IAICredentialStore* credentialStore,
        ISettingsService* settingsService);

    QString providerId() const override;
    QString providerName() const override;
    bool isAvailable() const override;
    AIProviderCapabilities capabilities() const override;
    AIResponse chat(const AIRequest& request) override;
    AIAudioTranscriptionResult transcribe(const AIAudioTranscriptionRequest& request) override;

private:
    QByteArray _apiKey(
        const QString& protocol,
        const QString& baseUrl,
        QString* error = nullptr) const;
    QString _baseUrl() const;
    QString _modelForRequest(const AIRequest& request) const;
    QString _transcriptionModelForRequest(const AIAudioTranscriptionRequest& request) const;
    QJsonObject _buildRequestBody(const AIRequest& request, const QString& resolvedModel) const;
    QJsonObject _buildChatCompletionsRequestBody(const AIRequest& request, const QString& resolvedModel) const;
    QString _composeUserText(const AIRequest& request) const;
    QString _extractResponsesText(const QJsonObject& responseObject) const;
    QString _extractChatCompletionsText(const QJsonObject& responseObject) const;

    IAICredentialStore* _credentialStore = nullptr;
    ISettingsService* _settingsService = nullptr;
};

} // namespace cgplay
