#pragma once

#include <QJsonObject>
#include <QString>

namespace cgplay {

enum class SubtitleAsrProtocol {
    MimoChat,
    QwenDashScopeAsr,
    GeminiGenerateContent,
    OpenAITranscriptions,
    OpenAIResponsesAudio
};

struct SubtitleAsrApiConfig {
    SubtitleAsrProtocol protocol = SubtitleAsrProtocol::OpenAIResponsesAudio;
    QString baseUrl;
    QString apiKey;
    QString model;
    QString sourceLanguageHint;
    QString targetLanguage;
    bool requestTranslation = false;
    int timeoutMs = 60000;
};

struct SubtitleAsrApiResult {
    bool success = false;
    QString errorMessage;
    QString backend;
    QString backendDetail;
    QString text;
    QString translatedText;
    QString detectedLanguage;
    QJsonObject rawJson;
    qint64 durationMs = 0;
};

SubtitleAsrApiResult transcribeSubtitleAudioWithProtocol(
    const SubtitleAsrApiConfig& config,
    const QString& audioPath);

} // namespace cgplay
