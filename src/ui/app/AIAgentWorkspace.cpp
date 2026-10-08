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

using namespace ai_agent_workspace_support;

AIAgentWorkspace::AIAgentWorkspace(
    IAIProviderManager* providerManager,
    IAIWorkflowService* workflowService,
    IPlaybackService* playbackService,
    IAICredentialStore* credentialStore,
    ISettingsService* userSettings,
    IEventBus* eventBus,
    IAnnotationService* annotationService,
    QWidget* parent)
    : QWidget(parent)
    , _providerManager(providerManager)
    , _workflowService(workflowService)
    , _playbackService(playbackService)
    , _credentialStore(credentialStore)
    , _userSettings(userSettings)
    , _eventBus(eventBus)
    , _annotationService(annotationService)
{
    _setupUi();

    if (auto* playbackSignals = _playbackService ? _playbackService->signalProxy() : nullptr) {
        connect(playbackSignals, &PlaybackServiceSignals::fileOpened, this, [this](const QString&) {
            _refreshContextSummary();
            _refreshActionState();
        });
        connect(playbackSignals, &PlaybackServiceSignals::fileClosed, this, [this]() {
            _refreshContextSummary();
            _refreshActionState();
        });
        connect(playbackSignals, &PlaybackServiceSignals::currentFrameChanged, this, [this](int, int) {
            _refreshContextSummary();
        });
        connect(playbackSignals, &PlaybackServiceSignals::fpsChanged, this, [this](double) {
            _refreshContextSummary();
        });
    }

    if (_eventBus) {
        _analysisCompletedSubscription =
            _eventBus->subscribe<AIAnalysisCompletedEvent>([this](const AIAnalysisCompletedEvent& event) {
                QMetaObject::invokeMethod(
                    this,
                    [this, event]() {
                        if (event.jobId != _activeJobId) {
                            return;
                        }
                        _activeJobId.clear();
                        const ActiveJobMode completedMode = _activeJobMode;
                        const QString submittedPrompt = _activeSubmittedPrompt.trimmed();
                        _activeSubmittedPrompt.clear();
                        _activeJobMode = ActiveJobMode::None;
                        if (completedMode == ActiveJobMode::ProbeConnection) {
                            AIResponse probeResponse = event.response;
                            QStringList probeLines;
                            if (_userSettings && !probeResponse.model.trimmed().isEmpty()) {
                                QStringList availableModels =
                                    _userSettings->value(QString::fromLatin1(kAvailableModelsKey)).toStringList();
                                appendUniqueModel(&availableModels, probeResponse.model);
                                _userSettings->setValue(QString::fromLatin1(kAvailableModelsKey), availableModels);
                                _userSettings->setValue(QString::fromLatin1(kRecommendedModelKey), probeResponse.model);
                                _userSettings->setValue(QString::fromLatin1(kGenericModelKey), probeResponse.model);
                                _userSettings->setValue(QString::fromLatin1(kOpenAIModelKey), probeResponse.model);
                                _userSettings->sync();
                            }
                            if (_userSettings) {
                                const QString detectedProvider =
                                    _userSettings->value(QString::fromLatin1(kDetectedProviderKey)).toString().trimmed();
                                const QString detectedProtocol =
                                    _userSettings->value(QString::fromLatin1(kDetectedProtocolKey)).toString().trimmed();
                                const QString detectedEndpoint =
                                    _userSettings->value(QString::fromLatin1(kDetectedEndpointKey)).toString().trimmed();
                                if (!detectedProvider.isEmpty()) {
                                    probeLines << zh(u8"识别到的服务：%1").arg(detectedProvider);
                                } else if (!detectedProtocol.isEmpty()) {
                                    probeLines << zh(u8"识别到的协议：%1").arg(detectedProtocol);
                                }
                                if (!detectedEndpoint.isEmpty()) {
                                    probeLines << zh(u8"接口地址：%1").arg(detectedEndpoint);
                                }
                            }
                            if (!probeResponse.rawText.trimmed().isEmpty()) {
                                probeLines << zh(u8"测试返回：%1").arg(probeResponse.rawText.trimmed());
                            }
                            QString html = QStringLiteral(
                                               "<h3 style='margin:0 0 8px 0;color:%1;'>%2</h3>"
                                               "<div style='color:%3;margin-bottom:10px;'>%4<br/>%5<br/>%6</div>")
                                               .arg(
                                                   kSuccess,
                                                   htmlEscape(zh(u8"连接测试通过")),
                                                   kMuted,
                                                   htmlEscape(zh(u8"服务：%1").arg(probeResponse.providerId)),
                                                   htmlEscape(zh(u8"模型：%1").arg(probeResponse.model)),
                                                   htmlEscape(zh(u8"耗时：%1 ms").arg(QString::number(probeResponse.durationMs))));
                            if (!probeLines.isEmpty()) {
                                html += QStringLiteral("<div style='color:%1;'>%2</div>")
                                            .arg(kMuted, htmlEscape(probeLines.join(QStringLiteral("\n"))));
                            }
                            _refreshProviders();
                            _resultView->setHtml(html);
                            _refreshActionState();
                            _setStatusMessage(zh(u8"连接测试通过"), kSuccess);
                            return;
                        }

                        AIResponse completedResponse = event.response;
                        if (completedMode == ActiveJobMode::SuggestAnnotations) {
                            IPlaybackService* playback = runtimePlaybackService(_playbackService);
                            const int fallbackFrame = playback ? playback->currentFrame() : 0;
                            completedResponse.annotationSuggestions =
                                _parseAnnotationSuggestions(completedResponse.rawText, fallbackFrame);
                        }

                        QString actionPayload;
                        const QString cleanedRawText =
                            _stripAiActionBlock(completedResponse.rawText, &actionPayload);
                        const QStringList executedActions = _executeAiControlActions(actionPayload);
                        if (!executedActions.isEmpty()) {
                            QStringList sections;
                            if (!cleanedRawText.trimmed().isEmpty()) {
                                sections << cleanedRawText.trimmed();
                            }
                            sections << zh(u8"已执行动作：%1").arg(executedActions.join(zh(u8"；")));
                            completedResponse.rawText = sections.join(QStringLiteral("\n\n"));
                        } else {
                            completedResponse.rawText = cleanedRawText;
                        }

                        if (_userSettings && !completedResponse.model.trimmed().isEmpty()) {
                            QStringList availableModels =
                                _userSettings->value(QString::fromLatin1(kAvailableModelsKey)).toStringList();
                            appendUniqueModel(&availableModels, completedResponse.model);
                            _userSettings->setValue(QString::fromLatin1(kAvailableModelsKey), availableModels);
                            _userSettings->setValue(QString::fromLatin1(kRecommendedModelKey), completedResponse.model);
                            _userSettings->setValue(QString::fromLatin1(kGenericModelKey), completedResponse.model);
                            _userSettings->setValue(QString::fromLatin1(kOpenAIModelKey), completedResponse.model);
                            _userSettings->sync();
                        }
                        _refreshProviders();
                        _lastResponse = completedResponse;

                        // Add to chat history for multi-turn conversation
                        if (completedMode == ActiveJobMode::AnalyzeFrame ||
                            completedMode == ActiveJobMode::AnalyzeShot ||
                            completedMode == ActiveJobMode::AnalyzeSequence ||
                            completedMode == ActiveJobMode::SuggestAnnotations ||
                            completedMode == ActiveJobMode::GenerateSummary ||
                            completedMode == ActiveJobMode::QCScan ||
                            completedMode == ActiveJobMode::CompareAB) {
                            AIChatMessage userMsg;
                            userMsg.role = AIChatRole::User;
                            userMsg.content = submittedPrompt;
                            userMsg.timestamp = QDateTime::currentMSecsSinceEpoch();
                            if (!userMsg.content.isEmpty()) {
                                _chatHistory.append(userMsg);
                            }

                            AIChatMessage assistantMsg;
                            assistantMsg.role = AIChatRole::Assistant;
                            assistantMsg.content = completedResponse.rawText.trimmed().isEmpty()
                                ? completedResponse.reviewSummary.plainText
                                : completedResponse.rawText;
                            assistantMsg.model = completedResponse.model;
                            assistantMsg.timestamp = QDateTime::currentMSecsSinceEpoch();
                            if (!assistantMsg.content.isEmpty()) {
                                _chatHistory.append(assistantMsg);
                            }
                        }

                        if (!_chatHistory.isEmpty()) {
                            _resultView->setHtml(_renderChatHtml());
                            // Append the detailed response below
                            QString detailHtml = _renderResponseHtml(completedResponse);
                            _resultView->append(QStringLiteral("<hr style='border:none;border-top:1px solid %1;'/>").arg(kBorder));
                            _resultView->append(detailHtml);
                        } else if (completedMode == ActiveJobMode::GenerateSummary) {
                            QString html = _renderResponseHtml(completedResponse);
                            _resultView->setHtml(html);
                        } else {
                            _resultView->setHtml(_renderResponseHtml(completedResponse));
                        }

                        if (completedMode == ActiveJobMode::SuggestAnnotations) {
                            if (!completedResponse.annotationSuggestions.isEmpty()) {
                                _setStatusMessage(
                                    zh(u8"已生成 %1 条批注建议").arg(completedResponse.annotationSuggestions.size()),
                                    kSuccess);
                            } else if (!executedActions.isEmpty()) {
                                _setStatusMessage(zh(u8"AI 已执行播放器动作"), kSuccess);
                            } else {
                                _setStatusMessage(zh(u8"已返回结果，但没有解析出可应用批注建议"), kWarning);
                            }
                        } else if (!executedActions.isEmpty()) {
                            _setStatusMessage(zh(u8"AI 已执行播放器动作"), kSuccess);
                        } else {
                            _setStatusMessage(zh(u8"分析完成"), kSuccess);
                        }
                        _refreshActionState();
                    },
                    Qt::QueuedConnection);
            });

        _analysisFailedSubscription =
            _eventBus->subscribe<AIAnalysisFailedEvent>([this](const AIAnalysisFailedEvent& event) {
                QMetaObject::invokeMethod(
                    this,
                    [this, event]() {
                        if (event.jobId != _activeJobId) {
                            return;
                        }
                        _activeJobId.clear();
                        const ActiveJobMode failedMode = _activeJobMode;
                        _activeSubmittedPrompt.clear();
                        _activeJobMode = ActiveJobMode::None;
                        const AIJobSnapshot snapshot =
                            _workflowService ? _workflowService->jobSnapshot(event.jobId) : AIJobSnapshot{};
                        const QString detail = !snapshot.errorMessage.isEmpty()
                            ? snapshot.errorMessage
                            : event.errorMessage;
                        if (failedMode == ActiveJobMode::ProbeConnection) {
                            _resultView->setHtml(_renderFailureHtml(zh(u8"连接测试失败"), detail));
                            _refreshActionState();
                            _setStatusMessage(zh(u8"连接测试失败"), kError);
                            return;
                        }
                        _resultView->setHtml(_renderFailureHtml(zh(u8"分析失败"), detail));
                        _setStatusMessage(zh(u8"分析失败"), kError);
                        _refreshActionState();
                    },
                    Qt::QueuedConnection);
            });

        _providerAvailabilitySubscription =
            _eventBus->subscribe<AIProviderAvailabilityChangedEvent>([this](const AIProviderAvailabilityChangedEvent&) {
                QMetaObject::invokeMethod(
                    this,
                    [this]() {
                        _refreshProviders();
                    },
                    Qt::QueuedConnection);
            });
        _modelListUpdatedSubscription =
            _eventBus->subscribe<AIModelListUpdatedEvent>([this](const AIModelListUpdatedEvent& event) {
                QMetaObject::invokeMethod(
                    this,
                    [this, event]() {
                        if (_userSettings && !event.result.models.isEmpty()) {
                            QStringList availableModels = _userSettings
                                ->value(QString::fromLatin1(kAvailableModelsKey))
                                .toStringList();
                            for (const QString& model : event.result.models) {
                                appendUniqueModel(&availableModels, model);
                            }
                            _userSettings->setValue(
                                QString::fromLatin1(kAvailableModelsKey),
                                availableModels);
                            _userSettings->sync();
                        }
                        _refreshModels();
                        _refreshActionState();
                    },
                    Qt::QueuedConnection);
            });
    }

    refreshFromRuntime();
}

AIAgentWorkspace::~AIAgentWorkspace()
{
    if (_eventBus && _analysisCompletedSubscription != 0) {
        _eventBus->unsubscribe<AIAnalysisCompletedEvent>(_analysisCompletedSubscription);
    }
    if (_eventBus && _analysisFailedSubscription != 0) {
        _eventBus->unsubscribe<AIAnalysisFailedEvent>(_analysisFailedSubscription);
    }
    if (_eventBus && _providerAvailabilitySubscription != 0) {
        _eventBus->unsubscribe<AIProviderAvailabilityChangedEvent>(_providerAvailabilitySubscription);
    }
    if (_eventBus && _modelListUpdatedSubscription != 0) {
        _eventBus->unsubscribe<AIModelListUpdatedEvent>(_modelListUpdatedSubscription);
    }
}

void AIAgentWorkspace::refreshFromRuntime()
{
    _applyThemeStyle();
    _refreshProviders();
    _loadProviderConfig();
    _loadImageGenerationConfig();
    _loadMediaGenerationConfigs();
    _refreshContextSummary();
    _refreshActionState();
}

bool AIAgentWorkspace::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == _promptEdit && event && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent &&
            (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter))
        {
            const Qt::KeyboardModifiers modifiers = keyEvent->modifiers();
            const bool allowNewLine =
                modifiers.testFlag(Qt::ShiftModifier) ||
                modifiers.testFlag(Qt::ControlModifier) ||
                modifiers.testFlag(Qt::AltModifier) ||
                modifiers.testFlag(Qt::MetaModifier);
            if (!allowNewLine) {
                _submitCurrentFrameAnalysis();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AIAgentWorkspace::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (!event || event->type() != QEvent::PaletteChange || _applyingThemeStyle) {
        return;
    }
    _applyThemeStyle();
}

void AIAgentWorkspace::_applyThemeStyle()
{
    if (_applyingThemeStyle) return;
    _applyingThemeStyle = true;

    const QPalette appPalette = QApplication::palette();
    const QColor surface = appThemeColor("cgplay.panelColor", appPalette.color(QPalette::Base));
    const QColor toolbar = appThemeColor("cgplay.toolbarColor", appPalette.color(QPalette::Button));
    const QColor input = appThemeColor("cgplay.timelineColor", surface.darker(108));
    const QColor text = appThemeColor("cgplay.textColor", appPalette.color(QPalette::Text));
    const QColor muted = appPalette.color(QPalette::Disabled, QPalette::Text);
    const QColor border = appThemeColor("cgplay.borderColor", appPalette.color(QPalette::Mid));
    const QColor accent = appThemeColor("cgplay.accentColor", appPalette.color(QPalette::Highlight));
    const QColor accentText = readableAccentText(accent);
    QColor workspaceSurface = surface;
    const int panelOpacity = qApp ? qBound(0, qApp->property("cgplay.panelOpacity").toInt(), 100) : 96;
    workspaceSurface.setAlpha(qRound(panelOpacity * 255.0 / 100.0));

    const QString css = QStringLiteral(
        "#AIAgentWorkspace{background:%1;color:%2;}"
        "QLabel{color:%2;}"
        "QFrame#AIAgentCard,QFrame#SettingsCard{background:transparent;border:1px solid %3;border-radius:8px;}"
        "QFrame#ComposerCard{background:transparent;border:1px solid %3;border-radius:8px;}"
        "QFrame#AIAgentInset{background:%4;border:1px solid %3;border-radius:6px;}"
        "QScrollArea{background:transparent;border:0;} QScrollArea QWidget{background:transparent;}"
        "QComboBox,QLineEdit,QPlainTextEdit,QTextBrowser{background:%4;color:%2;border:1px solid %3;border-radius:6px;padding:6px 8px;}"
        "QComboBox::drop-down{border:none;width:20px;}"
        "QPushButton{background:%5;color:%2;border:1px solid %3;border-radius:6px;padding:6px 14px;}"
        "QPushButton:hover{background:%6;border-color:%7;}"
        "QPushButton:pressed{background:%7;color:%8;}"
        "QPushButton#settingsToggle:checked,QPushButton#sectionToggle:checked{background:%6;border-color:%7;color:%7;}"
        "QPushButton:disabled{color:%9;background:%4;border-color:%3;}"
        "QPushButton#accent{background:%7;color:%8;border:1px solid %7;font-weight:700;}"
        "QPushButton#accent:hover{background:%6;}"
        "QSplitter::handle{background:%3;} QSplitter::handle:vertical{height:8px;margin:2px 0;border-radius:4px;}"
        "QSplitter::handle:hover{background:%7;} QTextBrowser{padding:8px;} QPlainTextEdit{padding:8px;}"
        "QScrollBar::handle:vertical{background:%3;border-radius:4px;min-height:24px;}")
        .arg(workspaceSurface.name(QColor::HexArgb), text.name(QColor::HexArgb), border.name(QColor::HexArgb),
             input.name(QColor::HexArgb), toolbar.name(QColor::HexArgb),
             accent.lighter(115).name(QColor::HexArgb), accent.name(QColor::HexArgb),
             accentText.name(QColor::HexArgb), muted.name(QColor::HexArgb));
    setStyleSheet(css);

    // Existing child-level rules used fixed dark colors.  Strip only visual
    // declarations so their sizing/font rules remain while the root token CSS
    // controls colors and surfaces in every theme mode.
    const QRegularExpression visualDeclaration(
        QStringLiteral("(?:selection-background-color|selection-color|background-color|border-color|background|border|color)\\s*:\\s*[^;{}]+;"));
    for (QWidget* child : findChildren<QWidget*>()) {
        const QString localStyle = child->styleSheet();
        if (localStyle.isEmpty()) continue;
        const QString cleaned = QString(localStyle).remove(visualDeclaration);
        if (cleaned != localStyle) child->setStyleSheet(cleaned);
    }

    if (_settingsDialog) {
        _settingsDialog->setStyleSheet(QStringLiteral(
            "QDialog{background:%1;color:%2;} QLabel{color:%2;}"
            "QFrame#AIAgentCard{background:%1;border:1px solid %3;border-radius:8px;}"
            "QFrame#AIAgentInset{background:%4;border:1px solid %3;border-radius:6px;}"
            "QComboBox,QLineEdit{background:%4;color:%2;border:1px solid %3;border-radius:6px;padding:6px 8px;}"
            "QComboBox::drop-down{border:none;width:20px;}"
            "QPushButton{background:%5;color:%2;border:1px solid %3;border-radius:6px;padding:6px 12px;}"
            "QPushButton:hover{background:%6;border-color:%7;}")
            .arg(surface.name(QColor::HexArgb), text.name(QColor::HexArgb), border.name(QColor::HexArgb),
                 input.name(QColor::HexArgb), toolbar.name(QColor::HexArgb),
                 accent.lighter(115).name(QColor::HexArgb), accent.name(QColor::HexArgb)));
        if (auto* viewport = _settingsDialog->findChild<QWidget*>(QStringLiteral("AIAgentSettingsViewport"))) {
            viewport->setStyleSheet(QStringLiteral("background:%1;border:0;").arg(surface.name(QColor::HexArgb)));
        }
    }

    update();
    _applyingThemeStyle = false;
}

void AIAgentWorkspace::_setupUi()
{
    setObjectName(QStringLiteral("AIAgentWorkspace"));
    setMinimumWidth(320);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(10);

    auto* headerCard = new QFrame(this);
    headerCard->setObjectName(QStringLiteral("AIAgentCard"));
    auto* headerLayout = new QVBoxLayout(headerCard);
    headerLayout->setContentsMargins(10, 10, 10, 10);
    headerLayout->setSpacing(6);

    auto* titleRow = new QHBoxLayout();
    titleRow->setSpacing(8);

    auto* titleLabel = new QLabel(zh(u8"AI 工作台"), headerCard);
    titleLabel->setStyleSheet(QStringLiteral("font-size:15px;font-weight:700;color:%1;").arg(kText));
    titleRow->addWidget(titleLabel);
    titleRow->addStretch(1);

    _settingsToggleButton = new QPushButton(zh(u8"设置"), headerCard);
    _settingsToggleButton->setObjectName(QStringLiteral("settingsToggle"));
    titleRow->addWidget(_settingsToggleButton);
    headerLayout->addLayout(titleRow);

    _contextLabel = new QLabel(headerCard);
    _contextLabel->setWordWrap(true);
    _contextLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    headerLayout->addWidget(_contextLabel);

    _phaseNoteLabel = new QLabel(
        zh(u8""),
        headerCard);
    _phaseNoteLabel->setWordWrap(true);
    _phaseNoteLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kWarning));
    _phaseNoteLabel->hide();
    rootLayout->addWidget(headerCard);

    // ============================================================
    // Tools card — all features visible simultaneously, no mode switching
    // ============================================================
    auto* toolsCard = new QFrame(this);
    toolsCard->setObjectName(QStringLiteral("AIAgentCard"));
    auto* toolsLayout = new QVBoxLayout(toolsCard);
    toolsLayout->setContentsMargins(10, 10, 10, 10);
    toolsLayout->setSpacing(8);

    auto* toolsHeaderRow = new QHBoxLayout();
    toolsHeaderRow->setSpacing(8);

    auto* toolsLabel = new QLabel(zh(u8"智能体工具"), toolsCard);
    toolsLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    toolsHeaderRow->addWidget(toolsLabel);

    auto* toolsStateLabel = new QLabel(zh(u8"默认已启用"), toolsCard);
    toolsStateLabel->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(kMuted));
    toolsHeaderRow->addWidget(toolsStateLabel);
    toolsHeaderRow->addStretch(1);

    _toolsToggleButton = new QPushButton(toolsCard);
    _toolsToggleButton->setObjectName(QStringLiteral("sectionToggle"));
    _toolsToggleButton->setCheckable(true);
    toolsHeaderRow->addWidget(_toolsToggleButton);
    toolsLayout->addLayout(toolsHeaderRow);

    _toolsBodyFrame = new QFrame(toolsCard);
    _toolsBodyFrame->setFrameShape(QFrame::NoFrame);
    auto* toolsBodyLayout = new QVBoxLayout(_toolsBodyFrame);
    toolsBodyLayout->setContentsMargins(0, 4, 0, 0);
    toolsBodyLayout->setSpacing(8);

    auto* toolsHint = new QLabel(
        zh(u8"所有智能体能力默认可用，不需要手动切换模式。直接点击下面的入口，或者在底部直接提问。"),
        _toolsBodyFrame);
    toolsHint->setWordWrap(true);
    toolsHint->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    toolsBodyLayout->addWidget(toolsHint);

    auto* agentGrid = new QGridLayout();
    agentGrid->setHorizontalSpacing(6);
    agentGrid->setVerticalSpacing(6);

    _frameCriticButton = new QPushButton(zh(u8"当前帧审片"), _toolsBodyFrame);
    _frameCriticButton->setObjectName(QStringLiteral("accent"));
    agentGrid->addWidget(_frameCriticButton, 0, 0);

    _shotReviewButton = new QPushButton(zh(u8"镜头审片"), _toolsBodyFrame);
    agentGrid->addWidget(_shotReviewButton, 0, 1);

    _sequenceReviewButton = new QPushButton(zh(u8"序列审片"), _toolsBodyFrame);
    agentGrid->addWidget(_sequenceReviewButton, 1, 0);

    _annotationSuggestButton = new QPushButton(zh(u8"批注建议"), _toolsBodyFrame);
    agentGrid->addWidget(_annotationSuggestButton, 1, 1);

    toolsBodyLayout->addLayout(agentGrid);

    // --- Row 1: Batch scan (range + sample count + button) ---
    auto* batchRow = new QHBoxLayout();
    batchRow->setSpacing(6);
    _scanRangeCombo = new QComboBox(_toolsBodyFrame);
    _scanRangeCombo->addItem(zh(u8"当前镜头"), static_cast<int>(AIRequestScope::CurrentShot));
    _scanRangeCombo->addItem(zh(u8"整个序列"), static_cast<int>(AIRequestScope::CurrentSequence));
    batchRow->addWidget(_scanRangeCombo, 2);

    _sampleCountCombo = new QComboBox(_toolsBodyFrame);
    _sampleCountCombo->addItem(zh(u8"4 帧"), 4);
    _sampleCountCombo->addItem(zh(u8"8 帧"), 8);
    _sampleCountCombo->addItem(zh(u8"16 帧"), 16);
    _sampleCountCombo->addItem(zh(u8"32 帧"), 32);
    _sampleCountCombo->setCurrentIndex(1);
    batchRow->addWidget(_sampleCountCombo, 1);

    _batchScanButton = new QPushButton(zh(u8"批量扫描"), _toolsBodyFrame);
    _batchScanButton->setObjectName(QStringLiteral("accent"));
    batchRow->addWidget(_batchScanButton);
    toolsBodyLayout->addLayout(batchRow);

    // --- Row 2: QC check + Generate report + Compare AB ---
    auto* actionRow = new QHBoxLayout();
    actionRow->setSpacing(6);
    _qcButton = new QPushButton(zh(u8"QC 检查"), _toolsBodyFrame);
    actionRow->addWidget(_qcButton);

    _generateReportButton = new QPushButton(zh(u8"生成报告"), _toolsBodyFrame);
    actionRow->addWidget(_generateReportButton);

    _compareABButton = new QPushButton(zh(u8"A/B 对比"), _toolsBodyFrame);
    actionRow->addWidget(_compareABButton);

    _exportReportButton = new QPushButton(zh(u8"导出报告"), _toolsBodyFrame);
    actionRow->addWidget(_exportReportButton);
    toolsBodyLayout->addLayout(actionRow);

    // --- Row 3: Problem frames + Annotation summary ---
    auto* navRow = new QHBoxLayout();
    navRow->setSpacing(6);
    _problemFramesButton = new QPushButton(zh(u8"问题帧导航"), _toolsBodyFrame);
    navRow->addWidget(_problemFramesButton);

    _annotationSummaryButton = new QPushButton(zh(u8"批注聚合"), _toolsBodyFrame);
    navRow->addWidget(_annotationSummaryButton);
    toolsBodyLayout->addLayout(navRow);

    // --- Row 4: Smart search ---
    auto* searchRow = new QHBoxLayout();
    searchRow->setSpacing(6);
    _searchEdit = new QLineEdit(_toolsBodyFrame);
    _searchEdit->setPlaceholderText(zh(u8"搜索批注：输入关键词如\"严重\"\"未解决\"..."));
    searchRow->addWidget(_searchEdit, 3);
    _searchButton = new QPushButton(zh(u8"搜索"), _toolsBodyFrame);
    searchRow->addWidget(_searchButton);
    toolsBodyLayout->addLayout(searchRow);

    toolsLayout->addWidget(_toolsBodyFrame);

    _toolsBodyFrame->setVisible(false);
    _toolsToggleButton->setChecked(false);
    _toolsToggleButton->setText(zh(u8"展开"));

    rootLayout->addWidget(toolsCard);

    _settingsDialog = new QDialog(this);
    _settingsDialog->setWindowTitle(zh(u8"AI 设置"));
    _settingsDialog->setModal(false);
    _settingsDialog->resize(560, 760);
    _settingsDialog->setStyleSheet(QString(
        "QDialog{background:%1;color:%2;}"
        "QLabel{color:%2;}"
        "QFrame#AIAgentCard{background:%1;border:1px solid rgba(255,255,255,0.06);border-radius:8px;}"
        "QFrame#AIAgentInset{background:#111418;border:1px solid rgba(255,255,255,0.05);border-radius:6px;}"
        "QComboBox,QLineEdit{background:#111418;color:%2;border:1px solid %3;border-radius:6px;padding:6px 8px;}"
        "QComboBox::drop-down{border:none;width:20px;}"
        "QPushButton{background:#171B20;color:%2;border:1px solid rgba(255,255,255,0.08);border-radius:6px;padding:6px 12px;}"
        "QPushButton:hover{background:rgba(255,138,61,0.15);border-color:%5;}")
        .arg(kSurface, kText, kBorder, kMuted, kAccent));

    auto* dialogLayout = new QVBoxLayout(_settingsDialog);
    dialogLayout->setContentsMargins(0, 0, 0, 0);
    dialogLayout->setSpacing(0);

    auto* settingsScroll = new QScrollArea(_settingsDialog);
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setFrameShape(QFrame::NoFrame);
    settingsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    settingsScroll->setStyleSheet(QStringLiteral("QScrollArea{background:transparent;border:none;}"));
    settingsScroll->viewport()->setObjectName(QStringLiteral("AIAgentSettingsViewport"));
    settingsScroll->viewport()->setStyleSheet(QStringLiteral("background:%1;border:0;").arg(kSurface));

    auto* settingsContent = new QWidget(settingsScroll);
    settingsContent->setStyleSheet(QStringLiteral("background:transparent;"));
    auto* settingsLayout = new QVBoxLayout(settingsContent);
    settingsLayout->setContentsMargins(12, 12, 12, 12);
    settingsLayout->setSpacing(10);
    settingsScroll->setWidget(settingsContent);
    dialogLayout->addWidget(settingsScroll);

    auto* settingsTitle = new QLabel(zh(u8"设置"), settingsContent);
    settingsTitle->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;color:%1;").arg(kText));
    settingsLayout->addWidget(settingsTitle);

    auto* settingsHint = new QLabel(
        zh(u8""),
        settingsContent);
    settingsHint->setWordWrap(true);
    settingsHint->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    settingsHint->hide();

    auto* connectionCard = new QFrame(settingsContent);
    connectionCard->setObjectName(QStringLiteral("AIAgentCard"));
    auto* connectionLayout = new QVBoxLayout(connectionCard);
    connectionLayout->setContentsMargins(10, 10, 10, 10);
    connectionLayout->setSpacing(8);

    auto* connectionTitle = new QLabel(zh(u8"智能识别 / Workbench API"), connectionCard);
    connectionTitle->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;color:%1;").arg(kText));
    connectionLayout->addWidget(connectionTitle);

    auto* connectionHint = new QLabel(
        zh(u8"用途：搜字幕 / 读画面字 / 文本翻译 / 修复 / 融合。"),
        connectionCard);
    connectionHint->setWordWrap(false);
    connectionHint->setMinimumHeight(22);
    connectionHint->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    connectionHint->hide();

    auto* providerLabel = new QLabel(zh(u8"Workbench 服务提供方"), connectionCard);
    providerLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    connectionLayout->addWidget(providerLabel);

    _providerCombo = new QComboBox(connectionCard);
    connectionLayout->addWidget(_providerCombo);

    _providerConfigFrame = new QFrame(connectionCard);
    _providerConfigFrame->setFrameShape(QFrame::NoFrame);
    auto* providerConfigLayout = new QVBoxLayout(_providerConfigFrame);
    providerConfigLayout->setContentsMargins(0, 4, 0, 0);
    providerConfigLayout->setSpacing(10);

    _providerConfigHintLabel = new QLabel(_providerConfigFrame);
    _providerConfigHintLabel->setWordWrap(true);
    _providerConfigHintLabel->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(kMuted));
    providerConfigLayout->addWidget(_providerConfigHintLabel);

    _apiKeyEdit = new QLineEdit(_providerConfigFrame);
    _apiKeyEdit->setEchoMode(QLineEdit::Password);
    _apiKeyEdit->setPlaceholderText(zh(u8"Workbench API Key（留空继续使用已保存密钥）"));
    providerConfigLayout->addWidget(_apiKeyEdit);

    _baseUrlEdit = new QLineEdit(_providerConfigFrame);
    _baseUrlEdit->setPlaceholderText(zh(u8"Workbench Base URL / API 地址"));
    providerConfigLayout->addWidget(_baseUrlEdit);

    auto* providerConfigButtons = new QGridLayout();
    providerConfigButtons->setHorizontalSpacing(8);
    providerConfigButtons->setVerticalSpacing(8);

    _testConnectionButton = new QPushButton(zh(u8"测试连接"), _providerConfigFrame);
    _testConnectionButton->setMinimumHeight(36);
    providerConfigButtons->addWidget(_testConnectionButton, 0, 0);

    _saveProviderConfigButton = new QPushButton(zh(u8"保存配置"), _providerConfigFrame);
    _saveProviderConfigButton->setMinimumHeight(36);
    providerConfigButtons->addWidget(_saveProviderConfigButton, 0, 1);

    _clearProviderConfigButton = new QPushButton(zh(u8"清除密钥"), _providerConfigFrame);
    _clearProviderConfigButton->setMinimumHeight(36);
    providerConfigButtons->addWidget(_clearProviderConfigButton, 1, 0, 1, 2);

    providerConfigLayout->addLayout(providerConfigButtons);
    connectionLayout->addWidget(_providerConfigFrame);
    settingsLayout->addWidget(connectionCard);

    auto* modelCard = new QFrame(settingsContent);
    modelCard->setObjectName(QStringLiteral("AIAgentCard"));
    auto* modelCardLayout = new QVBoxLayout(modelCard);
    modelCardLayout->setContentsMargins(10, 10, 10, 10);
    modelCardLayout->setSpacing(8);

    auto* modelTitle = new QLabel(zh(u8"模型"), modelCard);
    modelTitle->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;color:%1;").arg(kText));
    modelCardLayout->addWidget(modelTitle);

    auto* modelHint = new QLabel(
        zh(u8""),
        modelCard);
    modelHint->setWordWrap(true);
    modelHint->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    modelHint->hide();

    _autoModelCheck = new QCheckBox(zh(u8"自动选择推荐模型"), modelCard);
    _autoModelCheck->setChecked(false);
    modelCardLayout->addWidget(_autoModelCheck);

    _manualModelFrame = new QFrame(modelCard);
    _manualModelFrame->setObjectName(QStringLiteral("AIAgentInset"));
    auto* manualModelLayout = new QVBoxLayout(_manualModelFrame);
    manualModelLayout->setContentsMargins(10, 10, 10, 10);
    manualModelLayout->setSpacing(8);

    auto* manualModelLabel = new QLabel(zh(u8"手动模型"), _manualModelFrame);
    manualModelLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    manualModelLayout->addWidget(manualModelLabel);

    auto* manualModelHint = new QLabel(
        zh(u8""),
        _manualModelFrame);
    manualModelHint->setWordWrap(true);
    manualModelHint->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(kMuted));
    manualModelHint->hide();

    _modelCombo = new QComboBox(_manualModelFrame);
    _modelCombo->setEditable(true);
    _modelCombo->setInsertPolicy(QComboBox::NoInsert);
    manualModelLayout->addWidget(_modelCombo);

    modelCardLayout->addWidget(_manualModelFrame);
    settingsLayout->addWidget(modelCard);

    auto* imageCard = new QFrame(settingsContent);
    imageCard->setObjectName(QStringLiteral("AIAgentCard"));
    auto* imageLayout = new QVBoxLayout(imageCard);
    imageLayout->setContentsMargins(10, 10, 10, 10);
    imageLayout->setSpacing(8);

    auto* imageTitle = new QLabel(zh(u8"图片生成 API"), imageCard);
    imageTitle->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;color:%1;").arg(kText));
    imageLayout->addWidget(imageTitle);
    auto* imageHint = new QLabel(zh(u8"独立于 Workbench 文本 API，用于中转站的 OpenAI-compatible Images 接口。"), imageCard);
    imageHint->setWordWrap(true);
    imageHint->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(kMuted));
    imageLayout->addWidget(imageHint);

    _imageEndpointEdit = new QLineEdit(imageCard);
    _imageEndpointEdit->setPlaceholderText(QStringLiteral("https://image-gateway.example/v1/images/generations"));
    imageLayout->addWidget(_imageEndpointEdit);
    _imageApiKeyEdit = new QLineEdit(imageCard);
    _imageApiKeyEdit->setEchoMode(QLineEdit::Password);
    _imageApiKeyEdit->setPlaceholderText(zh(u8"图片 API Key（留空继续使用已保存密钥）"));
    imageLayout->addWidget(_imageApiKeyEdit);
    _imageModelEdit = new QLineEdit(imageCard);
    _imageModelEdit->setPlaceholderText(QStringLiteral("gpt-image-1"));
    imageLayout->addWidget(_imageModelEdit);
    _saveImageConfigButton = new QPushButton(zh(u8"保存图片生成设置"), imageCard);
    _saveImageConfigButton->setMinimumHeight(36);
    imageLayout->addWidget(_saveImageConfigButton);
    _imageConfigStatusLabel = new QLabel(imageCard);
    _imageConfigStatusLabel->setWordWrap(true);
    imageLayout->addWidget(_imageConfigStatusLabel);
    settingsLayout->addWidget(imageCard);

    auto* mediaCard = new QFrame(settingsContent);
    mediaCard->setObjectName(QStringLiteral("AIAgentCard"));
    auto* mediaLayout = new QVBoxLayout(mediaCard);
    mediaLayout->setContentsMargins(10, 10, 10, 10);
    mediaLayout->setSpacing(8);
    auto* mediaTitle = new QLabel(zh(u8"视频、音频与图片编辑 API"), mediaCard);
    mediaTitle->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;color:%1;").arg(kText));
    mediaLayout->addWidget(mediaTitle);
    auto* mediaHint = new QLabel(zh(u8"分别配置服务商 Endpoint、模型和密钥。不会复用文本或图片生成密钥。仅在完整配置后向 Codex 暴露对应工具。"), mediaCard);
    mediaHint->setWordWrap(true);
    mediaHint->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(kMuted));
    mediaLayout->addWidget(mediaHint);
    const auto addMediaFields = [mediaCard, mediaLayout](const QString& title, QLineEdit** endpoint, QLineEdit** model, QLineEdit** key, const QString& placeholder) {
        auto* label = new QLabel(title, mediaCard);
        label->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
        mediaLayout->addWidget(label);
        *endpoint = new QLineEdit(mediaCard); (*endpoint)->setPlaceholderText(placeholder); mediaLayout->addWidget(*endpoint);
        *model = new QLineEdit(mediaCard); (*model)->setPlaceholderText(QStringLiteral("model")); mediaLayout->addWidget(*model);
        *key = new QLineEdit(mediaCard); (*key)->setEchoMode(QLineEdit::Password); (*key)->setPlaceholderText(zh(u8"API Key（留空继续使用已保存密钥）")); mediaLayout->addWidget(*key);
    };
    addMediaFields(zh(u8"视频生成"), &_videoEndpointEdit, &_videoModelEdit, &_videoApiKeyEdit, QStringLiteral("https://gateway.example/v1/videos"));
    _videoStatusEndpointEdit = new QLineEdit(mediaCard); _videoStatusEndpointEdit->setPlaceholderText(QStringLiteral("https://gateway.example/v1/videos/{id} (optional async status URL)")); mediaLayout->addWidget(_videoStatusEndpointEdit);
    addMediaFields(zh(u8"音频生成"), &_audioEndpointEdit, &_audioModelEdit, &_audioApiKeyEdit, QStringLiteral("https://gateway.example/v1/audio/generations"));
    _audioStatusEndpointEdit = new QLineEdit(mediaCard); _audioStatusEndpointEdit->setPlaceholderText(QStringLiteral("https://gateway.example/v1/audio/{id} (optional async status URL)")); mediaLayout->addWidget(_audioStatusEndpointEdit);
    addMediaFields(zh(u8"图片编辑"), &_imageEditEndpointEdit, &_imageEditModelEdit, &_imageEditApiKeyEdit, QStringLiteral("https://gateway.example/v1/images/edits"));
    _saveMediaConfigsButton = new QPushButton(zh(u8"保存媒体生成设置"), mediaCard);
    _saveMediaConfigsButton->setMinimumHeight(36);
    mediaLayout->addWidget(_saveMediaConfigsButton);
    _mediaConfigsStatusLabel = new QLabel(mediaCard);
    _mediaConfigsStatusLabel->setWordWrap(true);
    mediaLayout->addWidget(_mediaConfigsStatusLabel);
    settingsLayout->addWidget(mediaCard);

    auto* subtitleCard = new QFrame(settingsContent);
    subtitleCard->setObjectName(QStringLiteral("AIAgentCard"));
    auto* subtitleLayout = new QVBoxLayout(subtitleCard);
    subtitleLayout->setContentsMargins(10, 10, 10, 10);
    subtitleLayout->setSpacing(8);

    auto* subtitleTitle = new QLabel(zh(u8"ASR API（只用于音频转写）"), subtitleCard);
    subtitleTitle->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;color:%1;").arg(kText));
    subtitleLayout->addWidget(subtitleTitle);

    auto* asrLabel = new QLabel(zh(u8"ASR API 协议"), subtitleCard);
    asrLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    subtitleLayout->addWidget(asrLabel);

    _subtitleAsrProviderCombo = new QComboBox(subtitleCard);
    _subtitleAsrProviderCombo->setEditable(false);
    _subtitleAsrProviderCombo->setInsertPolicy(QComboBox::NoInsert);
    _subtitleAsrProviderCombo->addItem(QStringLiteral("Mimo"), QStringLiteral("mimo"));
    _subtitleAsrProviderCombo->addItem(QStringLiteral("Qwen3-ASR / DashScope"), QStringLiteral("qwen"));
    _subtitleAsrProviderCombo->addItem(QStringLiteral("Gemini"), QStringLiteral("gemini"));
    _subtitleAsrProviderCombo->addItem(QStringLiteral("OpenAI transcription"), QStringLiteral("openai"));
    _subtitleAsrProviderCombo->addItem(QStringLiteral("Responses Audio"), QStringLiteral("responses_audio"));
    subtitleLayout->addWidget(_subtitleAsrProviderCombo);

    _subtitleAsrBaseUrlEdit = new QLineEdit(subtitleCard);
    _subtitleAsrBaseUrlEdit->setPlaceholderText(zh(u8"ASR Base URL（只用于音频转写）"));
    subtitleLayout->addWidget(_subtitleAsrBaseUrlEdit);

    _subtitleAsrApiKeyEdit = new QLineEdit(subtitleCard);
    _subtitleAsrApiKeyEdit->setEchoMode(QLineEdit::Password);
    _subtitleAsrApiKeyEdit->setPlaceholderText(zh(u8"ASR API Key（留空继续使用已保存密钥）"));
    subtitleLayout->addWidget(_subtitleAsrApiKeyEdit);

    _subtitleAsrModelCombo = new QComboBox(subtitleCard);
    _subtitleAsrModelCombo->setEditable(true);
    _subtitleAsrModelCombo->setInsertPolicy(QComboBox::NoInsert);
    _subtitleAsrModelCombo->addItem(QStringLiteral("auto"));
    _subtitleAsrModelCombo->addItem(QString::fromLatin1(kQwenDefaultAsrModel));
    _subtitleAsrModelCombo->addItem(QStringLiteral("qwen3-asr"));
    _subtitleAsrModelCombo->addItem(QStringLiteral("gemini-2.5-flash"));
    _subtitleAsrModelCombo->addItem(QStringLiteral("gemini-2.0-flash"));
    _subtitleAsrModelCombo->addItem(QStringLiteral("mimo-v2.5-asr"));
    _subtitleAsrModelCombo->addItem(QStringLiteral("whisper-large-v3-turbo"));
    _subtitleAsrModelCombo->addItem(QStringLiteral("whisper-large-v3"));
    _subtitleAsrModelCombo->addItem(QStringLiteral("whisper-1"));
    _subtitleAsrModelCombo->addItem(QStringLiteral("gpt-4o-transcribe"));
    subtitleLayout->addWidget(_subtitleAsrModelCombo);

    _testSubtitleAsrButton = new QPushButton(zh(u8"测试 ASR API"), subtitleCard);
    _testSubtitleAsrButton->setMinimumHeight(34);
    subtitleLayout->addWidget(_testSubtitleAsrButton);

    auto* translateProviderLabel = new QLabel(zh(u8"工作台 API（搜字幕 / 读画面字 / 翻译 / 修复 / 融合）"), subtitleCard);
    translateProviderLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    subtitleLayout->addWidget(translateProviderLabel);
    translateProviderLabel->hide();

    _subtitleTranslateProviderCombo = new QComboBox(subtitleCard);
    _subtitleTranslateProviderCombo->setEditable(false);
    _subtitleTranslateProviderCombo->setInsertPolicy(QComboBox::NoInsert);
    _subtitleTranslateProviderCombo->addItem(QStringLiteral("OpenAI compatible"), QStringLiteral("openai-compatible"));
    _subtitleTranslateProviderCombo->addItem(QStringLiteral("Qwen / DashScope"), QStringLiteral("qwen"));
    _subtitleTranslateProviderCombo->addItem(QStringLiteral("Mimo compatible"), QStringLiteral("mimo"));
    subtitleLayout->addWidget(_subtitleTranslateProviderCombo);
    _subtitleTranslateProviderCombo->hide();

    auto* translateLabel = new QLabel(zh(u8"工作台 API 地址"), subtitleCard);
    translateLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    subtitleLayout->addWidget(translateLabel);
    translateLabel->hide();

    _subtitleTranslateBaseUrlEdit = new QLineEdit(subtitleCard);
    _subtitleTranslateBaseUrlEdit->setPlaceholderText(zh(u8"工作台 Base URL，例如 DeepSeek / GPT 代理地址"));
    subtitleLayout->addWidget(_subtitleTranslateBaseUrlEdit);
    _subtitleTranslateBaseUrlEdit->hide();

    _subtitleTranslateApiKeyEdit = new QLineEdit(subtitleCard);
    _subtitleTranslateApiKeyEdit->setEchoMode(QLineEdit::Password);
    _subtitleTranslateApiKeyEdit->setPlaceholderText(zh(u8"工作台 API Key（留空继续使用已保存密钥）"));
    subtitleLayout->addWidget(_subtitleTranslateApiKeyEdit);
    _subtitleTranslateApiKeyEdit->hide();

    _subtitleTranslateModelCombo = new QComboBox(subtitleCard);
    _subtitleTranslateModelCombo->setEditable(true);
    _subtitleTranslateModelCombo->setInsertPolicy(QComboBox::NoInsert);
    subtitleLayout->addWidget(_subtitleTranslateModelCombo);
    _subtitleTranslateModelCombo->hide();

    auto* polishLabel = new QLabel(zh(u8"播放器只需要上方 Workbench API 和本处 ASR API：Workbench 负责搜字幕、读画面字、翻译、修复和融合；ASR 只在没有本地/在线/视觉字幕时听音频。"), subtitleCard);
    polishLabel->setWordWrap(true);
    polishLabel->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(kMuted));
    subtitleLayout->addWidget(polishLabel);

    _onlineSubtitleSearchCheck = new QCheckBox(zh(u8"高质量时让工作台尝试在线字幕/参考字幕"), subtitleCard);
    _onlineSubtitleSearchCheck->setChecked(false);
    subtitleLayout->addWidget(_onlineSubtitleSearchCheck);
    _onlineSubtitleSearchCheck->hide();

    _onlineSubtitleProviderCombo = new QComboBox(subtitleCard);
    _onlineSubtitleProviderCombo->setEditable(false);
    _onlineSubtitleProviderCombo->addItem(QStringLiteral("OpenSubtitles compatible"), QStringLiteral("opensubtitles-compatible"));
    _onlineSubtitleProviderCombo->addItem(QStringLiteral("Custom compatible"), QStringLiteral("custom-compatible"));
    subtitleLayout->addWidget(_onlineSubtitleProviderCombo);
    _onlineSubtitleProviderCombo->hide();

    _onlineSubtitleBaseUrlEdit = new QLineEdit(subtitleCard);
    _onlineSubtitleBaseUrlEdit->setPlaceholderText(QStringLiteral("https://api.opensubtitles.com/api/v1"));
    subtitleLayout->addWidget(_onlineSubtitleBaseUrlEdit);
    _onlineSubtitleBaseUrlEdit->hide();

    _onlineSubtitleApiKeyEdit = new QLineEdit(subtitleCard);
    _onlineSubtitleApiKeyEdit->setEchoMode(QLineEdit::Password);
    _onlineSubtitleApiKeyEdit->setPlaceholderText(zh(u8"在线字幕 API Key（留空继续使用已保存密钥）"));
    subtitleLayout->addWidget(_onlineSubtitleApiKeyEdit);
    _onlineSubtitleApiKeyEdit->hide();

    _onlineSubtitleLanguageEdit = new QLineEdit(subtitleCard);
    _onlineSubtitleLanguageEdit->setPlaceholderText(QStringLiteral("zh,ja,en"));
    subtitleLayout->addWidget(_onlineSubtitleLanguageEdit);
    _onlineSubtitleLanguageEdit->hide();

    _saveSubtitlePipelineButton = new QPushButton(zh(u8"保存 ASR 设置"), subtitleCard);
    _saveSubtitlePipelineButton->setMinimumHeight(36);
    subtitleLayout->addWidget(_saveSubtitlePipelineButton);
    settingsLayout->addWidget(subtitleCard);

    auto* resultCard = new QFrame(this);
    resultCard->setObjectName(QStringLiteral("AIAgentCard"));
    resultCard->setMinimumHeight(200);
    auto* resultLayout = new QVBoxLayout(resultCard);
    resultLayout->setContentsMargins(10, 10, 10, 10);
    resultLayout->setSpacing(6);

    auto* resultTitle = new QLabel(zh(u8"结果"), resultCard);
    resultTitle->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;color:%1;").arg(kText));
    resultLayout->addWidget(resultTitle);

    _resultView = new QTextBrowser(resultCard);
    _resultView->setOpenExternalLinks(false);
    _resultView->setHtml(
        QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"等待发送。"))));
    if (auto* document = _resultView->document()) {
        connect(document, &QTextDocument::contentsChanged, this, [this]() {
            scrollTextBrowserToBottom(_resultView);
        });
    }
    resultLayout->addWidget(_resultView, 1);
    auto* composerCard = new QFrame(this);
    composerCard->setObjectName(QStringLiteral("ComposerCard"));
    composerCard->setMinimumHeight(220);
    auto* composerLayout = new QVBoxLayout(composerCard);
    composerLayout->setContentsMargins(10, 10, 10, 10);
    composerLayout->setSpacing(6);

    auto* composerTitle = new QLabel(zh(u8"通用对话"), composerCard);
    composerTitle->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;color:%1;").arg(kText));
    composerLayout->addWidget(composerTitle);

    _promptEdit = new QPlainTextEdit(composerCard);
    _promptEdit->setPlaceholderText(
        zh(u8"例如：当前帧有什么问题？这个镜头需要怎么改？即使没打开视频也可以直接对话。"));
    _promptEdit->setMinimumHeight(112);
    _promptEdit->installEventFilter(this);
    composerLayout->addWidget(_promptEdit);

    _statusLabel = new QLabel(composerCard);
    _statusLabel->setWordWrap(true);
    _statusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    composerLayout->addWidget(_statusLabel);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(8);
    buttonRow->addStretch(1);

    _clearChatButton = new QPushButton(zh(u8"清空对话"), composerCard);
    buttonRow->addWidget(_clearChatButton);

    _exportReportButton = new QPushButton(zh(u8"导出报告"), composerCard);
    buttonRow->addWidget(_exportReportButton);

    _refreshButton = new QPushButton(zh(u8"刷新服务"), composerCard);
    buttonRow->addWidget(_refreshButton);

    _cancelButton = new QPushButton(zh(u8"取消"), composerCard);
    buttonRow->addWidget(_cancelButton);

    _runButton = new QPushButton(zh(u8"直接提问"), composerCard);
    _runButton->setObjectName(QStringLiteral("accent"));
    buttonRow->addWidget(_runButton);

    composerLayout->addLayout(buttonRow);

    auto* interactionSplitter = new QSplitter(Qt::Vertical, this);
    interactionSplitter->setChildrenCollapsible(false);
    interactionSplitter->setHandleWidth(10);
    interactionSplitter->setOpaqueResize(true);
    interactionSplitter->addWidget(resultCard);
    interactionSplitter->addWidget(composerCard);
    interactionSplitter->setStretchFactor(0, 3);
    interactionSplitter->setStretchFactor(1, 2);
    interactionSplitter->setSizes({ 400, 300 });
    rootLayout->addWidget(interactionSplitter, 1);

    connect(_refreshButton, &QPushButton::clicked, this, [this]() {
        _refreshProviders();
    });
    connect(_frameCriticButton, &QPushButton::clicked, this, [this]() {
        _submitFrameCriticAgent();
    });
    connect(_shotReviewButton, &QPushButton::clicked, this, [this]() {
        _submitShotReviewAgent();
    });
    connect(_sequenceReviewButton, &QPushButton::clicked, this, [this]() {
        _submitSequenceReviewAgent();
    });
    connect(_annotationSuggestButton, &QPushButton::clicked, this, [this]() {
        _submitAnnotationSuggestionAgent();
    });
    connect(_runButton, &QPushButton::clicked, this, [this]() {
        _submitCurrentFrameAnalysis();
    });
    connect(_cancelButton, &QPushButton::clicked, this, [this]() {
        _cancelCurrentAnalysis();
    });
    connect(_batchScanButton, &QPushButton::clicked, this, [this]() {
        _submitBatchScan();
    });
    connect(_qcButton, &QPushButton::clicked, this, [this]() {
        _submitQCScan();
    });
    connect(_generateReportButton, &QPushButton::clicked, this, [this]() {
        _submitReviewReport();
    });
    connect(_compareABButton, &QPushButton::clicked, this, [this]() {
        _submitCompareAB();
    });
    connect(_problemFramesButton, &QPushButton::clicked, this, [this]() {
        _showProblemFrames();
    });
    connect(_annotationSummaryButton, &QPushButton::clicked, this, [this]() {
        _showAnnotationSummary();
    });
    connect(_searchButton, &QPushButton::clicked, this, [this]() {
        _submitSmartSearch();
    });
    connect(_searchEdit, &QLineEdit::returnPressed, this, [this]() {
        _submitSmartSearch();
    });
    connect(_clearChatButton, &QPushButton::clicked, this, [this]() {
        _clearChatHistory();
    });
    connect(_exportReportButton, &QPushButton::clicked, this, [this]() {
        _exportReviewReport(_lastResponse);
    });
    connect(_resultView, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        _onResultAnchorClicked(url);
    });
    connect(_toolsToggleButton, &QPushButton::toggled, this, [this](bool expanded) {
        if (_toolsBodyFrame) {
            _toolsBodyFrame->setVisible(expanded);
        }
        if (_toolsToggleButton) {
            _toolsToggleButton->setText(expanded ? zh(u8"收起") : zh(u8"展开"));
        }
    });
    connect(_settingsToggleButton, &QPushButton::clicked, this, [this]() {
        if (_settingsDialog) {
            _loadProviderConfig();
            _loadSubtitlePipelineConfig();
            _refreshModels();
            _refreshActionState();
            _settingsDialog->show();
            _settingsDialog->raise();
            _settingsDialog->activateWindow();
        }
    });
    connect(_providerCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        _loadProviderConfig();
        _refreshModels();
        _storeSelections();
        _refreshActionState();
    });
    connect(_subtitleAsrProviderCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        const QString protocol = _subtitleAsrProviderCombo
            ? normalizeSubtitleAsrProtocol(_subtitleAsrProviderCombo->currentData().toString())
            : QStringLiteral("mimo");
        if (protocol == QStringLiteral("qwen")) {
            if (_subtitleAsrModelCombo && modelLooksUnsuitableForQwenAsr(_subtitleAsrModelCombo->currentText())) {
                _subtitleAsrModelCombo->setCurrentText(QString::fromLatin1(kQwenDefaultAsrModel));
            }
            if (_subtitleAsrBaseUrlEdit && _subtitleAsrBaseUrlEdit->text().trimmed().isEmpty()) {
                _subtitleAsrBaseUrlEdit->setText(QString::fromLatin1(kQwenDefaultAsrBaseUrl));
            }
        }
        if (_userSettings && _subtitleAsrProviderCombo) {
            _userSettings->setValue(
                QString::fromLatin1(kSubtitleAsrProtocolKey),
                protocol);
            _userSettings->sync();
        }
    });
    connect(_modelCombo, &QComboBox::currentTextChanged, this, [this](const QString&) {
        _storeSelections();
        _refreshActionState();
    });
    connect(_autoModelCheck, &QCheckBox::toggled, this, [this](bool) {
        _refreshModels();
        _storeSelections();
        _refreshActionState();
    });
    connect(_saveProviderConfigButton, &QPushButton::clicked, this, [this]() {
        _saveProviderConfig();
    });
    connect(_saveImageConfigButton, &QPushButton::clicked, this, [this]() {
        _saveImageGenerationConfig();
    });
    connect(_saveMediaConfigsButton, &QPushButton::clicked, this, [this]() { _saveMediaGenerationConfigs(); });
    connect(_testConnectionButton, &QPushButton::clicked, this, [this]() {
        _submitConnectionProbe();
    });
    connect(_clearProviderConfigButton, &QPushButton::clicked, this, [this]() {
        _clearStoredProviderCredential();
    });
    connect(_testSubtitleAsrButton, &QPushButton::clicked, this, [this]() {
        _testSubtitleAsrConfig();
    });
    connect(_saveSubtitlePipelineButton, &QPushButton::clicked, this, [this]() {
        _saveSubtitlePipelineConfig();
    });
    _applyThemeStyle();
}

void AIAgentWorkspace::_refreshProviders()
{
    const QString previousProviderId = _selectedProviderId();
    const QString configuredProviderId = _userSettings
        ? _userSettings->value(QStringLiteral("ai/workspace/providerId")).toString()
        : QString();

    _providerInfos = _providerManager ? _providerManager->providers() : QVector<AIProviderInfo>{};

    QSignalBlocker providerBlocker(_providerCombo);
    _providerCombo->clear();

    if (_providerInfos.isEmpty()) {
        _providerCombo->addItem(zh(u8"未检测到 AI 服务"), QString());
        _providerCombo->setEnabled(false);
    } else {
        _providerCombo->setEnabled(true);
        for (const auto& providerInfo : _providerInfos) {
            _providerCombo->addItem(providerDisplayText(providerInfo), providerInfo.providerId);
        }

        QString targetProviderId = previousProviderId;
        if (targetProviderId.isEmpty()) {
            targetProviderId = configuredProviderId;
        }
        if (targetProviderId.isEmpty() && _providerManager) {
            targetProviderId = _providerManager->defaultProviderId();
        }
        if (!targetProviderId.isEmpty()) {
            const int index = _providerCombo->findData(targetProviderId);
            if (index >= 0) {
                _providerCombo->setCurrentIndex(index);
            }
        }
    }

    _loadProviderConfig();
    _refreshModels();
    _refreshActionState();
}

void AIAgentWorkspace::_refreshModels()
{
    const bool autoModelSelection = _isAutoModelSelectionEnabled();
    const QString previousModel = _selectedModel();
    const QString configuredModel = _userSettings
        ? _userSettings->value(QStringLiteral("ai/workspace/model")).toString()
        : QString();
    const QString recommendedModel = _userSettings
        ? _userSettings->value(QString::fromLatin1(kRecommendedModelKey)).toString().trimmed()
        : QString();
    const QString persistedModel = _userSettings
        ? _userSettings->value(QString::fromLatin1(kGenericModelKey)).toString().trimmed()
        : QString();
    const AIProviderInfo providerInfo = _selectedProviderInfo();

    QSignalBlocker modelBlocker(_modelCombo);
    _modelCombo->clear();

    QStringList modelOptions = providerInfo.capabilities.supportedModels;
    const QStringList detectedApiModels = _userSettings
        ? _userSettings->value(QString::fromLatin1(kAvailableModelsKey)).toStringList()
        : QStringList{};
    for (const QString& model : detectedApiModels) {
        appendUniqueModel(&modelOptions, model);
    }
    appendUniqueModel(&modelOptions, recommendedModel);
    appendUniqueModel(&modelOptions, persistedModel);
    appendUniqueModel(&modelOptions, configuredModel);

    for (const QString& model : modelOptions) {
        _modelCombo->addItem(model);
    }

    QString targetModel;
    if (autoModelSelection) {
        targetModel = recommendedModel;
        if (targetModel.isEmpty() && !modelOptions.isEmpty()) {
            targetModel = modelOptions.front().trimmed();
        }
        if (targetModel.isEmpty()) {
            targetModel = configuredModel;
        }
        if (targetModel.isEmpty()) {
            targetModel = previousModel;
        }
    } else {
        targetModel = previousModel;
        if (targetModel.isEmpty()) {
            targetModel = configuredModel;
        }
        if (targetModel.isEmpty()) {
            targetModel = recommendedModel;
        }
    }

    if (!targetModel.isEmpty()) {
        int modelIndex = _modelCombo->findText(targetModel);
        if (modelIndex < 0) {
            _modelCombo->addItem(targetModel);
            modelIndex = _modelCombo->findText(targetModel);
        }
        if (modelIndex >= 0) {
            _modelCombo->setCurrentIndex(modelIndex);
        }
    } else if (_modelCombo->count() > 0) {
        _modelCombo->setCurrentIndex(0);
    }

    _modelCombo->setEnabled(true);
    _loadSubtitlePipelineConfig();
    _storeSelections();
}

void AIAgentWorkspace::_refreshContextSummary()
{
    IPlaybackService* playback = runtimePlaybackService(_playbackService);
    QString summary = formatFrameSummary(playback);
    const QString activeViewId = runtimeActiveViewId();
    if (!activeViewId.isEmpty()) {
        summary += zh(u8"\n当前视图：%1").arg(activeViewId);
    }
    _contextLabel->setText(summary);
}

void AIAgentWorkspace::_refreshActionState()
{
    const bool hasProvider = !_selectedProviderId().trimmed().isEmpty();
    const AIProviderInfo providerInfo = _selectedProviderInfo();
    const bool providerAvailable = hasProvider && providerInfo.available;
    const bool busy = !_activeJobId.isEmpty();
    const bool openAISelected = _selectedProviderId() == QString::fromLatin1(kOpenAIProviderId);
    const bool autoModelSelection = _isAutoModelSelectionEnabled();
    IPlaybackService* playback = runtimePlaybackService(_playbackService);
    const bool hasMedia = playback && playback->isValid();

    _runButton->setEnabled(!busy);
    _cancelButton->setEnabled(busy && _workflowService);

    if (_frameCriticButton) {
        _frameCriticButton->setEnabled(!busy);
    }
    if (_shotReviewButton) {
        _shotReviewButton->setEnabled(!busy);
    }
    if (_sequenceReviewButton) {
        _sequenceReviewButton->setEnabled(!busy);
    }
    if (_annotationSuggestButton) {
        _annotationSuggestButton->setEnabled(!busy);
    }

    // Tool buttons — always enabled, only disabled while AI is busy
    if (_batchScanButton) {
        _batchScanButton->setEnabled(!busy);
    }
    if (_qcButton) {
        _qcButton->setEnabled(!busy);
    }
    if (_generateReportButton) {
        _generateReportButton->setEnabled(!busy);
    }
    if (_compareABButton) {
        _compareABButton->setEnabled(!busy);
    }
    if (_problemFramesButton) {
        _problemFramesButton->setEnabled(!busy);
    }
    if (_annotationSummaryButton) {
        _annotationSummaryButton->setEnabled(!busy);
    }
    if (_exportReportButton) {
        _exportReportButton->setEnabled(!busy);
    }
    if (_searchButton) {
        _searchButton->setEnabled(!busy);
    }
    if (_searchEdit) {
        _searchEdit->setEnabled(!busy);
    }
    if (_scanRangeCombo) {
        _scanRangeCombo->setEnabled(!busy);
    }
    if (_sampleCountCombo) {
        _sampleCountCombo->setEnabled(!busy);
    }
    if (_autoModelCheck) {
        _autoModelCheck->setEnabled(!busy && hasProvider);
    }
    if (_modelCombo) {
        _modelCombo->setEnabled(!busy && hasProvider);
    }
    if (_manualModelFrame) {
        _manualModelFrame->setVisible(true);
    }
    if (_providerConfigFrame) {
        _providerConfigFrame->setVisible(openAISelected);
    }
    if (_saveProviderConfigButton) {
        _saveProviderConfigButton->setEnabled(openAISelected && _userSettings);
    }
    if (_clearProviderConfigButton) {
        _clearProviderConfigButton->setEnabled(openAISelected && _credentialStore);
    }
    if (_testConnectionButton) {
        _testConnectionButton->setEnabled(!busy && openAISelected && providerAvailable && _providerManager);
    }
    if (_testSubtitleAsrButton) {
        _testSubtitleAsrButton->setEnabled(!busy);
    }
    if (_saveSubtitlePipelineButton) {
        _saveSubtitlePipelineButton->setEnabled(!busy && _userSettings);
    }
    if (!providerAvailable && hasProvider && !busy && _workflowService) {
        _setStatusMessage(zh(u8"当前服务不可用。"), kWarning);
        return;
    }

    if (busy) {
        if (_activeJobMode == ActiveJobMode::ProbeConnection) {
            _setStatusMessage(zh(u8"正在测试连接..."), kWarning);
            return;
        }
        _setStatusMessage(zh(u8"AI 正在处理你的消息..."), kWarning);
        return;
    }
    if (!_workflowService) {
        _setStatusMessage(zh(u8"AI 工作流不可用。"), kError);
        return;
    }
    if (!hasProvider) {
        _setStatusMessage(zh(u8"尚未选择可用服务。"), kWarning);
        return;
    }
    if (hasMedia) {
        _setStatusMessage(zh(u8"就绪。可发送。"), kSuccess);
        return;
    }
    _setStatusMessage(zh(u8"就绪。可发送。"), kSuccess);
}


} // namespace cgplay
