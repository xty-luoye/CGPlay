#include "AIModelDiscovery.h"

#include "AIConnectionValidator.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace cgplay {

namespace {

constexpr int kRequestTimeoutMs = 30000;
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

QString stripAnthropicCompatPath(QString value)
{
    value = trimTrailingSlashes(value);
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

QString normalizedGeminiModelsEndpoint(QString baseUrl)
{
    baseUrl = trimTrailingSlashes(withoutGeminiApiKeyQuery(baseUrl));
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromLatin1(kDefaultGeminiBaseUrl);
    }
    if (baseUrl.endsWith(QStringLiteral("/v1beta/models"), Qt::CaseInsensitive)) {
        return withoutGeminiApiKeyQuery(baseUrl);
    }
    if (baseUrl.endsWith(QStringLiteral("/v1beta"), Qt::CaseInsensitive)) {
        return withoutGeminiApiKeyQuery(baseUrl + QStringLiteral("/models"));
    }
    return withoutGeminiApiKeyQuery(baseUrl + QStringLiteral("/v1beta/models"));
}

QString normalizedClaudeModelsEndpoint(QString baseUrl)
{
    baseUrl = trimTrailingSlashes(baseUrl);
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromLatin1(kDefaultClaudeBaseUrl);
    }
    if (baseUrl.endsWith(QStringLiteral("/v1/models"), Qt::CaseInsensitive)) {
        return baseUrl;
    }
    if (baseUrl.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive)) {
        return baseUrl + QStringLiteral("/models");
    }
    return baseUrl + QStringLiteral("/v1/models");
}

QString normalizedOllamaModelsEndpoint(QString baseUrl)
{
    baseUrl = trimTrailingSlashes(baseUrl);
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromLatin1(kDefaultOllamaBaseUrl);
    }
    if (baseUrl.endsWith(QStringLiteral("/api/tags"), Qt::CaseInsensitive)) {
        return baseUrl;
    }
    if (baseUrl.endsWith(QStringLiteral("/api"), Qt::CaseInsensitive)) {
        return baseUrl + QStringLiteral("/tags");
    }
    return baseUrl + QStringLiteral("/api/tags");
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
    const QUrl& url,
    const QList<QPair<QByteArray, QByteArray>>& headers)
{
    HttpJsonResult result;
    if (!url.isValid()) {
        result.error = QStringLiteral("Invalid endpoint: %1").arg(url.toString());
        return result;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(kRequestTimeoutMs);
    for (const auto& header : headers) {
        request.setRawHeader(header.first, header.second);
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(kRequestTimeoutMs);

    QNetworkReply* reply = manager.get(request);
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

QStringList dedupedModels(const QStringList& models)
{
    QStringList deduped;
    for (QString model : models) {
        model = model.trimmed();
        if (model.startsWith(QStringLiteral("models/"), Qt::CaseInsensitive)) {
            model.remove(0, QStringLiteral("models/").size());
        }
        if (!model.isEmpty() && !deduped.contains(model, Qt::CaseInsensitive)) {
            deduped.push_back(model);
        }
    }
    return deduped;
}

void collectModelIds(const QJsonValue& value, QStringList* models, int depth = 0)
{
    if (!models || depth > 5) {
        return;
    }
    if (value.isArray()) {
        for (const QJsonValue& item : value.toArray()) {
            if (item.isString()) {
                models->push_back(item.toString());
            } else {
                collectModelIds(item, models, depth + 1);
            }
        }
        return;
    }
    if (!value.isObject()) {
        return;
    }

    const QJsonObject object = value.toObject();
    for (const char* key : { "id", "model", "name", "slug" }) {
        const QString candidate = object.value(QString::fromLatin1(key)).toString().trimmed();
        if (!candidate.isEmpty()) {
            models->push_back(candidate);
            break;
        }
    }
    for (const char* key : { "data", "models", "items", "results", "result" }) {
        const QJsonValue nested = object.value(QString::fromLatin1(key));
        if (!nested.isUndefined() && !nested.isNull()) {
            collectModelIds(nested, models, depth + 1);
        }
    }
}

QStringList extractGenericModelIds(const QJsonDocument& document)
{
    QStringList models;
    if (document.isArray()) {
        collectModelIds(document.array(), &models);
    } else if (document.isObject()) {
        collectModelIds(document.object(), &models);
    }
    return dedupedModels(models);
}

} // namespace

AIModelDiscoveryResult AIModelDiscovery::discover(
    AIProviderCategory category,
    const QString& baseUrl,
    const QByteArray& apiKey) const
{
    AIModelDiscoveryResult result;

    if (category != AIProviderCategory::Ollama && apiKey.trimmed().isEmpty()) {
        result.error = QStringLiteral("API key is required before model discovery.");
        return result;
    }

    if (category == AIProviderCategory::OpenAICompatible ||
        category == AIProviderCategory::Qwen) {
        const AIOpenAIEndpointCandidates candidates =
            AIConnectionValidator::openAICompatibleEndpointCandidates(baseUrl);
        for (const QString& endpoint : candidates.models) {
            const HttpJsonResult http = executeJsonRequest(
                QUrl(endpoint),
                AIConnectionValidator::openAICompatibleHeaders(endpoint, apiKey));
            if (!http.success) {
                result.diagnostics.push_back(QStringLiteral("%1 -> %2").arg(endpoint, http.error));
                continue;
            }
            const QStringList models = extractGenericModelIds(http.document);
            if (!models.isEmpty()) {
                result.success = true;
                result.endpoint = endpoint;
                result.models = models;
                return result;
            }
            result.diagnostics.push_back(QStringLiteral("%1 -> no models returned").arg(endpoint));
        }
        result.error = result.diagnostics.join(QLatin1Char('\n'));
        return result;
    }

    if (category == AIProviderCategory::Gemini) {
        result.endpoint = normalizedGeminiModelsEndpoint(baseUrl);
        const QList<QPair<QByteArray, QByteArray>> headers{
            { QByteArray("x-goog-api-key"), apiKey }
        };
        const HttpJsonResult http = executeJsonRequest(QUrl(result.endpoint), headers);
        if (!http.success) {
            result.error = http.error;
            result.diagnostics.push_back(http.error);
            return result;
        }
        result.models = extractGenericModelIds(http.document);
        result.success = !result.models.isEmpty();
        result.error = result.success ? QString() : QStringLiteral("No Gemini models were returned.");
        if (!result.success) {
            result.diagnostics.push_back(result.error);
        }
        return result;
    }

    if (category == AIProviderCategory::Claude) {
        const QList<QPair<QByteArray, QByteArray>> anthropicHeaders{
            { QByteArray("x-api-key"), apiKey },
            { QByteArray("anthropic-version"), QByteArray("2023-06-01") }
        };

        result.endpoint = normalizedClaudeModelsEndpoint(baseUrl);
        const HttpJsonResult http = executeJsonRequest(QUrl(result.endpoint), anthropicHeaders);
        if (http.success) {
            result.models = extractGenericModelIds(http.document);
            result.success = !result.models.isEmpty();
            if (result.success) {
                return result;
            }
            result.diagnostics.push_back(QStringLiteral("%1 -> no models returned").arg(result.endpoint));
        } else {
            result.diagnostics.push_back(QStringLiteral("%1 -> %2").arg(result.endpoint, http.error));
        }

        const QString openAICompatBase = stripAnthropicCompatPath(baseUrl);
        if (!openAICompatBase.isEmpty() &&
            openAICompatBase.compare(trimTrailingSlashes(baseUrl), Qt::CaseInsensitive) != 0) {
            const AIOpenAIEndpointCandidates candidates =
                AIConnectionValidator::openAICompatibleEndpointCandidates(openAICompatBase);
            for (const QString& endpoint : candidates.models) {
                const HttpJsonResult compatHttp = executeJsonRequest(
                    QUrl(endpoint),
                    AIConnectionValidator::openAICompatibleHeaders(endpoint, apiKey));
                if (!compatHttp.success) {
                    result.diagnostics.push_back(QStringLiteral("%1 -> %2").arg(endpoint, compatHttp.error));
                    continue;
                }
                result.models = extractGenericModelIds(compatHttp.document);
                if (!result.models.isEmpty()) {
                    result.success = true;
                    result.endpoint = endpoint;
                    return result;
                }
                result.diagnostics.push_back(QStringLiteral("%1 -> no models returned").arg(endpoint));
            }
        }

        result.error = result.diagnostics.join(QLatin1Char('\n'));
        return result;
    }

    result.endpoint = normalizedOllamaModelsEndpoint(baseUrl);
    const HttpJsonResult http = executeJsonRequest(QUrl(result.endpoint), {});
    if (!http.success) {
        result.error = http.error;
        result.diagnostics.push_back(http.error);
        return result;
    }
    result.models = extractGenericModelIds(http.document);
    result.success = !result.models.isEmpty();
    result.error = result.success ? QString() : QStringLiteral("No Ollama models were returned.");
    if (!result.success) {
        result.diagnostics.push_back(result.error);
    }
    return result;
}

} // namespace cgplay
