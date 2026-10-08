#include "AIAgentWorkspace.h"
#include "AIAgentWorkspaceSupport.h"

#include "common/core/ServiceLocator.h"
#include "core/playback/api/IPlaybackService.h"
#include "core/playback/api/PlaybackServiceSignals.h"
#include "common/events/api/EventTypes.h"
#include "services/ai/SubtitleAsrApiClient.h"
#include "services/ai/api/IAICredentialStore.h"
#include "settings/api/ISettingsService.h"
#include "services/ai/api/IAIProviderManager.h"
#include "services/ai/api/IAIWorkflowService.h"
#include "features/annotation/api/IAnnotationService.h"
#include "features/annotation/AnnotationToolbar.h"
#include "features/annotation/AnnotationItem.h"
#include "features/annotation/ReviewExport.h"
#include "features/annotation/ReviewPanel.h"
#include "features/playlist/PlaylistPanel.h"
#include "ui/app/NavigationRail.h"
#include "viewer/ViewerWidget.h"
#include "ui/viewer/api/IActivePlaybackView.h"
#include "ui/viewer/CompareToolbar.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QSet>
#include <QSplitter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QPalette>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>

namespace cgplay {

namespace ai_agent_workspace_support {

const char* const kSurface = "#171B20";
const char* const kBorder = "#252B33";
const char* const kText = "#D8DEE7";
const char* const kMuted = "#9AA4B2";
const char* const kAccent = "#FF8A3D";
const char* const kSuccess = "#7BD88F";
const char* const kWarning = "#FFC857";
const char* const kError = "#FF6B6B";

QColor appThemeColor(const char* propertyName, const QColor& fallback)
{
    if (!qApp) return fallback;
    const QColor value(qApp->property(propertyName).toString());
    return value.isValid() ? value : fallback;
}

QColor readableAccentText(const QColor& accent)
{
    const int luminance = (accent.red() * 299 + accent.green() * 587 + accent.blue() * 114) / 1000;
    return luminance > 155 ? QColor(QStringLiteral("#111418")) : QColor(Qt::white);
}
const char* const kOpenAIProviderId = "openai";
const char* const kGenericBaseUrlKey = "ai/connection/baseUrl";
const char* const kGenericModelKey = "ai/connection/model";
const char* const kGenericCredentialId = "ai/defaultApiKey";
const char* const kDetectedProtocolKey = "ai/connection/detectedProtocol";
const char* const kDetectedProviderKey = "ai/connection/detectedProvider";
const char* const kDetectedEndpointKey = "ai/connection/detectedEndpoint";
const char* const kResponsesEndpointKey = "ai/connection/responsesEndpoint";
const char* const kCompatibleProtocolsKey = "ai/connection/compatibleProtocols";
const char* const kConnectionDiagnosticsKey = "ai/connection/diagnostics";
const char* const kProviderTypeKey = "ai/connection/providerType";
const char* const kProviderNameKey = "ai/connection/providerName";
const char* const kAvailableModelsKey = "ai/connection/availableModels";
const char* const kRecommendedModelKey = "ai/connection/recommendedModel";
const char* const kAutoModelSelectionKey = "ai/connection/autoSelectModel";
const char* const kImageEndpointKey = "ai/imageGeneration/endpoint";
const char* const kImageModelKey = "ai/imageGeneration/model";
const char* const kImageCredentialId = "cgplay.ai.image-generation";
const char* const kVideoEndpointKey = "ai/videoGeneration/endpoint";
const char* const kVideoModelKey = "ai/videoGeneration/model";
const char* const kVideoCredentialId = "cgplay.ai.video-generation";
const char* const kVideoStatusEndpointKey = "ai/videoGeneration/statusEndpoint";
const char* const kAudioEndpointKey = "ai/audioGeneration/endpoint";
const char* const kAudioModelKey = "ai/audioGeneration/model";
const char* const kAudioCredentialId = "cgplay.ai.audio-generation";
const char* const kAudioStatusEndpointKey = "ai/audioGeneration/statusEndpoint";
const char* const kImageEditEndpointKey = "ai/imageEditing/endpoint";
const char* const kImageEditModelKey = "ai/imageEditing/model";
const char* const kImageEditCredentialId = "cgplay.ai.image-editing";
const char* const kOpenAIBaseUrlKey = "ai/providers/openai/baseUrl";
const char* const kOpenAIModelKey = "ai/providers/openai/model";
const char* const kOpenAICredentialId = "openai/apiKey";
const char* const kSubtitleAsrBaseUrlKey = "ai/subtitles/asr/baseUrl";
const char* const kLegacySubtitleAsrBaseUrlKey = "ai/subtitles/mimo/baseUrl";
const char* const kSubtitleAsrApiKeyId = "subtitles/asrApiKey";
const char* const kLegacySubtitleAsrApiKeyId = "mimo/apiKey";
const char* const kQwenApiKeyId = "qwen/apiKey";
const char* const kSubtitleAsrProtocolKey = "ai/subtitles/asr/protocol";
const char* const kSubtitleAsrModelKey = "ai/subtitles/transcriptionModel";
const char* const kSubtitleTranslateProviderKey = "ai/subtitles/translation/provider";
const char* const kSubtitleTranslateBaseUrlKey = "ai/subtitles/translation/baseUrl";
const char* const kSubtitleTranslateApiKeyId = "subtitles/translationApiKey";
const char* const kSubtitleTranslateModelKey = "ai/subtitles/translationModel";
const char* const kOnlineSubtitleEnabledKey = "ai/subtitles/online/enabled";
const char* const kOnlineSubtitleProviderKey = "ai/subtitles/online/provider";
const char* const kOnlineSubtitleBaseUrlKey = "ai/subtitles/online/baseUrl";
const char* const kOnlineSubtitleLanguageKey = "ai/subtitles/online/language";
const char* const kOnlineSubtitleApiKeyId = "subtitles/onlineApiKey";
const char* const kQwenSubtitleBaseUrlKey = "ai/subtitles/qwen/baseUrl";
const char* const kQwenSubtitleAsrBaseUrlKey = "ai/subtitles/qwen/asrBaseUrl";
const char* const kQwenDefaultCompatibleBaseUrl = "https://dashscope.aliyuncs.com/compatible-mode/v1";
const char* const kQwenDefaultAsrBaseUrl = "https://dashscope.aliyuncs.com/compatible-mode/v1";
const char* const kQwenDefaultTranslationModel = "qwen-plus";
const char* const kQwenDefaultAsrModel = "qwen3-asr-flash";

QString zh(const char* text)
{
    return QString::fromUtf8(text);
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

QString normalizeSubtitleAsrProtocol(QString value)
{
    const QString lower = value.trimmed().toLower();
    if (lower == QStringLiteral("gemini") || lower == QStringLiteral("gemini_generate_content")) {
        return QStringLiteral("gemini");
    }
    if (lower == QStringLiteral("qwen") || lower == QStringLiteral("qwen3_asr") ||
        lower == QStringLiteral("qwen_dashscope_asr")) {
        return QStringLiteral("qwen");
    }
    if (lower == QStringLiteral("openai") || lower == QStringLiteral("openai_transcriptions") ||
        lower == QStringLiteral("openai_audio_transcriptions")) {
        return QStringLiteral("openai");
    }
    if (lower == QStringLiteral("responses_audio") || lower == QStringLiteral("openai_responses_audio") ||
        lower == QStringLiteral("responses")) {
        return QStringLiteral("responses_audio");
    }
    return QStringLiteral("mimo");
}

bool modelLooksUnsuitableForQwenAsr(const QString& model)
{
    const QString normalized = model.trimmed();
    return normalized.isEmpty() ||
        normalized.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0 ||
        normalized.compare(QStringLiteral("qwen3-asr"), Qt::CaseInsensitive) == 0 ||
        normalized.contains(QStringLiteral("mimo"), Qt::CaseInsensitive) ||
        normalized.contains(QStringLiteral("whisper"), Qt::CaseInsensitive) ||
        normalized.contains(QStringLiteral("gemini"), Qt::CaseInsensitive) ||
        normalized.contains(QStringLiteral("gpt-"), Qt::CaseInsensitive);
}

QString normalizeSubtitleTranslateProvider(QString value)
{
    const QString lower = value.trimmed().toLower();
    if (lower == QStringLiteral("qwen") || lower == QStringLiteral("dashscope")) {
        return QStringLiteral("qwen");
    }
    if (lower == QStringLiteral("mimo")) {
        return QStringLiteral("mimo");
    }
    return QStringLiteral("openai-compatible");
}

QString formatFrameSummary(IPlaybackService* playbackService)
{
    if (!playbackService || !playbackService->isValid()) {
        return zh(u8"当前未打开媒体。可直接提问。");
    }

    const QString mediaPath = playbackService->currentPath();
    const QString mediaName = mediaPath.isEmpty()
        ? zh(u8"未命名媒体")
        : QFileInfo(mediaPath).fileName();
    const int currentFrame = playbackService->currentFrame();
    const int totalFrames = playbackService->totalFrames();
    const double fps = playbackService->fps();

    return zh(u8"媒体：%1\n当前帧：%2 / %3    FPS：%4")
        .arg(mediaName)
        .arg(currentFrame)
        .arg(totalFrames > 0 ? QString::number(totalFrames - 1) : QStringLiteral("--"))
        .arg(fps > 0.0 ? QString::number(fps, 'f', 2) : QStringLiteral("--"));
}

IPlaybackService* runtimePlaybackService(IPlaybackService* fallback)
{
    if (auto* playback = ServiceLocator::getService<IPlaybackService>()) {
        return playback;
    }
    return fallback;
}

QString runtimeActiveViewId()
{
    if (auto* activeView = ServiceLocator::getService<IActivePlaybackView>()) {
        return activeView->activeViewId();
    }
    return QString();
}

IAnnotationService* runtimeAnnotationService(IAnnotationService* fallback)
{
    if (auto* annotationService = ServiceLocator::getService<IAnnotationService>()) {
        return annotationService;
    }
    return fallback;
}

QString providerDisplayText(const AIProviderInfo& info)
{
    if (info.available) {
        return info.providerName;
    }
    return zh(u8"%1（不可用）").arg(info.providerName);
}

QString htmlEscape(const QString& text)
{
    return text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
}

bool isLikelyComparePrompt(const QString& normalizedPrompt)
{
    if (normalizedPrompt.isEmpty()) {
        return false;
    }

    static const QRegularExpression abTokenPattern(
        QStringLiteral("(^|[^a-z0-9])a\\s*/?\\s*b([^a-z0-9]|$)"));
    if (abTokenPattern.match(normalizedPrompt).hasMatch()) {
        return true;
    }

    static const QStringList strongCompareKeywords{
        QStringLiteral("对比"),
        QStringLiteral("比对"),
        QStringLiteral("版本a"),
        QStringLiteral("版本b"),
        QStringLiteral("a版本"),
        QStringLiteral("b版本"),
        QStringLiteral("a版"),
        QStringLiteral("b版"),
        QStringLiteral("两个版本"),
        QStringLiteral("两版"),
        QStringLiteral("before/after"),
        QStringLiteral("before after")
    };

    for (const QString& keyword : strongCompareKeywords) {
        if (normalizedPrompt.contains(keyword)) {
            return true;
        }
    }

    const bool mentionsDualTargets =
        normalizedPrompt.contains(QStringLiteral("两个")) ||
        normalizedPrompt.contains(QStringLiteral("两张")) ||
        normalizedPrompt.contains(QStringLiteral("两帧")) ||
        normalizedPrompt.contains(QStringLiteral("两版")) ||
        normalizedPrompt.contains(QStringLiteral("前后"));
    const bool mentionsDifference =
        normalizedPrompt.contains(QStringLiteral("区别")) ||
        normalizedPrompt.contains(QStringLiteral("差别")) ||
        normalizedPrompt.contains(QStringLiteral("差异")) ||
        normalizedPrompt.contains(QStringLiteral("不同")) ||
        normalizedPrompt.contains(QStringLiteral("哪个好")) ||
        normalizedPrompt.contains(QStringLiteral("哪个更好")) ||
        normalizedPrompt.contains(QStringLiteral("谁更好")) ||
        normalizedPrompt.contains(QStringLiteral("哪版更好"));

    return mentionsDualTargets && mentionsDifference;
}

QString compareModeDisplayText(int mode)
{
    switch (mode) {
    case 0: return zh(u8"A");
    case 1: return zh(u8"B");
    case 2: return zh(u8"Wipe");
    case 3: return zh(u8"Overlay");
    case 4: return zh(u8"Difference");
    case 5: return zh(u8"H");
    case 6: return zh(u8"V");
    case 7: return zh(u8"Tile");
    default: return zh(u8"Unknown");
    }
}

QString annotationToolDisplayText(int tool)
{
    switch (static_cast<AnnotationToolbar::Tool>(tool)) {
    case AnnotationToolbar::Select: return zh(u8"选择");
    case AnnotationToolbar::Arrow: return zh(u8"箭头");
    case AnnotationToolbar::Rectangle: return zh(u8"矩形");
    case AnnotationToolbar::Circle: return zh(u8"圆形");
    case AnnotationToolbar::Text: return zh(u8"文字");
    case AnnotationToolbar::FreeDraw: return zh(u8"自由绘制");
    case AnnotationToolbar::Point: return zh(u8"点标记");
    }
    return zh(u8"未知");
}

int annotationToolFromCode(const QString& code)
{
    const QString normalized = code.trimmed().toLower();
    if (normalized.isEmpty()) {
        return -1;
    }
    if (normalized == QStringLiteral("select") ||
        normalized == QStringLiteral("s") ||
        normalized.contains(QStringLiteral("选择")))
    {
        return AnnotationToolbar::Select;
    }
    if (normalized == QStringLiteral("arrow") ||
        normalized == QStringLiteral("a") ||
        normalized.contains(QStringLiteral("箭头")))
    {
        return AnnotationToolbar::Arrow;
    }
    if (normalized == QStringLiteral("rectangle") ||
        normalized == QStringLiteral("rect") ||
        normalized == QStringLiteral("r") ||
        normalized.contains(QStringLiteral("矩形")) ||
        normalized.contains(QStringLiteral("方框")))
    {
        return AnnotationToolbar::Rectangle;
    }
    if (normalized == QStringLiteral("circle") ||
        normalized == QStringLiteral("c") ||
        normalized.contains(QStringLiteral("圆")))
    {
        return AnnotationToolbar::Circle;
    }
    if (normalized == QStringLiteral("text") ||
        normalized == QStringLiteral("t") ||
        normalized.contains(QStringLiteral("文字")) ||
        normalized.contains(QStringLiteral("文本")))
    {
        return AnnotationToolbar::Text;
    }
    if (normalized == QStringLiteral("freedraw") ||
        normalized == QStringLiteral("free_draw") ||
        normalized == QStringLiteral("draw") ||
        normalized == QStringLiteral("d") ||
        normalized.contains(QStringLiteral("自由")) ||
        normalized.contains(QStringLiteral("绘制")))
    {
        return AnnotationToolbar::FreeDraw;
    }
    if (normalized == QStringLiteral("point") ||
        normalized == QStringLiteral("dot") ||
        normalized == QStringLiteral("p") ||
        normalized.contains(QStringLiteral("点")))
    {
        return AnnotationToolbar::Point;
    }
    return -1;
}

int compareModeFromCode(const QString& code)
{
    const QString normalized = code.trimmed().toLower();
    if (normalized == QStringLiteral("a")) {
        return 0;
    }
    if (normalized == QStringLiteral("b")) {
        return 1;
    }
    if (normalized == QStringLiteral("w") || normalized == QStringLiteral("wipe")) {
        return 2;
    }
    if (normalized == QStringLiteral("n") || normalized == QStringLiteral("overlay")) {
        return 3;
    }
    if (normalized == QStringLiteral("d") || normalized == QStringLiteral("difference")) {
        return 4;
    }
    if (normalized == QStringLiteral("h") || normalized == QStringLiteral("horizontal")) {
        return 5;
    }
    if (normalized == QStringLiteral("v") || normalized == QStringLiteral("vertical")) {
        return 6;
    }
    if (normalized == QStringLiteral("t") || normalized == QStringLiteral("tile")) {
        return 7;
    }
    return -1;
}

QString navigationPageDisplayText(int page)
{
    switch (page) {
    case NavigationRail::Playlist: return zh(u8"播放列表");
    case NavigationRail::Review: return zh(u8"审片");
    case NavigationRail::Compare: return zh(u8"对比");
    case NavigationRail::Versions: return zh(u8"版本");
    case NavigationRail::Timeline: return zh(u8"时间线");
    case NavigationRail::Settings: return zh(u8"设置");
    default: break;
    }
    return zh(u8"未知");
}

int navigationPageFromCode(const QString& code)
{
    const QString normalized = code.trimmed().toLower();
    if (normalized == QStringLiteral("playlist") ||
        normalized == QStringLiteral("left") ||
        normalized.contains(QStringLiteral("播放列表")))
    {
        return NavigationRail::Playlist;
    }
    if (normalized == QStringLiteral("review") ||
        normalized == QStringLiteral("annotation") ||
        normalized.contains(QStringLiteral("审片")) ||
        normalized.contains(QStringLiteral("批注")))
    {
        return NavigationRail::Review;
    }
    if (normalized == QStringLiteral("compare") ||
        normalized.contains(QStringLiteral("对比")))
    {
        return NavigationRail::Compare;
    }
    if (normalized == QStringLiteral("versions") ||
        normalized == QStringLiteral("version") ||
        normalized.contains(QStringLiteral("版本")))
    {
        return NavigationRail::Versions;
    }
    if (normalized == QStringLiteral("timeline") ||
        normalized.contains(QStringLiteral("时间线")))
    {
        return NavigationRail::Timeline;
    }
    if (normalized == QStringLiteral("settings") ||
        normalized == QStringLiteral("setting") ||
        normalized.contains(QStringLiteral("设置")))
    {
        return NavigationRail::Settings;
    }
    return -1;
}

QString reviewTabDisplayText(int index)
{
    switch (index) {
    case 0: return zh(u8"批注");
    case 1: return zh(u8"元数据");
    case 2: return zh(u8"信息");
    case 3: return zh(u8"设置");
    default: break;
    }
    return zh(u8"未知");
}

int reviewTabFromCode(const QString& code)
{
    const QString normalized = code.trimmed().toLower();
    if (normalized == QStringLiteral("review") ||
        normalized == QStringLiteral("annotations") ||
        normalized == QStringLiteral("comments") ||
        normalized.contains(QStringLiteral("批注")) ||
        normalized.contains(QStringLiteral("评论")))
    {
        return 0;
    }
    if (normalized == QStringLiteral("metadata") ||
        normalized.contains(QStringLiteral("元数据")))
    {
        return 1;
    }
    if (normalized == QStringLiteral("info") ||
        normalized.contains(QStringLiteral("信息")))
    {
        return 2;
    }
    if (normalized == QStringLiteral("settings") ||
        normalized.contains(QStringLiteral("设置")))
    {
        return 3;
    }
    return -1;
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

void appendCommonChatModels(QStringList* models)
{
    appendUniqueModel(models, QStringLiteral("deepseek-v4"));
    appendUniqueModel(models, QStringLiteral("gpt-5.5"));
    appendUniqueModel(models, QStringLiteral("gpt-4.1"));
    appendUniqueModel(models, QStringLiteral("gpt-4o"));
}

QString normalizedToken(QString value)
{
    value = value.trimmed().toLower();
    value.replace(QLatin1Char('-'), QLatin1Char('_'));
    value.replace(QLatin1Char(' '), QLatin1Char('_'));
    return value;
}

QString sanitizedActionText(QString text)
{
    text.remove(QLatin1Char('&'));
    return text.simplified();
}

QString compactContextText(QString text, int maxChars)
{
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text = text.simplified();
    if (maxChars > 0 && text.size() > maxChars) {
        text = text.left(maxChars) + QStringLiteral("...");
    }
    return text;
}

QAction* findWindowAction(const QWidget* anchor, const QString& text)
{
    QWidget* topLevel = anchor ? anchor->window() : nullptr;
    if (!topLevel) {
        return nullptr;
    }

    const QString wanted = sanitizedActionText(text);
    const auto actions = topLevel->findChildren<QAction*>();
    for (QAction* action : actions) {
        if (!action) {
            continue;
        }
        if (sanitizedActionText(action->text()) == wanted) {
            return action;
        }
    }
    return nullptr;
}

bool setWindowActionChecked(const QWidget* anchor, const QString& text, bool checked)
{
    if (QAction* action = findWindowAction(anchor, text)) {
        if (!action->isEnabled()) {
            return false;
        }
        if (action->isCheckable()) {
            action->setChecked(checked);
        } else {
            action->trigger();
        }
        return true;
    }
    return false;
}

bool triggerWindowAction(const QWidget* anchor, const QString& text)
{
    if (QAction* action = findWindowAction(anchor, text)) {
        if (!action->isEnabled()) {
            return false;
        }
        action->trigger();
        return true;
    }
    return false;
}

bool uiTargetVisible(const QWidget* anchor, const QString& target, bool* known)
{
    QWidget* topLevel = anchor ? anchor->window() : nullptr;
    const QString normalized = normalizedToken(target);
    if (known) {
        *known = true;
    }
    if (!topLevel) {
        if (known) {
            *known = false;
        }
        return false;
    }

    if (normalized == QStringLiteral("left_panel") ||
        normalized == QStringLiteral("playlist") ||
        normalized == QStringLiteral("playlist_panel") ||
        normalized == QStringLiteral("left"))
    {
        if (auto* playlist = topLevel->findChild<PlaylistPanel*>()) {
            return playlist->isVisible();
        }
    } else if (normalized == QStringLiteral("right_panel") ||
               normalized == QStringLiteral("review_panel") ||
               normalized == QStringLiteral("review"))
    {
        if (auto* panel = topLevel->findChild<ReviewPanel*>(QStringLiteral("ReviewPanel"))) {
            return panel->isVisible();
        }
    } else if (normalized == QStringLiteral("annotation_toolbar") ||
               normalized == QStringLiteral("annotation_tools") ||
               normalized == QStringLiteral("anno_toolbar"))
    {
        if (auto* toolbar = topLevel->findChild<AnnotationToolbar*>(QStringLiteral("AnnotationToolbar"))) {
            return toolbar->isVisible();
        }
    } else if (normalized == QStringLiteral("compare_toolbar") ||
               normalized == QStringLiteral("compare_bar") ||
               normalized == QStringLiteral("compare"))
    {
        if (auto* toolbar = topLevel->findChild<CompareToolbar*>()) {
            return toolbar->isVisible();
        }
    } else if (normalized == QStringLiteral("ai_workspace") ||
               normalized == QStringLiteral("ai_dock") ||
               normalized == QStringLiteral("ai"))
    {
        if (auto* dock = topLevel->findChild<QDockWidget*>(QStringLiteral("AIAgentWorkspaceDock"))) {
            return dock->isVisible();
        }
        if (QAction* action = findWindowAction(anchor, zh(u8"AI 工作台"))) {
            return action->isChecked();
        }
    } else if (normalized == QStringLiteral("translation") ||
               normalized == QStringLiteral("subtitle_translation") ||
               normalized == QStringLiteral("translation_strip"))
    {
        if (QAction* action = findWindowAction(anchor, zh(u8"字幕翻译"))) {
            return action->isChecked();
        }
    } else if (normalized == QStringLiteral("nav") ||
               normalized == QStringLiteral("nav_rail"))
    {
        if (auto* nav = topLevel->findChild<NavigationRail*>()) {
            return nav->isVisible();
        }
    } else {
        if (known) {
            *known = false;
        }
        return false;
    }

    if (known) {
        *known = false;
    }
    return false;
}

bool setUiTargetVisible(const QWidget* anchor, const QString& target, bool visible, QString* appliedLabel)
{
    const QString normalized = normalizedToken(target);
    if (normalized == QStringLiteral("left_panel") ||
        normalized == QStringLiteral("playlist") ||
        normalized == QStringLiteral("playlist_panel") ||
        normalized == QStringLiteral("left"))
    {
        if (appliedLabel) {
            *appliedLabel = zh(u8"左侧面板");
        }
        return setWindowActionChecked(anchor, zh(u8"左侧面板"), visible);
    }
    if (normalized == QStringLiteral("right_panel") ||
        normalized == QStringLiteral("review_panel") ||
        normalized == QStringLiteral("review"))
    {
        if (appliedLabel) {
            *appliedLabel = zh(u8"右侧面板");
        }
        return setWindowActionChecked(anchor, zh(u8"右侧面板"), visible);
    }
    if (normalized == QStringLiteral("annotation_toolbar") ||
        normalized == QStringLiteral("annotation_tools") ||
        normalized == QStringLiteral("anno_toolbar"))
    {
        if (appliedLabel) {
            *appliedLabel = zh(u8"批注工具");
        }
        return setWindowActionChecked(anchor, zh(u8"批注工具"), visible);
    }
    if (normalized == QStringLiteral("compare_toolbar") ||
        normalized == QStringLiteral("compare_bar") ||
        normalized == QStringLiteral("compare"))
    {
        if (appliedLabel) {
            *appliedLabel = zh(u8"对比工具栏");
        }
        return setWindowActionChecked(anchor, zh(u8"对比工具栏"), visible);
    }
    if (normalized == QStringLiteral("ai_workspace") ||
        normalized == QStringLiteral("ai_dock") ||
        normalized == QStringLiteral("ai"))
    {
        if (appliedLabel) {
            *appliedLabel = zh(u8"AI 工作台");
        }
        return setWindowActionChecked(anchor, zh(u8"AI 工作台"), visible);
    }
    if (normalized == QStringLiteral("translation") ||
        normalized == QStringLiteral("subtitle_translation") ||
        normalized == QStringLiteral("translation_strip"))
    {
        if (appliedLabel) {
            *appliedLabel = zh(u8"字幕翻译");
        }
        return setWindowActionChecked(anchor, zh(u8"字幕翻译"), visible);
    }
    return false;
}

bool toggleUiTargetVisible(const QWidget* anchor, const QString& target, QString* appliedLabel)
{
    bool known = false;
    const bool currentVisible = uiTargetVisible(anchor, target, &known);
    if (!known) {
        return false;
    }
    return setUiTargetVisible(anchor, target, !currentVisible, appliedLabel);
}

bool setNavigationPage(const QWidget* anchor, int pageIndex)
{
    QWidget* topLevel = anchor ? anchor->window() : nullptr;
    if (!topLevel) {
        return false;
    }
    auto* nav = topLevel->findChild<NavigationRail*>();
    if (!nav || pageIndex < 0 || pageIndex > NavigationRail::Settings) {
        return false;
    }
    if (QToolButton* button = nav->btn(pageIndex)) {
        button->click();
        return true;
    }
    return false;
}

bool extractDesiredVisible(const QJsonObject& action, bool defaultValue)
{
    if (action.contains(QStringLiteral("visible"))) {
        return action.value(QStringLiteral("visible")).toBool(defaultValue);
    }

    const QString mode = normalizedToken(action.value(QStringLiteral("mode")).toString());
    if (mode == QStringLiteral("show") ||
        mode == QStringLiteral("open") ||
        mode == QStringLiteral("enable") ||
        mode == QStringLiteral("on") ||
        mode == QStringLiteral("visible"))
    {
        return true;
    }
    if (mode == QStringLiteral("hide") ||
        mode == QStringLiteral("close") ||
        mode == QStringLiteral("disable") ||
        mode == QStringLiteral("off") ||
        mode == QStringLiteral("hidden"))
    {
        return false;
    }
    return defaultValue;
}

AISeverity parseSeverityToken(QString token)
{
    token = token.trimmed().toLower();
    if (token.contains(QStringLiteral("critical")) ||
        token.contains(QStringLiteral("severe")) ||
        token.contains(QStringLiteral("严重")) ||
        token.contains(QStringLiteral("致命")))
    {
        return AISeverity::Critical;
    }
    if (token.contains(QStringLiteral("major")) ||
        token.contains(QStringLiteral("high")) ||
        token.contains(QStringLiteral("主要")))
    {
        return AISeverity::Major;
    }
    if (token.contains(QStringLiteral("minor")) ||
        token.contains(QStringLiteral("medium")) ||
        token.contains(QStringLiteral("次要")) ||
        token.contains(QStringLiteral("一般")))
    {
        return AISeverity::Minor;
    }
    if (token.contains(QStringLiteral("info")) ||
        token.contains(QStringLiteral("low")) ||
        token.contains(QStringLiteral("提示")))
    {
        return AISeverity::Info;
    }
    return AISeverity::Unknown;
}

QColor colorForSeverity(AISeverity severity)
{
    switch (severity) {
    case AISeverity::Critical:
        return QColor(QStringLiteral("#FF5A5F"));
    case AISeverity::Major:
        return QColor(QStringLiteral("#FF8A3D"));
    case AISeverity::Minor:
        return QColor(QStringLiteral("#FFC857"));
    case AISeverity::Info:
        return QColor(QStringLiteral("#76C7FF"));
    case AISeverity::Unknown:
    default:
        return QColor(QStringLiteral("#FFAA00"));
    }
}

AnnotationType parseAnnotationTypeToken(QString token)
{
    token = token.trimmed().toLower();
    if (token.contains(QStringLiteral("arrow")) || token.contains(QStringLiteral("箭头"))) {
        return AnnotationType::Arrow;
    }
    if (token.contains(QStringLiteral("rect")) ||
        token.contains(QStringLiteral("rectangle")) ||
        token.contains(QStringLiteral("box")) ||
        token.contains(QStringLiteral("矩形")) ||
        token.contains(QStringLiteral("框")))
    {
        return AnnotationType::Rectangle;
    }
    if (token.contains(QStringLiteral("circle")) ||
        token.contains(QStringLiteral("ellipse")) ||
        token.contains(QStringLiteral("圆")))
    {
        return AnnotationType::Circle;
    }
    if (token.contains(QStringLiteral("text")) ||
        token.contains(QStringLiteral("label")) ||
        token.contains(QStringLiteral("文字")) ||
        token.contains(QStringLiteral("文本")))
    {
        return AnnotationType::Text;
    }
    if (token.contains(QStringLiteral("free")) ||
        token.contains(QStringLiteral("brush")) ||
        token.contains(QStringLiteral("draw")) ||
        token.contains(QStringLiteral("涂")) ||
        token.contains(QStringLiteral("画")))
    {
        return AnnotationType::FreeDraw;
    }
    if (token.contains(QStringLiteral("point")) ||
        token.contains(QStringLiteral("dot")) ||
        token.contains(QStringLiteral("点")))
    {
        return AnnotationType::Point;
    }
    return AnnotationType::Rectangle;
}

bool isDirectMarkupPrompt(const QString& normalizedPrompt)
{
    if (normalizedPrompt.isEmpty()) {
        return false;
    }

    static const QStringList directMarkupKeywords{
        QStringLiteral("视窗"),
        QStringLiteral("画面里"),
        QStringLiteral("画面上"),
        QStringLiteral("标出来"),
        QStringLiteral("标出"),
        QStringLiteral("圈出来"),
        QStringLiteral("圈出"),
        QStringLiteral("框出来"),
        QStringLiteral("框出"),
        QStringLiteral("画出来"),
        QStringLiteral("画出"),
        QStringLiteral("标记出来"),
        QStringLiteral("标注出来"),
        QStringLiteral("圈一下"),
        QStringLiteral("框一下"),
        QStringLiteral("标一下"),
        QStringLiteral("画不同的地方"),
        QStringLiteral("把不同的地方画"),
        QStringLiteral("把差异画"),
        QStringLiteral("把差别画"),
        QStringLiteral("标出差异"),
        QStringLiteral("圈出差异"),
        QStringLiteral("框出差异"),
        QStringLiteral("标出不同"),
        QStringLiteral("圈出不同"),
        QStringLiteral("框出不同")
    };

    for (const QString& keyword : directMarkupKeywords) {
        if (normalizedPrompt.contains(keyword)) {
            return true;
        }
    }

    const bool hasMarkupVerb =
        normalizedPrompt.contains(QStringLiteral("画")) ||
        normalizedPrompt.contains(QStringLiteral("圈")) ||
        normalizedPrompt.contains(QStringLiteral("框")) ||
        normalizedPrompt.contains(QStringLiteral("标"));
    const bool hasTargetArea =
        normalizedPrompt.contains(QStringLiteral("地方")) ||
        normalizedPrompt.contains(QStringLiteral("位置")) ||
        normalizedPrompt.contains(QStringLiteral("区域")) ||
        normalizedPrompt.contains(QStringLiteral("范围"));
    const bool hasDifferenceConcept =
        normalizedPrompt.contains(QStringLiteral("不同")) ||
        normalizedPrompt.contains(QStringLiteral("差异")) ||
        normalizedPrompt.contains(QStringLiteral("差别")) ||
        normalizedPrompt.contains(QStringLiteral("变化"));

    return hasMarkupVerb && (hasTargetArea || hasDifferenceConcept);
}

ViewerWidget* runtimeActiveViewer()
{
    if (auto* overlayHost = ServiceLocator::getService<IOverlayHost>()) {
        if (auto* viewer = dynamic_cast<ViewerWidget*>(overlayHost)) {
            return viewer;
        }
    }
    return nullptr;
}

QSize runtimeActiveMediaSize()
{
    if (ViewerWidget* viewer = runtimeActiveViewer()) {
        if (viewer->mediaW() > 0 && viewer->mediaH() > 0) {
            return QSize(viewer->mediaW(), viewer->mediaH());
        }
    }
    return QSize(1920, 1080);
}

double scaleAnnotationAxis(double rawValue, const QString& coordSpace, int extent)
{
    if (extent <= 0) {
        return 0.0;
    }

    const QString normalizedSpace = normalizedToken(coordSpace);
    if (normalizedSpace == QStringLiteral("normalized1000") ||
        normalizedSpace == QStringLiteral("norm1000") ||
        normalizedSpace == QStringLiteral("canvas1000"))
    {
        rawValue = std::clamp(rawValue, 0.0, 1000.0);
        return (rawValue / 1000.0) * static_cast<double>(extent);
    }

    if (normalizedSpace == QStringLiteral("normalized01") ||
        normalizedSpace == QStringLiteral("normalized") ||
        normalizedSpace == QStringLiteral("norm01") ||
        normalizedSpace == QStringLiteral("relative"))
    {
        rawValue = std::clamp(rawValue, 0.0, 1.0);
        return rawValue * static_cast<double>(extent);
    }

    if (normalizedSpace == QStringLiteral("percent")) {
        rawValue = std::clamp(rawValue, 0.0, 100.0);
        return (rawValue / 100.0) * static_cast<double>(extent);
    }

    if (rawValue >= 0.0 && rawValue <= 1.0) {
        return rawValue * static_cast<double>(extent);
    }
    if (rawValue >= 0.0 && rawValue <= 1000.0) {
        return (rawValue / 1000.0) * static_cast<double>(extent);
    }
    return std::clamp(rawValue, 0.0, static_cast<double>(extent));
}

QPointF scaleAnnotationPoint(double rawX, double rawY, const QString& coordSpace, const QSize& mediaSize)
{
    return QPointF(
        scaleAnnotationAxis(rawX, coordSpace, mediaSize.width()),
        scaleAnnotationAxis(rawY, coordSpace, mediaSize.height()));
}

QVector<QPointF> buildAnnotationPoints(
    const QJsonObject& action,
    AnnotationType type,
    const QSize& mediaSize)
{
    QVector<QPointF> points;
    const QString coordSpace =
        action.value(QStringLiteral("coordSpace")).toString(
            action.value(QStringLiteral("space")).toString(QStringLiteral("normalized1000")));

    const auto appendScaledPoint = [&](double x, double y) {
        points.push_back(scaleAnnotationPoint(x, y, coordSpace, mediaSize));
    };

    const QJsonArray pointArray = action.value(QStringLiteral("points")).toArray();
    for (const QJsonValue& pointValue : pointArray) {
        if (pointValue.isObject()) {
            const QJsonObject pointObject = pointValue.toObject();
            appendScaledPoint(
                pointObject.value(QStringLiteral("x")).toDouble(),
                pointObject.value(QStringLiteral("y")).toDouble());
        } else if (pointValue.isArray()) {
            const QJsonArray xy = pointValue.toArray();
            if (xy.size() >= 2) {
                appendScaledPoint(xy.at(0).toDouble(), xy.at(1).toDouble());
            }
        }
    }

    if (points.isEmpty()) {
        const bool hasRect =
            action.contains(QStringLiteral("x1")) &&
            action.contains(QStringLiteral("y1")) &&
            action.contains(QStringLiteral("x2")) &&
            action.contains(QStringLiteral("y2"));
        if (hasRect) {
            const double x1 = action.value(QStringLiteral("x1")).toDouble();
            const double y1 = action.value(QStringLiteral("y1")).toDouble();
            const double x2 = action.value(QStringLiteral("x2")).toDouble();
            const double y2 = action.value(QStringLiteral("y2")).toDouble();
            if (type == AnnotationType::Circle) {
                const double cx = (x1 + x2) * 0.5;
                const double cy = (y1 + y2) * 0.5;
                const double radius = std::max(std::abs(x2 - x1), std::abs(y2 - y1)) * 0.5;
                appendScaledPoint(cx, cy);
                appendScaledPoint(cx + radius, cy);
            } else {
                appendScaledPoint(x1, y1);
                appendScaledPoint(x2, y2);
            }
        }
    }

    if (points.isEmpty() && action.contains(QStringLiteral("x")) && action.contains(QStringLiteral("y"))) {
        appendScaledPoint(
            action.value(QStringLiteral("x")).toDouble(),
            action.value(QStringLiteral("y")).toDouble());
    }

    if (points.isEmpty()) {
        const double centerX = static_cast<double>(mediaSize.width()) * 0.5;
        const double centerY = static_cast<double>(mediaSize.height()) * 0.5;
        points.push_back(QPointF(centerX, centerY));
    }

    if ((type == AnnotationType::Arrow ||
         type == AnnotationType::Rectangle ||
         type == AnnotationType::FreeDraw) &&
        points.size() < 2)
    {
        const QPointF origin = points.front();
        points.push_back(QPointF(
            std::min(origin.x() + mediaSize.width() * 0.12, static_cast<double>(mediaSize.width())),
            std::min(origin.y() + mediaSize.height() * 0.12, static_cast<double>(mediaSize.height()))));
    } else if (type == AnnotationType::Circle && points.size() < 2) {
        const QPointF center = points.front();
        points.push_back(QPointF(
            std::min(center.x() + mediaSize.width() * 0.06, static_cast<double>(mediaSize.width())),
            center.y()));
    }

    return points;
}

QColor annotationColorFromAction(const QJsonObject& action, const QColor& fallback)
{
    const QString colorText = action.value(QStringLiteral("color")).toString().trimmed();
    if (colorText.isEmpty()) {
        return fallback;
    }
    QColor color(colorText);
    return color.isValid() ? color : fallback;
}

void scrollTextBrowserToBottom(QTextBrowser* view)
{
    if (!view) {
        return;
    }

    const auto scrollNow = [view]() {
        if (!view) {
            return;
        }
        QTextCursor cursor = view->textCursor();
        cursor.movePosition(QTextCursor::End);
        view->setTextCursor(cursor);
        view->ensureCursorVisible();
        if (QScrollBar* scrollBar = view->verticalScrollBar()) {
            scrollBar->setValue(scrollBar->maximum());
        }
    };

    scrollNow();
    QMetaObject::invokeMethod(view, scrollNow, Qt::QueuedConnection);
}

} // namespace ai_agent_workspace_support

} // namespace cgplay
