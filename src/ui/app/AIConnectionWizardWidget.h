#pragma once

#include "services/ai/api/AIProviderTypes.h"

#include <QFutureWatcher>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;
QT_END_NAMESPACE

namespace cgplay {

class IAIProviderDetector;
class IAIProviderManager;
class IAICredentialStore;
class IEventBus;
class ISettingsService;

class AIConnectionWizardWidget final : public QWidget
{
public:
    explicit AIConnectionWizardWidget(QWidget* parent = nullptr);
    ~AIConnectionWizardWidget() override;

    void refreshFromRuntime();

private:
    void _setupUi();
    void _startDetection();
    void _handleDetectionFinished();
    void _applyDetectionResult(const AIDetectionResult& result);
    void _saveConfiguration();
    void _saveImageConfiguration();
    void _setBusy(bool busy);
    void _setStatus(const QString& text, const QString& color);
    bool _isAutoModelSelectionEnabled() const;
    void _syncModelSelectionMode(const AIDetectionResult& result);
    QString _selectedModel() const;
    QString _renderResultHtml(const AIDetectionResult& result) const;
    void _publishAvailabilityRefresh() const;

    IAIProviderDetector* _detector = nullptr;
    IAIProviderManager* _providerManager = nullptr;
    IAICredentialStore* _credentialStore = nullptr;
    IEventBus* _eventBus = nullptr;
    ISettingsService* _userSettings = nullptr;

    QLabel* _hintLabel = nullptr;
    QLabel* _statusLabel = nullptr;
    QLineEdit* _apiKeyEdit = nullptr;
    QLineEdit* _baseUrlEdit = nullptr;
    QPushButton* _detectButton = nullptr;
    QPushButton* _saveButton = nullptr;
    QCheckBox* _autoModelCheck = nullptr;
    QComboBox* _modelCombo = nullptr;
    QTextBrowser* _resultView = nullptr;
    QLineEdit* _imageApiKeyEdit = nullptr;
    QLineEdit* _imageEndpointEdit = nullptr;
    QLineEdit* _imageModelEdit = nullptr;
    QPushButton* _saveImageButton = nullptr;
    QLabel* _imageStatusLabel = nullptr;

    QFutureWatcher<AIDetectionResult>* _detectWatcher = nullptr;
    AIDetectionRequest _lastRequest;
    AIDetectionResult _lastResult;
};

} // namespace cgplay
