#pragma once

#include "AIConnectionValidator.h"
#include "AIModelDiscovery.h"
#include "ai/api/IAIProviderDetector.h"

namespace cgplay {

class IAICredentialStore;
class ISettingsService;
class IEventBus;

class AIProviderDetector final : public IAIProviderDetector
{
public:
    AIProviderDetector(
        IAICredentialStore* credentialStore,
        ISettingsService* settingsService,
        IEventBus* eventBus = nullptr);

    AIDetectionResult detect(const AIDetectionRequest& request) override;
    AIDetectionResult discoverCurrentModels() override;
    bool saveConfiguration(
        const AIDetectionRequest& request,
        const AIDetectionResult& result,
        QString* error = nullptr) override;
    AIDetectionResult currentConfiguration() const override;

private:
    AIProviderCategory _categoryFromKeyPattern(const QString& apiKey) const;
    AIProviderCategory _categoryFromProviderType(const QString& providerType) const;
    QVector<AIProviderCategory> _candidateCategories(const AIDetectionRequest& request) const;
    QByteArray _resolveApiKey(
        const QString& explicitApiKey,
        AIProviderCategory category,
        bool hasExplicitBaseUrl,
        QString* source = nullptr) const;
    QString _providerTypeText(AIProviderCategory category) const;
    QString _providerNameText(AIProviderCategory category) const;
    QString _recommendedModel(const QStringList& models) const;
    void _publishDetected(const AIDetectionResult& result) const;
    void _publishModelList(const AIDetectionResult& result) const;
    void _publishValidated(const AIDetectionResult& result) const;
    void _publishFailed(const AIDetectionResult& result) const;

    IAICredentialStore* _credentialStore = nullptr;
    ISettingsService* _settingsService = nullptr;
    IEventBus* _eventBus = nullptr;
    AIConnectionValidator _validator;
    AIModelDiscovery _modelDiscovery;
};

} // namespace cgplay
