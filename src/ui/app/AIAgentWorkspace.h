#pragma once

#include "common/events/api/IEventBus.h"
#include "services/ai/api/AIProviderTypes.h"

#include <QJsonObject>
#include <QWidget>
#include <QVector>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QDialog;
class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;
class QPlainTextEdit;
class QFrame;
QT_END_NAMESPACE

namespace cgplay {

class IAICredentialStore;
class IAIProviderManager;
class IAIWorkflowService;
class IAnnotationService;
class IPlaybackService;
class ISettingsService;

class AIAgentWorkspace final : public QWidget
{
public:
    explicit AIAgentWorkspace(
        IAIProviderManager* providerManager,
        IAIWorkflowService* workflowService,
        IPlaybackService* playbackService,
        IAICredentialStore* credentialStore,
        ISettingsService* userSettings,
        IEventBus* eventBus,
        IAnnotationService* annotationService = nullptr,
        QWidget* parent = nullptr);
    ~AIAgentWorkspace() override;

    void refreshFromRuntime();
    QJsonObject runQwenAsrProviderSmoke();
    bool saveSettingsDialogSmokeScreenshot(const QString& outputPath) const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    enum class ActiveJobMode
    {
        None,
        AnalyzeFrame,
        AnalyzeShot,
        AnalyzeSequence,
        SuggestAnnotations,
        GenerateSummary,
        QCScan,
        SmartSearch,
        CompareAB,
        ProbeConnection
    };

    void _setupUi();
    void _applyThemeStyle();
    void _refreshProviders();
    void _refreshModels();
    void _refreshContextSummary();
    void _refreshActionState();
    void _submitFrameCriticAgent();
    void _submitShotReviewAgent();
    void _submitSequenceReviewAgent();
    void _submitAnnotationSuggestionAgent();
    void _submitCurrentFrameAnalysis();
    void _submitBatchScan();
    void _submitReviewReport();
    void _submitQCScan();
    void _submitSmartSearch();
    void _submitCompareAB();
    void _showProblemFrames();
    void _showAnnotationSummary();
    void _submitConnectionProbe();
    void _cancelCurrentAnalysis();
    void _loadProviderConfig();
    void _saveProviderConfig();
    void _clearStoredProviderCredential();
    void _loadImageGenerationConfig();
    void _saveImageGenerationConfig();
    void _loadMediaGenerationConfigs();
    void _saveMediaGenerationConfigs();
    void _loadSubtitlePipelineConfig();
    void _saveSubtitlePipelineConfig();
    void _testSubtitleAsrConfig();
    void _storeSelections() const;
    void _setStatusMessage(const QString& text, const QString& colorHex);
    bool _isAutoModelSelectionEnabled() const;
    QString _requestModel() const;
    QString _selectedProviderId() const;
    QString _selectedModel() const;
    QString _defaultPrompt() const;
    AIProviderInfo _selectedProviderInfo() const;
    QString _renderResponseHtml(const AIResponse& response) const;
    QString _renderChatHtml() const;
    QString _renderSearchResults(const QVector<AISearchResult>& results) const;
    QString _renderProblemFramesHtml() const;
    QString _renderAnnotationSummaryHtml() const;
    QString _renderFailureHtml(const QString& title, const QString& detail) const;
    QString _buildRuntimeControlSummary() const;
    QJsonObject _buildRuntimeControlState() const;
    QString _buildConversationContextSummary(bool compact = false) const;
    QVector<AIChatMessage> _trimmedChatHistory(int maxMessages) const;
    bool _tryHandleLocalControlPrompt(const QString& prompt);
    QString _stripAiActionBlock(const QString& text, QString* actionPayload = nullptr) const;
    QStringList _executeAiControlActions(const QString& actionPayload) const;
    static QString _severityText(AISeverity severity);
    bool _shouldUseVisualContextForPrompt(const QString& prompt) const;
    void _seedPromptIfEmpty(const QString& text);
    void _setBatchScope(AIRequestScope scope);
    QVector<AIAnnotationSuggestion> _parseAnnotationSuggestions(
        const QString& rawText,
        int fallbackFrame) const;
    void _onResultAnchorClicked(const QUrl& url);
    void _applyAnnotationSuggestion(int index);
    void _applyAllAnnotationSuggestions();
    void _clearChatHistory();
    void _exportReviewReport(const AIResponse& response);

    IAIProviderManager* _providerManager = nullptr;
    IAIWorkflowService* _workflowService = nullptr;
    IPlaybackService* _playbackService = nullptr;
    IAICredentialStore* _credentialStore = nullptr;
    ISettingsService* _userSettings = nullptr;
    IEventBus* _eventBus = nullptr;
    IAnnotationService* _annotationService = nullptr;

    QLabel* _contextLabel = nullptr;
    QLabel* _statusLabel = nullptr;
    QLabel* _phaseNoteLabel = nullptr;
    QPushButton* _settingsToggleButton = nullptr;
    QPushButton* _toolsToggleButton = nullptr;
    QDialog* _settingsDialog = nullptr;
    QFrame* _toolsBodyFrame = nullptr;
    QComboBox* _providerCombo = nullptr;
    QComboBox* _modelCombo = nullptr;
    QCheckBox* _autoModelCheck = nullptr;
    QFrame* _manualModelFrame = nullptr;
    QFrame* _providerConfigFrame = nullptr;
    QLabel* _providerConfigHintLabel = nullptr;
    QLineEdit* _apiKeyEdit = nullptr;
    QLineEdit* _baseUrlEdit = nullptr;
    QPushButton* _saveProviderConfigButton = nullptr;
    QPushButton* _clearProviderConfigButton = nullptr;
    QPushButton* _testConnectionButton = nullptr;
    QLineEdit* _imageApiKeyEdit = nullptr;
    QLineEdit* _imageEndpointEdit = nullptr;
    QLineEdit* _imageModelEdit = nullptr;
    QPushButton* _saveImageConfigButton = nullptr;
    QLabel* _imageConfigStatusLabel = nullptr;
    QLineEdit* _videoEndpointEdit = nullptr;
    QLineEdit* _videoModelEdit = nullptr;
    QLineEdit* _videoApiKeyEdit = nullptr;
    QLineEdit* _videoStatusEndpointEdit = nullptr;
    QLineEdit* _audioEndpointEdit = nullptr;
    QLineEdit* _audioModelEdit = nullptr;
    QLineEdit* _audioApiKeyEdit = nullptr;
    QLineEdit* _audioStatusEndpointEdit = nullptr;
    QLineEdit* _imageEditEndpointEdit = nullptr;
    QLineEdit* _imageEditModelEdit = nullptr;
    QLineEdit* _imageEditApiKeyEdit = nullptr;
    QPushButton* _saveMediaConfigsButton = nullptr;
    QLabel* _mediaConfigsStatusLabel = nullptr;
    QLineEdit* _subtitleAsrBaseUrlEdit = nullptr;
    QLineEdit* _subtitleAsrApiKeyEdit = nullptr;
    QComboBox* _subtitleAsrModelCombo = nullptr;
    QComboBox* _subtitleAsrProviderCombo = nullptr;
    QComboBox* _subtitleTranslateProviderCombo = nullptr;
    QLineEdit* _subtitleTranslateBaseUrlEdit = nullptr;
    QLineEdit* _subtitleTranslateApiKeyEdit = nullptr;
    QComboBox* _subtitleTranslateModelCombo = nullptr;
    QCheckBox* _onlineSubtitleSearchCheck = nullptr;
    QComboBox* _onlineSubtitleProviderCombo = nullptr;
    QLineEdit* _onlineSubtitleBaseUrlEdit = nullptr;
    QLineEdit* _onlineSubtitleApiKeyEdit = nullptr;
    QLineEdit* _onlineSubtitleLanguageEdit = nullptr;
    QPushButton* _testSubtitleAsrButton = nullptr;
    QPushButton* _saveSubtitlePipelineButton = nullptr;

    // Agent shortcuts (all visible simultaneously)
    QPushButton* _frameCriticButton = nullptr;
    QPushButton* _shotReviewButton = nullptr;
    QPushButton* _sequenceReviewButton = nullptr;
    QPushButton* _annotationSuggestButton = nullptr;

    // Quick-action tools (all visible simultaneously)
    QPushButton* _batchScanButton = nullptr;
    QPushButton* _qcButton = nullptr;
    QPushButton* _generateReportButton = nullptr;
    QPushButton* _compareABButton = nullptr;
    QPushButton* _problemFramesButton = nullptr;
    QPushButton* _annotationSummaryButton = nullptr;

    // Batch scan controls
    QComboBox* _scanRangeCombo = nullptr;
    QComboBox* _sampleCountCombo = nullptr;

    // Smart search
    QLineEdit* _searchEdit = nullptr;
    QPushButton* _searchButton = nullptr;

    // Chat / prompt
    QPlainTextEdit* _promptEdit = nullptr;
    QPushButton* _runButton = nullptr;
    QPushButton* _cancelButton = nullptr;
    QPushButton* _refreshButton = nullptr;
    QPushButton* _clearChatButton = nullptr;
    QPushButton* _exportReportButton = nullptr;
    QTextBrowser* _resultView = nullptr;

    QVector<AIProviderInfo> _providerInfos;
    QString _activeJobId;
    ActiveJobMode _activeJobMode = ActiveJobMode::None;

    // Multi-turn chat history
    QVector<AIChatMessage> _chatHistory;
    QString _activeSubmittedPrompt;

    // Cached last response for annotation application
    AIResponse _lastResponse;

    // Cached search results
    QVector<AISearchResult> _lastSearchResults;

    IEventBus::SubscriptionId _analysisCompletedSubscription = 0;
    IEventBus::SubscriptionId _analysisFailedSubscription = 0;
    IEventBus::SubscriptionId _providerAvailabilitySubscription = 0;
    IEventBus::SubscriptionId _modelListUpdatedSubscription = 0;
    bool _applyingThemeStyle = false;
};

} // namespace cgplay
