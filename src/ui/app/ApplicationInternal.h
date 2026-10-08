#pragma once
#include "Application.h"
#include "ApplicationPrivate.h"
#include "PerformanceService.h"
#include "AIAgentWorkspace.h"
#include "AIConnectionWizardWidget.h"
#include "NavigationRail.h"
#include "PlaybackBar.h"
#include "TopBar.h"
#include "UpdateService.h"
#include "SecondaryWindow.h"

#include "common/core/ServiceLocator.h"
#include "plugins/CommandRegistry.h"
#include "ui/app/CommandDispatcher.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"
#include "services/ai/api/IAICredentialStore.h"
#include "services/ai/api/IAIProviderManager.h"
#include "services/ai/api/IAIWorkflowService.h"
#include "services/ai/api/IMediaFrameSnapshotService.h"
#include "services/ai/SubtitleGenerationService.h"
#include "services/ai/TranslationEnhancementScheduler.h"
#include "services/ai/TranslationGuardSkills.h"
#include "services/ai/TranslationPlaybackStrategy.h"
#include "viewer/ViewerWidget.h"
#include "viewer/TlViewport.h"
#include "viewer/CompareToolbar.h"
#include "annotation/AnnotationManager.h"
#include "annotation/AnnotationToolbar.h"
#include "annotation/ReviewPanel.h"
#include "annotation/api/IAnnotationService.h"
#include "annotation/api/IAnnotationViewBridge.h"
#include "annotation/AnnotationStorage.h"
#include "annotation/ReviewExport.h"
#include "playback/PlaybackController.h"
#include "playback/api/IPlaybackService.h"
#include "playback/api/PlaybackServiceSignals.h"
#include "playback/PlaybackStats.h"
#include "session/SessionManager.h"
#include "session/api/ISessionService.h"
#include "session/api/ISessionContributor.h"
#include "hwdecode/HardwareDecodeManager.h"
#include "timeline/TimelineWidget.h"
#include "playlist/PlaylistPanel.h"
#include "otio/OtioImporter.h"
#include "otio/OtioExporter.h"
#include "playlist/PlaylistModel.h"
#include "media/MediaProbe.h"
#include "media/api/IMediaService.h"
#include "ocio/OcioManager.h"
#include "cache/CacheManager.h"
#include "cache/ReadAheadCache.h"
#include "component/ComponentManager.h"
#include "settings/api/ISettingsService.h"
#include "services/settings/SettingsProfileService.h"
#include "services/settings/controllers/AppearanceController.h"
#include "services/settings/controllers/WorkspaceController.h"
#include "services/settings/controllers/ToolbarController.h"
#include "common/input/InputBindingStore.h"
#include "common/theme/BackdropRenderer.h"
#include "common/theme/ThemeService.h"
#include "plugins/PluginManager.h"
#include "plugins/api/HostExtensionPoints.h"

#include <tlRender/Timeline/CompareOptions.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <utility>

#include <QCloseEvent>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QComboBox>
#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMouseEvent>
#include <QSettings>
#include <QSet>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStandardPaths>
#include <QLabel>
#include <QApplication>
#include <QAbstractButton>
#include <QTimer>
#include <QToolButton>
#include <QAction>
#include <QTextStream>
#include <QListView>
#include <QInputDialog>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QPushButton>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QThread>
#include <QVector>
#include <QPixmap>
#include <QImage>
#include <QtConcurrent>
#include <QGraphicsDropShadowEffect>
#include <QPainter>
#include <QDebug>
#include <QEvent>
#include <QPointer>
#include <QScreen>

#ifdef Q_OS_WIN
#include <Xinput.h>
#pragma comment(lib, "Xinput9_1_0.lib")
#endif
#include <QScrollArea>
#include <QScrollBar>
#include <QSaveFile>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QWheelEvent>
#include <QSpinBox>
#include <QDir>
#include <QDesktopServices>
#include <QDockWidget>
#include <QStandardPaths>
#include <QUrl>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalBlocker>
#include <QPlainTextEdit>
#include <QTextBrowser>
#include <QTextEdit>

#include <atomic>
#include <initializer_list>
#include <memory>
#include <cmath>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "core/PlaybackLogger.h"
#include "core/CrashHandler.h"

namespace cgplay {
inline constexpr auto kWindow = "#111418";
inline constexpr auto kPanel = "#171B20";
inline constexpr auto kViewerBg = "#050505";
inline constexpr auto kBorder = "#252B33";
inline constexpr auto kText = "#D8DEE7";
inline constexpr auto kSec = "#9AA4B2";
inline constexpr auto kAccent = "#FF8A3D";
inline constexpr auto kBorderRgba = "rgba(255,255,255,0.06)";
class LiveSlider final : public QSlider
{
public:
    using QSlider::QSlider;

    void setSmoothRange(int minimum, int maximum)
    {
        QSlider::setRange(minimum * kSmoothScale, maximum * kSmoothScale);
    }

    void setSmoothValue(int value)
    {
        QSlider::setValue(value * kSmoothScale);
    }

    int logicalValue() const
    {
        return qRound(QSlider::value() / static_cast<double>(kSmoothScale));
    }

    static int logicalValue(int rawValue)
    {
        return qRound(rawValue / static_cast<double>(kSmoothScale));
    }

protected:
    bool event(QEvent* event) override
    {
        if (event && _dragging &&
            (event->type() == QEvent::UngrabMouse ||
             event->type() == QEvent::Hide ||
             event->type() == QEvent::WindowDeactivate)) {
            finishDrag();
        }
        return QSlider::event(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event && event->button() == Qt::LeftButton) {
            _dragging = true;
            grabMouse();
            setSliderDown(true);
            setValue(valueFromPoint(event->position().toPoint()));
            update();
            event->accept();
            return;
        }
        QSlider::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (event && _dragging) {
            setValue(valueFromPoint(event->position().toPoint()));
            update();
            event->accept();
            return;
        }
        QSlider::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event && event->button() == Qt::LeftButton && _dragging) {
            setValue(valueFromPoint(event->position().toPoint()));
            finishDrag();
            event->accept();
            return;
        }
        QSlider::mouseReleaseEvent(event);
    }

    void wheelEvent(QWheelEvent* event) override
    {
        if (!event) return;
        QWidget* ancestor = parentWidget();
        while (ancestor && !qobject_cast<QScrollArea*>(ancestor)) {
            ancestor = ancestor->parentWidget();
        }
        if (auto* scroll = qobject_cast<QScrollArea*>(ancestor)) {
            const int pixelDelta = event->pixelDelta().y();
            const int angleDelta = event->angleDelta().y();
            int delta = pixelDelta != 0 ? pixelDelta : qRound(angleDelta * 0.5);
            if (delta == 0 && angleDelta != 0) delta = angleDelta > 0 ? 1 : -1;
            if (event->inverted()) delta = -delta;
            if (delta != 0 && scroll->verticalScrollBar()) {
                auto* bar = scroll->verticalScrollBar();
                bar->setValue(bar->value() - delta);
                event->accept();
                return;
            }
        }
        event->ignore();
    }

private:
    void finishDrag()
    {
        if (!_dragging) return;
        _dragging = false;
        if (mouseGrabber() == this) releaseMouse();
        setSliderDown(false);
        update();
    }

    static constexpr int kSmoothScale = 10;
    bool _dragging = false;

    int valueFromPoint(const QPoint& point) const
    {
        QStyleOptionSlider option;
        initStyleOption(&option);
        const QRect groove = style()->subControlRect(
            QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
        const QRect handle = style()->subControlRect(
            QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        const bool horizontal = orientation() == Qt::Horizontal;
        const int handleLength = horizontal ? handle.width() : handle.height();
        const int start = (horizontal ? groove.x() : groove.y()) - handleLength / 2;
        const int end = (horizontal ? groove.right() : groove.bottom()) + handleLength / 2;
        const int position = (horizontal ? point.x() : point.y()) - start;
        return QStyle::sliderValueFromPosition(
            minimum(), maximum(), position, qMax(1, end - start), option.upsideDown);
    }
};
constexpr double kInitialSubtitleWindowSeconds = 24.0;
constexpr double kBackgroundSubtitleWindowSeconds = 180.0;
constexpr double kSubtitlePrefetchLeadSeconds = 210.0;
constexpr double kSubtitleContinuationOverlapSeconds = 18.0;
constexpr double kInitialSubtitlePrerollSeconds = 6.0;
constexpr double kGeneratedSubtitleHoldSeconds = 2.5;
constexpr double kGeneratedSubtitleNearNextCueSeconds = 2.0;
constexpr int kSubtitleRefinementIdleRetryMs = 15000;
constexpr double kHighQualityPrePlaybackTargetSeconds = 600.0;
constexpr double kHighQualityPrePlaybackTargetMaxSeconds = 720.0;
constexpr int kHighQualityVisualPrefetchCheckIntervalMs = 2000;
constexpr int kHighQualityPrePlaybackDefaultTimeoutMs = 600000;
constexpr auto kSubtitleTranslationCredentialId = "subtitles/translationApiKey";
constexpr auto kOnlineSubtitleCredentialId = "subtitles/onlineApiKey";
constexpr auto kMimoCredentialId = "mimo/apiKey";
constexpr auto kQwenApiKeyId = "qwen/apiKey";
constexpr auto kGenericCredentialId = "ai/defaultApiKey";
constexpr auto kOpenAICredentialId = "openai/apiKey";

void applyPlayerWindowOpacity(QWidget* window, int percent);
bool runProcessResponsive(const QString& program, const QStringList& arguments, int timeoutMs);
void showHelpDocument(QWidget* parent, const QString& title, const QString& html);
QString cgplayAboutHtml();
bool isTextInputFocusWidget(QWidget* widget);
QScrollArea* createSettingsScrollArea(QWidget* parent);
QWidget* createThemedBackdrop(QWidget* parent);
QDockWidget* createThemedDockWidget(const QString& title, QWidget* parent);
QDialog* createThemedDialog(QWidget* parent);

ReviewPanel* resolveReviewPanelWidget(QWidget* widget);
AnnotationToolbar* resolveAnnotationToolbarWidget(QWidget* widget);
bool isPluginFallbackDisabled();
bool shouldAllowAnnotationFallback();
bool hasAnnotationCapability(IAnnotationService* service, AnnotationManager* fallback);
IAnnotationService* resolveAnnotationService(IAnnotationService* service, AnnotationManager* fallback);

QColor appColorProperty(const char* name, const QColor& fallback = QColor());
QString appStringProperty(const char* name, const QString& fallback = {});
QString safeWorkspaceName(QString name);
int appIntProperty(const char* name, int fallback);
QColor compositeOpaque(QColor foreground, const QColor& background);
QColor adjustBackdropColor(QColor color, int brightness, int saturation);
QColor dialogSurfaceColor();
QColor viewerSurfaceColor();

QString generatedSubtitleOcrProgressPath(QString ocrSourcePath);
QJsonObject readGeneratedSubtitleProgress(const QString& path);
QString generatedSubtitleVisibleDisplayText(QString text, const QFontMetrics& metrics, int maxWidth);
QString recoverySessionPromptTitle();
QString recoverySessionPromptText();
QMessageBox* createYesNoQuestionBox(QWidget* parent, const QString& title, const QString& text);
QMessageBox* createRecoverySessionPromptBox(QWidget* parent);
QString localReferenceSubtitlePathForMedia(const QString& mediaPath);
bool settingOrEnvBool(const std::shared_ptr<ISettingsService>& settings, const QString& key, const QString& envName);
void markAppearanceCustomized(const std::shared_ptr<ISettingsService>& settings, QComboBox* modeCombo);
QString firstNonEmptySetting(const std::shared_ptr<ISettingsService>& settings, std::initializer_list<const char*> keys);
QString firstNonEmptyEnv(std::initializer_list<const char*> keys);
QString firstNonEmptySecret(std::initializer_list<const char*> ids);
bool hasDedicatedSubtitleRefinementWorkspace(const std::shared_ptr<ISettingsService>& settings);
double generatedSubtitleMaxEndSeconds(const QVector<GeneratedSubtitleCue>& cues);
bool generatedSubtitleCueHasDisplayableTranslation(const GeneratedSubtitleCue& cue);
double generatedSubtitleTranslatedMaxEndSeconds(const QVector<GeneratedSubtitleCue>& cues);
int generatedSubtitleMojibakeScore(const QString& text);
QString repairGeneratedSubtitleMojibake(const QString& text);
void repairGeneratedSubtitleCueText(GeneratedSubtitleCue* cue);
bool generatedSubtitleContainsHan(const QString& text);
QString generatedSubtitleNormalizeChineseLiteralForZhHans(QString text);
int generatedSubtitleCueQualityScore(const GeneratedSubtitleCue& cue);
bool isSubtitleGenerationSmokeTrack(const QVector<GeneratedSubtitleCue>& cues);
bool generatedSubtitleTrackUsable(const QVector<GeneratedSubtitleCue>& cues, double currentSeconds,
                                  double mediaDurationSeconds, bool allowSmokeTrack = false,
                                  QString* reason = nullptr);
QString generatedSubtitleTextForLeakScan(const QVector<GeneratedSubtitleCue>& cues);
QString companionSubtitleSourceTextForMedia(const QString& mediaPath);
bool sourceContextAllowsGeneratedSubtitleSignature(const QString& sourceText);
bool generatedSubtitleTextLooksCrossMediaLeaked(const QString& text, const QString& companionSourceText,
                                                QString* reason = nullptr);
int scrubGeneratedSubtitleCrossMediaLeaks(QVector<GeneratedSubtitleCue>* cues,
                                          const QString& companionSourceText, const QString& role,
                                          QString* firstReason = nullptr);
bool generatedSubtitleTextLooksLikeRawEnglishFinal(const QString& text);
bool generatedSubtitleHasCueAtSeconds(const QVector<GeneratedSubtitleCue>& cues, double seconds);
bool generatedSubtitleHasCueNearSeconds(const QVector<GeneratedSubtitleCue>& cues, double seconds,
                                        double toleranceSeconds);
bool generatedSubtitleCueTimingsAlign(const GeneratedSubtitleCue& quick, const GeneratedSubtitleCue& refined);
int findGeneratedSubtitleRefinedMatch(const QVector<GeneratedSubtitleCue>& refinedCues,
                                      const GeneratedSubtitleCue& quickCue, QSet<int>* usedRefinedIndexes);
int mergeGeneratedSubtitleRefinedEnhancements(QVector<GeneratedSubtitleCue>* quickCues,
                                              const QVector<GeneratedSubtitleCue>& refinedCues,
                                              int* matchedCueCount = nullptr);
double generatedSubtitleCurrentSeconds(IPlaybackService* playbackCtrl);
void mergeGeneratedSubtitleCues(QVector<GeneratedSubtitleCue>* target,
                                const QVector<GeneratedSubtitleCue>& incoming,
                                bool preserveExistingDisplayText = false);
bool subtitleGenerationErrorLooksRateLimited(const QString& errorMessage);
QString generatedSubtitleSrtTime(double seconds);
QString generatedSubtitleVttTime(double seconds);
bool writeGeneratedSubtitleTrack(const QString& path, const QVector<GeneratedSubtitleCue>& cues,
                                 bool translated, bool webVtt);
bool writeGeneratedTranslatedSubtitleSidecars(const QString& srtPath, const QString& vttPath,
                                              const QVector<GeneratedSubtitleCue>& cues,
                                              const QString& reason);
void writeGeneratedSubtitleMediaIdentitySidecars(const QStringList& paths, const QString& mediaPath,
                                                 double durationSeconds, const QString& reason);
bool generatedSubtitleCacheMatchesCurrentMedia(const QString& path, const QString& mediaPath,
                                               double durationSeconds, QString* mismatchReason = nullptr,
                                               QJsonObject* recordedIdentity = nullptr);
QString formatCodecText(const MediaInfo& media);
} // namespace cgplay
