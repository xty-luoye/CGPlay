#include "AIConnectionValidator.h"

#include <QEventLoop>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace cgplay {

namespace {

constexpr int kRequestTimeoutMs = 30000;
constexpr int kOpenAIValidationBudgetMs = 35000;
constexpr int kOpenAIPrimaryProbeBudgetMs = 17500;
constexpr int kOpenAISecondaryProbeBudgetMs = 5000;
constexpr int kOpenAIEndpointProbeTimeoutMs = 10000;
constexpr auto kDefaultOpenAIBaseUrl = "https://api.openai.com";
constexpr auto kDefaultQwenBaseUrl = "https://dashscope.aliyuncs.com/compatible-mode/v1";
constexpr auto kDefaultGeminiBaseUrl = "https://generativelanguage.googleapis.com";
constexpr auto kDefaultClaudeBaseUrl = "https://api.anthropic.com";
constexpr auto kDefaultOllamaBaseUrl = "http://127.0.0.1:11434";

struct HttpJsonResult
{
    bool success = false;
    int statusCode = 0;
    QByteArray payload;
    QJsonDocument document;
    QString error;
};

enum class OpenAIEndpointKind
{
    Base,
    Models,
    ChatCompletions,
    Responses
};

QString trimTrailingSlashes(QString value)
{
    while (value.endsWith(QLatin1Char('/'))) {
        value.chop(1);
    }
    return value.trimmed();
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

QString ensureScheme(QString value)
{
    value = value.trimmed();
    if (value.isEmpty() || value.contains(QStringLiteral("://"))) {
        return value;
    }
    const QString lower = value.toLower();
    if (lower.startsWith(QStringLiteral("localhost")) ||
        lower.startsWith(QStringLiteral("127.0.0.1")) ||
        lower.startsWith(QStringLiteral("0.0.0.0"))) {
        return QStringLiteral("http://") + value;
    }
    return QStringLiteral("https://") + value;
}

QString normalizedPath(QString path)
{
    if (path.isEmpty()) {
        return {};
    }
    if (!path.startsWith(QLatin1Char('/'))) {
        path.prepend(QLatin1Char('/'));
    }
    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    return path == QStringLiteral("/") ? QString() : path;
}

QString joinedPath(const QString& basePath, const QString& suffix)
{
    const QString normalizedBase = normalizedPath(basePath);
    QString normalizedSuffix = suffix;
    if (!normalizedSuffix.startsWith(QLatin1Char('/'))) {
        normalizedSuffix.prepend(QLatin1Char('/'));
    }
    return normalizedBase + normalizedSuffix;
}

void appendUnique(QStringList* values, const QString& value)
{
    if (!values) {
        return;
    }
    const QString trimmed = value.trimmed();
    if (!trimmed.isEmpty() && !values->contains(trimmed, Qt::CaseInsensitive)) {
        values->push_back(trimmed);
    }
}

QUrl normalizedOpenAIUrl(QString baseUrl)
{
    baseUrl = ensureScheme(baseUrl);
    if (baseUrl.trimmed().isEmpty()) {
        baseUrl = QString::fromLatin1(kDefaultOpenAIBaseUrl);
    }
    QUrl url(baseUrl);
    if (!url.isValid()) {
        return {};
    }
    url.setFragment(QString());
    url.setPath(normalizedPath(url.path()));
    return url;
}

QString endpointWithPath(QUrl url, const QString& path)
{
    if (!url.isValid()) {
        return {};
    }
    url.setPath(normalizedPath(path));
    return url.toString(QUrl::FullyEncoded);
}

OpenAIEndpointKind endpointKindForPath(const QString& path, QString* basePath)
{
    struct EndpointSuffix
    {
        const char* suffix;
        OpenAIEndpointKind kind;
    };
    static const EndpointSuffix suffixes[] = {
        { "/chat/completions", OpenAIEndpointKind::ChatCompletions },
        { "/responses", OpenAIEndpointKind::Responses },
        { "/models", OpenAIEndpointKind::Models }
    };

    const QString normalized = normalizedPath(path);
    for (const EndpointSuffix& item : suffixes) {
        const QString suffix = QString::fromLatin1(item.suffix);
        if (normalized.endsWith(suffix, Qt::CaseInsensitive)) {
            if (basePath) {
                *basePath = normalized.left(normalized.size() - suffix.size());
            }
            return item.kind;
        }
    }
    if (basePath) {
        *basePath = normalized;
    }
    return OpenAIEndpointKind::Base;
}

bool hasVersionTail(const QString& path)
{
    static const QRegularExpression expression(
        QStringLiteral("/v[0-9]+(?:[a-z][a-z0-9._-]*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    return expression.match(normalizedPath(path)).hasMatch();
}

bool hasAzureDeploymentPath(const QString& path)
{
    return normalizedPath(path).contains(
        QStringLiteral("/openai/deployments/"),
        Qt::CaseInsensitive);
}

bool isAzureHost(const QUrl& url)
{
    const QString host = url.host().toLower();
    static const QRegularExpression expression(
        QStringLiteral("\\.(?:openai|cognitiveservices|aoai)\\.azure\\.[a-z0-9.-]+$"),
        QRegularExpression::CaseInsensitiveOption);
    return expression.match(host).hasMatch();
}

QString azureDeploymentRoot(const QString& basePath, const QString& model)
{
    if (model.trimmed().isEmpty()) {
        return {};
    }
    QString root = normalizedPath(basePath);
    if (root.endsWith(QStringLiteral("/openai"), Qt::CaseInsensitive)) {
        return joinedPath(root, QStringLiteral("deployments/%1").arg(model.trimmed()));
    }
    return joinedPath(root, QStringLiteral("openai/deployments/%1").arg(model.trimmed()));
}

QString boundedDiagnostic(QString value)
{
    value = value.trimmed();
    constexpr int kMaximumDiagnosticLength = 600;
    if (value.size() > kMaximumDiagnosticLength) {
        value = value.left(kMaximumDiagnosticLength) + QStringLiteral("...");
    }
    return value;
}

QString stripGeminiPath(QString value)
{
    value = trimTrailingSlashes(ensureScheme(value));
    const QStringList markers{
        QStringLiteral("/v1beta/models"),
        QStringLiteral("/models/")
    };
    for (const QString& marker : markers) {
        const int index = value.indexOf(marker, 0, Qt::CaseInsensitive);
        if (index >= 0) {
            return value.left(index);
        }
    }
    if (value.endsWith(QStringLiteral("/v1beta"), Qt::CaseInsensitive)) {
        value.chop(QStringLiteral("/v1beta").size());
    }
    return value;
}

QString stripClaudePath(QString value)
{
    value = trimTrailingSlashes(ensureScheme(value));
    const QStringList markers{
        QStringLiteral("/v1/messages"),
        QStringLiteral("/v1/models")
    };
    for (const QString& marker : markers) {
        const int index = value.indexOf(marker, 0, Qt::CaseInsensitive);
        if (index >= 0) {
            return value.left(index);
        }
    }
    if (value.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive)) {
        value.chop(QStringLiteral("/v1").size());
    }
    return value;
}

QString stripOllamaPath(QString value)
{
    value = trimTrailingSlashes(ensureScheme(value));
    const QStringList markers{
        QStringLiteral("/api/chat"),
        QStringLiteral("/api/tags")
    };
    for (const QString& marker : markers) {
        const int index = value.indexOf(marker, 0, Qt::CaseInsensitive);
        if (index >= 0) {
            return value.left(index);
        }
    }
    if (value.endsWith(QStringLiteral("/api"), Qt::CaseInsensitive)) {
        value.chop(QStringLiteral("/api").size());
    }
    return value;
}

QString normalizedGeminiEndpoint(QString baseUrl, const QString& model)
{
    baseUrl = trimTrailingSlashes(withoutGeminiApiKeyQuery(baseUrl));
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromLatin1(kDefaultGeminiBaseUrl);
    }
    if (baseUrl.contains(QStringLiteral(":generateContent"), Qt::CaseInsensitive)) {
        return withoutGeminiApiKeyQuery(baseUrl);
    }
    if (baseUrl.endsWith(QStringLiteral("/v1beta"), Qt::CaseInsensitive)) {
        return withoutGeminiApiKeyQuery(
            baseUrl + QStringLiteral("/models/%1:generateContent").arg(model));
    }
    return withoutGeminiApiKeyQuery(
        baseUrl + QStringLiteral("/v1beta/models/%1:generateContent").arg(model));
}

QString normalizedClaudeEndpoint(QString baseUrl)
{
    baseUrl = trimTrailingSlashes(baseUrl);
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromLatin1(kDefaultClaudeBaseUrl);
    }
    if (baseUrl.endsWith(QStringLiteral("/messages"), Qt::CaseInsensitive)) {
        return baseUrl;
    }
    if (baseUrl.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive)) {
        return baseUrl + QStringLiteral("/messages");
    }
    return baseUrl + QStringLiteral("/v1/messages");
}

QString normalizedOllamaEndpoint(QString baseUrl)
{
    baseUrl = trimTrailingSlashes(baseUrl);
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromLatin1(kDefaultOllamaBaseUrl);
    }
    if (baseUrl.endsWith(QStringLiteral("/api/chat"), Qt::CaseInsensitive)) {
        return baseUrl;
    }
    if (baseUrl.endsWith(QStringLiteral("/api"), Qt::CaseInsensitive)) {
        return baseUrl + QStringLiteral("/chat");
    }
    return baseUrl + QStringLiteral("/api/chat");
}

QString httpErrorMessage(QNetworkReply* reply, const QByteArray& payload, const QJsonDocument& document)
{
    QString message;
    const QJsonObject object = document.isObject() ? document.object() : QJsonObject{};
    if (object.value(QStringLiteral("error")).isObject()) {
        message = object.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString().trimmed();
    } else if (object.value(QStringLiteral("error")).isString()) {
        message = object.value(QStringLiteral("error")).toString().trimmed();
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
    return boundedDiagnostic(message);
}

HttpJsonResult executeJsonRequest(
    const QString& method,
    const QUrl& url,
    const QList<QPair<QByteArray, QByteArray>>& headers,
    const QJsonObject* body = nullptr,
    int timeoutMs = kRequestTimeoutMs)
{
    HttpJsonResult result;
    if (!url.isValid()) {
        result.error = QStringLiteral("Invalid endpoint: %1").arg(url.toString());
        return result;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    timeoutMs = std::max(1, timeoutMs);
    request.setTransferTimeout(timeoutMs);
    if (body) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    }
    for (const auto& header : headers) {
        request.setRawHeader(header.first, header.second);
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(timeoutMs);

    QNetworkReply* reply = nullptr;
    if (method.compare(QStringLiteral("GET"), Qt::CaseInsensitive) == 0) {
        reply = manager.get(request);
    } else {
        const QByteArray payload = body
            ? QJsonDocument(*body).toJson(QJsonDocument::Compact)
            : QByteArray();
        reply = manager.post(request, payload);
    }

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        if (reply) {
            reply->abort();
        }
        loop.quit();
    });
    timer.start();
    loop.exec();
    timer.stop();

    result.payload = reply->readAll();
    result.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.document = QJsonDocument::fromJson(result.payload);
    if (reply->error() != QNetworkReply::NoError) {
        result.error = httpErrorMessage(reply, result.payload, result.document);
        reply->deleteLater();
        return result;
    }

    result.success = true;
    reply->deleteLater();
    return result;
}

} // namespace

AIOpenAIEndpointCandidates AIConnectionValidator::openAICompatibleEndpointCandidates(
    const QString& baseUrl,
    const QString& model)
{
    AIOpenAIEndpointCandidates result;
    const QUrl url = normalizedOpenAIUrl(baseUrl);
    if (!url.isValid()) {
        return result;
    }

    QString rootPath;
    const OpenAIEndpointKind exactKind = endpointKindForPath(url.path(), &rootPath);
    result.canonicalBaseUrl = endpointWithPath(url, rootPath);

    const auto addForRoot = [&result, &url](const QString& candidateRoot) {
        appendUnique(&result.models, endpointWithPath(url, joinedPath(candidateRoot, QStringLiteral("models"))));
        appendUnique(
            &result.chatCompletions,
            endpointWithPath(url, joinedPath(candidateRoot, QStringLiteral("chat/completions"))));
        appendUnique(&result.responses, endpointWithPath(url, joinedPath(candidateRoot, QStringLiteral("responses"))));
    };

    if (exactKind == OpenAIEndpointKind::Models) {
        appendUnique(&result.models, endpointWithPath(url, url.path()));
    } else if (exactKind == OpenAIEndpointKind::ChatCompletions) {
        appendUnique(&result.chatCompletions, endpointWithPath(url, url.path()));
    } else if (exactKind == OpenAIEndpointKind::Responses) {
        appendUnique(&result.responses, endpointWithPath(url, url.path()));
    }

    if (rootPath.isEmpty()) {
        addForRoot(QStringLiteral("/v1"));
        addForRoot(QString());
    } else {
        addForRoot(rootPath);
        if (!hasVersionTail(rootPath) && !hasAzureDeploymentPath(rootPath)) {
            addForRoot(joinedPath(rootPath, QStringLiteral("v1")));
        }
    }

    if (isAzureHost(url) || hasAzureDeploymentPath(rootPath)) {
        if (!hasAzureDeploymentPath(rootPath)) {
            const QString deploymentRoot = azureDeploymentRoot(rootPath, model);
            if (!deploymentRoot.isEmpty()) {
                addForRoot(deploymentRoot);
            }
        }
        QString azureV1Base = rootPath;
        const int deploymentIndex = azureV1Base.indexOf(
            QStringLiteral("/openai/deployments/"),
            0,
            Qt::CaseInsensitive);
        if (deploymentIndex >= 0) {
            azureV1Base = azureV1Base.left(deploymentIndex);
        }
        const QString azureV1Root = azureV1Base.endsWith(
            QStringLiteral("/openai"), Qt::CaseInsensitive)
            ? joinedPath(azureV1Base, QStringLiteral("v1"))
            : joinedPath(azureV1Base, QStringLiteral("openai/v1"));
        addForRoot(azureV1Root);
    }

    return result;
}

bool AIConnectionValidator::usesAzureApiKeyAuthentication(const QString& endpoint)
{
    const QUrl url(endpoint);
    return isAzureHost(url) ||
        hasAzureDeploymentPath(url.path()) ||
        (url.path().contains(QStringLiteral("/openai/"), Qt::CaseInsensitive) &&
         QUrlQuery(url).hasQueryItem(QStringLiteral("api-version")));
}

QList<QPair<QByteArray, QByteArray>> AIConnectionValidator::openAICompatibleHeaders(
    const QString& endpoint,
    const QByteArray& apiKey)
{
    if (usesAzureApiKeyAuthentication(endpoint)) {
        return {
            { QByteArray("api-key"), apiKey }
        };
    }
    return {
        { QByteArray("Authorization"), QByteArray("Bearer ") + apiKey }
    };
}

QString AIConnectionValidator::defaultBaseUrlForCategory(AIProviderCategory category) const
{
    switch (category) {
    case AIProviderCategory::Gemini:
        return QString::fromLatin1(kDefaultGeminiBaseUrl);
    case AIProviderCategory::Claude:
        return QString::fromLatin1(kDefaultClaudeBaseUrl);
    case AIProviderCategory::Ollama:
        return QString::fromLatin1(kDefaultOllamaBaseUrl);
    case AIProviderCategory::Qwen:
        return QString::fromLatin1(kDefaultQwenBaseUrl);
    case AIProviderCategory::OpenAICompatible:
    case AIProviderCategory::Unknown:
    default:
        return QString::fromLatin1(kDefaultOpenAIBaseUrl);
    }
}

QString AIConnectionValidator::canonicalBaseUrl(
    AIProviderCategory category,
    const QString& inputBaseUrl) const
{
    switch (category) {
    case AIProviderCategory::Gemini:
        return trimTrailingSlashes(stripGeminiPath(inputBaseUrl));
    case AIProviderCategory::Claude:
        return trimTrailingSlashes(stripClaudePath(inputBaseUrl));
    case AIProviderCategory::Ollama:
        return trimTrailingSlashes(stripOllamaPath(inputBaseUrl));
    case AIProviderCategory::Qwen:
    case AIProviderCategory::OpenAICompatible:
    case AIProviderCategory::Unknown:
    default:
        return openAICompatibleEndpointCandidates(inputBaseUrl).canonicalBaseUrl;
    }
}

AIValidationResult AIConnectionValidator::validate(
    AIProviderCategory category,
    const QString& baseUrl,
    const QByteArray& apiKey,
    const QString& model) const
{
    AIValidationResult result;

    if (category != AIProviderCategory::Ollama && apiKey.trimmed().isEmpty()) {
        result.error = QStringLiteral("API key is required for this provider.");
        return result;
    }
    if (model.trimmed().isEmpty()) {
        result.error = QStringLiteral("No model is available to validate.");
        return result;
    }

    if (category == AIProviderCategory::OpenAICompatible ||
        category == AIProviderCategory::Qwen) {
        const AIOpenAIEndpointCandidates candidates =
            openAICompatibleEndpointCandidates(baseUrl, model);
        const QJsonObject chatBody{
            { QStringLiteral("model"), model },
            { QStringLiteral("messages"), QJsonArray{
                  QJsonObject{
                      { QStringLiteral("role"), QStringLiteral("user") },
                      { QStringLiteral("content"), QStringLiteral("ping") }
                  }
              } },
            { QStringLiteral("max_tokens"), 1 }
        };
        const QJsonObject responsesBody{
            { QStringLiteral("model"), model },
            { QStringLiteral("input"), QJsonArray{
                  QJsonObject{
                      { QStringLiteral("role"), QStringLiteral("user") },
                      { QStringLiteral("content"), QJsonArray{
                            QJsonObject{
                                { QStringLiteral("type"), QStringLiteral("input_text") },
                                { QStringLiteral("text"), QStringLiteral("ping") }
                            }
                        } }
                  }
              } },
            { QStringLiteral("max_output_tokens"), 1 }
        };

        QElapsedTimer validationTimer;
        validationTimer.start();
        const auto probeProtocol = [&](
                                       const QString& protocol,
                                       const QStringList& endpoints,
                                       const QJsonObject& body) {
            QStringList failures;
            const int elapsedBeforeProbe = static_cast<int>(validationTimer.elapsed());
            const int overallRemaining = std::max(
                0,
                kOpenAIValidationBudgetMs - elapsedBeforeProbe);
            const int probeBudget = result.compatibleProtocols.isEmpty()
                ? std::min(overallRemaining, kOpenAIPrimaryProbeBudgetMs)
                : std::min(overallRemaining, kOpenAISecondaryProbeBudgetMs);
            QElapsedTimer probeTimer;
            probeTimer.start();
            for (const QString& endpoint : endpoints) {
                const int remaining = probeBudget - static_cast<int>(probeTimer.elapsed());
                if (remaining <= 0) {
                    failures.push_back(QStringLiteral("probe budget exhausted"));
                    break;
                }
                const HttpJsonResult http = executeJsonRequest(
                    QStringLiteral("POST"),
                    QUrl(endpoint),
                    openAICompatibleHeaders(endpoint, apiKey),
                    &body,
                    std::min(remaining, kOpenAIEndpointProbeTimeoutMs));
                if (http.success) {
                    appendUnique(&result.compatibleProtocols, protocol);
                    if (protocol == QStringLiteral("openai_responses")) {
                        result.responsesEndpoint = endpoint;
                    }
                    if (result.protocol.isEmpty()) {
                        result.protocol = protocol;
                        result.endpoint = endpoint;
                    }
                    return;
                }
                failures.push_back(QStringLiteral("%1 -> %2")
                                       .arg(endpoint, http.error.isEmpty()
                                               ? QStringLiteral("validation failed")
                                               : http.error));
            }
            if (!failures.isEmpty()) {
                result.diagnostics.push_back(
                    QStringLiteral("%1: %2").arg(protocol, failures.join(QStringLiteral(" | "))));
            }
        };

        // Preserve the AI workbench's existing Chat preference while recording both capabilities.
        probeProtocol(QStringLiteral("openai_chat"), candidates.chatCompletions, chatBody);
        probeProtocol(QStringLiteral("openai_responses"), candidates.responses, responsesBody);

        result.success = !result.compatibleProtocols.isEmpty();
        if (!result.success) {
            result.error = result.diagnostics.isEmpty()
                ? QStringLiteral("No compatible OpenAI protocol succeeded.")
                : result.diagnostics.join(QLatin1Char('\n'));
        }
        return result;
    }

    if (category == AIProviderCategory::Gemini) {
        result.protocol = QStringLiteral("gemini_generate_content");
        result.endpoint = normalizedGeminiEndpoint(baseUrl, model);
        const QJsonObject body{
            { QStringLiteral("contents"), QJsonArray{
                  QJsonObject{
                      { QStringLiteral("parts"), QJsonArray{
                            QJsonObject{
                                { QStringLiteral("text"), QStringLiteral("ping") }
                            }
                        } }
                  }
              } },
            { QStringLiteral("generationConfig"), QJsonObject{
                  { QStringLiteral("maxOutputTokens"), 1 }
              } }
        };
        const QList<QPair<QByteArray, QByteArray>> headers{
            { QByteArray("x-goog-api-key"), apiKey }
        };
        const HttpJsonResult http = executeJsonRequest(
            QStringLiteral("POST"), QUrl(result.endpoint), headers, &body);
        result.success = http.success;
        result.error = http.error;
        if (result.success) {
            result.compatibleProtocols.push_back(result.protocol);
        } else if (!result.error.isEmpty()) {
            result.diagnostics.push_back(result.error);
        }
        return result;
    }

    if (category == AIProviderCategory::Claude) {
        result.protocol = QStringLiteral("anthropic_messages");
        result.endpoint = normalizedClaudeEndpoint(baseUrl);
        const QList<QPair<QByteArray, QByteArray>> headers{
            { QByteArray("x-api-key"), apiKey },
            { QByteArray("anthropic-version"), QByteArray("2023-06-01") }
        };
        const QJsonObject body{
            { QStringLiteral("model"), model },
            { QStringLiteral("max_tokens"), 1 },
            { QStringLiteral("messages"), QJsonArray{
                  QJsonObject{
                      { QStringLiteral("role"), QStringLiteral("user") },
                      { QStringLiteral("content"), QStringLiteral("ping") }
                  }
              } }
        };
        const HttpJsonResult http = executeJsonRequest(
            QStringLiteral("POST"),
            QUrl(result.endpoint),
            headers,
            &body);
        result.success = http.success;
        result.error = http.error;
        if (result.success) {
            result.compatibleProtocols.push_back(result.protocol);
        } else if (!result.error.isEmpty()) {
            result.diagnostics.push_back(result.error);
        }
        return result;
    }

    result.protocol = QStringLiteral("ollama_chat");
    result.endpoint = normalizedOllamaEndpoint(baseUrl);
    const QJsonObject body{
        { QStringLiteral("model"), model },
        { QStringLiteral("stream"), false },
        { QStringLiteral("messages"), QJsonArray{
              QJsonObject{
                  { QStringLiteral("role"), QStringLiteral("user") },
                  { QStringLiteral("content"), QStringLiteral("ping") }
              }
          } }
    };
    const HttpJsonResult http = executeJsonRequest(
        QStringLiteral("POST"),
        QUrl(result.endpoint),
        {},
        &body);
    result.success = http.success;
    result.error = http.error;
    if (result.success) {
        result.compatibleProtocols.push_back(result.protocol);
    } else if (!result.error.isEmpty()) {
        result.diagnostics.push_back(result.error);
    }
    return result;
}

} // namespace cgplay
