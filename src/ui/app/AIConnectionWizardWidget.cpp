#include "AIConnectionWizardWidget.h"

#include "common/core/ServiceLocator.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"
#include "settings/api/ISettingsService.h"
#include "services/ai/api/IAIProviderDetector.h"
#include "services/ai/api/IAIProviderManager.h"
#include "services/ai/api/IAICredentialStore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <QtConcurrent>

namespace cgplay {

namespace {

constexpr auto kSurface = "#141920";
constexpr auto kBorder = "#252B33";
constexpr auto kText = "#D8DEE7";
constexpr auto kMuted = "#9AA4B2";
constexpr auto kAccent = "#FF8A3D";
constexpr auto kSuccess = "#7BD88F";
constexpr auto kWarning = "#FFC857";
constexpr auto kError = "#FF6B6B";
constexpr auto kProviderId = "openai";
constexpr auto kAutoModelSelectionKey = "ai/connection/autoSelectModel";
constexpr auto kAvailableModelsKey = "ai/connection/availableModels";
constexpr auto kRecommendedModelKey = "ai/connection/recommendedModel";
constexpr auto kGenericModelKey = "ai/connection/model";
constexpr auto kImageEndpointKey = "ai/imageGeneration/endpoint";
constexpr auto kImageModelKey = "ai/imageGeneration/model";
constexpr auto kImageCredentialId = "cgplay.ai.image-generation";

QString zh(const char* text)
{
    return QString::fromUtf8(text);
}

QString htmlEscape(const QString& text)
{
    return text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
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

QString localizedProviderType(const QString& providerType)
{
    const QString normalized = providerType.trimmed();
    if (normalized.compare(QStringLiteral("OpenAICompatible"), Qt::CaseInsensitive) == 0) {
        return zh(u8"OpenAI 兼容");
    }
    if (normalized.compare(QStringLiteral("Qwen"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("Qwen / DashScope");
    }
    if (normalized.compare(QStringLiteral("Gemini"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("Gemini");
    }
    if (normalized.compare(QStringLiteral("Claude"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("Claude");
    }
    if (normalized.compare(QStringLiteral("Ollama"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("Ollama");
    }
    if (normalized.compare(QStringLiteral("Unknown"), Qt::CaseInsensitive) == 0) {
        return zh(u8"未知");
    }
    return normalized;
}

QString localizedProviderName(const QString& providerName)
{
    const QString normalized = providerName.trimmed();
    if (normalized.compare(QStringLiteral("OpenAI Compatible"), Qt::CaseInsensitive) == 0) {
        return zh(u8"OpenAI 兼容网关");
    }
    if (normalized.compare(QStringLiteral("Qwen / DashScope"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("Qwen / DashScope");
    }
    if (normalized.compare(QStringLiteral("OpenAI Chat"), Qt::CaseInsensitive) == 0) {
        return zh(u8"OpenAI Chat 接口");
    }
    if (normalized.compare(QStringLiteral("OpenAI Responses"), Qt::CaseInsensitive) == 0) {
        return zh(u8"OpenAI Responses 接口");
    }
    if (normalized.compare(QStringLiteral("Anthropic Messages"), Qt::CaseInsensitive) == 0) {
        return zh(u8"Anthropic Messages 接口");
    }
    if (normalized.compare(QStringLiteral("Gemini GenerateContent"), Qt::CaseInsensitive) == 0) {
        return zh(u8"Gemini GenerateContent 接口");
    }
    if (normalized.compare(QStringLiteral("Ollama"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("Ollama");
    }
    return normalized;
}

QStringList fallbackModelOptions(IAIProviderManager* providerManager, ISettingsService* userSettings)
{
    QStringList models;
    if (providerManager) {
        const QVector<AIProviderInfo> providers = providerManager->providers();
        for (const AIProviderInfo& provider : providers) {
            if (provider.providerId == QString::fromLatin1(kProviderId)) {
                for (const QString& model : provider.capabilities.supportedModels) {
                    appendUniqueModel(&models, model);
                }
                break;
            }
        }
    }
    if (userSettings) {
        for (const QString& model :
             userSettings->value(QString::fromLatin1(kAvailableModelsKey)).toStringList()) {
            appendUniqueModel(&models, model);
        }
        appendUniqueModel(
            &models,
            userSettings->value(QString::fromLatin1(kRecommendedModelKey)).toString());
        appendUniqueModel(
            &models,
            userSettings->value(QString::fromLatin1(kGenericModelKey)).toString());
    }
    return models;
}

} // namespace

AIConnectionWizardWidget::AIConnectionWizardWidget(QWidget* parent)
    : QWidget(parent)
    , _detector(ServiceLocator::getService<IAIProviderDetector>())
    , _providerManager(ServiceLocator::getService<IAIProviderManager>())
    , _credentialStore(ServiceLocator::getService<IAICredentialStore>())
    , _eventBus(ServiceLocator::getService<IEventBus>())
    , _userSettings(ServiceLocator::getService<ISettingsService>())
{
    _setupUi();

    _detectWatcher = new QFutureWatcher<AIDetectionResult>(this);
    connect(_detectWatcher, &QFutureWatcher<AIDetectionResult>::finished, this, [this]() {
        _handleDetectionFinished();
    });

    refreshFromRuntime();
}

AIConnectionWizardWidget::~AIConnectionWizardWidget() = default;

void AIConnectionWizardWidget::refreshFromRuntime()
{
    if (!_detector) {
        _setStatus(zh(u8"AI 服务识别器不可用。"), QString::fromLatin1(kError));
        return;
    }

    const AIDetectionResult current = _detector->currentConfiguration();
    _lastResult = current;
    {
        QSignalBlocker blocker(_baseUrlEdit);
        _baseUrlEdit->setText(current.baseUrl);
    }
    {
        QSignalBlocker blocker(_apiKeyEdit);
        _apiKeyEdit->clear();
    }
    if (_userSettings) {
        _imageEndpointEdit->setText(_userSettings->value(QString::fromLatin1(kImageEndpointKey)).toString());
        _imageModelEdit->setText(_userSettings->value(QString::fromLatin1(kImageModelKey)).toString());
    }
    _imageApiKeyEdit->clear();
    const bool imageReady = _userSettings && _credentialStore &&
        !_imageEndpointEdit->text().trimmed().isEmpty() &&
        !_imageModelEdit->text().trimmed().isEmpty() &&
        _credentialStore->hasSecret(QString::fromLatin1(kImageCredentialId));
    _imageStatusLabel->setText(imageReady ? zh(u8"图片生成服务已配置。") : zh(u8"图片生成服务尚未配置。"));
    _imageStatusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(imageReady ? kSuccess : kMuted));
    if (_autoModelCheck) {
        QSignalBlocker blocker(_autoModelCheck);
        _autoModelCheck->setChecked(
            _userSettings
                ? _userSettings->value(QString::fromLatin1(kAutoModelSelectionKey), true).toBool()
                : true);
    }

    _hintLabel->setText(
        zh(u8"只需填写 API Key，或填写 API Key + Base URL。若 API Key 留空，将继续使用已保存的 Key 或环境变量。"));
    _applyDetectionResult(current);
    if (current.success) {
        _setStatus(zh(u8"当前 AI 连接已就绪。"), QString::fromLatin1(kSuccess));
    } else {
        _setStatus(zh(u8"请先点击自动识别，验证网关并获取模型列表。"), QString::fromLatin1(kMuted));
    }
}

void AIConnectionWizardWidget::_setupUi()
{
    setObjectName(QStringLiteral("AIConnectionWizardWidget"));
    setStyleSheet(QString(
        "#AIConnectionWizardWidget{background:transparent;color:%1;}"
        "#AIConnectionWizardWidget QFrame#Card{background:%2;border:1px solid rgba(255,255,255,0.05);border-radius:10px;}"
        "#AIConnectionWizardWidget QLabel{color:%1;}"
        "#AIConnectionWizardWidget QLineEdit,#AIConnectionWizardWidget QComboBox,#AIConnectionWizardWidget QTextBrowser{"
        "background:#111418;color:%1;border:1px solid %3;border-radius:8px;padding:6px;}"
        "#AIConnectionWizardWidget QPushButton{background:#202733;color:%1;border:1px solid rgba(255,255,255,0.06);"
        "border-radius:8px;padding:8px 12px;}"
        "#AIConnectionWizardWidget QPushButton:hover{background:#293241;border-color:rgba(255,140,61,0.35);}"
        "#AIConnectionWizardWidget QPushButton:disabled{color:%4;background:#1A2028;border-color:rgba(255,255,255,0.03);}"
        "#AIConnectionWizardWidget QPushButton#accent{background:%5;color:#111418;font-weight:700;}"
        "#AIConnectionWizardWidget QPushButton#accent:hover{background:#ff9d5c;}")
        .arg(kText, kSurface, kBorder, kMuted, kAccent));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("Card"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(12, 12, 12, 12);
    cardLayout->setSpacing(8);

    auto* title = new QLabel(zh(u8"AI 连接向导"), card);
    title->setStyleSheet(QStringLiteral("font-size:14px;font-weight:700;color:%1;").arg(kText));
    cardLayout->addWidget(title);

    _hintLabel = new QLabel(card);
    _hintLabel->setWordWrap(true);
    _hintLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    cardLayout->addWidget(_hintLabel);

    auto* apiLabel = new QLabel(QStringLiteral("API Key"), card);
    apiLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    cardLayout->addWidget(apiLabel);

    _apiKeyEdit = new QLineEdit(card);
    _apiKeyEdit->setEchoMode(QLineEdit::Password);
    _apiKeyEdit->setPlaceholderText(zh(u8"留空则继续使用已保存的 Key"));
    cardLayout->addWidget(_apiKeyEdit);

    auto* baseUrlLabel = new QLabel(QStringLiteral("Base URL"), card);
    baseUrlLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    cardLayout->addWidget(baseUrlLabel);

    _baseUrlEdit = new QLineEdit(card);
    _baseUrlEdit->setPlaceholderText(zh(u8"例如 https://api.openai.com 或 https://your-gateway/v1"));
    cardLayout->addWidget(_baseUrlEdit);

    auto* modelLabel = new QLabel(zh(u8"推荐模型 / 当前选择"), card);
    modelLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    cardLayout->addWidget(modelLabel);

    _autoModelCheck = new QCheckBox(zh(u8"自动选择推荐模型"), card);
    _autoModelCheck->setChecked(true);
    cardLayout->addWidget(_autoModelCheck);

    _modelCombo = new QComboBox(card);
    _modelCombo->setEditable(false);
    _modelCombo->setEnabled(false);
    cardLayout->addWidget(_modelCombo);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(8);

    _detectButton = new QPushButton(zh(u8"自动识别"), card);
    _detectButton->setObjectName(QStringLiteral("accent"));
    buttons->addWidget(_detectButton);

    _saveButton = new QPushButton(zh(u8"保存"), card);
    _saveButton->setEnabled(false);
    buttons->addWidget(_saveButton);

    cardLayout->addLayout(buttons);

    _statusLabel = new QLabel(card);
    _statusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    cardLayout->addWidget(_statusLabel);

    _resultView = new QTextBrowser(card);
    _resultView->setMinimumHeight(220);
    _resultView->setOpenExternalLinks(false);
    _resultView->setHtml(QStringLiteral("<div style='color:%1;'>%2</div>")
                             .arg(kMuted, htmlEscape(zh(u8"点击“自动识别”后，会验证连接、发现模型并保存推荐模型。"))));
    cardLayout->addWidget(_resultView, 1);

    auto* imageTitle = new QLabel(zh(u8"图片生成服务（独立 API）"), card);
    imageTitle->setStyleSheet(QStringLiteral("font-size:14px;font-weight:700;color:%1;margin-top:8px;").arg(kText));
    cardLayout->addWidget(imageTitle);

    auto* imageHint = new QLabel(zh(u8"用于中转站单独提供的 OpenAI-compatible Images API，不会覆盖上方文本 API。"), card);
    imageHint->setWordWrap(true);
    imageHint->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kMuted));
    cardLayout->addWidget(imageHint);

    cardLayout->addWidget(new QLabel(QStringLiteral("Image API Key"), card));
    _imageApiKeyEdit = new QLineEdit(card);
    _imageApiKeyEdit->setEchoMode(QLineEdit::Password);
    _imageApiKeyEdit->setPlaceholderText(zh(u8"留空则保留已安全保存的图片 Key"));
    cardLayout->addWidget(_imageApiKeyEdit);

    cardLayout->addWidget(new QLabel(zh(u8"图片生成 Endpoint"), card));
    _imageEndpointEdit = new QLineEdit(card);
    _imageEndpointEdit->setPlaceholderText(QStringLiteral("https://image-gateway.example/v1/images/generations"));
    cardLayout->addWidget(_imageEndpointEdit);

    cardLayout->addWidget(new QLabel(zh(u8"生图模型"), card));
    _imageModelEdit = new QLineEdit(card);
    _imageModelEdit->setPlaceholderText(QStringLiteral("gpt-image-1"));
    cardLayout->addWidget(_imageModelEdit);

    _saveImageButton = new QPushButton(zh(u8"保存图片生成服务"), card);
    _saveImageButton->setObjectName(QStringLiteral("accent"));
    cardLayout->addWidget(_saveImageButton);
    _imageStatusLabel = new QLabel(card);
    cardLayout->addWidget(_imageStatusLabel);

    root->addWidget(card);

    connect(_detectButton, &QPushButton::clicked, this, [this]() {
        _startDetection();
    });
    connect(_saveButton, &QPushButton::clicked, this, [this]() {
        _saveConfiguration();
    });
    connect(_autoModelCheck, &QCheckBox::toggled, this, [this](bool) {
        _syncModelSelectionMode(_lastResult);
    });
    connect(_saveImageButton, &QPushButton::clicked, this, [this]() {
        _saveImageConfiguration();
    });
}

void AIConnectionWizardWidget::_saveImageConfiguration()
{
    if (!_userSettings || !_credentialStore) {
        _imageStatusLabel->setText(zh(u8"图片配置或安全凭据服务不可用。"));
        _imageStatusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kError));
        return;
    }
    const QUrl endpoint(_imageEndpointEdit->text().trimmed());
    const QString model = _imageModelEdit->text().trimmed();
    if (!endpoint.isValid() || endpoint.isRelative() || endpoint.scheme() != QStringLiteral("https") ||
        endpoint.host().isEmpty() || !endpoint.userInfo().isEmpty() || model.isEmpty()) {
        _imageStatusLabel->setText(zh(u8"请填写有效的 HTTPS 图片 Endpoint 和模型。"));
        _imageStatusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kError));
        return;
    }
    const QString apiKey = _imageApiKeyEdit->text().trimmed();
    if (!apiKey.isEmpty()) {
        QString error;
        QByteArray secret = apiKey.toUtf8();
        const bool stored = _credentialStore->storeSecret(QString::fromLatin1(kImageCredentialId), secret, &error);
        secret.fill('\0');
        if (!stored) {
            _imageStatusLabel->setText(error.isEmpty() ? zh(u8"图片 API Key 安全保存失败。") : error);
            _imageStatusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kError));
            return;
        }
    }
    if (!_credentialStore->hasSecret(QString::fromLatin1(kImageCredentialId))) {
        _imageStatusLabel->setText(zh(u8"请填写图片 API Key。"));
        _imageStatusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kError));
        return;
    }
    _userSettings->setValue(QString::fromLatin1(kImageEndpointKey), endpoint.toString(QUrl::FullyEncoded));
    _userSettings->setValue(QString::fromLatin1(kImageModelKey), model);
    _userSettings->sync();
    _imageApiKeyEdit->clear();
    _imageStatusLabel->setText(zh(u8"图片生成服务已安全保存，重连 Codex 后生效。"));
    _imageStatusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(kSuccess));
}

void AIConnectionWizardWidget::_startDetection()
{
    if (!_detector || (_detectWatcher && _detectWatcher->isRunning())) {
        return;
    }

    _lastRequest.apiKey = _apiKeyEdit ? _apiKeyEdit->text().trimmed() : QString();
    _lastRequest.baseUrl = _baseUrlEdit ? _baseUrlEdit->text().trimmed() : QString();
    _lastRequest.model = _selectedModel();
    _setBusy(true);
    _setStatus(zh(u8"正在识别服务、验证协议并获取模型列表..."), QString::fromLatin1(kWarning));
    _resultView->setHtml(QStringLiteral("<div style='color:%1;'>%2<br/>%3<br/>%4<br/>%5</div>")
                             .arg(
                                 kMuted,
                                 htmlEscape(zh(u8"步骤 1/4：验证连接...")),
                                 htmlEscape(zh(u8"步骤 2/4：识别服务...")),
                                 htmlEscape(zh(u8"步骤 3/4：获取模型列表...")),
                                 htmlEscape(zh(u8"步骤 4/4：选择推荐模型..."))));

    auto* detector = _detector;
    const AIDetectionRequest request = _lastRequest;
    _detectWatcher->setFuture(QtConcurrent::run([detector, request]() {
        return detector->detect(request);
    }));
}

void AIConnectionWizardWidget::_handleDetectionFinished()
{
    _setBusy(false);
    if (!_detectWatcher) {
        return;
    }

    _lastResult = _detectWatcher->result();
    _applyDetectionResult(_lastResult);
    if (_lastResult.success) {
        _setStatus(zh(u8"连接验证通过。确认模型后点击保存。"), QString::fromLatin1(kSuccess));
    } else {
        _setStatus(zh(u8"自动识别失败，请查看下方详情。"), QString::fromLatin1(kError));
    }
}

void AIConnectionWizardWidget::_applyDetectionResult(const AIDetectionResult& result)
{
    QStringList models = result.models;
    appendUniqueModel(&models, result.recommendedModel);
    if (models.isEmpty()) {
        models = fallbackModelOptions(_providerManager, _userSettings);
    }

    const QString configuredModel = _userSettings
        ? _userSettings->value(QString::fromLatin1(kGenericModelKey)).toString().trimmed()
        : QString();
    appendUniqueModel(&models, configuredModel);

    const QString previousModel = _selectedModel();
    {
        QSignalBlocker blocker(_modelCombo);
        _modelCombo->clear();
        for (const QString& model : models) {
            _modelCombo->addItem(model);
        }

        QString targetModel = result.recommendedModel;
        if (targetModel.isEmpty()) {
            targetModel = configuredModel;
        }
        if (targetModel.isEmpty()) {
            targetModel = previousModel;
        }
        if (!targetModel.isEmpty()) {
            const int index = _modelCombo->findText(targetModel);
            if (index >= 0) {
                _modelCombo->setCurrentIndex(index);
            }
        } else if (_modelCombo->count() > 0) {
            _modelCombo->setCurrentIndex(0);
        }
    }

    _syncModelSelectionMode(result);
    _saveButton->setEnabled(result.success);
    _resultView->setHtml(_renderResultHtml(result));
}

void AIConnectionWizardWidget::_saveConfiguration()
{
    if (!_detector) {
        _setStatus(zh(u8"AI 服务识别器不可用。"), QString::fromLatin1(kError));
        return;
    }
    if (!_lastResult.success) {
        _setStatus(zh(u8"请先成功完成自动识别，再进行保存。"), QString::fromLatin1(kWarning));
        return;
    }

    AIDetectionResult resultToSave = _lastResult;
    const QString selectedModel = _selectedModel();
    if (_isAutoModelSelectionEnabled()) {
        if (resultToSave.recommendedModel.isEmpty() && !selectedModel.isEmpty()) {
            resultToSave.recommendedModel = selectedModel;
        }
    } else if (!selectedModel.isEmpty()) {
        resultToSave.recommendedModel = selectedModel;
    }

    QString error;
    if (!_detector->saveConfiguration(_lastRequest, resultToSave, &error)) {
        _setStatus(error.isEmpty() ? zh(u8"保存 AI 配置失败。") : error, QString::fromLatin1(kError));
        return;
    }

    if (_userSettings) {
        _userSettings->setValue(
            QString::fromLatin1(kAutoModelSelectionKey),
            _isAutoModelSelectionEnabled());
        _userSettings->sync();
    }

    _lastResult = resultToSave;
    _publishAvailabilityRefresh();
    refreshFromRuntime();
    _setStatus(zh(u8"AI 配置已保存。"), QString::fromLatin1(kSuccess));
}

void AIConnectionWizardWidget::_setBusy(bool busy)
{
    if (_detectButton) {
        _detectButton->setEnabled(!busy && _detector);
    }
    if (_saveButton) {
        _saveButton->setEnabled(!busy && _lastResult.success);
    }
    if (_modelCombo) {
        _modelCombo->setEnabled(!busy && _modelCombo->count() > 0 && !_isAutoModelSelectionEnabled());
    }
}

void AIConnectionWizardWidget::_setStatus(const QString& text, const QString& color)
{
    if (!_statusLabel) {
        return;
    }
    _statusLabel->setText(text);
    _statusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(color));
}

QString AIConnectionWizardWidget::_selectedModel() const
{
    return _modelCombo ? _modelCombo->currentText().trimmed() : QString();
}

bool AIConnectionWizardWidget::_isAutoModelSelectionEnabled() const
{
    if (_autoModelCheck) {
        return _autoModelCheck->isChecked();
    }
    return _userSettings
        ? _userSettings->value(QString::fromLatin1(kAutoModelSelectionKey), true).toBool()
        : true;
}

void AIConnectionWizardWidget::_syncModelSelectionMode(const AIDetectionResult& result)
{
    if (!_modelCombo) {
        return;
    }

    if (_isAutoModelSelectionEnabled() && !result.recommendedModel.isEmpty()) {
        const int index = _modelCombo->findText(result.recommendedModel);
        if (index >= 0) {
            _modelCombo->setCurrentIndex(index);
        }
    }
    _modelCombo->setEnabled(_modelCombo->count() > 0 && !_isAutoModelSelectionEnabled());
}

QString AIConnectionWizardWidget::_renderResultHtml(const AIDetectionResult& result) const
{
    if (result.providerType.isEmpty() && result.error.isEmpty()) {
        return QStringLiteral("<div style='color:%1;'>%2</div>")
            .arg(kMuted, htmlEscape(zh(u8"点击“自动识别”后，会验证连接、发现模型并保存推荐模型。")));
    }

    QStringList models = result.models;
    appendUniqueModel(&models, result.recommendedModel);
    if (models.isEmpty()) {
        models = fallbackModelOptions(_providerManager, _userSettings);
    }

    QString html;
    const QString titleColor = result.success ? QString::fromLatin1(kSuccess) : QString::fromLatin1(kError);
    const QString title = result.success ? zh(u8"连接成功") : zh(u8"识别结果");
    html += QStringLiteral("<h3 style='margin:0 0 8px 0;color:%1;'>%2</h3>").arg(titleColor, title);
    html += QStringLiteral(
        "<div style='color:%1;margin-bottom:10px;'>服务：%2<br/>服务类型：%3<br/>Base URL：%4</div>")
        .arg(
            kMuted,
            htmlEscape(localizedProviderName(result.providerName.isEmpty() ? zh(u8"未知") : result.providerName)),
            htmlEscape(localizedProviderType(result.providerType.isEmpty() ? zh(u8"未知") : result.providerType)),
            htmlEscape(result.baseUrl.isEmpty() ? zh(u8"（自动）") : result.baseUrl));

    if (!models.isEmpty()) {
        html += QStringLiteral("<div style='font-weight:600;color:%1;margin-bottom:4px;'>发现的模型</div><ul>").arg(kText);
        for (const QString& model : models) {
            html += QStringLiteral("<li style='color:%1;'>%2</li>").arg(kMuted, htmlEscape(model));
        }
        html += QStringLiteral("</ul>");
    }

    if (!result.recommendedModel.isEmpty()) {
        html += QStringLiteral(
            "<div style='margin-top:6px;color:%1;'>推荐模型：<b>%2</b></div>")
            .arg(kSuccess, htmlEscape(result.recommendedModel));
    }

    if (!result.detectedProtocol.isEmpty() || !result.detectedEndpoint.isEmpty()) {
        html += QStringLiteral(
            "<div style='margin-top:6px;color:%1;'>协议：%2<br/>接口地址：%3</div>")
            .arg(
                kMuted,
                htmlEscape(result.detectedProtocol.isEmpty() ? QStringLiteral("--") : localizedProviderName(result.detectedProtocol)),
                htmlEscape(result.detectedEndpoint.isEmpty() ? QStringLiteral("--") : result.detectedEndpoint));
    }

    if (!result.error.isEmpty()) {
        html += QStringLiteral("<div style='margin-top:10px;color:%1;'>%2</div>")
            .arg(kError, htmlEscape(result.error));
    }
    return html;
}

void AIConnectionWizardWidget::_publishAvailabilityRefresh() const
{
    if (!_eventBus || !_providerManager) {
        return;
    }

    bool available = false;
    const QVector<AIProviderInfo> providers = _providerManager->providers();
    for (const AIProviderInfo& provider : providers) {
        if (provider.providerId == QString::fromLatin1(kProviderId)) {
            available = provider.available;
            break;
        }
    }
    _eventBus->publish(AIProviderAvailabilityChangedEvent{
        QString::fromLatin1(kProviderId),
        available
    });
}

} // namespace cgplay
