#include "AIModelDiscovery.h"

#include "AIConnectionValidator.h"

#include <QEventLoop>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QSet>

#include <algorithm>

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
    QUrl url(AIConnectionValidator::geminiBaseUrl(baseUrl));
    url.setPath(url.path() + QStringLiteral("/models"));
    return url.toString(QUrl::FullyEncoded);
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
    const QList<QPair<QByteArray, QByteArray>>& headers,
    int timeoutMs = kRequestTimeoutMs)
{
    HttpJsonResult result;
    if (!url.isValid()) {
        result.error = QStringLiteral("Invalid endpoint: %1").arg(url.toString());
        return result;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(timeoutMs);
    for (const auto& header : headers) {
        request.setRawHeader(header.first, header.second);
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(timeoutMs);

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
    QJsonParseError parseError;
    result.document = QJsonDocument::fromJson(result.payload, &parseError);
    if (reply->error() != QNetworkReply::NoError) {
        result.error = httpErrorMessage(reply, result.payload, result.document);
        reply->deleteLater();
        return result;
    }

    if (result.statusCode < 200 || result.statusCode >= 300 ||
        parseError.error != QJsonParseError::NoError || result.document.isNull()) {
        result.error = QStringLiteral("The model endpoint did not return a successful JSON response.");
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

AIModelDiscoveryResult discoverPages(
    AIProviderCategory category,
    const QString& endpoint,
    const QList<QPair<QByteArray, QByteArray>>& headers)
{
    AIModelDiscoveryResult result;
    result.endpoint = endpoint;
    QUrl url(endpoint);
    QSet<QString> seenCursors;
    QElapsedTimer elapsed;
    elapsed.start();
    constexpr int kDiscoveryBudgetMs = 45000;
    constexpr int kMaximumPages = 20;
    for (int page = 0; page < kMaximumPages; ++page) {
        const int remaining = kDiscoveryBudgetMs - static_cast<int>(elapsed.elapsed());
        if (remaining <= 0) {
            result.error = QStringLiteral("Model discovery exceeded its time budget; retry or enter a model ID.");
            return result;
        }
        const HttpJsonResult http = executeJsonRequest(url, headers, std::min(remaining, kRequestTimeoutMs));
        if (!http.success) {
            result.error = http.error;
            return result;
        }
        const QJsonObject object = http.document.object();
        if (category == AIProviderCategory::Gemini) {
            for (const QJsonValue& item : object.value(QStringLiteral("models")).toArray()) {
                const QJsonObject model = item.toObject();
                const QJsonArray methods = model.value(QStringLiteral("supportedGenerationMethods")).toArray();
                if (model.contains(QStringLiteral("supportedGenerationMethods")) &&
                    !methods.contains(QJsonValue(QStringLiteral("generateContent")))) continue;
                result.models.append(model.value(QStringLiteral("name")).toString());
            }
        } else {
            result.models.append(extractGenericModelIds(http.document));
        }
        result.models = dedupedModels(result.models);
        const bool gemini = category == AIProviderCategory::Gemini;
        const QString cursor = gemini
            ? object.value(QStringLiteral("nextPageToken")).toString()
            : object.value(QStringLiteral("last_id")).toString();
        const bool hasMore = gemini ? !cursor.isEmpty() : object.value(QStringLiteral("has_more")).toBool();
        if (!hasMore) {
            result.success = !result.models.isEmpty();
            if (!result.success) result.error = QStringLiteral("No compatible chat models were returned.");
            return result;
        }
        if (cursor.isEmpty() || seenCursors.contains(cursor)) {
            result.error = QStringLiteral("The model endpoint returned an invalid or repeated pagination cursor.");
            return result;
        }
        seenCursors.insert(cursor);
        // Only the cursor is accepted; server-supplied URLs never receive credentials.
        QUrlQuery query(url);
        const QString parameter = gemini ? QStringLiteral("pageToken") : QStringLiteral("after_id");
        query.removeAllQueryItems(parameter);
        query.addQueryItem(parameter, cursor);
        url.setQuery(query);
    }
    result.error = QStringLiteral("The model list exceeded the page limit; enter a model ID directly.");
    return result;
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
            AIModelDiscoveryResult pageResult = discoverPages(
                category, endpoint,
                AIConnectionValidator::openAICompatibleHeaders(endpoint, apiKey));
            if (pageResult.success) return pageResult;
            result.diagnostics.push_back(QStringLiteral("%1 -> %2").arg(endpoint, pageResult.error));
        }
        result.error = result.diagnostics.join(QLatin1Char('\n'));
        return result;
    }

    if (category == AIProviderCategory::Gemini) {
        result.endpoint = normalizedGeminiModelsEndpoint(baseUrl);
        const QList<QPair<QByteArray, QByteArray>> headers{
            { QByteArray("x-goog-api-key"), apiKey }
        };
        return discoverPages(category, result.endpoint, headers);
    }

    if (category == AIProviderCategory::Claude) {
        const QList<QPair<QByteArray, QByteArray>> anthropicHeaders{
            { QByteArray("x-api-key"), apiKey },
            { QByteArray("anthropic-version"), QByteArray("2023-06-01") }
        };

        result.endpoint = normalizedClaudeModelsEndpoint(baseUrl);
        AIModelDiscoveryResult pageResult = discoverPages(category, result.endpoint, anthropicHeaders);
        if (pageResult.success) return pageResult;
        result.diagnostics.push_back(QStringLiteral("%1 -> %2").arg(result.endpoint, pageResult.error));

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
