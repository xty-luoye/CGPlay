#pragma once

#include "annotation/AnnotationItem.h"

#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QJsonObject>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace cgplay {

enum class AIRequestScope
{
    CurrentFrame,
    CurrentShot,
    CurrentSequence,
    CurrentReview,
    CompareAB,
    Custom
};

enum class AIWorkflowKind
{
    AnalyzeFrame,
    AnalyzeShot,
    AnalyzeSequence,
    AnalyzeReview,
    CompareShots,
    GenerateSummary,
    QCScan,
    SmartSearch
};

enum class AIChatRole
{
    System,
    User,
    Assistant
};

enum class AISeverity
{
    Unknown,
    Critical,
    Major,
    Minor,
    Info
};

struct AIChatMessage
{
    AIChatRole role = AIChatRole::User;
    QString content;
    QString model;
    qint64 timestamp = 0;
};

struct AIQCCheckItem
{
    QString id;
    QString name;
    QString description;
    QString systemPrompt;
    bool enabled = true;
    AISeverity severity = AISeverity::Major;
};

struct AISearchResult
{
    int frame = 0;
    QString source;
    QString title;
    QString snippet;
    QString annotationId;
    AISeverity severity = AISeverity::Unknown;
};

enum class AIJobState
{
    Unknown,
    Pending,
    Running,
    Succeeded,
    Failed,
    Canceled
};

struct AIProviderCapabilities
{
    bool supportsText = true;
    bool supportsImages = false;
    bool supportsAudioTranscription = false;
    bool supportsStructuredOutput = false;
    bool supportsLocalRuntime = false;
    bool supportsStreaming = false;
    bool requiresApiKey = true;
    QStringList supportedModels;
};

struct AIProviderInfo
{
    QString providerId;
    QString providerName;
    bool available = false;
    AIProviderCapabilities capabilities;
};

enum class AIProviderCategory
{
    OpenAICompatible,
    Qwen,
    Gemini,
    Claude,
    Ollama,
    Unknown
};

struct AIDetectionRequest
{
    QString apiKey;
    QString baseUrl;
    QString model;
};

struct AIDetectionResult
{
    bool success = false;
    AIProviderCategory category = AIProviderCategory::Unknown;
    QString providerType;
    QString providerName;
    QString baseUrl;
    QString detectedProtocol;
    QString detectedEndpoint;
    QString responsesEndpoint;
    QStringList compatibleProtocols;
    QStringList models;
    QString recommendedModel;
    QStringList diagnostics;
    QString error;
};

struct AIAnnotationSuggestion
{
    int frame = 0;
    QString title;
    QString comment;
    QString category;
    AISeverity severity = AISeverity::Unknown;
    AnnotationType annotationType = AnnotationType::Rectangle;
    QColor color = QColor(255, 170, 0);
    QVector<QPointF> points;
    QJsonObject metadata;
};

struct AIAnalysisFinding
{
    int frame = 0;
    QString category;
    AISeverity severity = AISeverity::Unknown;
    QString title;
    QString description;
    AIAnnotationSuggestion suggestedAnnotation;
    QJsonObject metadata;
};

struct AIReviewSummary
{
    int criticalCount = 0;
    int majorCount = 0;
    int minorCount = 0;
    QString plainText;
    QString markdown;
    QString html;
    QVector<AIAnalysisFinding> findings;
    QJsonObject metadata;
};

struct AIMediaFrameReference
{
    QString mediaPath;
    int frame = 0;
    QByteArray encodedBytes;
    QString mimeType;
    QSize size;
    QJsonObject metadata;
};

struct AIRequestContext
{
    AIRequestScope scope = AIRequestScope::CurrentFrame;
    QString sessionPath;
    QString mediaPath;
    QString mediaDisplayName;
    QString mediaFormat;
    QString activeViewId;
    int currentFrame = 0;
    int totalFrames = 0;
    int startFrame = -1;
    int endFrame = -1;
    double fps = 0.0;
    int mediaWidth = 0;
    int mediaHeight = 0;
    QVector<AnnotationItem> annotations;
    QString selectedAnnotationId;
    QJsonObject metadata;
};

struct AIFrameCaptureRequest
{
    AIRequestScope scope = AIRequestScope::CurrentFrame;
    QString mediaPath;
    int currentFrame = 0;
    int startFrame = -1;
    int endFrame = -1;
    int sampleCount = 1;
    QSize targetSize;
    QString encoding = QStringLiteral("png");
    QJsonObject options;
};

struct AIRequest
{
    QString jobId;
    QString providerId;
    QString model;
    QString systemPrompt;
    QString userPrompt;
    AIRequestContext context;
    QVector<AIMediaFrameReference> frames;
    QVector<AIChatMessage> chatHistory;
    QJsonObject options;
};

struct AIAudioTranscriptionRequest
{
    QString providerId;
    QString model;
    QString audioFilePath;
    QString mimeType;
    QString prompt;
    QString languageHint;
    QJsonObject options;
};

struct AIAudioTranscriptionResult
{
    QString providerId;
    QString model;
    bool success = false;
    QString errorMessage;
    QString text;
    QString detectedLanguage;
    QJsonObject rawJson;
    QDateTime startedAt;
    QDateTime finishedAt;
    qint64 durationMs = 0;
};

struct AIResponse
{
    QString jobId;
    QString providerId;
    QString model;
    bool success = false;
    QString errorMessage;
    QString rawText;
    QJsonObject rawJson;
    QVector<AIAnalysisFinding> findings;
    QVector<AIAnnotationSuggestion> annotationSuggestions;
    AIReviewSummary reviewSummary;
    QDateTime startedAt;
    QDateTime finishedAt;
    qint64 durationMs = 0;
    int promptTokens = -1;
    int completionTokens = -1;
    int totalTokens = -1;
    int cachedPromptTokens = -1;
    double promptCacheHitRate = -1.0;
    int frameCount = 0;          // number of frames actually captured and sent
    QString frameCaptureError;   // non-empty if frame capture failed
};

struct AIJobSnapshot
{
    QString jobId;
    QString providerId;
    AIJobState state = AIJobState::Unknown;
    QString errorMessage;
    QDateTime submittedAt;
    QDateTime startedAt;
    QDateTime finishedAt;
    AIRequest request;
    AIResponse response;
};

struct AIWorkflowRequest
{
    AIWorkflowKind workflow = AIWorkflowKind::AnalyzeFrame;
    AIRequestScope scope = AIRequestScope::CurrentFrame;
    QString providerId;
    QString model;
    QString systemPrompt;
    QString userPrompt;
    bool attachFrames = true;
    bool includeAnnotations = true;
    bool requireFrames = false;
    int sampleCount = 1;
    QSize targetSize;
    QVector<AIChatMessage> chatHistory;
    QJsonObject options;
};

} // namespace cgplay
