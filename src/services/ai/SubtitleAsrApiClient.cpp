#include "SubtitleAsrApiClient.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QStringList>
#include <QStringConverter>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QEventLoop>
#include <QVariant>

namespace cgplay {
namespace {

constexpr auto kDefaultMimoBaseUrl = "https://api.xiaomimimo.com";
constexpr auto kDefaultMimoModel = "mimo-v2.5-asr";
constexpr auto kDefaultQwenAsrBaseUrl = "https://dashscope.aliyuncs.com/compatible-mode/v1";
constexpr auto kDefaultQwenAsrModel = "qwen3-asr-flash";
constexpr auto kDefaultOpenAIBaseUrl = "https://api.openai.com";
constexpr auto kDefaultOpenAITranscriptionModel = "whisper-1";
constexpr auto kDefaultGeminiBaseUrl = "https://generativelanguage.googleapis.com";
constexpr auto kDefaultGeminiModel = "gemini-2.5-flash";

enum class MimoAuthMode {
    ApiKeyHeader,
    AuthorizationBearer
};

enum class GeminiRoute {
    OfficialHeader,
    OfficialQuery,
    OfficialBearer,
    OpenAICompatibleBearer
};

struct ProtocolAttemptResult {
    bool success = false;
    QString errorMessage;
    QString backend;
    QString backendDetail;
    QString text;
    QString translatedText;
    QString detectedLanguage;
    QJsonObject rawJson;
};

QString trimTrailingSlashes(QString value)
{
    value = value.trimmed();
    while (value.endsWith(QLatin1Char('/'))) {
        value.chop(1);
    }
    return value;
}

QString defaultModelForProtocol(SubtitleAsrProtocol protocol, QString model)
{
    model = model.trimmed();
    if (!model.isEmpty() && model.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
        return model;
    }
    switch (protocol) {
    case SubtitleAsrProtocol::QwenDashScopeAsr:
        return QString::fromLatin1(kDefaultQwenAsrModel);
    case SubtitleAsrProtocol::GeminiGenerateContent:
        return QString::fromLatin1(kDefaultGeminiModel);
    case SubtitleAsrProtocol::MimoChat:
        return QString::fromLatin1(kDefaultMimoModel);
    case SubtitleAsrProtocol::OpenAITranscriptions:
        return QString::fromLatin1(kDefaultOpenAITranscriptionModel);
    case SubtitleAsrProtocol::OpenAIResponsesAudio:
        return QString::fromLatin1(kDefaultOpenAITranscriptionModel);
    }
    return QString::fromLatin1(kDefaultOpenAITranscriptionModel);
}

QString modelForProtocol(SubtitleAsrProtocol protocol, const QString& requestedModel)
{
    const QString normalized = requestedModel.trimmed();
    if (normalized.isEmpty() || normalized.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
        return defaultModelForProtocol(protocol, normalized);
    }
    switch (protocol) {
    case SubtitleAsrProtocol::QwenDashScopeAsr:
        if (normalized.compare(QStringLiteral("qwen3-asr"), Qt::CaseInsensitive) == 0 ||
            normalized.contains(QStringLiteral("mimo"), Qt::CaseInsensitive) ||
            normalized.contains(QStringLiteral("whisper"), Qt::CaseInsensitive) ||
            normalized.contains(QStringLiteral("gemini"), Qt::CaseInsensitive) ||
            normalized.contains(QStringLiteral("gpt-"), Qt::CaseInsensitive)) {
            return QString::fromLatin1(kDefaultQwenAsrModel);
        }
        return normalized.isEmpty() ? QString::fromLatin1(kDefaultQwenAsrModel) : normalized;
    case SubtitleAsrProtocol::GeminiGenerateContent:
        return normalized;
    case SubtitleAsrProtocol::MimoChat:
        if (normalized == QStringLiteral("mimo-v2") ||
            normalized == QStringLiteral("mimo-v2-asr") ||
            !normalized.contains(QStringLiteral("mimo-v2.5-asr"), Qt::CaseInsensitive)) {
            return QString::fromLatin1(kDefaultMimoModel);
        }
        return normalized;
    case SubtitleAsrProtocol::OpenAITranscriptions:
        return normalized;
    case SubtitleAsrProtocol::OpenAIResponsesAudio:
        return normalized.isEmpty()
            ? QString::fromLatin1(kDefaultOpenAITranscriptionModel)
            : normalized;
    }
    return defaultModelForProtocol(protocol, normalized);
}

QString normalizeMimoChatEndpoint(QString baseUrl)
{
    baseUrl = trimTrailingSlashes(baseUrl);
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromLatin1(kDefaultMimoBaseUrl);
    }
    if (baseUrl.endsWith(QStringLiteral("/chat/completions"), Qt::CaseInsensitive)) {
        return baseUrl;
    }
    if (baseUrl.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive)) {
        return baseUrl + QStringLiteral("/chat/completions");
    }
    return baseUrl + QStringLiteral("/v1/chat/completions");
}

QString normalizeOpenAITranscriptionsEndpoint(QString baseUrl)
{
    baseUrl = trimTrailingSlashes(baseUrl);
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromLatin1(kDefaultOpenAIBaseUrl);
    }
    if (baseUrl.endsWith(QStringLiteral("/audio/transcriptions"), Qt::CaseInsensitive)) {
        return baseUrl;
    }
    if (baseUrl.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive)) {
        return baseUrl + QStringLiteral("/audio/transcriptions");
    }
    return baseUrl + QStringLiteral("/v1/audio/transcriptions");
}

QString normalizeOpenAIResponsesEndpoint(QString baseUrl)
{
    baseUrl = trimTrailingSlashes(baseUrl);
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromLatin1(kDefaultOpenAIBaseUrl);
    }
    if (baseUrl.endsWith(QStringLiteral("/responses"), Qt::CaseInsensitive)) {
        return baseUrl;
    }
    if (baseUrl.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive)) {
        return baseUrl + QStringLiteral("/responses");
    }
    return baseUrl + QStringLiteral("/v1/responses");
}

QString normalizeOpenAIChatEndpoint(QString baseUrl)
{
    baseUrl = trimTrailingSlashes(baseUrl);
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromLatin1(kDefaultOpenAIBaseUrl);
    }
    if (baseUrl.endsWith(QStringLiteral("/chat/completions"), Qt::CaseInsensitive)) {
        return baseUrl;
    }
    if (baseUrl.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive)) {
        return baseUrl + QStringLiteral("/chat/completions");
    }
    return baseUrl + QStringLiteral("/v1/chat/completions");
}

QString normalizeQwenChatEndpoint(QString baseUrl)
{
    baseUrl = trimTrailingSlashes(baseUrl);
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromLatin1(kDefaultQwenAsrBaseUrl);
    }
    const QUrl parsed(baseUrl);
    if (parsed.scheme().isEmpty() && !baseUrl.startsWith(QStringLiteral("http"), Qt::CaseInsensitive)) {
        baseUrl = QStringLiteral("https://") + baseUrl;
    }
    if (baseUrl.endsWith(QStringLiteral("/chat/completions"), Qt::CaseInsensitive)) {
        return baseUrl;
    }
    if (baseUrl.endsWith(QStringLiteral("/compatible-mode/v1"), Qt::CaseInsensitive) ||
        baseUrl.endsWith(QStringLiteral("/v1"), Qt::CaseInsensitive)) {
        return baseUrl + QStringLiteral("/chat/completions");
    }
    return baseUrl + QStringLiteral("/compatible-mode/v1/chat/completions");
}

QString normalizeGeminiEndpoint(QString baseUrl, const QString& model)
{
    baseUrl = trimTrailingSlashes(baseUrl);
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromLatin1(kDefaultGeminiBaseUrl);
    }
    if (baseUrl.contains(QStringLiteral(":generateContent"), Qt::CaseInsensitive)) {
        return baseUrl;
    }
    if (baseUrl.contains(QStringLiteral("/models/"), Qt::CaseInsensitive)) {
        return baseUrl + QStringLiteral(":generateContent");
    }
    if (baseUrl.endsWith(QStringLiteral("/v1beta"), Qt::CaseInsensitive)) {
        return baseUrl + QStringLiteral("/models/%1:generateContent").arg(model);
    }
    return baseUrl + QStringLiteral("/v1beta/models/%1:generateContent").arg(model);
}

bool isOfficialGeminiBaseUrl(const QString& baseUrl)
{
    const QString trimmed = baseUrl.trimmed();
    if (trimmed.isEmpty()) {
        return true;
    }
    const QUrl url(trimmed);
    const QString host = url.host().trimmed();
    if (!host.isEmpty()) {
        return host.compare(QStringLiteral("generativelanguage.googleapis.com"), Qt::CaseInsensitive) == 0;
    }
    return trimmed.contains(QStringLiteral("generativelanguage.googleapis.com"), Qt::CaseInsensitive);
}

QString audioFormatFromPath(const QString& audioPath)
{
    const QString suffix = QFileInfo(audioPath).suffix().trimmed().toLower();
    if (suffix == QStringLiteral("mp3")) {
        return QStringLiteral("mp3");
    }
    if (suffix == QStringLiteral("m4a")) {
        return QStringLiteral("mp4");
    }
    return QStringLiteral("wav");
}

QString audioMimeTypeFromFormat(const QString& format)
{
    if (format == QStringLiteral("mp3")) {
        return QStringLiteral("audio/mpeg");
    }
    if (format == QStringLiteral("mp4")) {
        return QStringLiteral("audio/mp4");
    }
    return QStringLiteral("audio/wav");
}

QString geminiAudioMimeTypeFromFormat(const QString& format)
{
    if (format == QStringLiteral("mp3")) {
        return QStringLiteral("audio/mp3");
    }
    if (format == QStringLiteral("mp4")) {
        return QStringLiteral("audio/mp4");
    }
    return QStringLiteral("audio/wav");
}

QString normalizedLanguage(QString language)
{
    language = language.trimmed();
    if (language.isEmpty() || language.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("auto");
    }
    return language;
}

QString responsesAudioFormat(const QString& audioPath)
{
    const QString suffix = QFileInfo(audioPath).suffix().trimmed().toLower();
    if (suffix == QStringLiteral("mp3")) {
        return QStringLiteral("mp3");
    }
    if (suffix == QStringLiteral("m4a")) {
        return QStringLiteral("mp4");
    }
    return QStringLiteral("wav");
}

QString responsesAudioMimeType(const QString& format)
{
    if (format == QStringLiteral("mp3")) {
        return QStringLiteral("audio/mpeg");
    }
    if (format == QStringLiteral("mp4")) {
        return QStringLiteral("audio/mp4");
    }
    return QStringLiteral("audio/wav");
}

QString stripMarkdownFence(QString text)
{
    text = text.trimmed();
    if (!text.startsWith(QStringLiteral("```"))) {
        return text;
    }
    const int firstLineEnd = text.indexOf(QLatin1Char('\n'));
    if (firstLineEnd >= 0) {
        text = text.mid(firstLineEnd + 1);
    }
    if (text.endsWith(QStringLiteral("```"))) {
        text.chop(3);
    }
    return text.trimmed();
}

int mojibakeScore(const QString& text)
{
    int score = text.count(QChar(0xFFFD)) * 5;
    static const QStringList markers = {
        QStringLiteral("Ã"),
        QStringLiteral("Â"),
        QStringLiteral("å"),
        QStringLiteral("æ"),
        QStringLiteral("ç"),
        QStringLiteral("è"),
        QStringLiteral("é"),
        QStringLiteral("ì"),
        QStringLiteral("ï"),
        QStringLiteral("ð"),
        QStringLiteral("õ"),
        QStringLiteral("锟")
    };
    for (const QString& marker : markers) {
        score += text.count(marker) * 2;
    }
    return score;
}

QString repairCommonMojibake(const QString& text)
{
    if (text.trimmed().isEmpty() || mojibakeScore(text) < 3) {
        return text;
    }
    const QString localRepaired = QString::fromUtf8(text.toLocal8Bit()).trimmed();
    if (!localRepaired.isEmpty() && mojibakeScore(localRepaired) + 2 < mojibakeScore(text)) {
        return localRepaired;
    }
    const auto encoding = QStringConverter::encodingForName("GB18030");
    if (!encoding.has_value()) {
        return text;
    }
    QStringEncoder encoder(*encoding);
    const QString repaired = QString::fromUtf8(encoder(text)).trimmed();
    if (repaired.isEmpty()) {
        return text;
    }
    return mojibakeScore(repaired) + 2 < mojibakeScore(text) ? repaired : text;
}

QString animeTerminologyHint()
{
    return QStringLiteral(
        "Anime terminology hint: keep these names consistent: "
        "Shadow Garden, Shadow, Rose, Iris, Beatrix, Oriana, Cid. "
        "Do not rewrite them into unrelated words.");
}

QString transcriptionPrompt(const QString& language)
{
    if (normalizedLanguage(language).compare(QStringLiteral("ja"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral(
            "ASR only. Transcribe the spoken Japanese verbatim in clean subtitle style. "
            "Output Japanese kana/kanji only unless a character clearly speaks another language. "
            "Do not translate into Chinese or English, summarize, or romanize. "
            "Do not replace Japanese short utterances with English fillers like Yeah, OK, ah, or hey unless the audio is clearly English. "
            "If a Japanese line sounds like English filler or phrases such as Yeah, Okay, Like my power, or My power, re-check it as Japanese and output the most likely Japanese kana/kanji. "
            "Do not output Simplified Chinese inside the transcript. "
            "Prefer natural sentence boundaries, preserve honorifics, pauses, names, and short utterances. "
            "If the audio is unclear, choose the most likely Japanese transcript instead of switching languages. "
            "Do not invent missing words. "
            "Return only the transcript text. ") + animeTerminologyHint();
    }
    return QStringLiteral(
        "ASR only. Transcribe the spoken audio verbatim in the original language in clean subtitle style. "
        "Do not translate, summarize, or explain. "
        "Prefer natural sentence boundaries, preserve pauses, names, and short utterances. "
        "Do not invent missing words. "
        "Return only the transcript text. ") + animeTerminologyHint();
}

QString transcriptionAndTranslationPrompt(const QString& sourceLanguage, const QString& targetLanguage)
{
    return QStringLiteral(
        "ASR only. First transcribe the spoken audio verbatim in %1. "
        "Then provide a %2 translation. "
        "For Japanese audio, keep the transcript in Japanese kana/kanji and do not switch it to English or Chinese. "
        "If Japanese audio is uncertain, prefer a plausible Japanese transcript over English fillers or Simplified Chinese drift. "
        "Keep subtitle tone natural, short, and literal enough to match lip movement. "
        "Do not summarize, explain, or invent missing dialogue. "
        "Return strict JSON with keys transcript and translation. ")
        .arg(normalizedLanguage(sourceLanguage),
            targetLanguage.trimmed().isEmpty() ? QStringLiteral("Chinese") : targetLanguage.trimmed())
        + animeTerminologyHint();
}

QString responsesAudioPrompt(const QString& sourceLanguage, const QString& targetLanguage)
{
    return QStringLiteral(
        "ASR only. Return strict JSON with keys transcript and translation. "
        "Transcribe the spoken audio verbatim in %1 and translate it to %2. "
        "For Japanese audio, transcript must stay Japanese kana/kanji; do not output English filler or Simplified Chinese as the transcript unless clearly spoken. "
        "Keep the translation concise, faithful, and subtitle-friendly. "
        "Do not summarize, explain, or add extra keys. ")
        .arg(normalizedLanguage(sourceLanguage),
            targetLanguage.trimmed().isEmpty() ? QStringLiteral("Chinese") : targetLanguage.trimmed())
        + animeTerminologyHint();
}
void extractDirectTranslationJson(
    const QString& responseText,
    QString* transcript,
    QString* translation)
{
    const QByteArray bytes = stripMarkdownFence(responseText).toUtf8();
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return;
    }

    const QJsonObject object = document.object();
    const QString parsedTranscript = object.value(QStringLiteral("transcript")).toString().trimmed();
    const QString parsedTranslation = object.value(QStringLiteral("translation")).toString().trimmed();
    if (transcript && !parsedTranscript.isEmpty()) {
        *transcript = parsedTranscript;
    }
    if (translation && !parsedTranslation.isEmpty()) {
        *translation = parsedTranslation;
    }
}

QString jsonContentText(const QJsonValue& value)
{
    if (value.isString()) {
        return value.toString().trimmed();
    }
    if (value.isArray()) {
        QStringList parts;
        for (const QJsonValue& item : value.toArray()) {
            const QJsonObject object = item.toObject();
            const QString text = object.value(QStringLiteral("text")).toString().trimmed();
            if (!text.isEmpty()) {
                parts.push_back(text);
            }
        }
        return parts.join(QLatin1Char('\n')).trimmed();
    }
    return {};
}

QString firstChatCompletionText(const QJsonObject& object)
{
    const QJsonArray choices = object.value(QStringLiteral("choices")).toArray();
    if (!choices.isEmpty()) {
        const QJsonObject message = choices.first().toObject().value(QStringLiteral("message")).toObject();
        const QString content = jsonContentText(message.value(QStringLiteral("content")));
        if (!content.isEmpty()) {
            return content;
        }
    }
    return object.value(QStringLiteral("text")).toString().trimmed();
}

QString extractGeminiText(const QJsonObject& object)
{
    QStringList parts;
    const QJsonArray candidates = object.value(QStringLiteral("candidates")).toArray();
    for (const QJsonValue& candidateValue : candidates) {
        const QJsonObject content = candidateValue.toObject().value(QStringLiteral("content")).toObject();
        for (const QJsonValue& partValue : content.value(QStringLiteral("parts")).toArray()) {
            const QString text = partValue.toObject().value(QStringLiteral("text")).toString().trimmed();
            if (!text.isEmpty()) {
                parts.push_back(text);
            }
        }
    }
    return parts.join(QLatin1Char('\n')).trimmed();
}

void collectResponsesText(const QJsonValue& value, QStringList* outTexts)
{
    if (!outTexts || value.isUndefined() || value.isNull()) {
        return;
    }
    if (value.isString()) {
        const QString text = value.toString().trimmed();
        if (!text.isEmpty()) {
            outTexts->push_back(text);
        }
        return;
    }
    if (value.isArray()) {
        for (const QJsonValue& item : value.toArray()) {
            collectResponsesText(item, outTexts);
        }
        return;
    }
    if (!value.isObject()) {
        return;
    }

    const QJsonObject object = value.toObject();
    const QString outputText = object.value(QStringLiteral("output_text")).toString().trimmed();
    if (!outputText.isEmpty()) {
        outTexts->push_back(outputText);
        return;
    }

    const QString type = object.value(QStringLiteral("type")).toString().trimmed();
    const QString text = object.value(QStringLiteral("text")).toString().trimmed();
    if (type == QStringLiteral("output_text") && !text.isEmpty()) {
        outTexts->push_back(text);
    }

    collectResponsesText(object.value(QStringLiteral("output")), outTexts);
    collectResponsesText(object.value(QStringLiteral("content")), outTexts);
    collectResponsesText(object.value(QStringLiteral("response")), outTexts);
    collectResponsesText(object.value(QStringLiteral("messages")), outTexts);
    collectResponsesText(object.value(QStringLiteral("parts")), outTexts);
}

QString extractResponsesText(const QJsonObject& object)
{
    const QString outputText = object.value(QStringLiteral("output_text")).toString().trimmed();
    if (!outputText.isEmpty()) {
        return outputText;
    }
    QStringList texts;
    collectResponsesText(object.value(QStringLiteral("output")), &texts);
    collectResponsesText(object.value(QStringLiteral("content")), &texts);
    collectResponsesText(object.value(QStringLiteral("response")), &texts);
    collectResponsesText(object.value(QStringLiteral("messages")), &texts);
    collectResponsesText(object.value(QStringLiteral("parts")), &texts);
    if (texts.isEmpty()) {
        return {};
    }
    return texts.join(QStringLiteral("\n")).trimmed();
}

QString segmentText(const QJsonObject& object)
{
    QString text = object.value(QStringLiteral("text")).toString().trimmed();
    if (!text.isEmpty()) {
        return text;
    }
    text = object.value(QStringLiteral("sentence")).toString().trimmed();
    if (!text.isEmpty()) {
        return text;
    }
    return object.value(QStringLiteral("content")).toString().trimmed();
}

QJsonArray extractSegments(const QJsonObject& object)
{
    QJsonArray segments = object.value(QStringLiteral("segments")).toArray();
    if (!segments.isEmpty()) {
        return segments;
    }
    segments = object.value(QStringLiteral("chunks")).toArray();
    return segments;
}

QString textFromResponse(SubtitleAsrProtocol protocol, const QJsonObject& object)
{
    QStringList texts;
    for (const QJsonValue& value : extractSegments(object)) {
        const QString text = segmentText(value.toObject());
        if (!text.isEmpty()) {
            texts.push_back(text);
        }
    }
    if (!texts.isEmpty()) {
        return texts.join(QLatin1Char('\n')).trimmed();
    }
    if (protocol == SubtitleAsrProtocol::GeminiGenerateContent) {
        return extractGeminiText(object);
    }
    if (protocol == SubtitleAsrProtocol::OpenAIResponsesAudio) {
        return extractResponsesText(object);
    }
    return firstChatCompletionText(object);
}

QString languageFromChatCompletionAnnotations(const QJsonObject& object)
{
    const QJsonArray choices = object.value(QStringLiteral("choices")).toArray();
    for (const QJsonValue& choiceValue : choices) {
        const QJsonObject message = choiceValue.toObject().value(QStringLiteral("message")).toObject();
        for (const QJsonValue& annotationValue : message.value(QStringLiteral("annotations")).toArray()) {
            const QJsonObject annotation = annotationValue.toObject();
            const QString language = annotation.value(QStringLiteral("language")).toString().trimmed();
            if (!language.isEmpty()) {
                return language;
            }
        }
    }
    return {};
}

QString serverErrorText(const QJsonObject& object, const QByteArray& responseBytes)
{
    const QJsonValue errorValue = object.value(QStringLiteral("error"));
    if (errorValue.isString()) {
        return errorValue.toString().trimmed();
    }
    if (errorValue.isObject()) {
        const QJsonObject errorObject = errorValue.toObject();
        const QString compact = QString::fromUtf8(QJsonDocument(errorObject).toJson(QJsonDocument::Compact)).trimmed();
        if (!compact.isEmpty()) {
            return compact.left(1200);
        }
    }
    return QString::fromUtf8(responseBytes).trimmed().left(1200);
}

QString requestSizeSummary(
    const QByteArray& audioBytes,
    const QString& audioBase64,
    const QByteArray& body)
{
    return QStringLiteral("audioBytes=%1; audioBase64Chars=%2; requestBytes=%3")
        .arg(audioBytes.size())
        .arg(audioBase64.size())
        .arg(body.size());
}

QString redactSensitiveText(QString text, const QString& apiKey)
{
    const QString key = apiKey.trimmed();
    if (!key.isEmpty()) {
        text.replace(key, QStringLiteral("[REDACTED]"), Qt::CaseSensitive);
    }
    return text;
}

QString geminiFailureHint(int statusCode, QString responseText)
{
    responseText = responseText.toLower();
    if (statusCode == 429) {
        return QStringLiteral("Gemini quota/rate limit");
    }
    if (statusCode == 401 || statusCode == 403) {
        return QStringLiteral("Gemini API key permission/auth");
    }
    if (responseText.contains(QStringLiteral("api_key_invalid")) ||
        responseText.contains(QStringLiteral("api key not valid")) ||
        responseText.contains(QStringLiteral("permission")) ||
        responseText.contains(QStringLiteral("auth"))) {
        return QStringLiteral("Gemini API key permission/auth");
    }
    if (statusCode == 400) {
        if (responseText.contains(QStringLiteral("not support")) ||
            responseText.contains(QStringLiteral("unsupported")) ||
            responseText.contains(QStringLiteral("model"))) {
            return QStringLiteral("Gemini model or audio input incompatible");
        }
        if (responseText.contains(QStringLiteral("invalid")) ||
            responseText.contains(QStringLiteral("unknown name")) ||
            responseText.contains(QStringLiteral("inline")) ||
            responseText.contains(QStringLiteral("mime"))) {
            return QStringLiteral("Gemini payload format error");
        }
        return QStringLiteral("Gemini bad request");
    }
    if (statusCode == 404) {
        return QStringLiteral("Gemini model or endpoint not found");
    }
    return {};
}

QString protocolName(SubtitleAsrProtocol protocol)
{
    switch (protocol) {
    case SubtitleAsrProtocol::QwenDashScopeAsr:
        return QStringLiteral("qwen_dashscope_asr");
    case SubtitleAsrProtocol::GeminiGenerateContent:
        return QStringLiteral("gemini_generate_content");
    case SubtitleAsrProtocol::MimoChat:
        return QStringLiteral("mimo_chat");
    case SubtitleAsrProtocol::OpenAITranscriptions:
        return QStringLiteral("openai_audio_transcriptions");
    case SubtitleAsrProtocol::OpenAIResponsesAudio:
        return QStringLiteral("openai_responses_audio");
    }
    return QStringLiteral("unknown");
}

QString geminiRouteName(GeminiRoute route)
{
    switch (route) {
    case GeminiRoute::OfficialHeader:
        return QStringLiteral("gemini_generate_content");
    case GeminiRoute::OfficialQuery:
        return QStringLiteral("gemini_generate_content/query_key");
    case GeminiRoute::OfficialBearer:
        return QStringLiteral("gemini_generate_content/authorization_bearer");
    case GeminiRoute::OpenAICompatibleBearer:
        return QStringLiteral("gemini_openai_chat/authorization_bearer");
    }
    return QStringLiteral("gemini_unknown");
}

QString mimoAuthModeName(MimoAuthMode mode)
{
    switch (mode) {
    case MimoAuthMode::ApiKeyHeader:
        return QStringLiteral("api-key");
    case MimoAuthMode::AuthorizationBearer:
        return QStringLiteral("authorization_bearer");
    }
    return QStringLiteral("unknown");
}

bool isRetryableFailure(int statusCode, const QString& networkError, const QString& responseText)
{
    if (statusCode == 0) {
        return true;
    }
    if (statusCode == 408 || statusCode == 429 || statusCode == 500 || statusCode == 502 ||
        statusCode == 503 || statusCode == 504) {
        return true;
    }
    const QString normalized = (networkError + QLatin1Char(' ') + responseText).trimmed().toLower();
    return normalized.contains(QStringLiteral("connection closed")) ||
        normalized.contains(QStringLiteral("timed out")) ||
        normalized.contains(QStringLiteral("timeout")) ||
        normalized.contains(QStringLiteral("temporarily unavailable")) ||
        normalized.contains(QStringLiteral("bad gateway")) ||
        normalized.contains(QStringLiteral("service unavailable"));
}

bool isRetryableAttemptError(const QString& errorMessage)
{
    const QString normalized = errorMessage.trimmed().toLower();
    if (normalized.isEmpty()) {
        return false;
    }
    if (normalized.contains(QStringLiteral("http 400")) ||
        normalized.contains(QStringLiteral("http 401")) ||
        normalized.contains(QStringLiteral("http 403")) ||
        normalized.contains(QStringLiteral("invalid_api_key")) ||
        normalized.contains(QStringLiteral("incorrect api key")) ||
        normalized.contains(QStringLiteral("key is missing"))) {
        return false;
    }
    return normalized.contains(QStringLiteral("http 0")) ||
        normalized.contains(QStringLiteral("http 408")) ||
        normalized.contains(QStringLiteral("http 429")) ||
        normalized.contains(QStringLiteral("http 500")) ||
        normalized.contains(QStringLiteral("http 502")) ||
        normalized.contains(QStringLiteral("http 503")) ||
        normalized.contains(QStringLiteral("http 504")) ||
        normalized.contains(QStringLiteral("connection closed")) ||
        normalized.contains(QStringLiteral("timed out")) ||
        normalized.contains(QStringLiteral("timeout")) ||
        normalized.contains(QStringLiteral("temporarily unavailable")) ||
        normalized.contains(QStringLiteral("bad gateway")) ||
        normalized.contains(QStringLiteral("service unavailable"));
}

bool isGenericDashScopeQwenEndpoint(const QUrl& url)
{
    const QString host = url.host().trimmed().toLower();
    return host == QStringLiteral("dashscope.aliyuncs.com") ||
        host == QStringLiteral("dashscope-intl.aliyuncs.com");
}

QString qwenEndpointHint(const QUrl& url, const QString& detailText)
{
    const QString normalized = detailText.toLower();
    if (!isGenericDashScopeQwenEndpoint(url) ||
        !(normalized.contains(QStringLiteral("connection closed")) ||
          normalized.contains(QStringLiteral("timed out")) ||
          normalized.contains(QStringLiteral("timeout")))) {
        return {};
    }
    return QStringLiteral(
        "If this repeats, set the Qwen ASR base URL to the Model Studio Workspace endpoint, "
        "for example https://{WorkspaceId}.cn-beijing.maas.aliyuncs.com/compatible-mode/v1; "
        "the generic dashscope.aliyuncs.com endpoint can work for text models but may close Qwen3-ASR audio requests.");
}

ProtocolAttemptResult transcribeWithProtocolAndMimoAuth(
    SubtitleAsrProtocol protocol,
    const SubtitleAsrApiConfig& config,
    const QString& audioPath,
    MimoAuthMode mimoAuthMode,
    GeminiRoute geminiRoute = GeminiRoute::OfficialHeader)
{
    ProtocolAttemptResult result;

    QFile audioFile(audioPath);
    if (!audioFile.exists() || !audioFile.open(QIODevice::ReadOnly)) {
        result.errorMessage = QStringLiteral("Failed to open audio clip");
        return result;
    }
    const QByteArray audioBytes = audioFile.readAll();
    audioFile.close();
    if (audioBytes.isEmpty()) {
        result.errorMessage = QStringLiteral("Audio clip is empty");
        return result;
    }

    const QString model = modelForProtocol(protocol, config.model);
    const QString language = normalizedLanguage(config.sourceLanguageHint);
    const bool directTranslation =
        config.requestTranslation &&
        protocol == SubtitleAsrProtocol::OpenAIResponsesAudio;
    const QString promptText = directTranslation
        ? transcriptionAndTranslationPrompt(language, config.targetLanguage)
        : transcriptionPrompt(language);
    const QString audioFormat = audioFormatFromPath(audioPath);
    const QString audioMimeType = audioMimeTypeFromFormat(audioFormat);
    const QString geminiAudioMimeType = geminiAudioMimeTypeFromFormat(audioFormat);
    const QString audioBase64 = QString::fromLatin1(audioBytes.toBase64());
    const QString audioDataUrl = QStringLiteral("data:%1;base64,%2").arg(audioMimeType, audioBase64);

    QNetworkRequest request;
    QByteArray body;
    QHttpMultiPart* multipart = nullptr;

    if (protocol == SubtitleAsrProtocol::QwenDashScopeAsr) {
        request.setUrl(QUrl(normalizeQwenChatEndpoint(config.baseUrl)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Authorization", "Bearer " + config.apiKey.trimmed().toUtf8());

        QJsonObject asrOptions{
            { QStringLiteral("enable_itn"), false }
        };
        if (language.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
            asrOptions.insert(QStringLiteral("language"), language);
        }
        const QJsonObject payload{
            { QStringLiteral("model"), model },
            { QStringLiteral("messages"), QJsonArray{
                QJsonObject{
                    { QStringLiteral("role"), QStringLiteral("user") },
                    { QStringLiteral("content"), QJsonArray{
                        QJsonObject{
                            { QStringLiteral("type"), QStringLiteral("input_audio") },
                            { QStringLiteral("input_audio"), QJsonObject{
                                { QStringLiteral("data"), audioDataUrl }
                            } }
                        }
                    } }
                }
            } },
            { QStringLiteral("stream"), false },
            { QStringLiteral("asr_options"), asrOptions }
        };
        body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    } else if (protocol == SubtitleAsrProtocol::OpenAITranscriptions) {
        request.setUrl(QUrl(normalizeOpenAITranscriptionsEndpoint(config.baseUrl)));
        request.setRawHeader("Authorization", "Bearer " + config.apiKey.trimmed().toUtf8());

        multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

        QHttpPart modelPart;
        modelPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(QStringLiteral("form-data; name=\"model\"")));
        modelPart.setBody(model.toUtf8());
        multipart->append(modelPart);

        QHttpPart filePart;
        filePart.setHeader(QNetworkRequest::ContentTypeHeader, audioMimeType);
        filePart.setHeader(
            QNetworkRequest::ContentDispositionHeader,
            QVariant(QStringLiteral("form-data; name=\"file\"; filename=\"%1\"").arg(QFileInfo(audioPath).fileName())));
        filePart.setBody(audioBytes);
        multipart->append(filePart);

        QHttpPart responseFormatPart;
        responseFormatPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(QStringLiteral("form-data; name=\"response_format\"")));
        responseFormatPart.setBody("verbose_json");
        multipart->append(responseFormatPart);

        QHttpPart granularityPart;
        granularityPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(QStringLiteral("form-data; name=\"timestamp_granularities[]\"")));
        granularityPart.setBody("segment");
        multipart->append(granularityPart);

        if (language.compare(QStringLiteral("auto"), Qt::CaseInsensitive) != 0) {
            QHttpPart languagePart;
            languagePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(QStringLiteral("form-data; name=\"language\"")));
            languagePart.setBody(language.toUtf8());
            multipart->append(languagePart);
        }

        QHttpPart promptPart;
        promptPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant(QStringLiteral("form-data; name=\"prompt\"")));
        promptPart.setBody(promptText.toUtf8());
        multipart->append(promptPart);
    } else if (protocol == SubtitleAsrProtocol::GeminiGenerateContent &&
               geminiRoute != GeminiRoute::OpenAICompatibleBearer) {
        QUrl geminiUrl(normalizeGeminiEndpoint(config.baseUrl, model));
        if (geminiRoute == GeminiRoute::OfficialQuery) {
            QUrlQuery query(geminiUrl);
            query.addQueryItem(QStringLiteral("key"), QString::fromUtf8(config.apiKey.trimmed().toUtf8()));
            geminiUrl.setQuery(query);
        }
        request.setUrl(geminiUrl);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        if (geminiRoute == GeminiRoute::OfficialBearer) {
            request.setRawHeader("Authorization", "Bearer " + config.apiKey.trimmed().toUtf8());
        } else if (geminiRoute == GeminiRoute::OfficialHeader) {
            request.setRawHeader("x-goog-api-key", config.apiKey.trimmed().toUtf8());
        }

        const QJsonObject payload{
            { QStringLiteral("contents"), QJsonArray{
                QJsonObject{
                    { QStringLiteral("parts"), QJsonArray{
                        QJsonObject{ { QStringLiteral("text"), promptText } },
                        QJsonObject{ { QStringLiteral("inline_data"), QJsonObject{
                            { QStringLiteral("mime_type"), geminiAudioMimeType },
                            { QStringLiteral("data"), audioBase64 }
                        } } }
                    } }
                }
            } },
            { QStringLiteral("generation_config"), QJsonObject{
                { QStringLiteral("temperature"), 0.0 },
                { QStringLiteral("max_output_tokens"), 2048 }
            } }
        };
        body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    } else if (protocol == SubtitleAsrProtocol::GeminiGenerateContent) {
        request.setUrl(QUrl(normalizeOpenAIChatEndpoint(config.baseUrl)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Authorization", "Bearer " + config.apiKey.trimmed().toUtf8());

        const QJsonObject payload{
            { QStringLiteral("model"), model },
            { QStringLiteral("temperature"), 0.0 },
            { QStringLiteral("messages"), QJsonArray{
                QJsonObject{
                    { QStringLiteral("role"), QStringLiteral("user") },
                    { QStringLiteral("content"), QJsonArray{
                        QJsonObject{
                            { QStringLiteral("type"), QStringLiteral("input_audio") },
                            { QStringLiteral("input_audio"), QJsonObject{
                                { QStringLiteral("data"), audioBase64 },
                                { QStringLiteral("format"), audioFormat }
                            } }
                        }
                    } }
                }
            } }
        };
        body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    } else if (protocol == SubtitleAsrProtocol::OpenAIResponsesAudio) {
        request.setUrl(QUrl(normalizeOpenAIResponsesEndpoint(config.baseUrl)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Authorization", "Bearer " + config.apiKey.trimmed().toUtf8());

        const QJsonObject payload{
            { QStringLiteral("model"), model },
            { QStringLiteral("input"), QJsonArray{
                QJsonObject{
                    { QStringLiteral("role"), QStringLiteral("user") },
                    { QStringLiteral("content"), QJsonArray{
                        QJsonObject{
                            { QStringLiteral("type"), QStringLiteral("input_text") },
                            { QStringLiteral("text"), responsesAudioPrompt(language, config.targetLanguage) }
                        },
                        QJsonObject{
                            { QStringLiteral("type"), QStringLiteral("input_file") },
                            { QStringLiteral("input_file"), QJsonObject{
                                { QStringLiteral("file_data"), audioBase64 },
                                { QStringLiteral("filename"), QFileInfo(audioPath).fileName() }
                            } }
                        }
                    } }
                }
            } }
        };
        body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    } else {
        request.setUrl(QUrl(normalizeMimoChatEndpoint(config.baseUrl)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        if (mimoAuthMode == MimoAuthMode::ApiKeyHeader) {
            request.setRawHeader("api-key", config.apiKey.trimmed().toUtf8());
        } else {
            request.setRawHeader("Authorization", "Bearer " + config.apiKey.trimmed().toUtf8());
        }

        const QJsonObject payload{
            { QStringLiteral("model"), model },
            { QStringLiteral("messages"), QJsonArray{
                QJsonObject{
                    { QStringLiteral("role"), QStringLiteral("user") },
                    { QStringLiteral("content"), QJsonArray{
                        QJsonObject{
                            { QStringLiteral("type"), QStringLiteral("input_audio") },
                            { QStringLiteral("input_audio"), QJsonObject{
                                { QStringLiteral("data"), audioDataUrl }
                            } }
                        }
                    } }
                }
            } },
            { QStringLiteral("asr_options"), QJsonObject{
                { QStringLiteral("language"), QStringLiteral("auto") }
            } }
        };
        body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    }

    QNetworkAccessManager manager;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QNetworkReply* reply = multipart ? manager.post(request, multipart) : manager.post(request, body);
    if (multipart) {
        multipart->setParent(reply);
    }
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(config.timeoutMs > 0 ? config.timeoutMs : 60000);
    loop.exec();

    if (!timer.isActive()) {
        reply->abort();
        reply->deleteLater();
        result.errorMessage = QStringLiteral("ASR API timed out");
        return result;
    }
    timer.stop();

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBytes = reply->readAll();
    const QString networkError = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
    reply->deleteLater();

    const QJsonDocument document = QJsonDocument::fromJson(responseBytes);
    const QJsonObject object = document.object();
    if (statusCode < 200 || statusCode >= 300 || !networkError.isEmpty()) {
        const QString routeLabel = protocol == SubtitleAsrProtocol::MimoChat
            ? protocolName(protocol) + QLatin1Char('/') + mimoAuthModeName(mimoAuthMode)
            : (protocol == SubtitleAsrProtocol::GeminiGenerateContent
                ? geminiRouteName(geminiRoute)
                : protocolName(protocol));
        QString detailText = serverErrorText(object, responseBytes);
        if (!networkError.isEmpty()) {
            detailText = detailText.isEmpty()
                ? networkError
                : networkError + QStringLiteral(": ") + detailText;
        }
        detailText = redactSensitiveText(detailText, config.apiKey);
        const QString hint = protocol == SubtitleAsrProtocol::GeminiGenerateContent
            ? geminiFailureHint(statusCode, detailText)
            : QString();
        if (!hint.isEmpty()) {
            detailText = QStringLiteral("%1; %2").arg(hint, detailText);
        }
        if (protocol == SubtitleAsrProtocol::QwenDashScopeAsr) {
            const QString qwenHint = qwenEndpointHint(request.url(), detailText);
            if (!qwenHint.isEmpty()) {
                detailText = detailText.isEmpty()
                    ? qwenHint
                    : detailText + QStringLiteral("; ") + qwenHint;
            }
            result.errorMessage = QStringLiteral("ASR API failed HTTP %1 provider=qwen model=%2 endpoint=%3 route=%4: %5")
                .arg(statusCode)
                .arg(model)
                .arg(request.url().toString(QUrl::RemoveQuery))
                .arg(routeLabel)
                .arg(detailText);
        } else {
            result.errorMessage = QStringLiteral("ASR API failed HTTP %1 [%2] at %3: %4")
                .arg(statusCode)
                .arg(routeLabel)
                .arg(request.url().toString(QUrl::RemoveQuery))
                .arg(detailText);
        }
        if (protocol == SubtitleAsrProtocol::MimoChat) {
            result.errorMessage += QStringLiteral(" (%1)").arg(requestSizeSummary(audioBytes, audioBase64, body));
        }
        return result;
    }

    QString text = protocol == SubtitleAsrProtocol::GeminiGenerateContent &&
            geminiRoute == GeminiRoute::OpenAICompatibleBearer
        ? firstChatCompletionText(object).trimmed()
        : textFromResponse(protocol, object).trimmed();
    QString directTranslatedText;
    if (directTranslation) {
        extractDirectTranslationJson(text, &text, &directTranslatedText);
    }
    if (text.isEmpty()) {
        result.errorMessage = QStringLiteral("ASR API returned empty text [%1]").arg(protocolName(protocol));
        return result;
    }

    result.success = true;
    result.backend = QStringLiteral("subtitle_asr:%1").arg(
        protocol == SubtitleAsrProtocol::GeminiGenerateContent
            ? geminiRouteName(geminiRoute)
            : protocolName(protocol));
    result.backendDetail = protocol == SubtitleAsrProtocol::QwenDashScopeAsr
        ? QStringLiteral("ASR protocol / provider=qwen / model=%1 / endpoint=%2")
            .arg(model, request.url().toString(QUrl::RemoveQuery))
        : QStringLiteral("ASR protocol / model=%1 / endpoint=%2")
            .arg(model, request.url().toString(QUrl::RemoveQuery));
    result.text = repairCommonMojibake(text);
    result.translatedText = repairCommonMojibake(directTranslatedText);
    result.detectedLanguage = object.value(QStringLiteral("language")).toString().trimmed();
    if (result.detectedLanguage.isEmpty() && protocol == SubtitleAsrProtocol::QwenDashScopeAsr) {
        result.detectedLanguage = languageFromChatCompletionAnnotations(object);
    }
    result.rawJson = object;
    return result;
}

} // namespace

SubtitleAsrApiResult transcribeSubtitleAudioWithProtocol(
    const SubtitleAsrApiConfig& config,
    const QString& audioPath)
{
    SubtitleAsrApiResult result;
    QElapsedTimer elapsed;
    elapsed.start();
    if (config.apiKey.trimmed().isEmpty()) {
        result.errorMessage = config.protocol == SubtitleAsrProtocol::QwenDashScopeAsr
            ? QStringLiteral("ASR API key is missing provider=qwen")
            : QStringLiteral("ASR API key is missing");
        return result;
    }

    const QVector<MimoAuthMode> mimoAuthModes = { MimoAuthMode::ApiKeyHeader };
    const bool officialGeminiBaseUrl =
        config.protocol == SubtitleAsrProtocol::GeminiGenerateContent &&
        isOfficialGeminiBaseUrl(config.baseUrl);
    const bool likelyOpenAICompatibleGemini =
        config.protocol == SubtitleAsrProtocol::GeminiGenerateContent &&
        !officialGeminiBaseUrl &&
        (config.apiKey.trimmed().startsWith(QStringLiteral("sk-"), Qt::CaseInsensitive) ||
         config.baseUrl.trimmed().contains(QStringLiteral("/v1"), Qt::CaseInsensitive));
    QVector<GeminiRoute> geminiRoutes;
    if (config.protocol == SubtitleAsrProtocol::GeminiGenerateContent) {
        if (officialGeminiBaseUrl) {
            geminiRoutes = { GeminiRoute::OfficialHeader };
        } else if (likelyOpenAICompatibleGemini) {
            geminiRoutes = {
                GeminiRoute::OpenAICompatibleBearer,
                GeminiRoute::OfficialHeader,
                GeminiRoute::OfficialQuery,
                GeminiRoute::OfficialBearer
            };
        } else {
            geminiRoutes = {
                GeminiRoute::OfficialHeader,
                GeminiRoute::OfficialQuery,
                GeminiRoute::OfficialBearer,
                GeminiRoute::OpenAICompatibleBearer
            };
        }
    } else {
        geminiRoutes = { GeminiRoute::OfficialHeader };
    }

    QString lastError;
    const int maxAttempts = config.protocol == SubtitleAsrProtocol::QwenDashScopeAsr ? 3 : 1;
    for (const MimoAuthMode mimoAuthMode : mimoAuthModes) {
        for (const GeminiRoute geminiRoute : geminiRoutes) {
            for (int attemptIndex = 0; attemptIndex < maxAttempts; ++attemptIndex) {
                const ProtocolAttemptResult attempt =
                    transcribeWithProtocolAndMimoAuth(config.protocol, config, audioPath, mimoAuthMode, geminiRoute);
                if (attempt.success) {
                    result.success = true;
                    result.backend = attempt.backend;
                    result.backendDetail = attempt.backendDetail;
                    result.text = attempt.text;
                    result.translatedText = attempt.translatedText;
                    result.detectedLanguage = attempt.detectedLanguage;
                    result.rawJson = attempt.rawJson;
                    result.durationMs = elapsed.elapsed();
                    return result;
                }

                lastError = attempt.errorMessage.trimmed();
                const bool retryable = isRetryableAttemptError(lastError);
                if (!retryable || attemptIndex + 1 >= maxAttempts) {
                    break;
                }
                QThread::msleep(static_cast<unsigned long>(350 + attemptIndex * 900));
            }
            if (config.protocol != SubtitleAsrProtocol::GeminiGenerateContent &&
                !isRetryableAttemptError(lastError)) {
                break;
            }
        }
    }

    result.errorMessage = lastError.isEmpty()
        ? QStringLiteral("ASR API failed")
        : lastError;
    result.durationMs = elapsed.elapsed();
    return result;
}

} // namespace cgplay

