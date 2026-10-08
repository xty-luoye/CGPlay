#include "OpenAIResponsesProvider.h"

#include "ai/api/IAICredentialStore.h"
#include "ai/discovery/AIConnectionValidator.h"
#include "annotation/AnnotationItem.h"
#include "settings/api/ISettingsService.h"

#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace cgplay {

namespace {

constexpr auto kProviderId = "openai";
constexpr auto kProviderName = "智能识别";
constexpr auto kDefaultOpenAIBaseUrl = "https://api.openai.com";
constexpr auto kDefaultOpenAIModel = "gpt-4o-mini";
constexpr auto kDefaultTranscriptionModel = "gpt-4o-mini-transcribe";
constexpr auto kDefaultAnthropicModel = "claude-sonnet-4-6";
constexpr auto kDefaultGeminiModel = "gemini-2.5-flash";
constexpr auto kDefaultOllamaModel = "gemma3:4b";
constexpr auto kOpenAIApiKeyEnv = "OPENAI_API_KEY";
constexpr auto kQwenApiKeyEnv = "QWEN_API_KEY";
constexpr auto kDashScopeApiKeyEnv = "DASHSCOPE_API_KEY";
constexpr auto kAzureOpenAIApiKeyEnv = "AZURE_OPENAI_API_KEY";
constexpr auto kAnthropicApiKeyEnv = "ANTHROPIC_API_KEY";
constexpr auto kGeminiApiKeyEnv = "GEMINI_API_KEY";
constexpr auto kGoogleApiKeyEnv = "GOOGLE_API_KEY";
constexpr auto kOpenAIBaseUrlEnv = "OPENAI_BASE_URL";
constexpr auto kQwenBaseUrlEnv = "QWEN_BASE_URL";
constexpr auto kDashScopeBaseUrlEnv = "DASHSCOPE_BASE_URL";
constexpr auto kGenericApiKeyEnv = "AI_API_KEY";
constexpr auto kGenericBaseUrlEnv = "AI_BASE_URL";
constexpr auto kGenericModelEnv = "AI_MODEL";
constexpr auto kQwenModelEnv = "QWEN_MODEL";
constexpr auto kDashScopeModelEnv = "DASHSCOPE_MODEL";
constexpr auto kOpenAICredentialId = "openai/apiKey";
constexpr auto kQwenCredentialId = "qwen/apiKey";
constexpr auto kGenericCredentialId = "ai/defaultApiKey";
constexpr auto kAzureOpenAICredentialId = "azure/openaiApiKey";
constexpr auto kAnthropicCredentialId = "anthropic/apiKey";
constexpr auto kGeminiCredentialId = "gemini/apiKey";
constexpr auto kOpenAIBaseUrlSettingsKey = "ai/providers/openai/baseUrl";
constexpr auto kOpenAIModelSettingsKey = "ai/providers/openai/model";
constexpr auto kGenericBaseUrlSettingsKey = "ai/connection/baseUrl";
constexpr auto kGenericModelSettingsKey = "ai/connection/model";
constexpr auto kAvailableModelsSettingsKey = "ai/connection/availableModels";
constexpr auto kRecommendedModelSettingsKey = "ai/connection/recommendedModel";
constexpr auto kAutoModelSelectionSettingsKey = "ai/connection/autoSelectModel";
constexpr auto kProviderTypeSettingsKey = "ai/connection/providerType";
constexpr auto kDetectedProtocolSettingsKey = "ai/connection/detectedProtocol";
constexpr auto kDetectedProviderSettingsKey = "ai/connection/detectedProvider";
constexpr auto kDetectedEndpointSettingsKey = "ai/connection/detectedEndpoint";
constexpr auto kResponsesEndpointSettingsKey = "ai/connection/responsesEndpoint";
constexpr int kRequestTimeoutMs = 120000;
constexpr int kAnnotationPreviewLimit = 8;

enum class GatewayProtocol
{
    OpenAIResponses,
    OpenAIChatCompletions,
    AnthropicMessages,
    GeminiGenerateContent,
    OllamaChat
};

struct GatewayAttemptResult
{
    bool success = false;
    GatewayProtocol protocol = GatewayProtocol::OpenAIChatCompletions;
    QString endpoint;
    QString protocolDisplayName;
    QString detectedProviderName;
    QString resolvedModel;
    QString text;
    QString errorMessage;
    int statusCode = 0;
    QJsonObject json;
};

struct HttpJsonResult
{
    bool success = false;
    bool timedOut = false;
    int statusCode = 0;
    QByteArray payload;
    QJsonDocument document;
    QString errorMessage;
};

QString trimTrailingSlashes(QString value)
{
    while (value.endsWith(QLatin1Char('/'))) {
        value.chop(1);
    }
    return value;
}

void appendUniqueEndpoint(QStringList* endpoints, const QString& endpoint)
{
    if (!endpoints) {
        return;
    }
    const QString trimmed = endpoint.trimmed();
    if (!trimmed.isEmpty() && !endpoints->contains(trimmed, Qt::CaseInsensitive)) {
        endpoints->push_back(trimmed);
    }
}

QUrl urlWithDefaultScheme(QString value)
{
    value = value.trimmed();
    if (value.isEmpty()) {
        return {};
    }
    if (!value.contains(QStringLiteral("://"))) {
        const QString lower = value.toLower();
        value.prepend(
            lower.startsWith(QStringLiteral("localhost")) ||
                    lower.startsWith(QStringLiteral("127.0.0.1")) ||
                    lower.startsWith(QStringLiteral("0.0.0.0"))
                ? QStringLiteral("http://")
                : QStringLiteral("https://"));
    }
    return QUrl(value);
}

int effectivePort(const QUrl& url)
{
    return url.port(url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0 ? 80 : 443);
}

QString withoutGeminiApiKeyQuery(const QString& endpoint)
{
    QUrl url(endpoint);
    if (!url.isValid()) {
        return endpoint.trimmed();
    }
    QUrlQuery query(url);
    query.removeAllQueryItems(QStringLiteral("key"));
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

QString stripAnthropicCompatPath(QString value)
{
    value = trimTrailingSlashes(value.trimmed());
    const QString lower = value.toLower();
    const QStringList markers{
        QStringLiteral("/anthropic/v1/messages"),
        QStringLiteral("/anthropic/v1/models"),
        QStringLiteral("/anthropic/v1"),
        QStringLiteral("/anthropic/messages"),
        QStringLiteral("/anthropic/models"),
        QStringLiteral("/anthropic")
    };
    for (const QString& marker : markers) {
        const int index = lower.indexOf(marker, 0, Qt::CaseInsensitive);
        if (index >= 0) {
            return trimTrailingSlashes(value.left(index));
        }
    }
    return value;
}

QString normalizedSingleLine(QString text, int maxLength = 180)
{
    text = text.simplified();
    if (text.size() <= maxLength) {
        return text;
    }
    return text.left(std::max(0, maxLength - 3)) + QStringLiteral("...");
}

void appendUniqueModel(QStringList* models, const QString& model)
{
    if (!models) {
        return;
    }
    const QString trimmed = model.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    if (!models->contains(trimmed, Qt::CaseInsensitive)) {
        models->push_back(trimmed);
    }
}

QStringList defaultSelectableModels()
{
    return {
        QStringLiteral("deepseek-v3"),
        QStringLiteral("qwen-max"),
        QStringLiteral("gpt-5.5"),
        QStringLiteral("gpt-5.5-mini"),
        QStringLiteral("gpt-5.4"),
        QStringLiteral("gpt-5.4-mini"),
        QStringLiteral("gpt-5-mini"),
        QStringLiteral("gpt-5"),
        QStringLiteral("gemini-2.5-flash"),
        QStringLiteral("gemini-2.5-pro"),
        QStringLiteral("claude-sonnet-4-6")
    };
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

bool isVisionCapableModel(const QString& model)
{
    const QString lower = model.trimmed().toLower();
    // Known text-only model families
    if (lower.startsWith(QStringLiteral("deepseek")) ||
        lower.startsWith(QStringLiteral("qwen-")) ||
        lower.startsWith(QStringLiteral("kimi")) ||
        lower.startsWith(QStringLiteral("doubao")) ||
        lower.contains(QStringLiteral("text-only")) ||
        lower.contains(QStringLiteral("-text")) ||
        lower.contains(QStringLiteral("embedding")) ||
        lower.contains(QStringLiteral("rerank"))) {
        return false;
    }
    // Known vision-capable models
    if (lower.startsWith(QStringLiteral("gpt-4o")) ||
        lower.startsWith(QStringLiteral("gpt-5")) ||
        lower.startsWith(QStringLiteral("claude")) ||
        lower.startsWith(QStringLiteral("gemini")) ||
        lower.startsWith(QStringLiteral("o1")) ||
        lower.startsWith(QStringLiteral("o3")) ||
        lower.startsWith(QStringLiteral("o4")) ||
        lower.contains(QStringLiteral("vision")) ||
        lower.contains(QStringLiteral("-vl")) ||
        lower.contains(QStringLiteral("llava")) ||
        lower.contains(QStringLiteral("-mm")) ||
        lower.startsWith(QStringLiteral("gemma"))) {
        return true;
    }
    // Unknown — assume capable (let the API decide)
    return true;
}

int openAIModelPriorityScore(const QString& model)
{
    const QString lower = model.trimmed().toLower();
    if (lower.isEmpty()) {
        return 1000;
    }
    if (isNonChatModel(lower)) {
        return 900;
    }
    if (lower == QStringLiteral("deepseek-v4-pro")) {
        return 0;
    }
    if (lower == QStringLiteral("deepseek-v4-flash")) {
        return 5;
    }
    if (lower == QStringLiteral("deepseek-v3")) {
        return 10;
    }
    if (lower == QStringLiteral("deepseek_v4")) {
        return 0;
    }
    if (lower == QStringLiteral("qwen-max")) {
        return 20;
    }
    if (lower == QStringLiteral("gpt-5.5")) {
        return 30;
    }
    if (lower == QStringLiteral("gpt-5.5-mini")) {
        return 35;
    }
    if (lower == QStringLiteral("gpt-5.4")) {
        return 40;
    }
    if (lower == QStringLiteral("gpt-5.4-mini")) {
        return 45;
    }
    if (lower == QStringLiteral("gpt-5-mini")) {
        return 50;
    }
    if (lower == QStringLiteral("gpt-5")) {
        return 55;
    }
    if (lower == QStringLiteral("gemini-2.5-flash")) {
        return 60;
    }
    if (lower == QStringLiteral("gemini-2.5-pro")) {
        return 65;
    }
    if (lower == QStringLiteral("claude-sonnet-4-6") ||
        lower == QStringLiteral("claude-sonnet-4") ||
        lower == QStringLiteral("claude-sonnet")) {
        return 70;
    }
    if (lower.contains(QStringLiteral("deepseek"))) {
        return 80;
    }
    if (lower.contains(QStringLiteral("qwen"))) {
        return 90;
    }
    if (lower.contains(QStringLiteral("gpt-5.5-openai-compact"))) {
        return 95;
    }
    if (lower.contains(QStringLiteral("gpt-5.5"))) {
        return 100;
    }
    if (lower.contains(QStringLiteral("gpt-5.4-openai-compact"))) {
        return 105;
    }
    if (lower.contains(QStringLiteral("gpt-5.4"))) {
        return 110;
    }
    if (lower.contains(QStringLiteral("gpt-5-mini"))) {
        return 115;
    }
    if (lower.contains(QStringLiteral("gpt-5"))) {
        return lower.contains(QStringLiteral("codex")) ? 180 : 120;
    }
    if (lower.contains(QStringLiteral("gemini"))) {
        return 130;
    }
    if (lower.contains(QStringLiteral("claude"))) {
        return 140;
    }
    if (lower.contains(QStringLiteral("codex"))) {
        return 190;
    }
    return 200;
}

QString preferredModelFromList(const QStringList& models)
{
    QStringList candidates;
    for (const QString& model : models) {
        appendUniqueModel(&candidates, model);
    }
    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [](const QString& lhs, const QString& rhs) {
            const int lhsScore = openAIModelPriorityScore(lhs);
            const int rhsScore = openAIModelPriorityScore(rhs);
            if (lhsScore != rhsScore) {
                return lhsScore < rhsScore;
            }
            if (lhs.size() != rhs.size()) {
                return lhs.size() < rhs.size();
            }
            return lhs.compare(rhs, Qt::CaseInsensitive) < 0;
        });
    return candidates.isEmpty() ? QString() : candidates.front();
}

QString scopeText(AIRequestScope scope)
{
    switch (scope) {
    case AIRequestScope::CurrentFrame:
        return QStringLiteral("current-frame");
    case AIRequestScope::CurrentShot:
        return QStringLiteral("current-shot");
    case AIRequestScope::CurrentSequence:
        return QStringLiteral("current-sequence");
    case AIRequestScope::CurrentReview:
        return QStringLiteral("current-review");
    case AIRequestScope::CompareAB:
        return QStringLiteral("compare-ab");
    case AIRequestScope::Custom:
    default:
        return QStringLiteral("custom");
    }
}

QString annotationPreview(const AnnotationItem& annotation)
{
    const QString comment = normalizedSingleLine(annotation.latestCommentText(), 120);
    QStringList parts{
        QStringLiteral("frame=%1").arg(annotation.frame),
        QStringLiteral("type=%1").arg(annotationTypeString(annotation.type)),
        QStringLiteral("status=%1").arg(reviewStatusString(annotation.status))
    };
    if (!annotation.id.isEmpty()) {
        parts.push_back(QStringLiteral("id=%1").arg(annotation.id));
    }
    if (!comment.isEmpty()) {
        parts.push_back(QStringLiteral("comment=%1").arg(comment));
    }
    return QStringLiteral("- %1").arg(parts.join(QStringLiteral(", ")));
}

QString framePreview(const AIMediaFrameReference& frame, int index)
{
    QStringList parts{
        QStringLiteral("frame[%1]").arg(index),
        QStringLiteral("size=%1x%2").arg(frame.size.width()).arg(frame.size.height())
    };

    const QString compareRole = frame.metadata.value(QStringLiteral("compareRole")).toString().trimmed();
    if (!compareRole.isEmpty()) {
        parts.push_back(QStringLiteral("compareRole=%1").arg(compareRole));
    }
    const QString compareSegment = frame.metadata.value(QStringLiteral("compareSegment")).toString().trimmed();
    if (!compareSegment.isEmpty()) {
        parts.push_back(QStringLiteral("compareSegment=%1").arg(compareSegment));
    }
    const QString compareModeName = frame.metadata.value(QStringLiteral("compareModeName")).toString().trimmed();
    if (!compareModeName.isEmpty()) {
        parts.push_back(QStringLiteral("compareMode=%1").arg(compareModeName));
    }
    const QString compareALabel = frame.metadata.value(QStringLiteral("compareALabel")).toString().trimmed();
    if (!compareALabel.isEmpty()) {
        parts.push_back(QStringLiteral("compareALabel=%1").arg(compareALabel));
    }
    const QString compareBLabel = frame.metadata.value(QStringLiteral("compareBLabel")).toString().trimmed();
    if (!compareBLabel.isEmpty()) {
        parts.push_back(QStringLiteral("compareBLabel=%1").arg(compareBLabel));
    }
    const QString captureMode = frame.metadata.value(QStringLiteral("captureMode")).toString().trimmed();
    if (!captureMode.isEmpty()) {
        parts.push_back(QStringLiteral("captureMode=%1").arg(captureMode));
    }
    return QStringLiteral("- %1").arg(parts.join(QStringLiteral(", ")));
}

QString extractTextField(const QJsonValue& value)
{
    if (value.isString()) {
        return value.toString().trimmed();
    }
    if (!value.isObject()) {
        return {};
    }

    const QJsonObject object = value.toObject();
    const QString valueText = object.value(QStringLiteral("value")).toString().trimmed();
    if (!valueText.isEmpty()) {
        return valueText;
    }
    return object.value(QStringLiteral("text")).toString().trimmed();
}

void collectOutputText(const QJsonValue& value, QStringList* outTexts)
{
    if (!outTexts) {
        return;
    }

    if (value.isArray()) {
        for (const QJsonValue& item : value.toArray()) {
            collectOutputText(item, outTexts);
        }
        return;
    }

    if (!value.isObject()) {
        return;
    }

    const QJsonObject object = value.toObject();
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type.contains(QStringLiteral("text"))) {
        const QString text = extractTextField(object.value(QStringLiteral("text")));
        if (!text.isEmpty()) {
            outTexts->append(text);
        }
    }

    if (object.value(QStringLiteral("content")).isArray()) {
        collectOutputText(object.value(QStringLiteral("content")), outTexts);
    }
}

QString protocolDisplayName(GatewayProtocol protocol)
{
    switch (protocol) {
    case GatewayProtocol::OpenAIResponses:
        return QStringLiteral("OpenAI Responses 接口");
    case GatewayProtocol::OpenAIChatCompletions:
        return QStringLiteral("OpenAI Chat 接口");
    case GatewayProtocol::AnthropicMessages:
        return QStringLiteral("Anthropic Messages 接口");
    case GatewayProtocol::GeminiGenerateContent:
        return QStringLiteral("Gemini GenerateContent 接口");
    case GatewayProtocol::OllamaChat:
        return QStringLiteral("Ollama Chat 接口");
    default:
        return QStringLiteral("未知协议");
    }
}

bool looksLikeOpenAIModel(const QString& model)
{
    const QString normalized = model.trimmed().toLower();
    return normalized.startsWith(QStringLiteral("gpt-")) ||
        normalized.startsWith(QStringLiteral("o1")) ||
        normalized.startsWith(QStringLiteral("o3")) ||
        normalized.startsWith(QStringLiteral("o4"));
}

bool looksLikeAnthropicModel(const QString& model)
{
    const QString normalized = model.trimmed().toLower();
    return normalized.startsWith(QStringLiteral("claude")) ||
        normalized.startsWith(QStringLiteral("deepseek")) ||
        normalized.startsWith(QStringLiteral("qwen")) ||
        normalized.startsWith(QStringLiteral("kimi")) ||
        normalized.startsWith(QStringLiteral("doubao"));
}

bool looksLikeGeminiModel(const QString& model)
{
    return model.trimmed().toLower().startsWith(QStringLiteral("gemini"));
}

bool looksLikeOllamaModelName(const QString& model)
{
    const QString normalized = model.trimmed().toLower();
    return normalized.contains(QLatin1Char(':')) ||
        normalized.startsWith(QStringLiteral("llama")) ||
        normalized.startsWith(QStringLiteral("qwen")) ||
        normalized.startsWith(QStringLiteral("gemma")) ||
        normalized.startsWith(QStringLiteral("mistral"));
}

QString normalizeModelForProtocol(QString model, GatewayProtocol protocol)
{
    model = model.trimmed();
    const QString normalized = model.toLower();
    if (normalized == QStringLiteral("deepseek_v4") ||
        normalized == QStringLiteral("deepseek-v4")) {
        model = QStringLiteral("deepseek-v4-pro");
    } else if (normalized == QStringLiteral("deepseek_v4_flash")) {
        model = QStringLiteral("deepseek-v4-flash");
    } else if (normalized == QStringLiteral("deepseek_v4_pro")) {
        model = QStringLiteral("deepseek-v4-pro");
    }

    if (model.isEmpty()) {
        return model;
    }

    switch (protocol) {
    case GatewayProtocol::OpenAIResponses:
    case GatewayProtocol::OpenAIChatCompletions:
        return model;
    case GatewayProtocol::AnthropicMessages:
        return model;
    case GatewayProtocol::GeminiGenerateContent:
        return looksLikeGeminiModel(model) ? model : QString();
    case GatewayProtocol::OllamaChat:
        return looksLikeOllamaModelName(model) ? model : QString();
    default:
        return model;
    }
}

QString protocolSettingValue(GatewayProtocol protocol)
{
    switch (protocol) {
    case GatewayProtocol::OpenAIResponses:
        return QStringLiteral("openai_responses");
    case GatewayProtocol::OpenAIChatCompletions:
        return QStringLiteral("openai_chat");
    case GatewayProtocol::AnthropicMessages:
        return QStringLiteral("anthropic_messages");
    case GatewayProtocol::GeminiGenerateContent:
        return QStringLiteral("gemini_generate_content");
    case GatewayProtocol::OllamaChat:
        return QStringLiteral("ollama_chat");
    default:
        return QString();
    }
}

GatewayProtocol protocolFromSettingValue(const QString& value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("openai_responses")) {
        return GatewayProtocol::OpenAIResponses;
    }
    if (normalized == QStringLiteral("anthropic_messages")) {
        return GatewayProtocol::AnthropicMessages;
    }
    if (normalized == QStringLiteral("gemini_generate_content")) {
        return GatewayProtocol::GeminiGenerateContent;
    }
    if (normalized == QStringLiteral("ollama_chat")) {
        return GatewayProtocol::OllamaChat;
    }
    return GatewayProtocol::OpenAIChatCompletions;
}

bool isOfficialOpenAIBaseUrl(const QString& baseUrl)
{
    const QString normalized = trimTrailingSlashes(baseUrl.trimmed()).toLower();
    return normalized == QStringLiteral("https://api.openai.com") ||
        normalized == QStringLiteral("https://api.openai.com/v1");
}

bool looksLikeAnthropicBaseUrl(const QString& baseUrl)
{
    return baseUrl.contains(QStringLiteral("anthropic"), Qt::CaseInsensitive);
}

bool looksLikeGeminiBaseUrl(const QString& baseUrl)
{
    return baseUrl.contains(QStringLiteral("generativelanguage.googleapis.com"), Qt::CaseInsensitive) ||
        baseUrl.contains(QStringLiteral("googleapis.com"), Qt::CaseInsensitive) ||
        baseUrl.contains(QStringLiteral("google.ai"), Qt::CaseInsensitive);
}

bool looksLikeOllamaBaseUrl(const QString& baseUrl)
{
    return baseUrl.contains(QStringLiteral("ollama"), Qt::CaseInsensitive) ||
        baseUrl.contains(QStringLiteral("127.0.0.1:11434"), Qt::CaseInsensitive) ||
        baseUrl.contains(QStringLiteral("localhost:11434"), Qt::CaseInsensitive);
}

QString normalizeOpenAIResponsesEndpoint(QString configuredBaseUrl)
{
    configuredBaseUrl = trimTrailingSlashes(configuredBaseUrl.trimmed());
    if (configuredBaseUrl.isEmpty()) {
        configuredBaseUrl = QString::fromLatin1(kDefaultOpenAIBaseUrl);
    }
    if (configuredBaseUrl.endsWith(QStringLiteral("/responses"))) {
        return configuredBaseUrl;
    }
    if (configuredBaseUrl.endsWith(QStringLiteral("/v1"))) {
        return configuredBaseUrl + QStringLiteral("/responses");
    }
    return configuredBaseUrl + QStringLiteral("/v1/responses");
}

QString normalizeOpenAIChatEndpoint(QString configuredBaseUrl)
{
    configuredBaseUrl = trimTrailingSlashes(configuredBaseUrl.trimmed());
    if (configuredBaseUrl.isEmpty()) {
        configuredBaseUrl = QString::fromLatin1(kDefaultOpenAIBaseUrl);
    }
    if (configuredBaseUrl.endsWith(QStringLiteral("/chat/completions"))) {
        return configuredBaseUrl;
    }
    if (configuredBaseUrl.endsWith(QStringLiteral("/v1"))) {
        return configuredBaseUrl + QStringLiteral("/chat/completions");
    }
    return configuredBaseUrl + QStringLiteral("/v1/chat/completions");
}

QString normalizeAnthropicEndpoint(QString configuredBaseUrl)
{
    configuredBaseUrl = trimTrailingSlashes(configuredBaseUrl.trimmed());
    if (configuredBaseUrl.isEmpty()) {
        configuredBaseUrl = QStringLiteral("https://api.anthropic.com");
    }
    if (configuredBaseUrl.endsWith(QStringLiteral("/messages"))) {
        return configuredBaseUrl;
    }
    if (configuredBaseUrl.endsWith(QStringLiteral("/v1"))) {
        return configuredBaseUrl + QStringLiteral("/messages");
    }
    return configuredBaseUrl + QStringLiteral("/v1/messages");
}

QString normalizeGeminiEndpoint(QString configuredBaseUrl, const QString& model)
{
    configuredBaseUrl = trimTrailingSlashes(withoutGeminiApiKeyQuery(configuredBaseUrl));
    if (configuredBaseUrl.isEmpty()) {
        configuredBaseUrl = QStringLiteral("https://generativelanguage.googleapis.com");
    }
    if (configuredBaseUrl.contains(QStringLiteral(":generateContent"), Qt::CaseInsensitive)) {
        return withoutGeminiApiKeyQuery(configuredBaseUrl);
    }
    if (configuredBaseUrl.contains(QStringLiteral("/models/"), Qt::CaseInsensitive)) {
        return withoutGeminiApiKeyQuery(configuredBaseUrl + QStringLiteral(":generateContent"));
    }
    if (configuredBaseUrl.endsWith(QStringLiteral("/v1beta"))) {
        return withoutGeminiApiKeyQuery(
            configuredBaseUrl + QStringLiteral("/models/%1:generateContent").arg(model));
    }
    return withoutGeminiApiKeyQuery(
        configuredBaseUrl + QStringLiteral("/v1beta/models/%1:generateContent").arg(model));
}

QString normalizeOllamaEndpoint(QString configuredBaseUrl)
{
    configuredBaseUrl = trimTrailingSlashes(configuredBaseUrl.trimmed());
    if (configuredBaseUrl.isEmpty()) {
        configuredBaseUrl = QStringLiteral("http://127.0.0.1:11434");
    }
    if (configuredBaseUrl.endsWith(QStringLiteral("/api/chat"))) {
        return configuredBaseUrl;
    }
    if (configuredBaseUrl.endsWith(QStringLiteral("/api"))) {
        return configuredBaseUrl + QStringLiteral("/chat");
    }
    return configuredBaseUrl + QStringLiteral("/api/chat");
}

QString defaultBaseUrlForProtocol(GatewayProtocol protocol)
{
    switch (protocol) {
    case GatewayProtocol::AnthropicMessages:
        return QStringLiteral("https://api.anthropic.com");
    case GatewayProtocol::GeminiGenerateContent:
        return QStringLiteral("https://generativelanguage.googleapis.com");
    case GatewayProtocol::OllamaChat:
        return QStringLiteral("http://127.0.0.1:11434");
    case GatewayProtocol::OpenAIResponses:
    case GatewayProtocol::OpenAIChatCompletions:
    default:
        return QString::fromLatin1(kDefaultOpenAIBaseUrl);
    }
}

bool detectedEndpointMatchesBase(
    const QString& detectedEndpoint,
    const QString& configuredBaseUrl,
    GatewayProtocol protocol)
{
    const QUrl endpointUrl = urlWithDefaultScheme(detectedEndpoint);
    const QUrl baseUrl = urlWithDefaultScheme(
        configuredBaseUrl.trimmed().isEmpty()
            ? defaultBaseUrlForProtocol(protocol)
            : configuredBaseUrl);
    if (!endpointUrl.isValid() || endpointUrl.host().isEmpty() ||
        !baseUrl.isValid() || baseUrl.host().isEmpty()) {
        return false;
    }
    return endpointUrl.scheme().compare(baseUrl.scheme(), Qt::CaseInsensitive) == 0 &&
        endpointUrl.host().compare(baseUrl.host(), Qt::CaseInsensitive) == 0 &&
        effectivePort(endpointUrl) == effectivePort(baseUrl);
}

QStringList runtimeEndpointCandidates(
    GatewayProtocol protocol,
    const QString& baseUrl,
    const QString& detectedProtocol,
    const QString& detectedEndpoint,
    const QString& detectedResponsesEndpoint,
    const QString& model)
{
    QStringList endpoints;
    if (protocol == GatewayProtocol::OpenAIResponses &&
        detectedEndpointMatchesBase(detectedResponsesEndpoint, baseUrl, protocol)) {
        appendUniqueEndpoint(&endpoints, detectedResponsesEndpoint);
    }
    if (protocolSettingValue(protocol).compare(detectedProtocol.trimmed(), Qt::CaseInsensitive) == 0 &&
        detectedEndpointMatchesBase(detectedEndpoint, baseUrl, protocol)) {
        appendUniqueEndpoint(
            &endpoints,
            protocol == GatewayProtocol::GeminiGenerateContent
                ? withoutGeminiApiKeyQuery(detectedEndpoint)
                : detectedEndpoint);
    }

    if (protocol == GatewayProtocol::OpenAIResponses ||
        protocol == GatewayProtocol::OpenAIChatCompletions) {
        const auto appendOpenAICandidates = [&](const QString& candidateBaseUrl) {
            const AIOpenAIEndpointCandidates candidates =
                AIConnectionValidator::openAICompatibleEndpointCandidates(candidateBaseUrl, model);
            const QStringList protocolEndpoints = protocol == GatewayProtocol::OpenAIResponses
                ? candidates.responses
                : candidates.chatCompletions;
            for (const QString& endpoint : protocolEndpoints) {
                appendUniqueEndpoint(&endpoints, endpoint);
            }
        };

        appendOpenAICandidates(baseUrl);
        const QString openAICompatBase = stripAnthropicCompatPath(baseUrl);
        if (!openAICompatBase.isEmpty() &&
            openAICompatBase.compare(trimTrailingSlashes(baseUrl), Qt::CaseInsensitive) != 0) {
            appendOpenAICandidates(openAICompatBase);
        }
    } else if (protocol == GatewayProtocol::AnthropicMessages) {
        appendUniqueEndpoint(&endpoints, normalizeAnthropicEndpoint(baseUrl));
    } else if (protocol == GatewayProtocol::GeminiGenerateContent) {
        appendUniqueEndpoint(&endpoints, normalizeGeminiEndpoint(baseUrl, model));
    } else {
        appendUniqueEndpoint(&endpoints, normalizeOllamaEndpoint(baseUrl));
    }
    return endpoints;
}

QString normalizeOpenAITranscriptionsEndpoint(QString configuredBaseUrl)
{
    configuredBaseUrl = stripAnthropicCompatPath(configuredBaseUrl);
    configuredBaseUrl = trimTrailingSlashes(configuredBaseUrl.trimmed());
    if (configuredBaseUrl.isEmpty()) {
        configuredBaseUrl = QString::fromLatin1(kDefaultOpenAIBaseUrl);
    }
    if (configuredBaseUrl.endsWith(QStringLiteral("/audio/transcriptions"))) {
        return configuredBaseUrl;
    }
    if (configuredBaseUrl.endsWith(QStringLiteral("/v1"))) {
        return configuredBaseUrl + QStringLiteral("/audio/transcriptions");
    }
    return configuredBaseUrl + QStringLiteral("/v1/audio/transcriptions");
}

QString normalizeOpenAIModelsEndpoint(QString configuredBaseUrl)
{
    configuredBaseUrl = trimTrailingSlashes(configuredBaseUrl.trimmed());
    if (configuredBaseUrl.isEmpty()) {
        configuredBaseUrl = QString::fromLatin1(kDefaultOpenAIBaseUrl);
    }
    if (configuredBaseUrl.endsWith(QStringLiteral("/models"))) {
        return configuredBaseUrl;
    }
    if (configuredBaseUrl.endsWith(QStringLiteral("/v1"))) {
        return configuredBaseUrl + QStringLiteral("/models");
    }
    return configuredBaseUrl + QStringLiteral("/v1/models");
}

QString normalizeOllamaModelsEndpoint(QString configuredBaseUrl)
{
    configuredBaseUrl = trimTrailingSlashes(configuredBaseUrl.trimmed());
    if (configuredBaseUrl.isEmpty()) {
        configuredBaseUrl = QStringLiteral("http://127.0.0.1:11434");
    }
    if (configuredBaseUrl.endsWith(QStringLiteral("/api/tags"))) {
        return configuredBaseUrl;
    }
    if (configuredBaseUrl.endsWith(QStringLiteral("/api"))) {
        return configuredBaseUrl + QStringLiteral("/tags");
    }
    return configuredBaseUrl + QStringLiteral("/api/tags");
}

QString formatModelListingError(const QString& endpoint)
{
    return QStringLiteral("未配置可用模型，且自动探测模型失败：%1").arg(endpoint);
}

QString httpErrorMessage(QNetworkReply* reply, const QByteArray& payload, const QJsonObject& payloadObject)
{
    QString message;
    if (payloadObject.value(QStringLiteral("error")).isObject()) {
        message = payloadObject.value(QStringLiteral("error")).toObject()
            .value(QStringLiteral("message")).toString().trimmed();
    } else if (payloadObject.value(QStringLiteral("error")).isString()) {
        message = payloadObject.value(QStringLiteral("error")).toString().trimmed();
    }
    if (message.isEmpty() && reply) {
        message = reply->errorString().trimmed();
    }
    if (message.isEmpty() && !payload.isEmpty()) {
        message = QString::fromUtf8(payload).trimmed();
    }
    const int statusCode = reply
        ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
        : 0;
    if (statusCode > 0 && !message.startsWith(QStringLiteral("HTTP "))) {
        message = QStringLiteral("HTTP %1: %2").arg(statusCode).arg(message);
    }
    return message;
}

HttpJsonResult executeJsonRequest(
    const QString& method,
    const QUrl& url,
    const QList<QPair<QByteArray, QByteArray>>& headers,
    const QJsonObject* body = nullptr)
{
    HttpJsonResult result;
    if (!url.isValid()) {
        result.errorMessage = QStringLiteral("接口地址无效：%1").arg(url.toString());
        return result;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    if (body) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    }
    for (const auto& header : headers) {
        request.setRawHeader(header.first, header.second);
    }
    request.setTransferTimeout(kRequestTimeoutMs);

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(kRequestTimeoutMs);

    bool timedOut = false;
    QNetworkReply* reply = nullptr;
    if (method.compare(QStringLiteral("GET"), Qt::CaseInsensitive) == 0) {
        reply = manager.get(request);
    } else {
        const QByteArray bodyJson = body
            ? QJsonDocument(*body).toJson(QJsonDocument::Compact)
            : QByteArray();
        reply = manager.post(request, bodyJson);
    }

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        if (reply) {
            reply->abort();
        }
        loop.quit();
    });

    timeoutTimer.start();
    loop.exec();
    timeoutTimer.stop();

    result.payload = reply->readAll();
    result.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    QJsonParseError parseError;
    result.document = QJsonDocument::fromJson(result.payload, &parseError);
    if (timedOut) {
        result.timedOut = true;
        result.errorMessage = QStringLiteral("请求超时");
        reply->deleteLater();
        return result;
    }

    const QJsonObject payloadObject = result.document.isObject() ? result.document.object() : QJsonObject{};
    if (reply->error() != QNetworkReply::NoError) {
        result.errorMessage = httpErrorMessage(reply, result.payload, payloadObject);
        reply->deleteLater();
        return result;
    }

    if (parseError.error != QJsonParseError::NoError) {
        result.errorMessage = QStringLiteral("返回内容不是合法 JSON：%1").arg(parseError.errorString());
        reply->deleteLater();
        return result;
    }

    result.success = true;
    reply->deleteLater();
    return result;
}

HttpJsonResult executeMultipartRequest(
    const QUrl& url,
    const QList<QPair<QByteArray, QByteArray>>& headers,
    QHttpMultiPart* multipart)
{
    HttpJsonResult result;
    if (!multipart || !url.isValid()) {
        result.errorMessage = QStringLiteral("Invalid multipart request");
        return result;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    for (const auto& header : headers) {
        request.setRawHeader(header.first, header.second);
    }
    request.setTransferTimeout(kRequestTimeoutMs);

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(kRequestTimeoutMs);

    bool timedOut = false;
    QNetworkReply* reply = manager.post(request, multipart);
    multipart->setParent(reply);

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        if (reply) {
            reply->abort();
        }
        loop.quit();
    });

    timeoutTimer.start();
    loop.exec();
    timeoutTimer.stop();

    result.payload = reply->readAll();
    result.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    QJsonParseError parseError;
    result.document = QJsonDocument::fromJson(result.payload, &parseError);
    if (timedOut) {
        result.timedOut = true;
        result.errorMessage = QStringLiteral("Request timed out");
        reply->deleteLater();
        return result;
    }

    const QJsonObject payloadObject = result.document.isObject() ? result.document.object() : QJsonObject{};
    if (reply->error() != QNetworkReply::NoError) {
        result.errorMessage = httpErrorMessage(reply, result.payload, payloadObject);
        reply->deleteLater();
        return result;
    }

    if (parseError.error != QJsonParseError::NoError) {
        result.errorMessage = QStringLiteral("Response is not valid JSON: %1").arg(parseError.errorString());
        reply->deleteLater();
        return result;
    }

    result.success = true;
    reply->deleteLater();
    return result;
}

QList<QPair<QByteArray, QByteArray>> bearerHeaders(const QByteArray& apiKey)
{
    return {
        { QByteArray("Authorization"), QByteArray("Bearer ") + apiKey }
    };
}

QString firstAvailableOpenAIModel(const QString& baseUrl, const QByteArray& apiKey)
{
    QStringList endpoints;
    const auto appendModelCandidates = [&endpoints](const QString& candidateBaseUrl) {
        const AIOpenAIEndpointCandidates candidates =
            AIConnectionValidator::openAICompatibleEndpointCandidates(candidateBaseUrl);
        for (const QString& endpoint : candidates.models) {
            appendUniqueEndpoint(&endpoints, endpoint);
        }
    };

    appendModelCandidates(baseUrl);

    const QString openAICompatBase = stripAnthropicCompatPath(baseUrl);
    if (!openAICompatBase.isEmpty() &&
        openAICompatBase.compare(trimTrailingSlashes(baseUrl), Qt::CaseInsensitive) != 0) {
        appendModelCandidates(openAICompatBase);
    }

    for (const QString& endpoint : endpoints) {
        const HttpJsonResult result = executeJsonRequest(
            QStringLiteral("GET"),
            QUrl(endpoint),
            AIConnectionValidator::openAICompatibleHeaders(endpoint, apiKey));
        if (!result.success || !result.document.isObject()) {
            continue;
        }

        const QJsonArray data = result.document.object().value(QStringLiteral("data")).toArray();
        QStringList models;
        for (const QJsonValue& item : data) {
            const QString id = item.toObject().value(QStringLiteral("id")).toString().trimmed();
            appendUniqueModel(&models, id);
        }
        const QString preferred = preferredModelFromList(models);
        if (!preferred.isEmpty()) {
            return preferred;
        }
    }
    return {};
}

QString firstAvailableOllamaModel(const QString& baseUrl)
{
    const HttpJsonResult result = executeJsonRequest(
        QStringLiteral("GET"),
        QUrl(normalizeOllamaModelsEndpoint(baseUrl)),
        {});
    if (!result.success || !result.document.isObject()) {
        return {};
    }

    const QJsonArray models = result.document.object().value(QStringLiteral("models")).toArray();
    for (const QJsonValue& item : models) {
        const QJsonObject modelObject = item.toObject();
        const QString name = modelObject.value(QStringLiteral("model")).toString().trimmed();
        if (!name.isEmpty()) {
            return name;
        }
        const QString fallbackName = modelObject.value(QStringLiteral("name")).toString().trimmed();
        if (!fallbackName.isEmpty()) {
            return fallbackName;
        }
    }
    return {};
}

QString extractOpenAIChatText(const QJsonObject& responseObject)
{
    const QJsonArray choices = responseObject.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        return {};
    }

    const QJsonObject message = choices.first().toObject().value(QStringLiteral("message")).toObject();
    const QJsonValue content = message.value(QStringLiteral("content"));
    if (content.isString()) {
        return content.toString().trimmed();
    }
    QStringList texts;
    collectOutputText(content, &texts);
    return texts.join(QStringLiteral("\n\n")).trimmed();
}

QString extractAnthropicText(const QJsonObject& responseObject)
{
    QStringList texts;
    const QJsonArray content = responseObject.value(QStringLiteral("content")).toArray();
    for (const QJsonValue& item : content) {
        const QJsonObject object = item.toObject();
        if (object.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
            const QString text = object.value(QStringLiteral("text")).toString().trimmed();
            if (!text.isEmpty()) {
                texts.push_back(text);
            }
        }
    }
    return texts.join(QStringLiteral("\n\n")).trimmed();
}

QString extractGeminiText(const QJsonObject& responseObject)
{
    QStringList texts;
    const QJsonArray candidates = responseObject.value(QStringLiteral("candidates")).toArray();
    for (const QJsonValue& candidateValue : candidates) {
        const QJsonObject content = candidateValue.toObject().value(QStringLiteral("content")).toObject();
        const QJsonArray parts = content.value(QStringLiteral("parts")).toArray();
        for (const QJsonValue& partValue : parts) {
            const QString text = partValue.toObject().value(QStringLiteral("text")).toString().trimmed();
            if (!text.isEmpty()) {
                texts.push_back(text);
            }
        }
    }
    return texts.join(QStringLiteral("\n\n")).trimmed();
}

QString extractOllamaText(const QJsonObject& responseObject)
{
    return responseObject.value(QStringLiteral("message")).toObject()
        .value(QStringLiteral("content")).toString().trimmed();
}

QString extractResponseModelId(const QJsonObject& responseObject)
{
    QString model = responseObject.value(QStringLiteral("model")).toString().trimmed();
    if (!model.isEmpty()) {
        return model;
    }

    model = responseObject.value(QStringLiteral("modelVersion")).toString().trimmed();
    if (!model.isEmpty()) {
        return model;
    }

    return {};
}

int firstNonNegativeTokenValue(std::initializer_list<int> values)
{
    for (const int value : values) {
        if (value >= 0) {
            return value;
        }
    }
    return -1;
}

int nestedTokenValue(const QJsonObject& object, const QString& outerKey, const QString& innerKey)
{
    return object.value(outerKey).toObject().value(innerKey).toInt(-1);
}

double computeTokenCacheHitRate(int cachedTokens, int totalPromptTokens)
{
    if (cachedTokens < 0 || totalPromptTokens <= 0) {
        return -1.0;
    }
    return (static_cast<double>(cachedTokens) * 100.0) /
        static_cast<double>(totalPromptTokens);
}

void extractUsageMetrics(const QJsonObject& responseObject, AIResponse* response)
{
    if (!response) {
        return;
    }

    const QJsonObject usage = responseObject.value(QStringLiteral("usage")).toObject();
    if (!usage.isEmpty()) {
        const int promptTokens = firstNonNegativeTokenValue({
            usage.value(QStringLiteral("prompt_tokens")).toInt(-1),
            usage.value(QStringLiteral("input_tokens")).toInt(-1)
        });
        const int completionTokens = firstNonNegativeTokenValue({
            usage.value(QStringLiteral("completion_tokens")).toInt(-1),
            usage.value(QStringLiteral("output_tokens")).toInt(-1)
        });
        const int totalTokens = usage.value(QStringLiteral("total_tokens")).toInt(-1);
        const int cachedPromptTokens = firstNonNegativeTokenValue({
            nestedTokenValue(usage, QStringLiteral("prompt_tokens_details"), QStringLiteral("cached_tokens")),
            nestedTokenValue(usage, QStringLiteral("input_tokens_details"), QStringLiteral("cached_tokens")),
            usage.value(QStringLiteral("cache_read_input_tokens")).toInt(-1)
        });

        response->promptTokens = promptTokens;
        response->completionTokens = completionTokens;
        response->totalTokens = totalTokens >= 0
            ? totalTokens
            : ((promptTokens >= 0 && completionTokens >= 0) ? (promptTokens + completionTokens) : -1);
        response->cachedPromptTokens = cachedPromptTokens;

        int cacheRatePromptTokens = promptTokens;
        if (usage.contains(QStringLiteral("cache_read_input_tokens")) &&
            promptTokens >= 0 &&
            cachedPromptTokens >= 0)
        {
            cacheRatePromptTokens = promptTokens + cachedPromptTokens;
        }
        response->promptCacheHitRate =
            computeTokenCacheHitRate(cachedPromptTokens, cacheRatePromptTokens);
        return;
    }

    const QJsonObject usageMetadata = responseObject.value(QStringLiteral("usageMetadata")).toObject();
    if (!usageMetadata.isEmpty()) {
        const int promptTokens = usageMetadata.value(QStringLiteral("promptTokenCount")).toInt(-1);
        const int completionTokens = firstNonNegativeTokenValue({
            usageMetadata.value(QStringLiteral("candidatesTokenCount")).toInt(-1),
            usageMetadata.value(QStringLiteral("outputTokenCount")).toInt(-1)
        });
        const int totalTokens = usageMetadata.value(QStringLiteral("totalTokenCount")).toInt(-1);
        const int cachedPromptTokens =
            usageMetadata.value(QStringLiteral("cachedContentTokenCount")).toInt(-1);

        response->promptTokens = promptTokens;
        response->completionTokens = completionTokens;
        response->totalTokens = totalTokens >= 0
            ? totalTokens
            : ((promptTokens >= 0 && completionTokens >= 0) ? (promptTokens + completionTokens) : -1);
        response->cachedPromptTokens = cachedPromptTokens;
        response->promptCacheHitRate =
            computeTokenCacheHitRate(cachedPromptTokens, promptTokens);
        return;
    }

    const int ollamaPromptTokens = responseObject.value(QStringLiteral("prompt_eval_count")).toInt(-1);
    const int ollamaCompletionTokens = responseObject.value(QStringLiteral("eval_count")).toInt(-1);
    if (ollamaPromptTokens >= 0 || ollamaCompletionTokens >= 0) {
        response->promptTokens = ollamaPromptTokens;
        response->completionTokens = ollamaCompletionTokens;
        response->totalTokens =
            (ollamaPromptTokens >= 0 && ollamaCompletionTokens >= 0)
            ? (ollamaPromptTokens + ollamaCompletionTokens)
            : -1;
    }
}

QJsonObject buildAnthropicRequestBody(
    const AIRequest& request,
    const QString& resolvedModel,
    const QString& userText)
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), resolvedModel);
    body.insert(QStringLiteral("max_tokens"), request.options.value(QStringLiteral("maxOutputTokens")).toInt(1200));
    if (request.options.value(QStringLiteral("disableThinking")).toBool(false)) {
        body.insert(
            QStringLiteral("thinking"),
            QJsonObject{ { QStringLiteral("type"), QStringLiteral("disabled") } });
    }

    const QString instructions = request.systemPrompt.trimmed();
    if (!instructions.isEmpty()) {
        body.insert(QStringLiteral("system"), instructions);
    }

    QJsonArray content;
    content.append(QJsonObject{
        { QStringLiteral("type"), QStringLiteral("text") },
        { QStringLiteral("text"), userText }
    });

    for (const AIMediaFrameReference& frame : request.frames) {
        if (frame.encodedBytes.isEmpty()) {
            continue;
        }
        const QString mimeType = frame.mimeType.trimmed().isEmpty()
            ? QStringLiteral("image/png")
            : frame.mimeType.trimmed();
        content.append(QJsonObject{
            { QStringLiteral("type"), QStringLiteral("image") },
            { QStringLiteral("source"), QJsonObject{
                  { QStringLiteral("type"), QStringLiteral("base64") },
                  { QStringLiteral("media_type"), mimeType },
                  { QStringLiteral("data"), QString::fromLatin1(frame.encodedBytes.toBase64()) }
              } }
        });
    }

    body.insert(QStringLiteral("messages"), QJsonArray{
        QJsonObject{
            { QStringLiteral("role"), QStringLiteral("user") },
            { QStringLiteral("content"), content }
        }
    });
    return body;
}

QJsonObject buildGeminiRequestBody(
    const AIRequest& request,
    const QString& userText)
{
    QJsonObject body;
    const QString instructions = request.systemPrompt.trimmed();
    if (!instructions.isEmpty()) {
        body.insert(
            QStringLiteral("system_instruction"),
            QJsonObject{
                { QStringLiteral("parts"), QJsonArray{
                      QJsonObject{ { QStringLiteral("text"), instructions } }
                  } }
            });
    }

    QJsonArray parts;
    parts.append(QJsonObject{ { QStringLiteral("text"), userText } });
    for (const AIMediaFrameReference& frame : request.frames) {
        if (frame.encodedBytes.isEmpty()) {
            continue;
        }
        parts.append(QJsonObject{
            { QStringLiteral("inlineData"), QJsonObject{
                  { QStringLiteral("mimeType"),
                    frame.mimeType.trimmed().isEmpty() ? QStringLiteral("image/png") : frame.mimeType.trimmed() },
                  { QStringLiteral("data"), QString::fromLatin1(frame.encodedBytes.toBase64()) }
              } }
        });
    }

    body.insert(QStringLiteral("contents"), QJsonArray{
        QJsonObject{
            { QStringLiteral("role"), QStringLiteral("user") },
            { QStringLiteral("parts"), parts }
        }
    });
    return body;
}

QJsonObject buildOllamaRequestBody(
    const AIRequest& request,
    const QString& resolvedModel,
    const QString& userText)
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), resolvedModel);
    body.insert(QStringLiteral("stream"), false);

    QJsonArray messages;
    const QString instructions = request.systemPrompt.trimmed();
    if (!instructions.isEmpty()) {
        messages.append(QJsonObject{
            { QStringLiteral("role"), QStringLiteral("system") },
            { QStringLiteral("content"), instructions }
        });
    }

    QJsonObject userMessage{
        { QStringLiteral("role"), QStringLiteral("user") },
        { QStringLiteral("content"), userText }
    };
    if (!request.frames.isEmpty()) {
        QJsonArray images;
        for (const AIMediaFrameReference& frame : request.frames) {
            if (!frame.encodedBytes.isEmpty()) {
                images.append(QString::fromLatin1(frame.encodedBytes.toBase64()));
            }
        }
        if (!images.isEmpty()) {
            userMessage.insert(QStringLiteral("images"), images);
        }
    }
    messages.append(userMessage);
    body.insert(QStringLiteral("messages"), messages);
    return body;
}

QString formatFailureSummary(const QVector<GatewayAttemptResult>& failures)
{
    if (failures.isEmpty()) {
        return QStringLiteral("没有任何兼容的 AI 协议调用成功。");
    }

    bool all503 = true;
    bool sawOpenAI503 = false;
    bool sawPlatformNotGemini = false;
    bool sawOllama404 = false;
    QStringList lines;
    for (const GatewayAttemptResult& failure : failures) {
        if (failure.statusCode != 503) {
            all503 = false;
        }
        if ((failure.protocol == GatewayProtocol::OpenAIChatCompletions ||
             failure.protocol == GatewayProtocol::OpenAIResponses) &&
            failure.statusCode == 503) {
            sawOpenAI503 = true;
        }
        if (failure.protocol == GatewayProtocol::GeminiGenerateContent &&
            failure.errorMessage.contains(QStringLiteral("not gemini"), Qt::CaseInsensitive)) {
            sawPlatformNotGemini = true;
        }
        if (failure.protocol == GatewayProtocol::OllamaChat && failure.statusCode == 404) {
            sawOllama404 = true;
        }
        QString line = QStringLiteral("- %1").arg(failure.protocolDisplayName);
        if (!failure.endpoint.isEmpty()) {
            line += QStringLiteral(" @ %1").arg(failure.endpoint);
        }
        if (failure.statusCode > 0) {
            line += QStringLiteral(" -> HTTP %1").arg(failure.statusCode);
        }
        if (!failure.errorMessage.isEmpty()) {
            line += QStringLiteral(": %1").arg(failure.errorMessage);
        }
        lines.push_back(line);
    }

    if (all503) {
        return QStringLiteral(
                   "当前配置的网关对所有兼容协议都返回了 HTTP 503。"
                   "这通常表示上游中转服务暂时不可用或已经过载。\n%1")
            .arg(lines.join(QLatin1Char('\n')));
    }

    if (sawOpenAI503 && sawPlatformNotGemini) {
        QString message =
            QStringLiteral("当前网关可以访问，但它的 OpenAI 兼容后端暂时不可用。"
                           "这不是本地 RVLite 抓帧故障。\n");
        message += QStringLiteral("这说明：\n");
        message += QStringLiteral("- 所有 OpenAI 兼容请求都返回了 HTTP 503，说明中转平台后端宕机或过载。\n");
        message += QStringLiteral("- 当前这把 Key 不是 Gemini Key，所以 Gemini 回退链路无法接管。\n");
        if (sawOllama404) {
            message += QStringLiteral("- 当前 Base URL 对应的活跃后端不是 Ollama。\n");
        }
        message += QStringLiteral("建议下一步：\n");
        message += QStringLiteral("- 更换其他中转地址或 Base URL。\n");
        message += QStringLiteral("- 联系中转站提供方，确认哪个 OpenAI 兼容端点当前可用。\n");
        message += QStringLiteral("- 如果需要离线回退，可以切换到本地 Ollama。\n");
        message += QLatin1Char('\n');
        message += lines.join(QLatin1Char('\n'));
        return message;
    }

    return QStringLiteral("没有任何兼容的 AI 协议调用成功。\n%1")
        .arg(lines.join(QLatin1Char('\n')));
}

QVector<GatewayProtocol> candidateProtocols(
    const QString& baseUrl,
    const QString& cachedProtocol,
    const QString& requestedModel)
{
    QVector<GatewayProtocol> protocols;
    const auto appendUnique = [&protocols](GatewayProtocol protocol) {
        if (!protocols.contains(protocol)) {
            protocols.push_back(protocol);
        }
    };

    if (!cachedProtocol.trimmed().isEmpty()) {
        appendUnique(protocolFromSettingValue(cachedProtocol));
    }

    const QString normalizedModel = requestedModel.trimmed();
    if (looksLikeAnthropicModel(normalizedModel)) {
        appendUnique(GatewayProtocol::AnthropicMessages);
    } else if (looksLikeGeminiModel(normalizedModel)) {
        appendUnique(GatewayProtocol::GeminiGenerateContent);
    } else if (looksLikeOpenAIModel(normalizedModel)) {
        appendUnique(GatewayProtocol::OpenAIChatCompletions);
        appendUnique(GatewayProtocol::OpenAIResponses);
    } else if (normalizedModel.contains(QLatin1Char(':'))) {
        appendUnique(GatewayProtocol::OllamaChat);
    }

    if (looksLikeAnthropicBaseUrl(baseUrl)) {
        appendUnique(GatewayProtocol::AnthropicMessages);
        appendUnique(GatewayProtocol::OpenAIChatCompletions);
        appendUnique(GatewayProtocol::OpenAIResponses);
    } else if (looksLikeGeminiBaseUrl(baseUrl)) {
        appendUnique(GatewayProtocol::GeminiGenerateContent);
        appendUnique(GatewayProtocol::OpenAIChatCompletions);
        appendUnique(GatewayProtocol::OpenAIResponses);
    } else if (looksLikeOllamaBaseUrl(baseUrl)) {
        appendUnique(GatewayProtocol::OllamaChat);
        appendUnique(GatewayProtocol::OpenAIChatCompletions);
    } else if (isOfficialOpenAIBaseUrl(baseUrl) || baseUrl.trimmed().isEmpty()) {
        appendUnique(GatewayProtocol::OpenAIResponses);
        appendUnique(GatewayProtocol::OpenAIChatCompletions);
    } else {
        appendUnique(GatewayProtocol::OpenAIChatCompletions);
        appendUnique(GatewayProtocol::OpenAIResponses);
        appendUnique(GatewayProtocol::AnthropicMessages);
        appendUnique(GatewayProtocol::GeminiGenerateContent);
        appendUnique(GatewayProtocol::OllamaChat);
    }

    appendUnique(GatewayProtocol::OpenAIChatCompletions);
    appendUnique(GatewayProtocol::OpenAIResponses);
    appendUnique(GatewayProtocol::AnthropicMessages);
    appendUnique(GatewayProtocol::GeminiGenerateContent);
    appendUnique(GatewayProtocol::OllamaChat);
    return protocols;
}

} // namespace

OpenAIResponsesProvider::OpenAIResponsesProvider(
    IAICredentialStore* credentialStore,
    ISettingsService* settingsService)
    : _credentialStore(credentialStore)
    , _settingsService(settingsService)
{
}

QString OpenAIResponsesProvider::providerId() const
{
    return QString::fromLatin1(kProviderId);
}

QString OpenAIResponsesProvider::providerName() const
{
    return QString::fromUtf8(u8"智能识别");
}

bool OpenAIResponsesProvider::isAvailable() const
{
    const QString baseUrl = _baseUrl();
    if (looksLikeOllamaBaseUrl(baseUrl)) return true;

    const QString cachedProtocol = _settingsService
        ? _settingsService->value(QString::fromLatin1(kDetectedProtocolSettingsKey)).toString()
        : QString();
    AIRequest request;
    const QVector<GatewayProtocol> protocols = candidateProtocols(
        baseUrl,
        cachedProtocol,
        _modelForRequest(request));
    for (GatewayProtocol protocol : protocols) {
        if (protocol == GatewayProtocol::OllamaChat) continue;
        if (!_apiKey(protocolSettingValue(protocol), baseUrl).isEmpty()) return true;
    }
    return false;
}

AIProviderCapabilities OpenAIResponsesProvider::capabilities() const
{
    AIProviderCapabilities capabilities;
    capabilities.supportsText = true;
    capabilities.supportsImages = true;
    capabilities.supportsAudioTranscription = true;
    capabilities.supportsStructuredOutput = false;
    capabilities.supportsLocalRuntime = true;
    capabilities.supportsStreaming = false;
    capabilities.requiresApiKey = false;
    if (_settingsService) {
        QStringList models =
            _settingsService->value(QString::fromLatin1(kAvailableModelsSettingsKey)).toStringList();
        const QString recommended =
            _settingsService->value(QString::fromLatin1(kRecommendedModelSettingsKey)).toString().trimmed();
        const QString configured =
            _settingsService->value(QString::fromLatin1(kGenericModelSettingsKey)).toString().trimmed();
        if (!recommended.isEmpty() && !models.contains(recommended)) {
            models.prepend(recommended);
        }
        if (!configured.isEmpty() && !models.contains(configured)) {
            models.prepend(configured);
        }
        const QStringList fallbackModels = defaultSelectableModels();
        for (const QString& model : fallbackModels) {
            appendUniqueModel(&models, model);
        }
        if (models.isEmpty()) {
            models = fallbackModels;
        }
        capabilities.supportedModels = models;
    } else {
        capabilities.supportedModels = defaultSelectableModels();
    }
    return capabilities;
}

AIResponse OpenAIResponsesProvider::chat(const AIRequest& request)
{
    AIResponse response;
    response.jobId = request.jobId;
    response.providerId = providerId();
    response.startedAt = QDateTime::currentDateTimeUtc();
    response.frameCount = request.frames.size();

    const auto finishWithError = [&response](const QString& errorMessage) {
        response.success = false;
        response.errorMessage = errorMessage;
        response.finishedAt = QDateTime::currentDateTimeUtc();
        response.durationMs = response.startedAt.msecsTo(response.finishedAt);
        return response;
    };

    const QString baseUrl = _baseUrl();
    const QString requestedModel = _modelForRequest(request);

    // Check if frames are attached but the model likely doesn't support vision
    const bool hasFrames = !request.frames.isEmpty();
    const bool modelSupportsVision = isVisionCapableModel(requestedModel);
    QString visionWarningNote;
    if (hasFrames && !modelSupportsVision) {
        visionWarningNote = QStringLiteral(
            "\n\n[系统提示] 当前模型 \"%1\" 可能不支持图片输入。"
            "如果你无法看到附带的帧截图，请直接告诉用户："
            "「当前模型不支持图片分析，请切换到支持视觉的模型（如 gpt-5、claude-sonnet、gemini-2.5-flash）后重试。」"
        ).arg(requestedModel);
    }

    // Build a modified request with the vision warning note appended to system prompt
    AIRequest effectiveRequest = request;
    if (!visionWarningNote.isEmpty()) {
        effectiveRequest.systemPrompt = request.systemPrompt + visionWarningNote;
    }

    const QString cachedProtocol = _settingsService
        ? _settingsService->value(QString::fromLatin1(kDetectedProtocolSettingsKey)).toString()
        : QString();
    const QString detectedEndpoint = _settingsService
        ? _settingsService->value(QString::fromLatin1(kDetectedEndpointSettingsKey)).toString().trimmed()
        : QString();
    const QString detectedResponsesEndpoint = _settingsService
        ? _settingsService->value(QString::fromLatin1(kResponsesEndpointSettingsKey)).toString().trimmed()
        : QString();

    QVector<GatewayAttemptResult> failures;
    const QVector<GatewayProtocol> protocols = candidateProtocols(baseUrl, cachedProtocol, requestedModel);
    for (GatewayProtocol protocol : protocols) {
        GatewayAttemptResult attempt;
        attempt.protocol = protocol;
        attempt.protocolDisplayName = protocolDisplayName(protocol);
        attempt.detectedProviderName = attempt.protocolDisplayName;

        QString keyError;
        const QByteArray apiKey = _apiKey(
            protocolSettingValue(protocol),
            baseUrl,
            &keyError);
        if (apiKey.isEmpty() && protocol != GatewayProtocol::OllamaChat) {
            attempt.errorMessage = keyError.isEmpty()
                ? QStringLiteral("No API key is configured for this protocol")
                : keyError;
            failures.push_back(attempt);
            continue;
        }

        QString resolvedModel = normalizeModelForProtocol(requestedModel, protocol);
        switch (protocol) {
        case GatewayProtocol::OpenAIResponses:
        case GatewayProtocol::OpenAIChatCompletions:
            if (resolvedModel.isEmpty()) {
                resolvedModel = firstAvailableOpenAIModel(baseUrl, apiKey);
            }
            if (resolvedModel.isEmpty()) {
                resolvedModel = QString::fromLatin1(kDefaultOpenAIModel);
            }
            break;
        case GatewayProtocol::AnthropicMessages:
            if (resolvedModel.isEmpty()) {
                resolvedModel = firstAvailableOpenAIModel(baseUrl, apiKey);
            }
            if (resolvedModel.isEmpty()) {
                resolvedModel = QString::fromLatin1(kDefaultAnthropicModel);
            }
            break;
        case GatewayProtocol::GeminiGenerateContent:
            if (resolvedModel.isEmpty()) {
                resolvedModel = QString::fromLatin1(kDefaultGeminiModel);
            }
            break;
        case GatewayProtocol::OllamaChat:
            if (resolvedModel.isEmpty()) {
                resolvedModel = firstAvailableOllamaModel(baseUrl);
            }
            if (resolvedModel.isEmpty()) {
                resolvedModel = QString::fromLatin1(kDefaultOllamaModel);
            }
            break;
        }

        attempt.resolvedModel = resolvedModel;
        if (attempt.resolvedModel.isEmpty()) {
            attempt.errorMessage = formatModelListingError(baseUrl);
            failures.push_back(attempt);
            continue;
        }

        const QString userText = _composeUserText(effectiveRequest);
        HttpJsonResult httpResult;
        QStringList endpointFailures;
        const QStringList endpoints = runtimeEndpointCandidates(
            protocol,
            baseUrl,
            cachedProtocol,
            detectedEndpoint,
            detectedResponsesEndpoint,
            attempt.resolvedModel);
        for (const QString& endpoint : endpoints) {
            attempt.endpoint = endpoint;
            attempt.text.clear();
            if (protocol == GatewayProtocol::OpenAIResponses) {
                const QJsonObject body = _buildRequestBody(effectiveRequest, attempt.resolvedModel);
                httpResult = executeJsonRequest(
                    QStringLiteral("POST"),
                    QUrl(attempt.endpoint),
                    AIConnectionValidator::openAICompatibleHeaders(attempt.endpoint, apiKey),
                    &body);
                if (httpResult.success && httpResult.document.isObject()) {
                    attempt.text = _extractResponsesText(httpResult.document.object());
                }
            } else if (protocol == GatewayProtocol::OpenAIChatCompletions) {
                const QJsonObject body =
                    _buildChatCompletionsRequestBody(effectiveRequest, attempt.resolvedModel);
                httpResult = executeJsonRequest(
                    QStringLiteral("POST"),
                    QUrl(attempt.endpoint),
                    AIConnectionValidator::openAICompatibleHeaders(attempt.endpoint, apiKey),
                    &body);
                if (httpResult.success && httpResult.document.isObject()) {
                    attempt.text = _extractChatCompletionsText(httpResult.document.object());
                }
            } else if (protocol == GatewayProtocol::AnthropicMessages) {
                const QList<QPair<QByteArray, QByteArray>> headers{
                    { QByteArray("x-api-key"), apiKey },
                    { QByteArray("anthropic-version"), QByteArray("2023-06-01") }
                };
                const QJsonObject body =
                    buildAnthropicRequestBody(effectiveRequest, attempt.resolvedModel, userText);
                httpResult = executeJsonRequest(
                    QStringLiteral("POST"), QUrl(attempt.endpoint), headers, &body);
                if (httpResult.success && httpResult.document.isObject()) {
                    attempt.text = extractAnthropicText(httpResult.document.object());
                }
            } else if (protocol == GatewayProtocol::GeminiGenerateContent) {
                const QList<QPair<QByteArray, QByteArray>> headers{
                    { QByteArray("x-goog-api-key"), apiKey }
                };
                const QJsonObject body = buildGeminiRequestBody(effectiveRequest, userText);
                httpResult = executeJsonRequest(
                    QStringLiteral("POST"), QUrl(attempt.endpoint), headers, &body);
                if (httpResult.success && httpResult.document.isObject()) {
                    attempt.text = extractGeminiText(httpResult.document.object());
                }
            } else {
                const QJsonObject body =
                    buildOllamaRequestBody(effectiveRequest, attempt.resolvedModel, userText);
                httpResult = executeJsonRequest(
                    QStringLiteral("POST"), QUrl(attempt.endpoint), {}, &body);
                if (httpResult.success && httpResult.document.isObject()) {
                    attempt.text = extractOllamaText(httpResult.document.object());
                }
            }

            if (httpResult.success && !attempt.text.trimmed().isEmpty()) {
                break;
            }
            endpointFailures.push_back(
                QStringLiteral("%1 -> %2")
                    .arg(
                        attempt.endpoint,
                        httpResult.errorMessage.isEmpty()
                            ? QStringLiteral("The gateway replied but returned no usable text")
                            : httpResult.errorMessage));
        }

        if (endpoints.isEmpty()) {
            attempt.errorMessage = QStringLiteral("No compatible endpoint candidate was available");
            failures.push_back(attempt);
            continue;
        }

        attempt.statusCode = httpResult.statusCode;
        attempt.json = httpResult.document.isObject() ? httpResult.document.object() : QJsonObject{};
        if (httpResult.success && !attempt.text.trimmed().isEmpty()) {
            const QString actualModel = extractResponseModelId(attempt.json);
            const QString finalModel = actualModel.isEmpty()
                ? attempt.resolvedModel
                : actualModel;
            response.success = true;
            response.providerId = attempt.protocolDisplayName;
            response.model = finalModel;
            response.rawJson = attempt.json;
            response.rawText = attempt.text.trimmed();
            response.reviewSummary.plainText = response.rawText;
            response.finishedAt = QDateTime::currentDateTimeUtc();
            response.durationMs = response.startedAt.msecsTo(response.finishedAt);
            extractUsageMetrics(attempt.json, &response);
            if (_settingsService) {
                QStringList availableModels =
                    _settingsService->value(QString::fromLatin1(kAvailableModelsSettingsKey)).toStringList();
                appendUniqueModel(&availableModels, finalModel);

                _settingsService->setValue(
                    QString::fromLatin1(kAvailableModelsSettingsKey),
                    availableModels);
                _settingsService->setValue(
                    QString::fromLatin1(kRecommendedModelSettingsKey),
                    finalModel);
                _settingsService->setValue(
                    QString::fromLatin1(kGenericModelSettingsKey),
                    finalModel);
                _settingsService->setValue(
                    QString::fromLatin1(kOpenAIModelSettingsKey),
                    finalModel);
                _settingsService->setValue(
                    QString::fromLatin1(kDetectedProtocolSettingsKey),
                    protocolSettingValue(protocol));
                _settingsService->setValue(
                    QString::fromLatin1(kDetectedProviderSettingsKey),
                    attempt.protocolDisplayName);
                _settingsService->setValue(
                    QString::fromLatin1(kDetectedEndpointSettingsKey),
                    attempt.endpoint);
                if (protocol == GatewayProtocol::OpenAIResponses) {
                    _settingsService->setValue(
                        QString::fromLatin1(kResponsesEndpointSettingsKey),
                        attempt.endpoint);
                }
                _settingsService->sync();
            }
            // Propagate frame capture diagnostics from request options to response
            response.frameCount = effectiveRequest.frames.size();
            response.frameCaptureError = effectiveRequest.options.value(
                QStringLiteral("frameCaptureError")).toString().trimmed();
            return response;
        }

        attempt.errorMessage = endpointFailures.isEmpty()
            ? (httpResult.errorMessage.isEmpty()
                  ? QStringLiteral("The gateway replied but returned no usable text")
                  : httpResult.errorMessage)
            : endpointFailures.join(QStringLiteral(" | "));
        failures.push_back(attempt);
    }

    return finishWithError(formatFailureSummary(failures));
}

AIAudioTranscriptionResult OpenAIResponsesProvider::transcribe(const AIAudioTranscriptionRequest& request)
{
    AIAudioTranscriptionResult result;
    result.providerId = providerId();
    result.startedAt = QDateTime::currentDateTimeUtc();

    const auto finishWithError = [&result](const QString& errorMessage) {
        result.success = false;
        result.errorMessage = errorMessage;
        result.finishedAt = QDateTime::currentDateTimeUtc();
        result.durationMs = result.startedAt.msecsTo(result.finishedAt);
        return result;
    };

    auto* audioFile = new QFile(request.audioFilePath);
    if (!audioFile->exists()) {
        delete audioFile;
        return finishWithError(QStringLiteral("Audio clip not found"));
    }
    if (!audioFile->open(QIODevice::ReadOnly)) {
        delete audioFile;
        return finishWithError(QStringLiteral("Failed to open audio clip"));
    }

    const QString baseUrl = _baseUrl();
    QString keyError;
    const QByteArray apiKey = _apiKey(
        QStringLiteral("openai_chat"),
        baseUrl,
        &keyError);
    if (apiKey.isEmpty() && !looksLikeOllamaBaseUrl(baseUrl)) {
        return finishWithError(
            keyError.isEmpty()
                ? QStringLiteral("Current AI gateway has no API key configured")
                : keyError);
    }

    const QString resolvedModel = _transcriptionModelForRequest(request);
    result.model = resolvedModel;

    auto* multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    QHttpPart modelPart;
    modelPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(QStringLiteral("form-data; name=\"model\"")));
    modelPart.setBody(resolvedModel.toUtf8());
    multipart->append(modelPart);

    if (!request.prompt.trimmed().isEmpty()) {
        QHttpPart promptPart;
        promptPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(QStringLiteral("form-data; name=\"prompt\"")));
        promptPart.setBody(request.prompt.trimmed().toUtf8());
        multipart->append(promptPart);
    }

    if (!request.languageHint.trimmed().isEmpty()) {
        QHttpPart languagePart;
        languagePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(QStringLiteral("form-data; name=\"language\"")));
        languagePart.setBody(request.languageHint.trimmed().toUtf8());
        multipart->append(languagePart);
    }

    QHttpPart responseFormatPart;
    responseFormatPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(QStringLiteral("form-data; name=\"response_format\"")));
    const QString responseFormat =
        request.options.value(QStringLiteral("response_format")).toString().trimmed().isEmpty()
            ? QStringLiteral("json")
            : request.options.value(QStringLiteral("response_format")).toString().trimmed();
    responseFormatPart.setBody(responseFormat.toUtf8());
    multipart->append(responseFormatPart);

    const QJsonArray timestampGranularities =
        request.options.value(QStringLiteral("timestamp_granularities")).toArray();
    for (const QJsonValue& granularityValue : timestampGranularities) {
        const QString granularity = granularityValue.toString().trimmed();
        if (granularity.isEmpty()) {
            continue;
        }
        QHttpPart granularityPart;
        granularityPart.setHeader(
            QNetworkRequest::ContentDispositionHeader,
            QVariant(QStringLiteral("form-data; name=\"timestamp_granularities[]\"")));
        granularityPart.setBody(granularity.toUtf8());
        multipart->append(granularityPart);
    }

    QHttpPart filePart;
    const QString mimeType = request.mimeType.trimmed().isEmpty()
        ? QStringLiteral("audio/wav")
        : request.mimeType.trimmed();
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, mimeType);
    filePart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QVariant(QStringLiteral("form-data; name=\"file\"; filename=\"%1\"").arg(QFileInfo(*audioFile).fileName())));
    filePart.setBodyDevice(audioFile);
    audioFile->setParent(multipart);
    multipart->append(filePart);

    const HttpJsonResult httpResult = executeMultipartRequest(
        QUrl(normalizeOpenAITranscriptionsEndpoint(baseUrl)),
        bearerHeaders(apiKey),
        multipart);

    if (!httpResult.success) {
        return finishWithError(httpResult.errorMessage.isEmpty()
            ? QStringLiteral("Audio transcription failed")
            : httpResult.errorMessage);
    }

    result.rawJson = httpResult.document.object();
    result.text = result.rawJson.value(QStringLiteral("text")).toString().trimmed();
    result.detectedLanguage = result.rawJson.value(QStringLiteral("language")).toString().trimmed();
    if (result.text.isEmpty()) {
        return finishWithError(QStringLiteral("Transcription returned empty text"));
    }

    result.success = true;
    result.finishedAt = QDateTime::currentDateTimeUtc();
    result.durationMs = result.startedAt.msecsTo(result.finishedAt);
    return result;
}

QByteArray OpenAIResponsesProvider::_apiKey(
    const QString& protocol,
    const QString& baseUrl,
    QString* error) const
{
    const QString normalizedProtocol = protocol.trimmed().toLower();
    const bool anthropicProtocol = normalizedProtocol.startsWith(QStringLiteral("anthropic"));
    const bool geminiProtocol = normalizedProtocol.startsWith(QStringLiteral("gemini"));
    const bool ollamaProtocol = normalizedProtocol.startsWith(QStringLiteral("ollama"));
    const bool openAIProtocol = !anthropicProtocol && !geminiProtocol && !ollamaProtocol;

    QString configuredFamily;
    if (_settingsService) {
        const QString configured = QStringLiteral("%1 %2 %3")
            .arg(
                _settingsService->value(QString::fromLatin1(kProviderTypeSettingsKey)).toString(),
                _settingsService->value(QString::fromLatin1(kDetectedProtocolSettingsKey)).toString(),
                _settingsService->value(QString::fromLatin1(kDetectedProviderSettingsKey)).toString())
            .trimmed()
            .toLower();
        if (configured.contains(QStringLiteral("anthropic")) ||
            configured.contains(QStringLiteral("claude"))) {
            configuredFamily = QStringLiteral("anthropic");
        } else if (configured.contains(QStringLiteral("gemini"))) {
            configuredFamily = QStringLiteral("gemini");
        } else if (configured.contains(QStringLiteral("ollama"))) {
            configuredFamily = QStringLiteral("ollama");
        } else if (!configured.isEmpty()) {
            configuredFamily = QStringLiteral("openai");
        }
    }

    const QString lowerBaseUrl = baseUrl.trimmed().toLower();
    QString baseFamily;
    if (looksLikeAnthropicBaseUrl(lowerBaseUrl)) {
        baseFamily = QStringLiteral("anthropic");
    } else if (looksLikeGeminiBaseUrl(lowerBaseUrl)) {
        baseFamily = QStringLiteral("gemini");
    } else if (looksLikeOllamaBaseUrl(lowerBaseUrl)) {
        baseFamily = QStringLiteral("ollama");
    } else if (lowerBaseUrl.contains(QStringLiteral("api.openai.com")) ||
               lowerBaseUrl.contains(QStringLiteral("dashscope")) ||
               lowerBaseUrl.contains(QStringLiteral("aliyuncs.com")) ||
               AIConnectionValidator::usesAzureApiKeyAuthentication(baseUrl)) {
        baseFamily = QStringLiteral("openai");
    }

    const QString protocolFamily = anthropicProtocol
        ? QStringLiteral("anthropic")
        : (geminiProtocol
               ? QStringLiteral("gemini")
               : (ollamaProtocol ? QStringLiteral("ollama") : QStringLiteral("openai")));
    const QString targetFamily = !configuredFamily.isEmpty() ? configuredFamily : baseFamily;
    const bool protocolMatchesTarget = targetFamily.isEmpty() || targetFamily == protocolFamily;
    const bool genericAllowed = protocolMatchesTarget &&
        (!baseUrl.trimmed().isEmpty() || !targetFamily.isEmpty() || openAIProtocol);
    const bool providerSpecificAllowed = protocolMatchesTarget &&
        (!targetFamily.isEmpty() || baseUrl.trimmed().isEmpty() || openAIProtocol);

    QString loadError;
    if (_credentialStore) {
        const QByteArray genericKey = _credentialStore
            ->loadSecret(QString::fromLatin1(kGenericCredentialId), &loadError)
            .trimmed();
        if (genericAllowed && !genericKey.isEmpty()) return genericKey;

        if (providerSpecificAllowed) {
            QStringList credentialIds;
            if (anthropicProtocol) {
                credentialIds.push_back(QString::fromLatin1(kAnthropicCredentialId));
            } else if (geminiProtocol) {
                credentialIds.push_back(QString::fromLatin1(kGeminiCredentialId));
            } else if (openAIProtocol) {
                if (AIConnectionValidator::usesAzureApiKeyAuthentication(baseUrl)) {
                    credentialIds.push_back(QString::fromLatin1(kAzureOpenAICredentialId));
                }
                if (lowerBaseUrl.contains(QStringLiteral("dashscope")) ||
                    lowerBaseUrl.contains(QStringLiteral("aliyuncs.com")) ||
                    lowerBaseUrl.contains(QStringLiteral("qwen"))) {
                    credentialIds.push_back(QString::fromLatin1(kQwenCredentialId));
                }
                credentialIds.push_back(QString::fromLatin1(kOpenAICredentialId));
            }
            for (const QString& credentialId : credentialIds) {
                const QByteArray storedKey = _credentialStore
                    ->loadSecret(credentialId, &loadError)
                    .trimmed();
                if (!storedKey.isEmpty()) return storedKey;
            }
        }
    }

    const QByteArray genericEnvKey = qgetenv(kGenericApiKeyEnv).trimmed();
    if (genericAllowed && !genericEnvKey.isEmpty()) return genericEnvKey;

    if (providerSpecificAllowed) {
        QStringList environmentKeys;
        if (anthropicProtocol) {
            environmentKeys.push_back(QString::fromLatin1(kAnthropicApiKeyEnv));
        } else if (geminiProtocol) {
            environmentKeys << QString::fromLatin1(kGeminiApiKeyEnv)
                            << QString::fromLatin1(kGoogleApiKeyEnv);
        } else if (openAIProtocol) {
            if (AIConnectionValidator::usesAzureApiKeyAuthentication(baseUrl)) {
                environmentKeys.push_back(QString::fromLatin1(kAzureOpenAIApiKeyEnv));
            }
            if (lowerBaseUrl.contains(QStringLiteral("dashscope")) ||
                lowerBaseUrl.contains(QStringLiteral("aliyuncs.com")) ||
                lowerBaseUrl.contains(QStringLiteral("qwen"))) {
                environmentKeys << QString::fromLatin1(kQwenApiKeyEnv)
                                << QString::fromLatin1(kDashScopeApiKeyEnv);
            }
            environmentKeys.push_back(QString::fromLatin1(kOpenAIApiKeyEnv));
        }
        for (const QString& environmentKey : environmentKeys) {
            const QByteArray environmentName = environmentKey.toLatin1();
            const QByteArray value = qgetenv(environmentName.constData()).trimmed();
            if (!value.isEmpty()) return value;
        }
    }

    if (error) *error = loadError;
    return {};
}

QString OpenAIResponsesProvider::_baseUrl() const
{
    QString baseUrl;
    if (_settingsService) {
        baseUrl = _settingsService->value(QString::fromLatin1(kGenericBaseUrlSettingsKey)).toString().trimmed();
        if (baseUrl.isEmpty()) {
            baseUrl = _settingsService->value(QString::fromLatin1(kOpenAIBaseUrlSettingsKey)).toString().trimmed();
        }
        if (baseUrl.isEmpty() &&
            _settingsService->value(QString::fromLatin1(kProviderTypeSettingsKey)).toString().trimmed()
                .compare(QStringLiteral("Qwen"), Qt::CaseInsensitive) == 0) {
            baseUrl = QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1");
        }
    }
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromUtf8(qgetenv(kGenericBaseUrlEnv)).trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromUtf8(qgetenv(kOpenAIBaseUrlEnv)).trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromUtf8(qgetenv(kQwenBaseUrlEnv)).trimmed();
    }
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromUtf8(qgetenv(kDashScopeBaseUrlEnv)).trimmed();
    }
    return trimTrailingSlashes(baseUrl);
}

QString OpenAIResponsesProvider::_modelForRequest(const AIRequest& request) const
{
    QString requestModel = request.model.trimmed();
    if (!requestModel.isEmpty()) {
        return requestModel;
    }

    if (_settingsService) {
        const bool autoModelSelection =
            _settingsService->value(QString::fromLatin1(kAutoModelSelectionSettingsKey), true).toBool();
        const QString configuredModel =
            _settingsService
                ->value(QString::fromLatin1(kGenericModelSettingsKey))
                .toString()
                .trimmed();
        const QString recommendedModel =
            _settingsService
                ->value(QString::fromLatin1(kRecommendedModelSettingsKey))
                .toString()
                .trimmed();
        const QStringList availableModels =
            _settingsService
                ->value(QString::fromLatin1(kAvailableModelsSettingsKey))
                .toStringList();
        const QString legacyModel =
            _settingsService
                ->value(QString::fromLatin1(kOpenAIModelSettingsKey))
                .toString()
                .trimmed();
        const QString preferredAvailableModel = preferredModelFromList(availableModels);

        if (autoModelSelection) {
            if (!recommendedModel.isEmpty()) {
                return recommendedModel;
            }
            if (!preferredAvailableModel.isEmpty()) {
                return preferredAvailableModel;
            }
        } else {
            if (!configuredModel.isEmpty()) {
                return configuredModel;
            }
            if (!recommendedModel.isEmpty()) {
                return recommendedModel;
            }
            if (!preferredAvailableModel.isEmpty()) {
                return preferredAvailableModel;
            }
            if (!legacyModel.isEmpty()) {
                return legacyModel;
            }
        }

        if (autoModelSelection &&
            !legacyModel.isEmpty() &&
            availableModels.contains(legacyModel, Qt::CaseInsensitive)) {
            return legacyModel;
        }
        if (_settingsService->value(QString::fromLatin1(kProviderTypeSettingsKey)).toString().trimmed()
                .compare(QStringLiteral("Qwen"), Qt::CaseInsensitive) == 0) {
            return QStringLiteral("qwen-plus");
        }
    }

    const QString envModel = QString::fromUtf8(qgetenv(kGenericModelEnv)).trimmed();
    if (!envModel.isEmpty()) {
        return envModel;
    }
    const QString qwenEnvModel = QString::fromUtf8(qgetenv(kQwenModelEnv)).trimmed();
    if (!qwenEnvModel.isEmpty()) {
        return qwenEnvModel;
    }
    const QString dashScopeEnvModel = QString::fromUtf8(qgetenv(kDashScopeModelEnv)).trimmed();
    if (!dashScopeEnvModel.isEmpty()) {
        return dashScopeEnvModel;
    }

    return {};
}

QString OpenAIResponsesProvider::_transcriptionModelForRequest(const AIAudioTranscriptionRequest& request) const
{
    const QString explicitModel = request.model.trimmed();
    if (!explicitModel.isEmpty() &&
        (explicitModel.contains(QStringLiteral("whisper"), Qt::CaseInsensitive) ||
         explicitModel.contains(QStringLiteral("transcribe"), Qt::CaseInsensitive) ||
         explicitModel.contains(QStringLiteral("audio"), Qt::CaseInsensitive))) {
        return explicitModel;
    }

    if (_settingsService) {
        const QString configured =
            _settingsService->value(QString::fromLatin1(kGenericModelSettingsKey)).toString().trimmed();
        if (!configured.isEmpty() &&
            (configured.contains(QStringLiteral("transcribe"), Qt::CaseInsensitive) ||
             configured.contains(QStringLiteral("whisper"), Qt::CaseInsensitive) ||
             configured.contains(QStringLiteral("audio"), Qt::CaseInsensitive))) {
            return configured;
        }
    }

    return QString::fromLatin1(kDefaultTranscriptionModel);
}

QJsonObject OpenAIResponsesProvider::_buildRequestBody(const AIRequest& request, const QString& resolvedModel) const
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), resolvedModel);

    const QString instructions = request.systemPrompt.trimmed();
    if (!instructions.isEmpty()) {
        body.insert(QStringLiteral("instructions"), instructions);
    }

    QJsonArray input;

    // Include chat history for multi-turn conversations
    for (const AIChatMessage& histMsg : request.chatHistory) {
        QString role;
        switch (histMsg.role) {
        case AIChatRole::System:
            continue; // system goes in instructions
        case AIChatRole::User:
            role = QStringLiteral("user");
            break;
        case AIChatRole::Assistant:
            role = QStringLiteral("assistant");
            break;
        }
        if (role.isEmpty() || histMsg.content.trimmed().isEmpty()) {
            continue;
        }
        input.append(QJsonObject{
            { QStringLiteral("role"), role },
            { QStringLiteral("content"), QJsonArray{
                  QJsonObject{
                      { QStringLiteral("type"), QStringLiteral("input_text") },
                      { QStringLiteral("text"), histMsg.content }
                  }
              } }
        });
    }

    QJsonArray content;
    content.append(QJsonObject{
        { QStringLiteral("type"), QStringLiteral("input_text") },
        { QStringLiteral("text"), _composeUserText(request) }
    });

    const QString imageDetail = request.options.value(QStringLiteral("imageDetail")).toString().trimmed();
    for (const AIMediaFrameReference& frame : request.frames) {
        if (frame.encodedBytes.isEmpty()) {
            continue;
        }

        const QString mimeType = frame.mimeType.trimmed().isEmpty()
            ? QStringLiteral("image/png")
            : frame.mimeType.trimmed();
        const QString imageUrl = QStringLiteral("data:%1;base64,%2")
            .arg(mimeType, QString::fromLatin1(frame.encodedBytes.toBase64()));

        QJsonObject imageInput{
            { QStringLiteral("type"), QStringLiteral("input_image") },
            { QStringLiteral("image_url"), imageUrl }
        };
        if (!imageDetail.isEmpty()) {
            imageInput.insert(QStringLiteral("detail"), imageDetail);
        }
        content.append(imageInput);
    }

    input.append(QJsonObject{
        { QStringLiteral("role"), QStringLiteral("user") },
        { QStringLiteral("content"), content }
    });
    body.insert(QStringLiteral("input"), input);

    const int maxOutputTokens = request.options.value(QStringLiteral("maxOutputTokens")).toInt();
    if (maxOutputTokens > 0) {
        body.insert(QStringLiteral("max_output_tokens"), maxOutputTokens);
    }

    return body;
}

QJsonObject OpenAIResponsesProvider::_buildChatCompletionsRequestBody(
    const AIRequest& request,
    const QString& resolvedModel) const
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), resolvedModel);

    QJsonArray messages;
    const QString instructions = request.systemPrompt.trimmed();
    if (!instructions.isEmpty()) {
        messages.append(QJsonObject{
            { QStringLiteral("role"), QStringLiteral("system") },
            { QStringLiteral("content"), instructions }
        });
    }

    // Include chat history for multi-turn conversations
    for (const AIChatMessage& histMsg : request.chatHistory) {
        QString role;
        switch (histMsg.role) {
        case AIChatRole::System:
            role = QStringLiteral("system");
            break;
        case AIChatRole::User:
            role = QStringLiteral("user");
            break;
        case AIChatRole::Assistant:
            role = QStringLiteral("assistant");
            break;
        }
        if (role.isEmpty() || histMsg.content.trimmed().isEmpty()) {
            continue;
        }
        messages.append(QJsonObject{
            { QStringLiteral("role"), role },
            { QStringLiteral("content"), histMsg.content }
        });
    }

    if (request.frames.isEmpty()) {
        messages.append(QJsonObject{
            { QStringLiteral("role"), QStringLiteral("user") },
            { QStringLiteral("content"), _composeUserText(request) }
        });
    } else {
        QJsonArray content;
        content.append(QJsonObject{
            { QStringLiteral("type"), QStringLiteral("text") },
            { QStringLiteral("text"), _composeUserText(request) }
        });
        for (const AIMediaFrameReference& frame : request.frames) {
            if (frame.encodedBytes.isEmpty()) {
                continue;
            }
            const QString mimeType = frame.mimeType.trimmed().isEmpty()
                ? QStringLiteral("image/png")
                : frame.mimeType.trimmed();
            const QString imageUrl = QStringLiteral("data:%1;base64,%2")
                .arg(mimeType, QString::fromLatin1(frame.encodedBytes.toBase64()));
            content.append(QJsonObject{
                { QStringLiteral("type"), QStringLiteral("image_url") },
                { QStringLiteral("image_url"), QJsonObject{
                      { QStringLiteral("url"), imageUrl }
                  } }
            });
        }
        messages.append(QJsonObject{
            { QStringLiteral("role"), QStringLiteral("user") },
            { QStringLiteral("content"), content }
        });
    }

    body.insert(QStringLiteral("messages"), messages);
    const int maxOutputTokens = request.options.value(QStringLiteral("maxOutputTokens")).toInt();
    if (maxOutputTokens > 0) {
        body.insert(QStringLiteral("max_tokens"), maxOutputTokens);
    }
    return body;
}

QString OpenAIResponsesProvider::_composeUserText(const AIRequest& request) const
{
    if (request.options.value(QStringLiteral("plainUserPromptOnly")).toBool(false)) {
        return request.userPrompt.trimmed().isEmpty()
            ? QStringLiteral("Please answer using the provided input only.")
            : request.userPrompt.trimmed();
    }

    QStringList lines;
    const AIRequestContext& context = request.context;

    lines << QStringLiteral("Task:");
    lines << (request.userPrompt.trimmed().isEmpty()
                  ? QStringLiteral("Analyze the current frame and list the most important visual issues.")
                  : request.userPrompt.trimmed());
    lines << QString();
    lines << QStringLiteral("Context:");
    lines << QStringLiteral("- scope=%1").arg(scopeText(context.scope));

    if (!context.mediaDisplayName.isEmpty()) {
        lines << QStringLiteral("- media=%1").arg(context.mediaDisplayName);
    } else if (!context.mediaPath.isEmpty()) {
        lines << QStringLiteral("- mediaPath=%1").arg(context.mediaPath);
    }

    if (!context.activeViewId.isEmpty()) {
        lines << QStringLiteral("- activeViewId=%1").arg(context.activeViewId);
    }
    lines << QStringLiteral("- frame=%1").arg(context.currentFrame);
    if (context.totalFrames > 0) {
        lines << QStringLiteral("- totalFrames=%1").arg(context.totalFrames);
    }
    if (context.fps > 0.0) {
        lines << QStringLiteral("- fps=%1").arg(QString::number(context.fps, 'f', 3));
    }
    if (context.mediaWidth > 0 && context.mediaHeight > 0) {
        lines << QStringLiteral("- resolution=%1x%2").arg(context.mediaWidth).arg(context.mediaHeight);
    }
    if (!context.mediaFormat.isEmpty()) {
        lines << QStringLiteral("- format=%1").arg(context.mediaFormat);
    }

    const QString runtimeControlSummary =
        request.options.value(QStringLiteral("runtimeControlSummary")).toString().trimmed();
    if (!runtimeControlSummary.isEmpty()) {
        lines << QString();
        lines << QStringLiteral("Player runtime state:");
        lines << runtimeControlSummary;
    }

    const QString conversationContextSummary =
        request.options.value(QStringLiteral("conversationContextSummary")).toString().trimmed();
    if (!conversationContextSummary.isEmpty()) {
        lines << QString();
        lines << QStringLiteral("Conversation context:");
        lines << conversationContextSummary;
    }

    if (!request.frames.isEmpty()) {
        lines << QStringLiteral("- attachedFrames=%1").arg(request.frames.size());
        const AIMediaFrameReference& firstFrame = request.frames.front();
        if (firstFrame.size.isValid()) {
            lines << QStringLiteral("- capturedFrameSize=%1x%2")
                .arg(firstFrame.size.width())
                .arg(firstFrame.size.height());
        }
        const QString captureMode = firstFrame.metadata.value(QStringLiteral("captureMode")).toString().trimmed();
        if (!captureMode.isEmpty()) {
            lines << QStringLiteral("- captureMode=%1").arg(captureMode);
        }
        lines << QStringLiteral("- frameImagesProvided=true (images are attached as base64 PNG in the message content)");
        if (context.scope == AIRequestScope::CompareAB) {
            lines << QStringLiteral("- compareInstruction=When compareRole=compare-a and compareRole=compare-b frames are present, treat those two labeled images as the authoritative A and B views. Use the compare-full frame only as layout context.");
        }
        lines << QStringLiteral("- attachedFrameDetails:");
        for (int i = 0; i < request.frames.size(); ++i) {
            lines << framePreview(request.frames.at(i), i);
        }
    } else {
        lines << QStringLiteral("- attachedFrames=0");
        lines << QStringLiteral("- frameImagesProvided=false (no frame screenshots could be captured)");
    }

    lines << QStringLiteral("- annotations=%1").arg(context.annotations.size());
    if (!context.selectedAnnotationId.isEmpty()) {
        lines << QStringLiteral("- selectedAnnotationId=%1").arg(context.selectedAnnotationId);
    }

    if (!context.annotations.isEmpty()) {
        lines << QString();
        lines << QStringLiteral("Annotation preview:");
        const int limit = std::min<int>(kAnnotationPreviewLimit, static_cast<int>(context.annotations.size()));
        for (int i = 0; i < limit; ++i) {
            lines << annotationPreview(context.annotations.at(i));
        }
        if (context.annotations.size() > limit) {
            lines << QStringLiteral("- ... %1 more annotations")
                .arg(context.annotations.size() - limit);
        }
    }

    lines << QString();
    lines << QStringLiteral("Output requirements:");
    lines << QStringLiteral("- Focus on practical review issues visible in the frame.");
    lines << QStringLiteral("- Group findings by Critical, Major, and Minor when applicable.");
    lines << QStringLiteral("- Keep the answer concise and actionable.");

    return lines.join(QLatin1Char('\n'));
}

QString OpenAIResponsesProvider::_extractResponsesText(const QJsonObject& responseObject) const
{
    const QString topLevelText = responseObject.value(QStringLiteral("output_text")).toString().trimmed();
    if (!topLevelText.isEmpty()) {
        return topLevelText;
    }

    QStringList texts;
    collectOutputText(responseObject.value(QStringLiteral("output")), &texts);
    if (texts.isEmpty()) {
        collectOutputText(responseObject.value(QStringLiteral("content")), &texts);
    }
    return texts.join(QStringLiteral("\n\n")).trimmed();
}

QString OpenAIResponsesProvider::_extractChatCompletionsText(const QJsonObject& responseObject) const
{
    return extractOpenAIChatText(responseObject);
}

} // namespace cgplay
