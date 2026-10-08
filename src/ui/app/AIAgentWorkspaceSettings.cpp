#include "AIAgentWorkspace.h"
#include "AIAgentWorkspaceSupport.h"

#include "services/ai/SubtitleAsrApiClient.h"
#include "services/ai/api/IAICredentialStore.h"
#include "services/ai/api/IAIProviderManager.h"
#include "settings/api/ISettingsService.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPixmap>
#include <QSignalBlocker>
#include <QUrl>
#include <QWidget>

namespace cgplay {

using namespace ai_agent_workspace_support;

void AIAgentWorkspace::_storeSelections() const
{
    if (!_userSettings) {
        return;
    }
    const QString providerId = _selectedProviderId();
    const QString model = _selectedModel();
    const bool autoModelSelection = _isAutoModelSelectionEnabled();
    const QString recommendedModel =
        _userSettings->value(QString::fromLatin1(kRecommendedModelKey)).toString().trimmed();
    const QStringList availableModels =
        _userSettings->value(QString::fromLatin1(kAvailableModelsKey)).toStringList();
    const bool hasDetectedModels =
        !recommendedModel.isEmpty() || !availableModels.isEmpty();
    const QString modelToPersist = autoModelSelection
        ? (hasDetectedModels ? (recommendedModel.isEmpty() ? model : recommendedModel) : QString())
        : model;
    const QString workspaceModel = autoModelSelection && !hasDetectedModels
        ? QString()
        : model;

    _userSettings->setValue(QStringLiteral("ai/workspace/providerId"), providerId);
    _userSettings->setValue(QStringLiteral("ai/workspace/model"), workspaceModel);
    _userSettings->setValue(QString::fromLatin1(kAutoModelSelectionKey), autoModelSelection);
    if (providerId == QString::fromLatin1(kOpenAIProviderId)) {
        if (modelToPersist.isEmpty()) {
            _userSettings->remove(QString::fromLatin1(kGenericModelKey));
            _userSettings->remove(QString::fromLatin1(kOpenAIModelKey));
        } else {
            _userSettings->setValue(QString::fromLatin1(kGenericModelKey), modelToPersist);
            _userSettings->setValue(QString::fromLatin1(kOpenAIModelKey), modelToPersist);
        }
        const QString smartBaseUrl =
            _userSettings->value(QString::fromLatin1(kGenericBaseUrlKey)).toString().trimmed();
        if (smartBaseUrl.isEmpty()) {
            _userSettings->remove(QString::fromLatin1(kSubtitleTranslateBaseUrlKey));
        } else {
            _userSettings->setValue(QString::fromLatin1(kSubtitleTranslateBaseUrlKey), smartBaseUrl);
        }
        _userSettings->setValue(QString::fromLatin1(kSubtitleTranslateProviderKey), QStringLiteral("openai-compatible"));
        if (modelToPersist.isEmpty()) {
            _userSettings->remove(QString::fromLatin1(kSubtitleTranslateModelKey));
        } else {
            _userSettings->setValue(QString::fromLatin1(kSubtitleTranslateModelKey), modelToPersist);
        }
    }
}

void AIAgentWorkspace::_loadProviderConfig()
{
    if (!_providerConfigFrame || !_providerConfigHintLabel || !_baseUrlEdit || !_apiKeyEdit) {
        return;
    }

    const bool isOpenAI = _selectedProviderId() == QString::fromLatin1(kOpenAIProviderId);
    _providerConfigFrame->setVisible(isOpenAI);
    if (!isOpenAI) {
        return;
    }

    QString baseUrl;
    QString detectedProtocol;
    QString detectedProvider;
    QString detectedEndpoint;
    bool autoModelSelection = true;
    if (_userSettings) {
        baseUrl = _userSettings->value(QString::fromLatin1(kGenericBaseUrlKey)).toString().trimmed();
        if (baseUrl.isEmpty()) {
            baseUrl = _userSettings->value(QString::fromLatin1(kOpenAIBaseUrlKey)).toString().trimmed();
        }
        detectedProtocol = _userSettings->value(QString::fromLatin1(kDetectedProtocolKey)).toString().trimmed();
        detectedProvider = _userSettings->value(QString::fromLatin1(kDetectedProviderKey)).toString().trimmed();
        detectedEndpoint = _userSettings->value(QString::fromLatin1(kDetectedEndpointKey)).toString().trimmed();
        autoModelSelection =
            _userSettings->value(QString::fromLatin1(kAutoModelSelectionKey), true).toBool();
    }

    {
        QSignalBlocker blocker(_baseUrlEdit);
        _baseUrlEdit->setText(baseUrl);
    }
    {
        QSignalBlocker blocker(_apiKeyEdit);
        _apiKeyEdit->clear();
    }
    if (_autoModelCheck) {
        QSignalBlocker blocker(_autoModelCheck);
        _autoModelCheck->setChecked(autoModelSelection);
    }

    QStringList hintLines;
    hintLines << zh(u8"先选协议，再填 Base URL 和 API Key。");
    if (_credentialStore && (_credentialStore->hasSecret(QString::fromLatin1(kGenericCredentialId)) ||
                             _credentialStore->hasSecret(QString::fromLatin1(kOpenAICredentialId)))) {
        hintLines.clear();
        hintLines << zh(u8"已检测到保存的 API Key。输入框留空即可继续使用。");
    } else if (!qgetenv("AI_API_KEY").trimmed().isEmpty() ||
               !qgetenv("OPENAI_API_KEY").trimmed().isEmpty() ||
               !qgetenv("AZURE_OPENAI_API_KEY").trimmed().isEmpty() ||
               !qgetenv("ANTHROPIC_API_KEY").trimmed().isEmpty() ||
               !qgetenv("GEMINI_API_KEY").trimmed().isEmpty() ||
               !qgetenv("GOOGLE_API_KEY").trimmed().isEmpty()) {
        hintLines.clear();
        hintLines << zh(u8"当前正在使用环境变量中的 API Key。");
    }
    if (!baseUrl.trimmed().isEmpty()) {
        hintLines << zh(u8"当前已启用自定义 Base URL。");
    }
    if (!detectedProvider.isEmpty()) {
        hintLines << zh(u8"最近识别结果：%1。").arg(detectedProvider);
    } else if (!detectedProtocol.isEmpty()) {
        hintLines << zh(u8"最近识别协议：%1。").arg(detectedProtocol);
    }
    if (!detectedEndpoint.isEmpty()) {
        hintLines << zh(u8"接口地址：%1。").arg(detectedEndpoint);
    }
    hintLines << (autoModelSelection
        ? zh(u8"当前模型选择模式：自动。")
        : zh(u8"当前模型选择模式：手动。"));
    _providerConfigHintLabel->setText(hintLines.join(QStringLiteral("\n")));
}

void AIAgentWorkspace::_saveProviderConfig()
{
    if (_selectedProviderId() != QString::fromLatin1(kOpenAIProviderId) || !_userSettings) {
        return;
    }

    const QString baseUrl = _baseUrlEdit ? _baseUrlEdit->text().trimmed() : QString();
    if (hasSensitiveCredentialQuery(baseUrl)) {
        _setStatusMessage(
            zh(u8"Base URL 不能包含 API Key 或访问令牌，请使用安全凭据输入框。"),
            kError);
        return;
    }
    if (baseUrl.isEmpty()) {
        _userSettings->remove(QString::fromLatin1(kGenericBaseUrlKey));
        _userSettings->remove(QString::fromLatin1(kOpenAIBaseUrlKey));
    } else {
        _userSettings->setValue(QString::fromLatin1(kGenericBaseUrlKey), baseUrl);
        _userSettings->setValue(QString::fromLatin1(kOpenAIBaseUrlKey), baseUrl);
    }
    _userSettings->remove(QString::fromLatin1(kDetectedProtocolKey));
    _userSettings->remove(QString::fromLatin1(kDetectedProviderKey));
    _userSettings->remove(QString::fromLatin1(kDetectedEndpointKey));
    _userSettings->remove(QString::fromLatin1(kResponsesEndpointKey));
    _userSettings->remove(QString::fromLatin1(kCompatibleProtocolsKey));
    _userSettings->remove(QString::fromLatin1(kConnectionDiagnosticsKey));
    _userSettings->remove(QString::fromLatin1(kProviderTypeKey));
    _userSettings->remove(QString::fromLatin1(kProviderNameKey));
    _userSettings->remove(QString::fromLatin1(kAvailableModelsKey));
    _userSettings->remove(QString::fromLatin1(kRecommendedModelKey));

    if (_credentialStore && _apiKeyEdit) {
        const QByteArray apiKey = _apiKeyEdit->text().trimmed().toUtf8();
        if (!apiKey.isEmpty()) {
            QString error;
            if (!_credentialStore->storeSecret(QString::fromLatin1(kGenericCredentialId), apiKey, &error) ||
                !_credentialStore->storeSecret(QString::fromLatin1(kOpenAICredentialId), apiKey, &error)) {
                _setStatusMessage(
                    error.isEmpty() ? zh(u8"保存 API Key 失败。") : error,
                    kError);
                return;
            }
            _apiKeyEdit->clear();
        }
    }

    _storeSelections();
    _userSettings->sync();
    _refreshProviders();
    _loadProviderConfig();
    _refreshActionState();
    _setStatusMessage(zh(u8"Workbench 连接已保存。"), kSuccess);
}

void AIAgentWorkspace::_loadImageGenerationConfig()
{
    if (!_userSettings || !_imageEndpointEdit || !_imageModelEdit || !_imageApiKeyEdit || !_imageConfigStatusLabel) return;
    _imageEndpointEdit->setText(_userSettings->value(QString::fromLatin1(kImageEndpointKey)).toString().trimmed());
    _imageModelEdit->setText(_userSettings->value(QString::fromLatin1(kImageModelKey), QStringLiteral("gpt-image-1")).toString().trimmed());
    _imageApiKeyEdit->clear();
    const bool ready = _credentialStore &&
        _credentialStore->hasSecret(QString::fromLatin1(kImageCredentialId)) &&
        !_imageEndpointEdit->text().trimmed().isEmpty() && !_imageModelEdit->text().trimmed().isEmpty();
    _imageConfigStatusLabel->setText(ready ? zh(u8"图片生成 API 已配置。") : zh(u8"填写完整后保存，重连 Codex 即可生效。"));
    _imageConfigStatusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(ready ? kSuccess : kMuted));
}

void AIAgentWorkspace::_saveImageGenerationConfig()
{
    if (!_userSettings || !_credentialStore) return;
    const QUrl endpoint(_imageEndpointEdit ? _imageEndpointEdit->text().trimmed() : QString());
    const QString model = _imageModelEdit ? _imageModelEdit->text().trimmed() : QString();
    if (!endpoint.isValid() || endpoint.isRelative() || endpoint.scheme() != QStringLiteral("https") ||
        endpoint.host().isEmpty() || !endpoint.userInfo().isEmpty() || hasSensitiveCredentialQuery(endpoint.toString()) || model.isEmpty()) {
        _imageConfigStatusLabel->setText(zh(u8"请填写不含密钥的有效 HTTPS Endpoint 和生图模型。"));
        _imageConfigStatusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(kError));
        return;
    }
    if (_imageApiKeyEdit) {
        QByteArray key = _imageApiKeyEdit->text().trimmed().toUtf8();
        if (!key.isEmpty()) {
            QString error;
            const bool stored = _credentialStore->storeSecret(QString::fromLatin1(kImageCredentialId), key, &error);
            key.fill('\0');
            if (!stored) {
                _imageConfigStatusLabel->setText(error.isEmpty() ? zh(u8"图片 API Key 安全保存失败。") : error);
                _imageConfigStatusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(kError));
                return;
            }
            _imageApiKeyEdit->clear();
        }
    }
    if (!_credentialStore->hasSecret(QString::fromLatin1(kImageCredentialId))) {
        _imageConfigStatusLabel->setText(zh(u8"请填写图片 API Key。"));
        _imageConfigStatusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(kError));
        return;
    }
    _userSettings->setValue(QString::fromLatin1(kImageEndpointKey), endpoint.toString(QUrl::FullyEncoded));
    _userSettings->setValue(QString::fromLatin1(kImageModelKey), model);
    _userSettings->sync();
    _loadImageGenerationConfig();
    _setStatusMessage(zh(u8"图片生成 API 已保存，重连 Codex 后生效。"), kSuccess);
}

void AIAgentWorkspace::_loadMediaGenerationConfigs()
{
    if (!_userSettings || !_credentialStore || !_mediaConfigsStatusLabel) return;
    const auto load = [this](QLineEdit* endpoint, QLineEdit* model, QLineEdit* key, const char* endpointKey, const char* modelKey) {
        endpoint->setText(_userSettings->value(QString::fromLatin1(endpointKey)).toString().trimmed());
        model->setText(_userSettings->value(QString::fromLatin1(modelKey)).toString().trimmed());
        key->clear();
    };
    load(_videoEndpointEdit, _videoModelEdit, _videoApiKeyEdit, kVideoEndpointKey, kVideoModelKey);
    _videoStatusEndpointEdit->setText(_userSettings->value(QString::fromLatin1(kVideoStatusEndpointKey)).toString().trimmed());
    load(_audioEndpointEdit, _audioModelEdit, _audioApiKeyEdit, kAudioEndpointKey, kAudioModelKey);
    _audioStatusEndpointEdit->setText(_userSettings->value(QString::fromLatin1(kAudioStatusEndpointKey)).toString().trimmed());
    load(_imageEditEndpointEdit, _imageEditModelEdit, _imageEditApiKeyEdit, kImageEditEndpointKey, kImageEditModelKey);
    const QString workbenchBase = _userSettings->value(QString::fromLatin1(kGenericBaseUrlKey)).toString().trimmed();
    const QString workbenchModel = _userSettings->value(QString::fromLatin1(kGenericModelKey)).toString().trimmed();
    const bool workbenchCredential = _credentialStore->hasSecret(QString::fromLatin1(kGenericCredentialId));
    const auto inheritedEndpoint = [&workbenchBase](const QString& suffix) {
        QUrl url(workbenchBase);
        if (!url.isValid() || url.isRelative() || url.scheme() != QStringLiteral("https") || url.host().isEmpty()) return QString();
        QString path = url.path();
        if (!path.endsWith(QStringLiteral("/v1"))) path = path.endsWith(QLatin1Char('/')) ? path + QStringLiteral("v1") : path + QStringLiteral("/v1");
        url.setPath(path + suffix); url.setQuery(QString()); url.setFragment(QString());
        return url.toString(QUrl::FullyEncoded);
    };
    if (_videoEndpointEdit->text().trimmed().isEmpty()) _videoEndpointEdit->setPlaceholderText(inheritedEndpoint(QStringLiteral("/videos")));
    if (_audioEndpointEdit->text().trimmed().isEmpty()) _audioEndpointEdit->setPlaceholderText(inheritedEndpoint(QStringLiteral("/audio/generations")));
    if (_imageEditEndpointEdit->text().trimmed().isEmpty()) _imageEditEndpointEdit->setPlaceholderText(inheritedEndpoint(QStringLiteral("/images/edits")));
    if (_videoModelEdit->text().trimmed().isEmpty()) _videoModelEdit->setPlaceholderText(workbenchModel);
    if (_audioModelEdit->text().trimmed().isEmpty()) _audioModelEdit->setPlaceholderText(workbenchModel);
    if (_imageEditModelEdit->text().trimmed().isEmpty()) _imageEditModelEdit->setPlaceholderText(workbenchModel);
    const auto configured = [this](const char* endpointKey, const char* modelKey, const char* credentialId) {
        return !_userSettings->value(QString::fromLatin1(endpointKey)).toString().trimmed().isEmpty() &&
            !_userSettings->value(QString::fromLatin1(modelKey)).toString().trimmed().isEmpty() &&
            _credentialStore->hasSecret(QString::fromLatin1(credentialId));
    };
    const bool inherited = !workbenchBase.isEmpty() && !workbenchModel.isEmpty() && workbenchCredential;
    _mediaConfigsStatusLabel->setText(zh(u8"视频：%1  音频：%2  图片编辑：%3%4").arg(
        configured(kVideoEndpointKey, kVideoModelKey, kVideoCredentialId) ? zh(u8"已配置") : zh(u8"未配置"),
        configured(kAudioEndpointKey, kAudioModelKey, kAudioCredentialId) ? zh(u8"已配置") : zh(u8"未配置"),
        configured(kImageEditEndpointKey, kImageEditModelKey, kImageEditCredentialId) ? zh(u8"已配置") : zh(u8"未配置"),
        inherited ? zh(u8"；空白项自动继承 Workbench API、模型和安全密钥。") : QString()));
    _mediaConfigsStatusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(kMuted));
}

void AIAgentWorkspace::_saveMediaGenerationConfigs()
{
    if (!_userSettings || !_credentialStore) return;
    const auto save = [this](QLineEdit* endpointEdit, QLineEdit* modelEdit, QLineEdit* keyEdit, const char* endpointKey, const char* modelKey, const char* credentialId, QString* error) {
        const QUrl endpoint(endpointEdit->text().trimmed());
        const QString model = modelEdit->text().trimmed();
        const bool anyValue = !endpointEdit->text().trimmed().isEmpty() || !model.isEmpty() || !keyEdit->text().trimmed().isEmpty();
        if (!anyValue) return true;
        if (!endpoint.isValid() || endpoint.isRelative() || endpoint.scheme() != QStringLiteral("https") || endpoint.host().isEmpty() || !endpoint.userInfo().isEmpty() || hasSensitiveCredentialQuery(endpoint.toString()) || model.isEmpty()) { *error = zh(u8"媒体 Endpoint 必须是无密钥的 HTTPS 地址，并填写模型。 "); return false; }
        QByteArray key = keyEdit->text().trimmed().toUtf8();
        if (!key.isEmpty()) { const bool stored = _credentialStore->storeSecret(QString::fromLatin1(credentialId), key, error); key.fill('\0'); if (!stored) return false; keyEdit->clear(); }
        if (!_credentialStore->hasSecret(QString::fromLatin1(credentialId))) { *error = zh(u8"请填写对应媒体 API Key。 "); return false; }
        _userSettings->setValue(QString::fromLatin1(endpointKey), endpoint.toString(QUrl::FullyEncoded));
        _userSettings->setValue(QString::fromLatin1(modelKey), model);
        return true;
    };
    QString error;
    if (!save(_videoEndpointEdit, _videoModelEdit, _videoApiKeyEdit, kVideoEndpointKey, kVideoModelKey, kVideoCredentialId, &error) ||
        !save(_audioEndpointEdit, _audioModelEdit, _audioApiKeyEdit, kAudioEndpointKey, kAudioModelKey, kAudioCredentialId, &error) ||
        !save(_imageEditEndpointEdit, _imageEditModelEdit, _imageEditApiKeyEdit, kImageEditEndpointKey, kImageEditModelKey, kImageEditCredentialId, &error)) {
        _mediaConfigsStatusLabel->setText(error); _mediaConfigsStatusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(kError)); return;
    }
    const auto saveStatusEndpoint = [this, &error](QLineEdit* edit, const char* key) {
        const QString value = edit->text().trimmed();
        if (value.isEmpty()) { _userSettings->remove(QString::fromLatin1(key)); return true; }
        const QUrl url(value);
        if (!url.isValid() || url.isRelative() || url.scheme() != QStringLiteral("https") || url.host().isEmpty() || !url.userInfo().isEmpty() || hasSensitiveCredentialQuery(url.toString()) || !value.contains(QStringLiteral("{id}"))) { error = zh(u8"异步状态 URL 必须为无密钥 HTTPS 地址，并包含 {id}。 "); return false; }
        _userSettings->setValue(QString::fromLatin1(key), url.toString(QUrl::FullyEncoded)); return true;
    };
    if (!saveStatusEndpoint(_videoStatusEndpointEdit, kVideoStatusEndpointKey) || !saveStatusEndpoint(_audioStatusEndpointEdit, kAudioStatusEndpointKey)) { _mediaConfigsStatusLabel->setText(error); _mediaConfigsStatusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(kError)); return; }
    _userSettings->sync();
    _loadMediaGenerationConfigs();
    _setStatusMessage(zh(u8"媒体生成设置已保存，重连 Codex 后生效。"), kSuccess);
}

void AIAgentWorkspace::_loadSubtitlePipelineConfig()
{
    if (!_userSettings) {
        return;
    }
    if (_subtitleAsrBaseUrlEdit) {
        QSignalBlocker blocker(_subtitleAsrBaseUrlEdit);
        QString current = _userSettings->value(QString::fromLatin1(kSubtitleAsrBaseUrlKey)).toString().trimmed();
        if (current.isEmpty()) {
            current = _userSettings->value(QString::fromLatin1(kLegacySubtitleAsrBaseUrlKey)).toString().trimmed();
        }
        if (current.isEmpty()) {
            current = _userSettings->value(QString::fromLatin1(kQwenSubtitleAsrBaseUrlKey)).toString().trimmed();
        }
        _subtitleAsrBaseUrlEdit->setText(current);
    }
    if (_subtitleAsrModelCombo) {
        QSignalBlocker blocker(_subtitleAsrModelCombo);
        const QString configuredProtocol =
            normalizeSubtitleAsrProtocol(_userSettings->value(QString::fromLatin1(kSubtitleAsrProtocolKey)).toString());
        const QString current =
            _userSettings->value(QString::fromLatin1(kSubtitleAsrModelKey)).toString().trimmed();
        for (const QString& option : {
                 QStringLiteral("auto"),
                 QString::fromLatin1(kQwenDefaultAsrModel),
                 QStringLiteral("qwen3-asr"),
                 QStringLiteral("gemini-2.5-flash"),
                 QStringLiteral("gemini-2.0-flash"),
                 QStringLiteral("gpt-5.4"),
                 QStringLiteral("gpt-5.5"),
                 QStringLiteral("whisper-large-v3-turbo"),
                 QStringLiteral("whisper-large-v3"),
                 QStringLiteral("whisper-1"),
                 QStringLiteral("gpt-4o-transcribe")
             }) {
            if (_subtitleAsrModelCombo->findText(option) < 0) {
                _subtitleAsrModelCombo->addItem(option);
            }
        }
        const QString effectiveCurrent =
            configuredProtocol == QStringLiteral("qwen") && modelLooksUnsuitableForQwenAsr(current)
            ? QString::fromLatin1(kQwenDefaultAsrModel)
            : (current.isEmpty() ? QStringLiteral("auto") : current);
        if (_subtitleAsrModelCombo->findText(effectiveCurrent) < 0) {
            _subtitleAsrModelCombo->addItem(effectiveCurrent);
        }
        _subtitleAsrModelCombo->setCurrentText(effectiveCurrent);
    }
    if (_subtitleAsrProviderCombo) {
        QSignalBlocker blocker(_subtitleAsrProviderCombo);
        const QString current =
            normalizeSubtitleAsrProtocol(_userSettings->value(QString::fromLatin1(kSubtitleAsrProtocolKey)).toString());
        const int index = _subtitleAsrProviderCombo->findData(current);
        if (index >= 0) {
            _subtitleAsrProviderCombo->setCurrentIndex(index);
        } else {
            _subtitleAsrProviderCombo->setCurrentIndex(0);
        }
    }
    if (_subtitleTranslateProviderCombo) {
        QSignalBlocker blocker(_subtitleTranslateProviderCombo);
        QString current =
            normalizeSubtitleTranslateProvider(
                _userSettings->value(QString::fromLatin1(kSubtitleTranslateProviderKey)).toString());
        if (current == QStringLiteral("openai-compatible")) {
            const QString baseUrl =
                _userSettings->value(QString::fromLatin1(kSubtitleTranslateBaseUrlKey)).toString().trimmed();
            const QString model =
                _userSettings->value(QString::fromLatin1(kSubtitleTranslateModelKey)).toString().trimmed();
            if (baseUrl.contains(QStringLiteral("dashscope"), Qt::CaseInsensitive) ||
                baseUrl.contains(QStringLiteral("aliyuncs.com"), Qt::CaseInsensitive) ||
                model.startsWith(QStringLiteral("qwen"), Qt::CaseInsensitive)) {
                current = QStringLiteral("qwen");
            }
        }
        const int index = _subtitleTranslateProviderCombo->findData(current);
        _subtitleTranslateProviderCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (_subtitleTranslateBaseUrlEdit) {
        QSignalBlocker blocker(_subtitleTranslateBaseUrlEdit);
        QString baseUrl =
            _userSettings->value(QString::fromLatin1(kSubtitleTranslateBaseUrlKey)).toString().trimmed();
        if (baseUrl.isEmpty()) {
            baseUrl = _userSettings->value(QString::fromLatin1(kQwenSubtitleBaseUrlKey)).toString().trimmed();
        }
        _subtitleTranslateBaseUrlEdit->setText(baseUrl);
    }
    if (_subtitleTranslateModelCombo) {
        QSignalBlocker blocker(_subtitleTranslateModelCombo);
        const QString current =
            _userSettings->value(QString::fromLatin1(kSubtitleTranslateModelKey)).toString().trimmed();
        QStringList modelOptions = _userSettings
            ? _userSettings->value(QString::fromLatin1(kAvailableModelsKey)).toStringList()
            : QStringList{};
        appendUniqueModel(&modelOptions, _userSettings
            ? _userSettings->value(QString::fromLatin1(kRecommendedModelKey)).toString().trimmed()
            : QString());
        appendUniqueModel(&modelOptions, _selectedModel());
        appendUniqueModel(&modelOptions, current);
        appendUniqueModel(&modelOptions, QString::fromLatin1(kQwenDefaultTranslationModel));
        appendUniqueModel(&modelOptions, QStringLiteral("qwen-max"));
        appendUniqueModel(&modelOptions, QStringLiteral("qwen-turbo"));
        appendCommonChatModels(&modelOptions);
        _subtitleTranslateModelCombo->clear();
        for (const QString& model : modelOptions) {
            if (!model.trimmed().isEmpty()) {
                _subtitleTranslateModelCombo->addItem(model.trimmed());
            }
        }
        if (!current.isEmpty()) {
            _subtitleTranslateModelCombo->setCurrentText(current);
        } else if (_subtitleTranslateProviderCombo &&
                   _subtitleTranslateProviderCombo->currentData().toString() == QStringLiteral("qwen")) {
            _subtitleTranslateModelCombo->setCurrentText(QString::fromLatin1(kQwenDefaultTranslationModel));
        }
    }
    if (_subtitleAsrApiKeyEdit) {
        QSignalBlocker blocker(_subtitleAsrApiKeyEdit);
        _subtitleAsrApiKeyEdit->clear();
    }
    if (_subtitleTranslateApiKeyEdit) {
        QSignalBlocker blocker(_subtitleTranslateApiKeyEdit);
        _subtitleTranslateApiKeyEdit->clear();
    }
    if (_onlineSubtitleSearchCheck) {
        QSignalBlocker blocker(_onlineSubtitleSearchCheck);
        _onlineSubtitleSearchCheck->setChecked(
            _userSettings->value(QString::fromLatin1(kOnlineSubtitleEnabledKey), false).toBool());
    }
    if (_onlineSubtitleProviderCombo) {
        QSignalBlocker blocker(_onlineSubtitleProviderCombo);
        const QString provider =
            _userSettings->value(QString::fromLatin1(kOnlineSubtitleProviderKey),
                                 QStringLiteral("opensubtitles-compatible")).toString().trimmed();
        const int index = _onlineSubtitleProviderCombo->findData(provider);
        _onlineSubtitleProviderCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (_onlineSubtitleBaseUrlEdit) {
        QSignalBlocker blocker(_onlineSubtitleBaseUrlEdit);
        _onlineSubtitleBaseUrlEdit->setText(
            _userSettings->value(QString::fromLatin1(kOnlineSubtitleBaseUrlKey)).toString().trimmed());
    }
    if (_onlineSubtitleLanguageEdit) {
        QSignalBlocker blocker(_onlineSubtitleLanguageEdit);
        _onlineSubtitleLanguageEdit->setText(
            _userSettings->value(QString::fromLatin1(kOnlineSubtitleLanguageKey), QStringLiteral("zh,ja,en")).toString().trimmed());
    }
    if (_onlineSubtitleApiKeyEdit) {
        QSignalBlocker blocker(_onlineSubtitleApiKeyEdit);
        _onlineSubtitleApiKeyEdit->clear();
    }
}

void AIAgentWorkspace::_saveSubtitlePipelineConfig()
{
    if (!_userSettings) {
        return;
    }

    const QString asrBaseUrl = _subtitleAsrBaseUrlEdit ? _subtitleAsrBaseUrlEdit->text().trimmed() : QString();
    QString asrModel = _subtitleAsrModelCombo ? _subtitleAsrModelCombo->currentText().trimmed() : QString();
    const QString asrProvider = _subtitleAsrProviderCombo ? _subtitleAsrProviderCombo->currentData().toString().trimmed() : QString();
    QString translateBaseUrl =
        _userSettings->value(QString::fromLatin1(kGenericBaseUrlKey)).toString().trimmed();
    if (translateBaseUrl.isEmpty()) {
        translateBaseUrl = _baseUrlEdit ? _baseUrlEdit->text().trimmed() : QString();
    }
    QString translateModel = _selectedModel();
    if (translateModel.isEmpty() || translateModel.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
        translateModel =
            _userSettings->value(QString::fromLatin1(kRecommendedModelKey)).toString().trimmed();
    }
    const QString translateProvider = QStringLiteral("openai-compatible");
    const bool onlineEnabled = true;
    const QString onlineProvider = QStringLiteral("workbench-api");
    const QString onlineLanguage = QStringLiteral("zh,ja,en");

    const QString normalizedProtocol = normalizeSubtitleAsrProtocol(asrProvider);
    if (normalizedProtocol == QStringLiteral("qwen") && modelLooksUnsuitableForQwenAsr(asrModel)) {
        asrModel = QString::fromLatin1(kQwenDefaultAsrModel);
        if (_subtitleAsrModelCombo) {
            _subtitleAsrModelCombo->setCurrentText(asrModel);
        }
    }
    _userSettings->setValue(QStringLiteral("ai/subtitles/mimo/enabled"), normalizedProtocol == QStringLiteral("mimo") && (!asrBaseUrl.isEmpty() || !asrModel.isEmpty()));
    _userSettings->setValue(QString::fromLatin1(kSubtitleAsrProtocolKey), normalizedProtocol);
    _userSettings->setValue(QString::fromLatin1(kSubtitleTranslateProviderKey), translateProvider);
    if (asrBaseUrl.isEmpty()) {
        _userSettings->remove(QString::fromLatin1(kSubtitleAsrBaseUrlKey));
        _userSettings->remove(QString::fromLatin1(kLegacySubtitleAsrBaseUrlKey));
        if (normalizedProtocol == QStringLiteral("qwen")) {
            _userSettings->setValue(QString::fromLatin1(kQwenSubtitleAsrBaseUrlKey), QString::fromLatin1(kQwenDefaultAsrBaseUrl));
        }
    } else {
        _userSettings->setValue(QString::fromLatin1(kSubtitleAsrBaseUrlKey), asrBaseUrl);
        if (normalizedProtocol == QStringLiteral("qwen")) {
            _userSettings->setValue(QString::fromLatin1(kQwenSubtitleAsrBaseUrlKey), asrBaseUrl);
        } else {
            _userSettings->setValue(QString::fromLatin1(kLegacySubtitleAsrBaseUrlKey), asrBaseUrl);
        }
    }
    if (asrModel.isEmpty() || asrModel.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0) {
        _userSettings->remove(QString::fromLatin1(kSubtitleAsrModelKey));
    } else {
        _userSettings->setValue(QString::fromLatin1(kSubtitleAsrModelKey), asrModel);
    }
    if (translateBaseUrl.isEmpty()) {
        _userSettings->remove(QString::fromLatin1(kSubtitleTranslateBaseUrlKey));
    } else {
        _userSettings->setValue(QString::fromLatin1(kSubtitleTranslateBaseUrlKey), translateBaseUrl);
    }
    if (translateModel.isEmpty()) {
        _userSettings->remove(QString::fromLatin1(kSubtitleTranslateModelKey));
    } else {
        _userSettings->setValue(QString::fromLatin1(kSubtitleTranslateModelKey), translateModel);
    }
    _userSettings->setValue(QString::fromLatin1(kOnlineSubtitleEnabledKey), onlineEnabled);
    _userSettings->setValue(QString::fromLatin1(kOnlineSubtitleProviderKey), onlineProvider);
    _userSettings->remove(QString::fromLatin1(kOnlineSubtitleBaseUrlKey));
    if (onlineLanguage.isEmpty()) {
        _userSettings->setValue(QString::fromLatin1(kOnlineSubtitleLanguageKey), QStringLiteral("zh,ja,en"));
    } else {
        _userSettings->setValue(QString::fromLatin1(kOnlineSubtitleLanguageKey), onlineLanguage);
    }

    if (_credentialStore && _subtitleAsrApiKeyEdit) {
        const QByteArray apiKey = _subtitleAsrApiKeyEdit->text().trimmed().toUtf8();
        if (!apiKey.isEmpty()) {
            QString error;
            if (!_credentialStore->storeSecret(QString::fromLatin1(kSubtitleAsrApiKeyId), apiKey, &error)) {
                _setStatusMessage(error.isEmpty() ? zh(u8"保存语音识别 API Key 失败。") : error, kError);
                return;
            }
            if (normalizedProtocol == QStringLiteral("qwen")) {
                _credentialStore->storeSecret(QString::fromLatin1(kQwenApiKeyId), apiKey);
            } else {
                _credentialStore->storeSecret(QString::fromLatin1(kLegacySubtitleAsrApiKeyId), apiKey);
            }
            _subtitleAsrApiKeyEdit->clear();
        }
    }
    if (_credentialStore && _subtitleTranslateApiKeyEdit && _subtitleTranslateApiKeyEdit->isVisible()) {
        const QByteArray apiKey = _subtitleTranslateApiKeyEdit->text().trimmed().toUtf8();
        if (!apiKey.isEmpty()) {
            QString error;
            if (!_credentialStore->storeSecret(QString::fromLatin1(kSubtitleTranslateApiKeyId), apiKey, &error)) {
                _setStatusMessage(error.isEmpty() ? zh(u8"保存字幕翻译 API Key 失败。") : error, kError);
                return;
            }
            if (translateProvider == QStringLiteral("qwen")) {
                _credentialStore->storeSecret(QString::fromLatin1(kQwenApiKeyId), apiKey);
            }
            _subtitleTranslateApiKeyEdit->clear();
        }
    }
    if (_credentialStore && _onlineSubtitleApiKeyEdit && _onlineSubtitleApiKeyEdit->isVisible()) {
        const QByteArray apiKey = _onlineSubtitleApiKeyEdit->text().trimmed().toUtf8();
        if (!apiKey.isEmpty()) {
            QString error;
            if (!_credentialStore->storeSecret(QString::fromLatin1(kOnlineSubtitleApiKeyId), apiKey, &error)) {
                _setStatusMessage(error.isEmpty() ? zh(u8"保存在线字幕 API Key 失败。") : error, kError);
                return;
            }
            _onlineSubtitleApiKeyEdit->clear();
        }
    }

    _userSettings->sync();
    _loadSubtitlePipelineConfig();
    _setStatusMessage(zh(u8"字幕管线已保存。"), kSuccess);
}

void AIAgentWorkspace::_testSubtitleAsrConfig()
{
    const QString baseUrl = _subtitleAsrBaseUrlEdit ? _subtitleAsrBaseUrlEdit->text().trimmed() : QString();
    const QString protocol = _subtitleAsrProviderCombo ? _subtitleAsrProviderCombo->currentData().toString().trimmed() : QStringLiteral("mimo");
    const QString normalizedProtocol = normalizeSubtitleAsrProtocol(protocol);
    const QString model = normalizedProtocol == QStringLiteral("qwen") &&
            _subtitleAsrModelCombo &&
            modelLooksUnsuitableForQwenAsr(_subtitleAsrModelCombo->currentText())
        ? QString::fromLatin1(kQwenDefaultAsrModel)
        : (_subtitleAsrModelCombo ? _subtitleAsrModelCombo->currentText().trimmed() : QString());
    QString apiKey;
    if (_subtitleAsrApiKeyEdit && !_subtitleAsrApiKeyEdit->text().trimmed().isEmpty()) {
        apiKey = _subtitleAsrApiKeyEdit->text().trimmed();
    } else if (_credentialStore) {
        QString loadError;
        if (normalizedProtocol == QStringLiteral("qwen")) {
            apiKey = _credentialStore->loadSecret(QString::fromLatin1(kQwenApiKeyId), &loadError).trimmed();
        }
        if (apiKey.isEmpty()) {
            apiKey = _credentialStore->loadSecret(QString::fromLatin1(kSubtitleAsrApiKeyId), &loadError).trimmed();
        }
        if (apiKey.isEmpty() && normalizedProtocol != QStringLiteral("qwen")) {
            apiKey = _credentialStore->loadSecret(QString::fromLatin1(kLegacySubtitleAsrApiKeyId), &loadError).trimmed();
        }
    }
    if (apiKey.isEmpty()) {
        apiKey = qgetenv("SUBTITLE_ASR_API_KEY").trimmed();
    }
    if (apiKey.isEmpty()) {
        apiKey = qgetenv("ASR_API_KEY").trimmed();
    }
    if (apiKey.isEmpty() && normalizedProtocol != QStringLiteral("qwen")) {
        apiKey = qgetenv("GROQ_API_KEY").trimmed();
    }
    if (apiKey.isEmpty() && normalizedProtocol != QStringLiteral("qwen")) {
        apiKey = qgetenv("OPENAI_API_KEY").trimmed();
    }
    if (apiKey.isEmpty() && normalizedProtocol != QStringLiteral("qwen")) {
        apiKey = qgetenv("GEMINI_API_KEY").trimmed();
    }
    if (apiKey.isEmpty() && normalizedProtocol != QStringLiteral("qwen")) {
        apiKey = qgetenv("MIMO_API_KEY").trimmed();
    }
    if (apiKey.isEmpty() && normalizedProtocol != QStringLiteral("qwen")) {
        apiKey = qgetenv("XIAOMI_MIMO_API_KEY").trimmed();
    }
    if (apiKey.isEmpty()) {
        apiKey = qgetenv("QWEN_API_KEY").trimmed();
    }
    if (apiKey.isEmpty()) {
        apiKey = qgetenv("DASHSCOPE_API_KEY").trimmed();
    }
    if (apiKey.isEmpty()) {
        _setStatusMessage(zh(u8"语音识别 API 检查失败：没有可用的 API Key。"), kError);
        return;
    }

    QString audioPath;
    for (const QString& candidate : {
             QCoreApplication::applicationDirPath() + QStringLiteral("/../../../tmp_local_transcribe_voice.wav"),
             QCoreApplication::applicationDirPath() + QStringLiteral("/../../../tmp_realtime_translation_test.wav"),
             QCoreApplication::applicationDirPath() + QStringLiteral("/../../../tmp_rt_scan.wav"),
             QCoreApplication::applicationDirPath() + QStringLiteral("/../../../tmp_no_speech_5s.wav") }) {
        if (QFileInfo::exists(candidate)) {
            audioPath = QFileInfo(candidate).absoluteFilePath();
            break;
        }
    }
    if (audioPath.isEmpty()) {
        _setStatusMessage(zh(u8"语音识别 API 检查失败：找不到可用的测试音频。"), kError);
        return;
    }

    SubtitleAsrApiConfig config;
    config.baseUrl = baseUrl;
    config.apiKey = apiKey;
    config.model = model.isEmpty() ? QStringLiteral("auto") : model;
    config.sourceLanguageHint = QStringLiteral("auto");
    config.timeoutMs = 25000;
    if (normalizedProtocol == QStringLiteral("gemini")) {
        config.protocol = SubtitleAsrProtocol::GeminiGenerateContent;
    } else if (normalizedProtocol == QStringLiteral("qwen")) {
        config.protocol = SubtitleAsrProtocol::QwenDashScopeAsr;
    } else if (normalizedProtocol == QStringLiteral("openai")) {
        config.protocol = SubtitleAsrProtocol::OpenAITranscriptions;
    } else if (normalizedProtocol == QStringLiteral("responses_audio")) {
        config.protocol = SubtitleAsrProtocol::OpenAIResponsesAudio;
    } else {
        config.protocol = SubtitleAsrProtocol::MimoChat;
    }

    _setStatusMessage(zh(u8"正在真测语音识别 API..."), kMuted);
    const SubtitleAsrApiResult result = transcribeSubtitleAudioWithProtocol(config, audioPath);
    if (!result.success) {
        _setStatusMessage(
            zh(u8"语音识别 API 真测失败：%1").arg(result.errorMessage.isEmpty() ? zh(u8"未知错误") : result.errorMessage),
            kError);
        return;
    }

    const QString preview = result.text.simplified().left(80);
    _setStatusMessage(
        zh(u8"语音识别 API 真测成功：%1 / %2 ms / %3")
            .arg(result.backend.isEmpty() ? zh(u8"未知后端") : result.backend)
            .arg(result.durationMs)
            .arg(preview.isEmpty() ? zh(u8"无文本") : preview),
        kSuccess);
}

QJsonObject AIAgentWorkspace::runQwenAsrProviderSmoke()
{
    if (_subtitleAsrProviderCombo) {
        const int index = _subtitleAsrProviderCombo->findData(QStringLiteral("qwen"));
        if (index >= 0) {
            _subtitleAsrProviderCombo->setCurrentIndex(index);
        }
    }
    if (_subtitleAsrModelCombo && modelLooksUnsuitableForQwenAsr(_subtitleAsrModelCombo->currentText())) {
        _subtitleAsrModelCombo->setCurrentText(QString::fromLatin1(kQwenDefaultAsrModel));
    }
    if (_subtitleAsrBaseUrlEdit && _subtitleAsrBaseUrlEdit->text().trimmed().isEmpty()) {
        _subtitleAsrBaseUrlEdit->setText(QString::fromLatin1(kQwenDefaultAsrBaseUrl));
    }
    if (_settingsDialog) {
        _settingsDialog->show();
        _settingsDialog->raise();
        _settingsDialog->activateWindow();
    }

    _testSubtitleAsrConfig();

    const QString status = _statusLabel ? _statusLabel->text() : QString();
    const QString provider = _subtitleAsrProviderCombo
        ? normalizeSubtitleAsrProtocol(_subtitleAsrProviderCombo->currentData().toString())
        : QString();
    const QString model = _subtitleAsrModelCombo ? _subtitleAsrModelCombo->currentText().trimmed() : QString();
    const QString endpoint = _subtitleAsrBaseUrlEdit ? _subtitleAsrBaseUrlEdit->text().trimmed() : QString();
    const bool stubRemoved = !status.contains(QStringLiteral("qwen-asr-provider-not-wired"), Qt::CaseInsensitive);
    const bool success = status.contains(zh(u8"语音识别 API 真测成功"));
    const bool localNoKeyError = status.contains(zh(u8"没有可用的 API Key")) ||
        status.contains(QStringLiteral("key is missing"), Qt::CaseInsensitive);
    const bool authRejected = status.contains(QStringLiteral("HTTP 401"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("invalid_api_key"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("Incorrect API key"), Qt::CaseInsensitive);
    const bool transientNetworkError = status.contains(QStringLiteral("HTTP 0"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("Connection closed"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("timed out"), Qt::CaseInsensitive);
    const bool serverOrNetworkError = status.contains(QStringLiteral("HTTP "), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("provider=qwen"), Qt::CaseInsensitive) ||
        status.contains(QStringLiteral("timed out"), Qt::CaseInsensitive);

    return QJsonObject{
        { QStringLiteral("provider"), provider },
        { QStringLiteral("model"), model },
        { QStringLiteral("endpoint"), endpoint },
        { QStringLiteral("status"), status },
        { QStringLiteral("success"), success },
        { QStringLiteral("stubRemoved"), stubRemoved },
        { QStringLiteral("realApiAttempted"), stubRemoved && (success || serverOrNetworkError) },
        { QStringLiteral("authRejected"), authRejected },
        { QStringLiteral("transientNetworkError"), transientNetworkError },
        { QStringLiteral("apiReachableButRejected"), authRejected },
        { QStringLiteral("noKeyError"), localNoKeyError },
        { QStringLiteral("expectedModel"), QString::fromLatin1(kQwenDefaultAsrModel) }
    };
}

bool AIAgentWorkspace::saveSettingsDialogSmokeScreenshot(const QString& outputPath) const
{
    if (outputPath.trimmed().isEmpty()) {
        return false;
    }
    QWidget* target = _settingsDialog && _settingsDialog->isVisible()
        ? static_cast<QWidget*>(_settingsDialog)
        : const_cast<AIAgentWorkspace*>(this);
    target->ensurePolished();
    if (QLayout* layout = target->layout()) {
        layout->activate();
    }
    const auto childWidgets = target->findChildren<QWidget*>();
    for (QWidget* child : childWidgets) {
        child->ensurePolished();
        if (QLayout* layout = child->layout()) {
            layout->activate();
        }
    }

    const qreal devicePixelRatio = target->devicePixelRatioF();
    const QSize pixelSize = target->size() * devicePixelRatio;
    if (pixelSize.isEmpty()) {
        return false;
    }
    QPixmap capture(pixelSize);
    capture.setDevicePixelRatio(devicePixelRatio);
    capture.fill(Qt::transparent);
    target->render(&capture);
    if (capture.isNull()) {
        return false;
    }
    const QFileInfo info(outputPath);
    QDir().mkpath(info.absolutePath());
    return capture.save(info.absoluteFilePath(), "PNG");
}

void AIAgentWorkspace::_clearStoredProviderCredential()
{
    if (_selectedProviderId() != QString::fromLatin1(kOpenAIProviderId) || !_credentialStore) {
        return;
    }

    QString error;
    const bool removedGeneric = _credentialStore->removeSecret(QString::fromLatin1(kGenericCredentialId), &error);
    const bool removedLegacy = _credentialStore->removeSecret(QString::fromLatin1(kOpenAICredentialId), &error);
    if (!removedGeneric && !removedLegacy) {
        _setStatusMessage(
            error.isEmpty() ? zh(u8"删除已保存的 API Key 失败。") : error,
            kError);
        return;
    }

    if (_apiKeyEdit) {
        _apiKeyEdit->clear();
    }
    if (_userSettings) {
        _userSettings->remove(QString::fromLatin1(kDetectedProtocolKey));
        _userSettings->remove(QString::fromLatin1(kDetectedProviderKey));
        _userSettings->remove(QString::fromLatin1(kDetectedEndpointKey));
        _userSettings->remove(QString::fromLatin1(kCompatibleProtocolsKey));
        _userSettings->remove(QString::fromLatin1(kConnectionDiagnosticsKey));
        _userSettings->sync();
    }
    _refreshProviders();
    _loadProviderConfig();
    _refreshActionState();
    _setStatusMessage(zh(u8"已删除保存的 API Key。"), kMuted);
}

void AIAgentWorkspace::_setStatusMessage(const QString& text, const QString& colorHex)
{
    _statusLabel->setText(text);
    _statusLabel->setStyleSheet(QStringLiteral("color:%1;font-size:12px;").arg(colorHex));
}

} // namespace cgplay
