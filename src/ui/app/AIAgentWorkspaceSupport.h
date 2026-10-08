#pragma once

#include "features/annotation/AnnotationItem.h"
#include "services/ai/api/AIProviderTypes.h"

#include <QColor>
#include <QJsonObject>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

QT_BEGIN_NAMESPACE
class QAction;
class QTextBrowser;
class QWidget;
QT_END_NAMESPACE

namespace cgplay {

class IAnnotationService;
class IPlaybackService;

namespace ai_agent_workspace_support {

QColor appThemeColor(const char* propertyName, const QColor& fallback);
QColor readableAccentText(const QColor& accent);

extern const char* const kSurface;
extern const char* const kBorder;
extern const char* const kText;
extern const char* const kMuted;
extern const char* const kAccent;
extern const char* const kSuccess;
extern const char* const kWarning;
extern const char* const kError;
extern const char* const kOpenAIProviderId;
extern const char* const kGenericBaseUrlKey;
extern const char* const kGenericModelKey;
extern const char* const kGenericCredentialId;
extern const char* const kDetectedProtocolKey;
extern const char* const kDetectedProviderKey;
extern const char* const kDetectedEndpointKey;
extern const char* const kResponsesEndpointKey;
extern const char* const kCompatibleProtocolsKey;
extern const char* const kConnectionDiagnosticsKey;
extern const char* const kProviderTypeKey;
extern const char* const kProviderNameKey;
extern const char* const kAvailableModelsKey;
extern const char* const kRecommendedModelKey;
extern const char* const kAutoModelSelectionKey;
extern const char* const kImageEndpointKey;
extern const char* const kImageModelKey;
extern const char* const kImageCredentialId;
extern const char* const kVideoEndpointKey;
extern const char* const kVideoModelKey;
extern const char* const kVideoCredentialId;
extern const char* const kVideoStatusEndpointKey;
extern const char* const kAudioEndpointKey;
extern const char* const kAudioModelKey;
extern const char* const kAudioCredentialId;
extern const char* const kAudioStatusEndpointKey;
extern const char* const kImageEditEndpointKey;
extern const char* const kImageEditModelKey;
extern const char* const kImageEditCredentialId;
extern const char* const kOpenAIBaseUrlKey;
extern const char* const kOpenAIModelKey;
extern const char* const kOpenAICredentialId;
extern const char* const kSubtitleAsrBaseUrlKey;
extern const char* const kLegacySubtitleAsrBaseUrlKey;
extern const char* const kSubtitleAsrApiKeyId;
extern const char* const kLegacySubtitleAsrApiKeyId;
extern const char* const kQwenApiKeyId;
extern const char* const kSubtitleAsrProtocolKey;
extern const char* const kSubtitleAsrModelKey;
extern const char* const kSubtitleTranslateProviderKey;
extern const char* const kSubtitleTranslateBaseUrlKey;
extern const char* const kSubtitleTranslateApiKeyId;
extern const char* const kSubtitleTranslateModelKey;
extern const char* const kOnlineSubtitleEnabledKey;
extern const char* const kOnlineSubtitleProviderKey;
extern const char* const kOnlineSubtitleBaseUrlKey;
extern const char* const kOnlineSubtitleLanguageKey;
extern const char* const kOnlineSubtitleApiKeyId;
extern const char* const kQwenSubtitleBaseUrlKey;
extern const char* const kQwenSubtitleAsrBaseUrlKey;
extern const char* const kQwenDefaultCompatibleBaseUrl;
extern const char* const kQwenDefaultAsrBaseUrl;
extern const char* const kQwenDefaultTranslationModel;
extern const char* const kQwenDefaultAsrModel;

QString zh(const char* text);
bool hasSensitiveCredentialQuery(const QString& value);
QString normalizeSubtitleAsrProtocol(QString value);
bool modelLooksUnsuitableForQwenAsr(const QString& model);
QString normalizeSubtitleTranslateProvider(QString value);
QString formatFrameSummary(IPlaybackService* playbackService);
IPlaybackService* runtimePlaybackService(IPlaybackService* fallback);
QString runtimeActiveViewId();
IAnnotationService* runtimeAnnotationService(IAnnotationService* fallback);
QString providerDisplayText(const AIProviderInfo& info);
QString htmlEscape(const QString& text);
bool isLikelyComparePrompt(const QString& normalizedPrompt);
QString compareModeDisplayText(int mode);
QString annotationToolDisplayText(int tool);
int annotationToolFromCode(const QString& code);
int compareModeFromCode(const QString& code);
QString navigationPageDisplayText(int page);
int navigationPageFromCode(const QString& code);
QString reviewTabDisplayText(int index);
int reviewTabFromCode(const QString& code);
void appendUniqueModel(QStringList* models, const QString& model);
void appendCommonChatModels(QStringList* models);
QString normalizedToken(QString value);
QString compactContextText(QString text, int maxChars);
QAction* findWindowAction(const QWidget* anchor, const QString& text);
bool triggerWindowAction(const QWidget* anchor, const QString& text);
bool uiTargetVisible(const QWidget* anchor, const QString& target, bool* known = nullptr);
bool setUiTargetVisible(
    const QWidget* anchor,
    const QString& target,
    bool visible,
    QString* appliedLabel = nullptr);
bool toggleUiTargetVisible(
    const QWidget* anchor,
    const QString& target,
    QString* appliedLabel = nullptr);
bool setNavigationPage(const QWidget* anchor, int pageIndex);
bool extractDesiredVisible(const QJsonObject& action, bool defaultValue = true);
AISeverity parseSeverityToken(QString token);
QColor colorForSeverity(AISeverity severity);
AnnotationType parseAnnotationTypeToken(QString token);
bool isDirectMarkupPrompt(const QString& normalizedPrompt);
QSize runtimeActiveMediaSize();
QVector<QPointF> buildAnnotationPoints(
    const QJsonObject& action,
    AnnotationType type,
    const QSize& mediaSize);
QColor annotationColorFromAction(const QJsonObject& action, const QColor& fallback);
void scrollTextBrowserToBottom(QTextBrowser* view);

} // namespace ai_agent_workspace_support
} // namespace cgplay
