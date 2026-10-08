#include "AIProviderDetector.h"

#include "ai/api/IAICredentialStore.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"
#include "settings/api/ISettingsService.h"

#include <QCoreApplication>
#include <QHash>
#include <QMetaObject>
#include <QRegularExpression>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace cgplay {

namespace {

constexpr auto kGenericCredentialId = "ai/defaultApiKey";
constexpr auto kOpenAICredentialId = "openai/apiKey";
constexpr auto kQwenCredentialId = "qwen/apiKey";
constexpr auto kAzureOpenAICredentialId = "azure/openaiApiKey";
constexpr auto kAnthropicCredentialId = "anthropic/apiKey";
constexpr auto kGeminiCredentialId = "gemini/apiKey";
constexpr auto kGenericApiKeyEnv = "AI_API_KEY";
constexpr auto kOpenAIApiKeyEnv = "OPENAI_API_KEY";
constexpr auto kQwenApiKeyEnv = "QWEN_API_KEY";
constexpr auto kDashScopeApiKeyEnv = "DASHSCOPE_API_KEY";
constexpr auto kAzureOpenAIApiKeyEnv = "AZURE_OPENAI_API_KEY";
constexpr auto kAnthropicApiKeyEnv = "ANTHROPIC_API_KEY";
constexpr auto kGeminiApiKeyEnv = "GEMINI_API_KEY";
constexpr auto kGoogleApiKeyEnv = "GOOGLE_API_KEY";
constexpr auto kDefaultQwenBaseUrl = "https://dashscope.aliyuncs.com/compatible-mode/v1";
constexpr auto kBaseUrlKey = "ai/connection/baseUrl";
constexpr auto kModelKey = "ai/connection/model";
constexpr auto kProviderTypeKey = "ai/connection/providerType";
constexpr auto kProviderNameKey = "ai/connection/providerName";
constexpr auto kAvailableModelsKey = "ai/connection/availableModels";
constexpr auto kRecommendedModelKey = "ai/connection/recommendedModel";
constexpr auto kDetectedProtocolKey = "ai/connection/detectedProtocol";
constexpr auto kDetectedProviderKey = "ai/connection/detectedProvider";
constexpr auto kDetectedEndpointKey = "ai/connection/detectedEndpoint";
constexpr auto kResponsesEndpointKey = "ai/connection/responsesEndpoint";
constexpr auto kCompatibleProtocolsKey = "ai/connection/compatibleProtocols";
constexpr auto kDiagnosticsKey = "ai/connection/diagnostics";
constexpr auto kWorkspaceProviderKey = "ai/workspace/providerId";
constexpr auto kWorkspaceModelKey = "ai/workspace/model";
constexpr auto kLegacyBaseUrlKey = "ai/providers/openai/baseUrl";
constexpr auto kLegacyModelKey = "ai/providers/openai/model";
constexpr auto kProviderId = "openai";

QString trimTrailingSlashes(QString value)
{
    while (value.endsWith(QLatin1Char('/'))) {
        value.chop(1);
    }
    return value.trimmed();
}

template<typename Event>
void publishEvent(IEventBus* eventBus, const Event& event)
{
    if (!eventBus) {
        return;
    }
    auto* app = QCoreApplication::instance();
    if (app && QThread::currentThread() != app->thread()) {
        QMetaObject::invokeMethod(
            app,
            [eventBus, event]() {
                eventBus->publish(event);
            },
            Qt::QueuedConnection);
        return;
    }
    eventBus->publish(event);
}

QStringList dedupedModels(const QStringList& models)
{
    QStringList deduped;
    for (const QString& model : models) {
        const QString trimmed = model.trimmed();
        if (!trimmed.isEmpty() && !deduped.contains(trimmed, Qt::CaseInsensitive)) {
            deduped.push_back(trimmed);
        }
    }
    return deduped;
}

void appendUniqueModel(QStringList* models, const QString& model)
{
    if (!models) {
        return;
    }
    const QString trimmed = model.trimmed();
    if (!trimmed.isEmpty() && !models->contains(trimmed, Qt::CaseInsensitive)) {
        models->push_back(trimmed);
    }
}

bool hasSensitiveCredentialQuery(const QString& value)
{
    const QUrl url(value.trimmed());
    if (!url.userInfo().isEmpty()) return true;
    const QUrlQuery query(url);
    for (const auto& item : query.queryItems(QUrl::FullyDecoded)) {
        const QString key = item.first.trimmed().toLower();
        if (key == QStringLiteral("key") ||
            key == QStringLiteral("api_key") ||
            key == QStringLiteral("api-key") ||
            key == QStringLiteral("apikey") ||
            key == QStringLiteral("x-api-key") ||
            key == QStringLiteral("x_api_key") ||
            key == QStringLiteral("x-goog-api-key") ||
            key == QStringLiteral("x_goog_api_key") ||
            key == QStringLiteral("subscription-key") ||
            key == QStringLiteral("subscription_key") ||
            key == QStringLiteral("access_token") ||
            key == QStringLiteral("access-token") ||
            key == QStringLiteral("token") ||
            key == QStringLiteral("authorization") ||
            key == QStringLiteral("auth")) {
            return true;
        }
    }
    return false;
}

bool isNonChatModel(const QString& model)
{
    const QString lower = model.trimmed().toLower();
    return lower.contains(QStringLiteral("image")) ||
        lower.contains(QStringLiteral("embedding")) ||
        lower.contains(QStringLiteral("rerank")) ||
        lower.contains(QStringLiteral("moderation")) ||
        lower.contains(QStringLiteral("whisper")) ||
        lower.contains(QStringLiteral("transcribe")) ||
        lower.contains(QStringLiteral("speech")) ||
        lower.contains(QStringLiteral("tts"));
}

QString modelFamily(const QString& model)
{
    const QString lower = model.trimmed().toLower();
    const QStringList tokens = lower.split(
        QRegularExpression(QStringLiteral("[-_.:/]+")),
        Qt::SkipEmptyParts);
    QStringList familyTokens;
    for (const QString& token : tokens) {
        if (token.contains(QRegularExpression(QStringLiteral("[0-9]")))) {
            break;
        }
        familyTokens.push_back(token);
    }
    if (!familyTokens.isEmpty()) {
        return familyTokens.join(QLatin1Char('-'));
    }
    return tokens.isEmpty() ? lower : tokens.front();
}

QVector<int> modelVersion(const QString& model)
{
    QVector<int> version;
    static const QRegularExpression expression(QStringLiteral("([0-9]+)"));
    QRegularExpressionMatchIterator matches = expression.globalMatch(model);
    while (matches.hasNext()) {
        bool ok = false;
        const int value = matches.next().captured(1).toInt(&ok);
        if (ok) {
            version.push_back(value);
        }
    }
    return version;
}

int lifecyclePenalty(const QString& model)
{
    const QString lower = model.toLower();
    for (const char* token : { "deprecated", "legacy", "preview", "experimental", "alpha", "beta" }) {
        if (lower.contains(QString::fromLatin1(token))) {
            return 1;
        }
    }
    return 0;
}

int capacityRank(const QString& model)
{
    const QString lower = model.toLower();
    for (const char* token : { "ultra", "max", "pro", "large", "xl" }) {
        if (lower.contains(QString::fromLatin1(token))) {
            return -1;
        }
    }
    for (const char* token : { "mini", "nano", "lite", "small", "tiny", "micro" }) {
        if (lower.contains(QString::fromLatin1(token))) {
            return 1;
        }
    }
    return 0;
}

int purposeRank(const QString& model)
{
    const QString lower = model.toLower();
    for (const char* token : { "reasoning", "instruct", "chat" }) {
        if (lower.contains(QString::fromLatin1(token))) {
            return -1;
        }
    }
    return 0;
}

bool versionIsNewer(const QVector<int>& lhs, const QVector<int>& rhs)
{
    const int count = std::max(lhs.size(), rhs.size());
    for (int index = 0; index < count; ++index) {
        const int lhsPart = index < lhs.size() ? lhs[index] : 0;
        const int rhsPart = index < rhs.size() ? rhs[index] : 0;
        if (lhsPart != rhsPart) {
            return lhsPart > rhsPart;
        }
    }
    return false;
}

struct ModelCandidate
{
    QString model;
    QString family;
    QVector<int> version;
    int familyOrder = 0;
    int originalOrder = 0;
    int nonChatPenalty = 0;
    int lifecycle = 0;
    int capacity = 0;
    int purpose = 0;
};

QStringList orderedModelsForSelection(const QStringList& models)
{
    const QStringList deduped = dedupedModels(models);
    QVector<ModelCandidate> candidates;
    QHash<QString, int> familyOrders;
    int nextFamilyOrder = 0;
    for (int index = 0; index < deduped.size(); ++index) {
        const QString& model = deduped[index];
        const QString family = modelFamily(model);
        if (!familyOrders.contains(family)) {
            familyOrders.insert(family, nextFamilyOrder++);
        }
        candidates.push_back(ModelCandidate{
            model,
            family,
            modelVersion(model),
            familyOrders.value(family),
            index,
            isNonChatModel(model) ? 1 : 0,
            lifecyclePenalty(model),
            capacityRank(model),
            purposeRank(model)
        });
    }
    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [](const ModelCandidate& lhs, const ModelCandidate& rhs) {
            if (lhs.nonChatPenalty != rhs.nonChatPenalty) {
                return lhs.nonChatPenalty < rhs.nonChatPenalty;
            }
            if (lhs.familyOrder != rhs.familyOrder) {
                return lhs.familyOrder < rhs.familyOrder;
            }
            if (lhs.lifecycle != rhs.lifecycle) {
                return lhs.lifecycle < rhs.lifecycle;
            }
            if (lhs.capacity != rhs.capacity) {
                return lhs.capacity < rhs.capacity;
            }
            if (lhs.purpose != rhs.purpose) {
                return lhs.purpose < rhs.purpose;
            }
            if (lhs.version != rhs.version) {
                return versionIsNewer(lhs.version, rhs.version);
            }
            return lhs.originalOrder < rhs.originalOrder;
        });

    QStringList ordered;
    ordered.reserve(candidates.size());
    for (const ModelCandidate& candidate : candidates) {
        ordered.push_back(candidate.model);
    }
    return ordered;
}

QString redactSecret(QString value, const QByteArray& secret)
{
    const QString secretText = QString::fromUtf8(secret);
    if (!secretText.isEmpty()) {
        value.replace(secretText, QStringLiteral("[redacted]"), Qt::CaseSensitive);
    }
    return value;
}

QStringList sanitizedDiagnostics(const QStringList& diagnostics, const QByteArray& secret)
{
    QStringList sanitized;
    for (const QString& diagnostic : diagnostics) {
        const QString value = redactSecret(diagnostic, secret).trimmed();
        if (!value.isEmpty() && !sanitized.contains(value)) {
            sanitized.push_back(value);
        }
    }
    return sanitized;
}

} // namespace

AIProviderDetector::AIProviderDetector(
    IAICredentialStore* credentialStore,
    ISettingsService* settingsService,
    IEventBus* eventBus)
    : _credentialStore(credentialStore)
    , _settingsService(settingsService)
    , _eventBus(eventBus)
{
}

AIDetectionResult AIProviderDetector::detect(const AIDetectionRequest& request)
{
    AIDetectionResult bestFailure;
    if (hasSensitiveCredentialQuery(request.baseUrl)) {
        bestFailure.error = QStringLiteral(
            "Base URL must not contain an API key or access token; use the secure credential field.");
        _publishFailed(bestFailure);
        return bestFailure;
    }
    QStringList errorLines;
    QStringList fallbackModels;
    appendUniqueModel(&fallbackModels, request.model);
    if (_settingsService) {
        for (const char* key : {
                 kModelKey,
                 kWorkspaceModelKey,
                 kRecommendedModelKey,
                 kLegacyModelKey }) {
            appendUniqueModel(
                &fallbackModels,
                _settingsService->value(QString::fromLatin1(key)).toString());
        }
    }

    for (AIProviderCategory category : _candidateCategories(request)) {
        const QByteArray apiKey = _resolveApiKey(
            request.apiKey,
            category,
            !request.baseUrl.trimmed().isEmpty());
        AIDetectionResult current;
        current.category = category;
        current.providerType = _providerTypeText(category);
        current.providerName = _providerNameText(category);
        current.baseUrl = trimTrailingSlashes(_validator.canonicalBaseUrl(category, request.baseUrl));
        if (current.baseUrl.isEmpty()) {
            current.baseUrl = trimTrailingSlashes(_validator.defaultBaseUrlForCategory(category));
        }

        const AIModelDiscoveryResult discovery =
            _modelDiscovery.discover(category, current.baseUrl, apiKey);
        current.detectedEndpoint = discovery.endpoint;
        current.models = dedupedModels(discovery.models);
        for (const QString& fallbackModel : fallbackModels) {
            appendUniqueModel(&current.models, fallbackModel);
        }
        current.diagnostics = sanitizedDiagnostics(discovery.diagnostics, apiKey);
        if (!discovery.success && !discovery.error.trimmed().isEmpty()) {
            const QString diagnostic = redactSecret(discovery.error, apiKey).trimmed();
            if (!diagnostic.isEmpty() && !current.diagnostics.contains(diagnostic)) {
                current.diagnostics.push_back(diagnostic);
            }
        }

        current.recommendedModel = request.model.trimmed();
        if (current.recommendedModel.isEmpty()) {
            current.recommendedModel = _recommendedModel(current.models);
        }
        if (category == AIProviderCategory::Qwen && current.recommendedModel.isEmpty()) {
            current.recommendedModel = QStringLiteral("qwen-plus");
            appendUniqueModel(&current.models, current.recommendedModel);
        }

        if (current.models.isEmpty()) {
            current.error = discovery.error.trimmed().isEmpty()
                ? QStringLiteral("No model is available for validation.")
                : redactSecret(discovery.error, apiKey);
            errorLines << QStringLiteral("%1 -> %2").arg(current.providerName, current.error);
            if (current.models.size() >= bestFailure.models.size()) {
                bestFailure = current;
            }
            continue;
        }

        _publishDetected(current);
        _publishModelList(current);

        QStringList validationModels = orderedModelsForSelection(current.models);
        if (!current.recommendedModel.isEmpty()) {
            validationModels.removeAll(current.recommendedModel);
            validationModels.prepend(current.recommendedModel);
        }
        if (!request.model.trimmed().isEmpty()) {
            // An explicit model is a contract, not a recommendation to replace on failure.
            validationModels = QStringList{request.model.trimmed()};
        }

        QStringList validationErrors;
        AIValidationResult bestValidation;
        for (const QString& candidateModel : validationModels) {
            const AIValidationResult validation =
                _validator.validate(category, current.baseUrl, apiKey, candidateModel);
            if (!validation.endpoint.trimmed().isEmpty()) {
                current.detectedEndpoint = validation.endpoint.trimmed();
            }
            if (!validation.responsesEndpoint.trimmed().isEmpty()) {
                current.responsesEndpoint = validation.responsesEndpoint.trimmed();
            }
            current.detectedProtocol = validation.protocol;
            const QStringList validationDiagnostics =
                sanitizedDiagnostics(validation.diagnostics, apiKey);
            for (const QString& diagnostic : validationDiagnostics) {
                const QString contextual = QStringLiteral("%1: %2").arg(candidateModel, diagnostic);
                if (!current.diagnostics.contains(contextual)) {
                    current.diagnostics.push_back(contextual);
                }
            }
            if (validation.success) {
                current.success = true;
                current.error.clear();
                current.recommendedModel = candidateModel;
                current.compatibleProtocols = validation.compatibleProtocols;
                _publishValidated(current);
                return current;
            }

            bestValidation = validation;
            validationErrors << QStringLiteral("%1 -> %2")
                .arg(
                    candidateModel,
                    validation.error.isEmpty()
                        ? QStringLiteral("validation failed")
                        : redactSecret(validation.error, apiKey));
        }

        if (!bestValidation.protocol.trimmed().isEmpty()) {
            current.detectedProtocol = bestValidation.protocol;
        }
        if (!bestValidation.endpoint.trimmed().isEmpty()) {
            current.detectedEndpoint = bestValidation.endpoint.trimmed();
        }
        if (!bestValidation.responsesEndpoint.trimmed().isEmpty()) {
            current.responsesEndpoint = bestValidation.responsesEndpoint.trimmed();
        }
        current.compatibleProtocols = bestValidation.compatibleProtocols;
        current.error = validationErrors.isEmpty()
            ? redactSecret(bestValidation.error, apiKey)
            : validationErrors.join(QStringLiteral("\n"));
        errorLines << QStringLiteral("%1 -> %2").arg(current.providerName, current.error);
        if (current.models.size() >= bestFailure.models.size()) {
            bestFailure = current;
        }
    }

    if (bestFailure.providerType.isEmpty()) {
        bestFailure.providerType = _providerTypeText(AIProviderCategory::Unknown);
        bestFailure.providerName = _providerNameText(AIProviderCategory::Unknown);
    }
    bestFailure.success = false;
    bestFailure.error = errorLines.isEmpty()
        ? QStringLiteral("No compatible AI protocol succeeded.")
        : QStringLiteral("No compatible AI protocol succeeded.\n- %1").arg(errorLines.join(QStringLiteral("\n- ")));
    _publishFailed(bestFailure);
    return bestFailure;
}

AIDetectionResult AIProviderDetector::discoverCurrentModels()
{
    AIDetectionResult result = currentConfiguration();
    if (result.category == AIProviderCategory::Unknown) {
        result.success = false;
        result.error = QStringLiteral("The saved AI provider category is unavailable.");
        return result;
    }

    QByteArray apiKey = _resolveApiKey(
        QString(),
        result.category,
        !result.baseUrl.trimmed().isEmpty());
    if (result.category != AIProviderCategory::Ollama && apiKey.trimmed().isEmpty()) {
        result.success = false;
        result.error = QStringLiteral("No saved credential is available for model discovery.");
        return result;
    }

    const AIModelDiscoveryResult discovery =
        _modelDiscovery.discover(result.category, result.baseUrl, apiKey);
    result.diagnostics = sanitizedDiagnostics(discovery.diagnostics, apiKey);
    result.error = redactSecret(discovery.error, apiKey);
    apiKey.fill('\0');

    result.models = dedupedModels(discovery.models);
    result.detectedEndpoint = discovery.endpoint.trimmed();
    result.success = discovery.success && !result.models.isEmpty();
    if (result.success) {
        _publishModelList(result);
    } else if (result.error.isEmpty()) {
        result.error = QStringLiteral("The current API returned no models.");
    }
    return result;
}

bool AIProviderDetector::saveConfiguration(
    const AIDetectionRequest& request,
    const AIDetectionResult& result,
    QString* error)
{
    if (!_settingsService) {
        if (error) {
            *error = QStringLiteral("Settings service is unavailable.");
        }
        return false;
    }
    if (!result.success) {
        if (error) {
            *error = result.error.isEmpty()
                ? QStringLiteral("Detection must succeed before saving.")
                : result.error;
        }
        return false;
    }
    if (hasSensitiveCredentialQuery(result.baseUrl)) {
        if (error) {
            *error = QStringLiteral(
                "Base URL must not contain an API key or access token; use the secure credential field.");
        }
        return false;
    }

    if (_credentialStore && !request.apiKey.trimmed().isEmpty()) {
        QString storeError;
        const QByteArray apiKey = request.apiKey.trimmed().toUtf8();
        const bool genericStored = _credentialStore->storeSecret(
            QString::fromLatin1(kGenericCredentialId), apiKey, &storeError);
        QString categoryCredentialId;
        if (result.category == AIProviderCategory::Qwen) {
            categoryCredentialId = QString::fromLatin1(kQwenCredentialId);
        } else if (result.category == AIProviderCategory::Claude) {
            categoryCredentialId = QString::fromLatin1(kAnthropicCredentialId);
        } else if (result.category == AIProviderCategory::Gemini) {
            categoryCredentialId = QString::fromLatin1(kGeminiCredentialId);
        } else if (result.category == AIProviderCategory::OpenAICompatible) {
            const QString endpoint = result.detectedEndpoint.trimmed().isEmpty()
                ? result.baseUrl
                : result.detectedEndpoint;
            categoryCredentialId = AIConnectionValidator::usesAzureApiKeyAuthentication(endpoint)
                ? QString::fromLatin1(kAzureOpenAICredentialId)
                : QString::fromLatin1(kOpenAICredentialId);
        }
        const bool categoryStored = categoryCredentialId.isEmpty() ||
            _credentialStore->storeSecret(categoryCredentialId, apiKey, &storeError);
        if (!genericStored || !categoryStored) {
            if (error) {
                *error = storeError.isEmpty()
                    ? QStringLiteral("Failed to store API key.")
                    : storeError;
            }
            return false;
        }
    }

    const QString baseUrl = trimTrailingSlashes(result.baseUrl.isEmpty()
        ? _validator.defaultBaseUrlForCategory(result.category)
        : result.baseUrl);
    QStringList models = dedupedModels(result.models);
    const QString recommendedModel = result.recommendedModel.trimmed().isEmpty()
        ? _recommendedModel(models)
        : result.recommendedModel.trimmed();
    if (models.isEmpty() && !recommendedModel.isEmpty()) {
        models.push_back(recommendedModel);
    }

    _settingsService->setValue(QString::fromLatin1(kBaseUrlKey), baseUrl);
    _settingsService->setValue(QString::fromLatin1(kLegacyBaseUrlKey), baseUrl);
    _settingsService->setValue(QString::fromLatin1(kProviderTypeKey), result.providerType);
    _settingsService->setValue(QString::fromLatin1(kProviderNameKey), result.providerName);
    _settingsService->setValue(QString::fromLatin1(kAvailableModelsKey), models);
    _settingsService->setValue(QString::fromLatin1(kRecommendedModelKey), recommendedModel);
    _settingsService->setValue(QString::fromLatin1(kModelKey), recommendedModel);
    _settingsService->setValue(QString::fromLatin1(kLegacyModelKey), recommendedModel);
    _settingsService->setValue(QString::fromLatin1(kWorkspaceProviderKey), QString::fromLatin1(kProviderId));
    _settingsService->setValue(QString::fromLatin1(kWorkspaceModelKey), recommendedModel);
    _settingsService->setValue(QString::fromLatin1(kDetectedProtocolKey), result.detectedProtocol);
    _settingsService->setValue(QString::fromLatin1(kDetectedProviderKey), result.providerName);
    _settingsService->setValue(
        QString::fromLatin1(kDetectedEndpointKey),
        result.detectedEndpoint.trimmed().isEmpty() ? baseUrl : result.detectedEndpoint.trimmed());
    if (result.responsesEndpoint.trimmed().isEmpty()) {
        _settingsService->remove(QString::fromLatin1(kResponsesEndpointKey));
    } else {
        _settingsService->setValue(
            QString::fromLatin1(kResponsesEndpointKey),
            result.responsesEndpoint.trimmed());
    }
    _settingsService->setValue(
        QString::fromLatin1(kCompatibleProtocolsKey),
        dedupedModels(result.compatibleProtocols));
    _settingsService->setValue(
        QString::fromLatin1(kDiagnosticsKey),
        sanitizedDiagnostics(result.diagnostics, request.apiKey.trimmed().toUtf8()));
    _settingsService->sync();
    return true;
}

AIDetectionResult AIProviderDetector::currentConfiguration() const
{
    AIDetectionResult result;
    if (!_settingsService) {
        return result;
    }

    result.providerType = _settingsService->value(QString::fromLatin1(kProviderTypeKey)).toString().trimmed();
    result.providerName = _settingsService->value(QString::fromLatin1(kProviderNameKey)).toString().trimmed();
    result.baseUrl = trimTrailingSlashes(_settingsService->value(QString::fromLatin1(kBaseUrlKey)).toString());
    if (result.baseUrl.isEmpty()) {
        result.baseUrl = trimTrailingSlashes(
            _settingsService->value(QString::fromLatin1(kLegacyBaseUrlKey)).toString());
    }
    result.models = dedupedModels(_settingsService->value(QString::fromLatin1(kAvailableModelsKey)).toStringList());
    result.recommendedModel = _settingsService->value(QString::fromLatin1(kRecommendedModelKey)).toString().trimmed();
    result.detectedProtocol = _settingsService->value(QString::fromLatin1(kDetectedProtocolKey)).toString().trimmed();
    result.detectedEndpoint = _settingsService->value(QString::fromLatin1(kDetectedEndpointKey)).toString().trimmed();
    result.responsesEndpoint = _settingsService->value(QString::fromLatin1(kResponsesEndpointKey)).toString().trimmed();
    result.compatibleProtocols = dedupedModels(
        _settingsService->value(QString::fromLatin1(kCompatibleProtocolsKey)).toStringList());
    result.diagnostics = _settingsService->value(QString::fromLatin1(kDiagnosticsKey)).toStringList();
    result.category = _categoryFromProviderType(result.providerType);
    if (result.providerType.isEmpty() && !result.baseUrl.isEmpty()) {
        const QString loweredBaseUrl = result.baseUrl.toLower();
        if (loweredBaseUrl.contains(QStringLiteral("11434")) || loweredBaseUrl.contains(QStringLiteral("ollama"))) {
            result.category = AIProviderCategory::Ollama;
        } else if (loweredBaseUrl.contains(QStringLiteral("googleapis.com")) || loweredBaseUrl.contains(QStringLiteral("google.ai"))) {
            result.category = AIProviderCategory::Gemini;
        } else if (loweredBaseUrl.contains(QStringLiteral("anthropic"))) {
            result.category = AIProviderCategory::Claude;
        } else if (loweredBaseUrl.contains(QStringLiteral("dashscope")) ||
                   loweredBaseUrl.contains(QStringLiteral("aliyuncs.com")) ||
                   loweredBaseUrl.contains(QStringLiteral("qwen"))) {
            result.category = AIProviderCategory::Qwen;
        } else {
            result.category = AIProviderCategory::OpenAICompatible;
        }
        result.providerType = _providerTypeText(result.category);
    }
    if (result.providerName.isEmpty()) {
        result.providerName = _providerNameText(result.category);
    }
    if (result.recommendedModel.isEmpty()) {
        result.recommendedModel = _settingsService->value(QString::fromLatin1(kModelKey)).toString().trimmed();
    }
    if (result.recommendedModel.isEmpty()) {
        result.recommendedModel = _settingsService->value(QString::fromLatin1(kLegacyModelKey)).toString().trimmed();
    }
    if (result.recommendedModel.isEmpty()) {
        result.recommendedModel = _recommendedModel(result.models);
    }
    if (result.recommendedModel.isEmpty() && result.category == AIProviderCategory::Qwen) {
        result.recommendedModel = QStringLiteral("qwen-plus");
    }
    if (result.baseUrl.isEmpty() && result.category == AIProviderCategory::Qwen) {
        result.baseUrl = QString::fromLatin1(kDefaultQwenBaseUrl);
    }
    result.success =
        !result.providerType.isEmpty() &&
        (!result.baseUrl.isEmpty() || result.category == AIProviderCategory::Ollama) &&
        (!result.recommendedModel.isEmpty() || !result.models.isEmpty());
    return result;
}

AIProviderCategory AIProviderDetector::_categoryFromKeyPattern(const QString& apiKey) const
{
    const QString trimmed = apiKey.trimmed();
    if (trimmed.startsWith(QStringLiteral("AIza"), Qt::CaseInsensitive)) {
        return AIProviderCategory::Gemini;
    }
    if (trimmed.startsWith(QStringLiteral("sk-ant-"), Qt::CaseInsensitive)) {
        return AIProviderCategory::Claude;
    }
    if (trimmed.startsWith(QStringLiteral("sk-"), Qt::CaseInsensitive)) {
        return AIProviderCategory::OpenAICompatible;
    }
    return AIProviderCategory::Unknown;
}

AIProviderCategory AIProviderDetector::_categoryFromProviderType(const QString& providerType) const
{
    const QString normalized = providerType.trimmed().toLower();
    if (normalized == QStringLiteral("openaicompatible")) {
        return AIProviderCategory::OpenAICompatible;
    }
    if (normalized == QStringLiteral("qwen")) {
        return AIProviderCategory::Qwen;
    }
    if (normalized == QStringLiteral("gemini")) {
        return AIProviderCategory::Gemini;
    }
    if (normalized == QStringLiteral("claude")) {
        return AIProviderCategory::Claude;
    }
    if (normalized == QStringLiteral("ollama")) {
        return AIProviderCategory::Ollama;
    }
    return AIProviderCategory::Unknown;
}

QVector<AIProviderCategory> AIProviderDetector::_candidateCategories(const AIDetectionRequest& request) const
{
    QVector<AIProviderCategory> categories;
    const auto appendUnique = [&categories](AIProviderCategory category) {
        if (!categories.contains(category)) {
            categories.push_back(category);
        }
    };

    const QString baseUrl = request.baseUrl.trimmed().toLower();
    if (!baseUrl.isEmpty()) {
        if (baseUrl.contains(QStringLiteral("11434")) || baseUrl.contains(QStringLiteral("ollama"))) {
            appendUnique(AIProviderCategory::Ollama);
        }
        if (baseUrl.contains(QStringLiteral("googleapis.com")) || baseUrl.contains(QStringLiteral("google.ai"))) {
            appendUnique(baseUrl.contains(QStringLiteral("/openai"))
                ? AIProviderCategory::OpenAICompatible : AIProviderCategory::Gemini);
        }
        if (baseUrl.contains(QStringLiteral("anthropic"))) {
            appendUnique(AIProviderCategory::Claude);
        }
        if (baseUrl.contains(QStringLiteral("dashscope")) ||
            baseUrl.contains(QStringLiteral("aliyuncs.com")) ||
            baseUrl.contains(QStringLiteral("qwen"))) {
            appendUnique(AIProviderCategory::Qwen);
        }
        appendUnique(AIProviderCategory::OpenAICompatible);
        appendUnique(AIProviderCategory::Gemini);
        appendUnique(AIProviderCategory::Claude);
        appendUnique(AIProviderCategory::Ollama);
        return categories;
    }

    const AIProviderCategory hinted = _categoryFromKeyPattern(request.apiKey);
    if (hinted != AIProviderCategory::Unknown) {
        appendUnique(hinted);
    }
    if (request.apiKey.trimmed().startsWith(QStringLiteral("sk-"), Qt::CaseInsensitive) ||
        !qgetenv(kQwenApiKeyEnv).trimmed().isEmpty() ||
        !qgetenv(kDashScopeApiKeyEnv).trimmed().isEmpty()) {
        appendUnique(AIProviderCategory::Qwen);
    }
    if (request.apiKey.trimmed().isEmpty()) {
        appendUnique(AIProviderCategory::Ollama);
    }
    appendUnique(AIProviderCategory::OpenAICompatible);
    appendUnique(AIProviderCategory::Gemini);
    appendUnique(AIProviderCategory::Claude);
    appendUnique(AIProviderCategory::Ollama);
    return categories;
}

QByteArray AIProviderDetector::_resolveApiKey(
    const QString& explicitApiKey,
    AIProviderCategory category,
    bool hasExplicitBaseUrl,
    QString* source) const
{
    const QByteArray direct = explicitApiKey.trimmed().toUtf8();
    if (!direct.isEmpty()) {
        const AIProviderCategory keyCategory = _categoryFromKeyPattern(explicitApiKey);
        if (!hasExplicitBaseUrl &&
            ((keyCategory != AIProviderCategory::Unknown && keyCategory != category) ||
             (keyCategory == AIProviderCategory::Unknown && category != AIProviderCategory::OpenAICompatible))) {
            return {};
        }
        if (source) {
            *source = QStringLiteral("explicit");
        }
        return direct;
    }

    QString loadError;
    const AIProviderCategory configuredCategory = _settingsService
        ? _categoryFromProviderType(
              _settingsService->value(QString::fromLatin1(kProviderTypeKey)).toString())
        : AIProviderCategory::Unknown;
    const bool allowGeneric = hasExplicitBaseUrl || configuredCategory == category;

    if (_credentialStore) {
        const QByteArray generic =
            _credentialStore->loadSecret(QString::fromLatin1(kGenericCredentialId), &loadError).trimmed();
        if (allowGeneric && !generic.isEmpty()) {
            if (source) {
                *source = QStringLiteral("stored");
            }
            return generic;
        }
        const char* credentialId = nullptr;
        const char* credentialSource = nullptr;
        switch (category) {
        case AIProviderCategory::OpenAICompatible:
            credentialId = kOpenAICredentialId;
            credentialSource = "stored-openai";
            break;
        case AIProviderCategory::Qwen:
            credentialId = kQwenCredentialId;
            credentialSource = "stored-qwen";
            break;
        case AIProviderCategory::Claude:
            credentialId = kAnthropicCredentialId;
            credentialSource = "stored-anthropic";
            break;
        case AIProviderCategory::Gemini:
            credentialId = kGeminiCredentialId;
            credentialSource = "stored-gemini";
            break;
        case AIProviderCategory::Ollama:
        case AIProviderCategory::Unknown:
            break;
        }
        if (credentialId) {
            const QByteArray stored = _credentialStore
                ->loadSecret(QString::fromLatin1(credentialId), &loadError)
                .trimmed();
            if (!stored.isEmpty()) {
                if (source) *source = QString::fromLatin1(credentialSource);
                return stored;
            }
        }
    }

    const QByteArray genericEnv = qgetenv(kGenericApiKeyEnv).trimmed();
    if (allowGeneric && !genericEnv.isEmpty()) {
        if (source) {
            *source = QStringLiteral("env");
        }
        return genericEnv;
    }

    QList<QPair<const char*, const char*>> candidates;
    switch (category) {
    case AIProviderCategory::OpenAICompatible:
        candidates = {qMakePair(kOpenAIApiKeyEnv, "env-openai")};
        if (hasExplicitBaseUrl) {
            candidates.push_back(qMakePair(kAzureOpenAIApiKeyEnv, "env-azure"));
        }
        break;
    case AIProviderCategory::Qwen:
        candidates = {
            qMakePair(kQwenApiKeyEnv, "env-qwen"),
            qMakePair(kDashScopeApiKeyEnv, "env-dashscope")
        };
        break;
    case AIProviderCategory::Claude:
        candidates = {qMakePair(kAnthropicApiKeyEnv, "env-anthropic")};
        break;
    case AIProviderCategory::Gemini:
        candidates = {
            qMakePair(kGeminiApiKeyEnv, "env-gemini"),
            qMakePair(kGoogleApiKeyEnv, "env-google")
        };
        break;
    case AIProviderCategory::Ollama:
    case AIProviderCategory::Unknown:
        break;
    }
    for (const auto& candidate : candidates) {
        const QByteArray value = qgetenv(candidate.first).trimmed();
        if (value.isEmpty()) continue;
        if (source) *source = QString::fromLatin1(candidate.second);
        return value;
    }

    if (source) {
        *source = loadError;
    }
    return {};
}

QString AIProviderDetector::_providerTypeText(AIProviderCategory category) const
{
    switch (category) {
    case AIProviderCategory::Qwen:
        return QStringLiteral("Qwen");
    case AIProviderCategory::OpenAICompatible:
        return QStringLiteral("OpenAICompatible");
    case AIProviderCategory::Gemini:
        return QStringLiteral("Gemini");
    case AIProviderCategory::Claude:
        return QStringLiteral("Claude");
    case AIProviderCategory::Ollama:
        return QStringLiteral("Ollama");
    case AIProviderCategory::Unknown:
    default:
        return QStringLiteral("Unknown");
    }
}

QString AIProviderDetector::_providerNameText(AIProviderCategory category) const
{
    switch (category) {
    case AIProviderCategory::Qwen:
        return QStringLiteral("Qwen / DashScope");
    case AIProviderCategory::OpenAICompatible:
        return QStringLiteral("OpenAI Compatible");
    case AIProviderCategory::Gemini:
        return QStringLiteral("Gemini");
    case AIProviderCategory::Claude:
        return QStringLiteral("Claude");
    case AIProviderCategory::Ollama:
        return QStringLiteral("Ollama");
    case AIProviderCategory::Unknown:
    default:
        return QStringLiteral("Unknown");
    }
}

QString AIProviderDetector::_recommendedModel(const QStringList& models) const
{
    const QStringList ordered = orderedModelsForSelection(models);
    return ordered.isEmpty() ? QString() : ordered.front();
}

void AIProviderDetector::_publishDetected(const AIDetectionResult& result) const
{
    publishEvent(_eventBus, AIProviderDetectedEvent{ result });
}

void AIProviderDetector::_publishModelList(const AIDetectionResult& result) const
{
    publishEvent(_eventBus, AIModelListUpdatedEvent{ result });
}

void AIProviderDetector::_publishValidated(const AIDetectionResult& result) const
{
    publishEvent(_eventBus, AIConnectionValidatedEvent{ result });
}

void AIProviderDetector::_publishFailed(const AIDetectionResult& result) const
{
    publishEvent(_eventBus, AIConnectionFailedEvent{ result });
}

} // namespace cgplay
