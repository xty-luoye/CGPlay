#include "services/ai/OpenAIResponsesProvider.h"
#include "services/ai/discovery/AIConnectionValidator.h"
#include "services/ai/discovery/AIModelDiscovery.h"
#include "services/ai/discovery/AIProviderDetector.h"
#include "services/ai/api/IAICredentialStore.h"
#include "common/core/ServiceLocator.h"
#include "settings/api/ISettingsService.h"
#include "ui/app/AIConnectionWizardWidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <functional>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace cgplay;

class Settings final : public ISettingsService {
public:
    QHash<QString, QVariant> data;
    QVariant value(const QString& k, const QVariant& v = {}) const override { return data.value(k, v); }
    void setValue(const QString& k, const QVariant& v) override { data.insert(k, v); }
    bool contains(const QString& k) const override { return data.contains(k); }
    QStringList allKeys() const override { return data.keys(); }
    void remove(const QString& k) override { data.remove(k); }
    void sync() override {}
    QString organization() const override { return "contract-test"; }
    QString application() const override { return "contract-test"; }
};

class Credentials final : public IAICredentialStore {
public:
    bool storeSecret(const QString&, const QByteArray&, QString*) override { return true; }
    QByteArray loadSecret(const QString&, QString*) const override { return "dummy-contract-key"; }
    bool removeSecret(const QString&, QString*) override { return true; }
    bool hasSecret(const QString&) const override { return true; }
};

struct Request {
    QByteArray method;
    QUrl url;
    QHash<QByteArray, QByteArray> headers;
    QJsonObject body;
};
struct Reply {
    int status = 200;
    QByteArray payload = "{}";
    QByteArray location;
};
Reply json(const QJsonObject& o) { return {200, QJsonDocument(o).toJson(QJsonDocument::Compact), {}}; }
Reply error() { return {400, R"({"error":{"message":"mock rejected"}})", {}}; }

class Server : public QTcpServer {
public:
    QList<Request> requests;
    std::function<Reply(const Request&)> handle;
    Server() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (auto* socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer = QByteArray()]() mutable {
                    buffer += socket->readAll();
                    const int split = buffer.indexOf("\r\n\r\n");
                    if (split < 0) return;
                    const auto lines = buffer.left(split).split('\n');
                    const auto first = lines.first().trimmed().split(' ');
                    if (first.size() < 2) return;
                    Request request;
                    request.method = first[0];
                    request.url = QUrl(QStringLiteral("http://127.0.0.1") + QString::fromUtf8(first[1]));
                    for (const auto& line : lines.mid(1)) {
                        const int colon = line.indexOf(':');
                        if (colon > 0) request.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
                    }
                    const int length = request.headers.value("content-length").toInt();
                    if (buffer.size() < split + 4 + length) return;
                    request.body = QJsonDocument::fromJson(buffer.mid(split + 4, length)).object();
                    requests.append(request);
                    const Reply reply = handle ? handle(request) : error();
                    QByteArray wire = "HTTP/1.1 " + QByteArray::number(reply.status) + " Test\r\nContent-Type: application/json\r\nConnection: close\r\n";
                    if (!reply.location.isEmpty()) wire += "Location: " + reply.location + "\r\n";
                    wire += "Content-Length: " + QByteArray::number(reply.payload.size()) + "\r\n\r\n" + reply.payload;
                    socket->write(wire);
                    socket->disconnectFromHost();
                    buffer.clear();
                });
            }
        });
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }
};

class WizardDetector final : public IAIProviderDetector {
public:
    AIDetectionRequest received;
    AIDetectionResult saved;
    AIDetectionResult detect(const AIDetectionRequest& request) override {
        received = request;
        AIDetectionResult result;
        result.success = true;
        result.category = AIProviderCategory::OpenAICompatible;
        result.providerType = "OpenAICompatible";
        result.baseUrl = request.baseUrl;
        result.recommendedModel = request.model.isEmpty() ? QStringLiteral("server-new-model") : request.model;
        result.models = {result.recommendedModel};
        return result;
    }
    AIDetectionResult discoverCurrentModels() override { return {}; }
    bool saveConfiguration(const AIDetectionRequest&, const AIDetectionResult& result, QString*) override {
        saved = result;
        return true;
    }
    AIDetectionResult currentConfiguration() const override { return saved; }
};

int visibleWindows() {
    int count = 0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner == GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(value);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&count));
#else
    for (QWidget* w : QApplication::topLevelWidgets()) if (w->isVisible()) ++count;
#endif
    return count;
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    for (const char* key : {"OPENAI_API_KEY", "AI_API_KEY", "QWEN_API_KEY", "DASHSCOPE_API_KEY",
                           "GEMINI_API_KEY", "GOOGLE_API_KEY", "ANTHROPIC_API_KEY", "AZURE_OPENAI_API_KEY",
                           "AI_BASE_URL", "OPENAI_BASE_URL", "QWEN_BASE_URL", "DASHSCOPE_BASE_URL"}) qunsetenv(key);
    QApplication app(argc, argv);
    QJsonArray checks;
    int failed = 0;
    const auto check = [&](const char* name, bool ok) {
        checks.append(QJsonObject{{"name", name}, {"pass", ok}});
        if (!ok) { ++failed; qWarning("FAIL: %s", name); }
    };
    check("background_offscreen", QApplication::platformName() == "offscreen" && visibleWindows() == 0);
    const auto endpoints = AIConnectionValidator::openAICompatibleEndpointCandidates("https://example.test/compatible-mode/v1/chat/completions?region=test");
    check("compatible_endpoint_no_duplicate_version", endpoints.chatCompletions.front() == "https://example.test/compatible-mode/v1/chat/completions?region=test");
    check("compatible_models_sibling", endpoints.models.front() == "https://example.test/compatible-mode/v1/models?region=test");
    check("openai_custom_model_completion_limit", AIConnectionValidator::chatTokenLimitParameter("https://api.openai.com", "custom-id") == "max_completion_tokens");
    check("reasoning_gateway_completion_limit", AIConnectionValidator::chatTokenLimitParameter("https://gateway.test", "gpt-6-astra") == "max_completion_tokens");
    check("deepseek_legacy_token_parameter", AIConnectionValidator::chatTokenLimitParameter("https://api.deepseek.com", "deepseek-flash") == "max_tokens");
    check("qwen_legacy_token_parameter", AIConnectionValidator::chatTokenLimitParameter("https://dashscope.aliyuncs.com/compatible-mode/v1", "qwen3.8-max") == "max_tokens");
    check("azure_key_header", AIConnectionValidator::openAICompatibleHeaders("https://contract.openai.azure.com/openai/v1/responses", "dummy").front().first == "api-key");
    check("gemini_model_switch_and_query", AIConnectionValidator::geminiGenerateContentEndpoint("https://example.test/proxy/v1/models/old:generateContent?key=discard&region=x", "models/new-model") == "https://example.test/proxy/v1/models/new-model:generateContent?region=x");

    Server server;
    AIConnectionValidator validator;
    AIModelDiscovery discovery;
    Credentials credentials;
    Settings settings;
    settings.setValue("ai/connection/baseUrl", server.base() + "/v1");
    settings.setValue("ai/connection/providerType", "OpenAICompatible");
    settings.setValue("ai/connection/detectedProtocol", "openai_chat");
    OpenAIResponsesProvider provider(&credentials, &settings);
    AIRequest request;
    request.model = "gpt-6-astra";
    request.userPrompt = "contract test";
    request.options.insert("maxOutputTokens", 64);
    server.handle = [&](const Request& r) {
        if (r.url.path().endsWith("/chat/completions")) {
            check("chat_request_actual_token_limit", r.body.value("max_completion_tokens").toInt() == 64 && !r.body.contains("max_tokens"));
            check("chat_bearer_header", r.headers.value("authorization") == "Bearer dummy-contract-key");
            return Reply{200, R"({"model":"gpt-server-snapshot","choices":[{"message":{"content":[{"type":"text","text":"answer"}]}}]})", {}};
        }
        return error();
    };
    auto response = provider.chat(request);
    check("chat_text_content_blocks", response.success && response.rawText == "answer");
    check("requested_alias_not_overwritten", settings.value("ai/connection/model").toString() == "gpt-6-astra");
    check("capability_list_not_padded_with_other_providers", !provider.capabilities().supportedModels.contains("deepseek-v3"));

    settings.setValue("ai/connection/detectedProtocol", "openai_responses");
    settings.remove("ai/connection/detectedEndpoint");
    server.handle = [&](const Request& r) {
        if (r.url.path().endsWith("/responses")) {
            check("responses_token_limit", r.body.value("max_output_tokens").toInt() >= 16);
            return Reply{200, R"({"status":"completed","output":[{"type":"reasoning","summary":[]},{"type":"message","content":[{"type":"output_text","text":"response answer"}]}]})", {}};
        }
        return error();
    };
    request.options.insert("maxOutputTokens", 1);
    response = provider.chat(request);
    check("responses_content_after_reasoning", response.success && response.rawText == "response answer");
    server.requests.clear();
    server.handle = [](const Request&) { return Reply{200, R"({"status":"incomplete","incomplete_details":{"reason":"max_output_tokens"},"output":[]})", {}}; };
    response = provider.chat(request);
    check("incomplete_response_explicit_no_repeated_generation", !response.success && response.errorMessage.contains("max_output_tokens") && server.requests.size() == 1);

    server.handle = [&](const Request& r) {
        if (r.url.path().endsWith("/responses")) {
            check("responses_probe_valid_minimum", r.body.value("max_output_tokens").toInt() == 16);
            return Reply{200, R"({"object":"response","status":"incomplete","output":[]})", {}};
        }
        if (r.url.path().endsWith("/chat/completions")) {
            check("reasoning_probe_completion_limit", r.body.value("max_completion_tokens").toInt() == 16 && !r.body.contains("max_tokens"));
            return Reply{200, R"({"choices":[{"message":{"content":"ok"}}]})", {}};
        }
        return error();
    };
    const auto valid = validator.validate(AIProviderCategory::OpenAICompatible, server.base() + "/v1", "dummy", "o3");
    check("both_openai_protocols_validate", valid.success && valid.compatibleProtocols.size() == 2);
    server.handle = [](const Request&) { return Reply{200, "<html>not an API</html>", {}}; };
    check("html_not_valid_api", !validator.validate(AIProviderCategory::OpenAICompatible, server.base() + "/v1", "dummy", "o3").success);
    server.handle = [](const Request&) { return Reply{200, R"({"models":[]})", {}}; };
    check("unrelated_json_not_valid_protocol", !validator.validate(AIProviderCategory::OpenAICompatible, server.base() + "/v1", "dummy", "o3").success);

    settings.setValue("ai/connection/providerType", "Gemini");
    settings.setValue("ai/connection/detectedProtocol", "gemini_generate_content");
    settings.setValue("ai/connection/baseUrl", server.base() + "/v1");
    settings.setValue("ai/connection/detectedEndpoint", server.base() + "/v1/models/old:generateContent");
    request.model = "models/custom-generation-model";
    request.systemPrompt = "system";
    request.options.insert("maxOutputTokens", 75);
    server.handle = [&](const Request& r) {
        check("gemini_uses_selected_model_not_cached", r.url.path() == "/v1/models/custom-generation-model:generateContent");
        check("gemini_key_header_not_query", r.headers.value("x-goog-api-key") == "dummy-contract-key" && !QUrlQuery(r.url).hasQueryItem("key"));
        check("gemini_generation_schema", r.body.contains("systemInstruction") && r.body.value("generationConfig").toObject().value("maxOutputTokens").toInt() == 75);
        return Reply{200, R"({"candidates":[{"content":{"parts":[{"thought":true,"text":"internal thought"},{"text":"gemini answer"}]}}]})", {}};
    };
    response = provider.chat(request);
    check("gemini_excludes_thought_part", response.success && response.rawText == "gemini answer");

    server.requests.clear();
    server.handle = [&](const Request& r) {
        check("gemini_discovery_url", r.url.path() == "/v1/models");
        if (!QUrlQuery(r.url).hasQueryItem("pageToken")) {
            return Reply{200, R"({"models":[{"name":"models/embedding","supportedGenerationMethods":["embedContent"]},{"name":"models/gemini-one","supportedGenerationMethods":["generateContent"]}],"nextPageToken":"page-two"})", {}};
        }
        check("gemini_cursor", QUrlQuery(r.url).queryItemValue("pageToken") == "page-two");
        return Reply{200, R"({"models":[{"name":"models/new-generation-id","supportedGenerationMethods":["generateContent"]}]})", {}};
    };
    auto models = discovery.discover(AIProviderCategory::Gemini, server.base() + "/v1/models/old:generateContent", "dummy");
    check("gemini_models_paginated_and_filtered", models.success && models.models == QStringList{"gemini-one", "new-generation-id"} && server.requests.size() == 2);

    server.requests.clear();
    server.handle = [&](const Request& r) {
        check("anthropic_models_auth", r.headers.value("x-api-key") == "dummy" && r.headers.value("anthropic-version") == "2023-06-01");
        if (!QUrlQuery(r.url).hasQueryItem("after_id")) return Reply{200, R"({"data":[{"id":"claude-one"}],"has_more":true,"last_id":"claude-one"})", {}};
        check("anthropic_cursor", QUrlQuery(r.url).queryItemValue("after_id") == "claude-one");
        return Reply{200, R"({"data":[{"id":"claude-new-id"}],"has_more":false})", {}};
    };
    models = discovery.discover(AIProviderCategory::Claude, server.base(), "dummy");
    check("anthropic_models_paginated", models.success && models.models.size() == 2 && server.requests.size() == 2);
    server.requests.clear();
    server.handle = [](const Request&) { return Reply{200, R"({"data":[{"id":"claude-one"}],"has_more":true,"last_id":"repeat"})", {}}; };
    models = discovery.discover(AIProviderCategory::Claude, server.base(), "dummy");
    check("repeated_cursor_bounded_error", !models.success && models.error.contains("pagination") && server.requests.size() == 2);

    Server otherOrigin;
    otherOrigin.handle = [](const Request&) { return Reply{200, R"({"models":[{"name":"models/stolen"}]})", {}}; };
    server.handle = [&](const Request&) { return Reply{307, "{}", (otherOrigin.base() + "/models").toUtf8()}; };
    models = discovery.discover(AIProviderCategory::Gemini, server.base(), "dummy-secret");
    check("discovery_credentials_not_redirected_cross_origin", !models.success && otherOrigin.requests.isEmpty());
    check("validator_credentials_not_redirected_cross_origin", !validator.validate(AIProviderCategory::Gemini, server.base(), "dummy-secret", "new-model").success && otherOrigin.requests.isEmpty());
    response = provider.chat(request);
    check("runtime_credentials_not_redirected_cross_origin", !response.success && otherOrigin.requests.isEmpty());

    settings.setValue("ai/connection/providerType", "OpenAICompatible");
    settings.setValue("ai/connection/detectedProvider", "OpenAI Chat");
    settings.setValue("ai/connection/baseUrl", server.base() + "/v1");
    settings.setValue("ai/connection/detectedProtocol", "openai_chat");
    settings.remove("ai/connection/detectedEndpoint");
    request.model = "deepseek_v4_custom_alias";
    server.handle = [&](const Request& r) {
        if (r.url.path().endsWith("chat/completions")) {
            check("custom_deepseek_alias_preserved", r.body.value("model").toString() == request.model);
            check("compatible_token_limit_retained", r.body.value("max_tokens").toInt() == 75 && !r.body.contains("max_completion_tokens"));
            return Reply{200, R"({"choices":[{"message":{"content":"deepseek answer","reasoning_content":"thinking"}}]})", {}};
        }
        return error();
    };
    response = provider.chat(request);
    check("deepseek_response_excludes_reasoning", response.success && response.rawText == "deepseek answer");

    request.model = "qwen3-vl-custom";
    request.frames.append(AIMediaFrameReference{});
    request.frames[0].encodedBytes = "mock-image";
    request.frames[0].mimeType = "image/png";
    settings.setValue("ai/connection/providerType", "Qwen");
    server.handle = [&](const Request& r) {
        if (r.method == "GET") return Reply{200, R"({"data":[{"id":"qwen-future-id"}]})", {}};
        check("qwen_custom_id_preserved", r.body.value("model").toString() == "qwen3-vl-custom");
        check("qwen_vision_no_text_only_warning", !QJsonDocument(r.body).toJson().contains("gpt-5"));
        const auto messages = r.body.value("messages").toArray();
        check("qwen_vision_image_body", messages.last().toObject().value("content").toArray().last().toObject().value("type").toString() == "image_url");
        return Reply{200, R"({"choices":[{"message":{"content":"qwen answer"}}]})", {}};
    };
    response = provider.chat(request);
    check("qwen_runtime_reply", response.success && response.rawText == "qwen answer");
    models = discovery.discover(AIProviderCategory::Qwen, server.base() + "/compatible-mode/v1", "dummy");
    check("qwen_discovers_opaque_new_id", models.success && models.models == QStringList{"qwen-future-id"});
    request.frames.clear();

    settings.setValue("ai/connection/providerType", "Claude");
    settings.setValue("ai/connection/detectedProvider", "Anthropic Messages");
    settings.setValue("ai/connection/detectedProtocol", "anthropic_messages");
    settings.remove("ai/connection/detectedEndpoint");
    request.model = "claude-custom-new";
    server.handle = [&](const Request& r) {
        check("anthropic_runtime_endpoint", r.url.path() == "/v1/messages");
        check("anthropic_runtime_auth", r.headers.value("x-api-key") == "dummy-contract-key" && r.headers.value("anthropic-version") == "2023-06-01");
        check("anthropic_runtime_schema", r.body.value("max_tokens").toInt() == 75 && r.body.value("model").toString() == request.model);
        return Reply{200, R"({"type":"message","content":[{"type":"thinking","thinking":"hidden"},{"type":"text","text":"claude answer"}]})", {}};
    };
    response = provider.chat(request);
    check("anthropic_excludes_thinking", response.success && response.rawText == "claude answer");

    settings.setValue("ai/connection/providerType", "Ollama");
    settings.setValue("ai/connection/detectedProvider", "Ollama");
    settings.setValue("ai/connection/detectedProtocol", "ollama_chat");
    settings.setValue("ai/connection/baseUrl", server.base());
    settings.remove("ai/connection/detectedEndpoint");
    request.model = "custom-local-no-colon";
    server.handle = [&](const Request& r) {
        check("ollama_preserves_custom_id", r.url.path() == "/api/chat" && r.body.value("model").toString() == request.model);
        check("ollama_nonstream_no_cloud_auth", r.body.value("stream").isBool() && !r.body.value("stream").toBool() && !r.headers.contains("authorization"));
        return Reply{200, R"({"message":{"role":"assistant","content":"local answer"}})", {}};
    };
    response = provider.chat(request);
    check("ollama_runtime_reply", response.success && response.rawText == "local answer");

    server.requests.clear();
    server.handle = [](const Request& r) {
        if (r.method == "GET") return Reply{200, R"({"data":[{"id":"other-model"}]})", {}};
        return error();
    };
    AIProviderDetector detector(&credentials, &settings);
    AIDetectionRequest detectionRequest;
    detectionRequest.apiKey = "dummy";
    detectionRequest.baseUrl = server.base() + "/v1";
    detectionRequest.model = "my-new-model";
    const auto detection = detector.detect(detectionRequest);
    bool changedModel = false;
    for (const auto& r : server.requests) {
        if (r.method == "POST" && r.body.contains("model") && r.body.value("model").toString() != detectionRequest.model) changedModel = true;
    }
    check("explicit_invalid_model_not_substituted", !detection.success && !changedModel);

    Settings dynamicSettings;
    dynamicSettings.setValue("ai/connection/baseUrl", server.base() + "/v1");
    dynamicSettings.setValue("ai/connection/providerType", "OpenAICompatible");
    dynamicSettings.setValue("ai/connection/availableModels", QStringList{"text-embedding-only", "future-vendor-id", "gpt-5"});
    OpenAIResponsesProvider dynamicProvider(&credentials, &dynamicSettings);
    server.handle = [&](const Request& r) {
        check("server_model_order_not_obsolete_priority", r.body.value("model").toString() == "future-vendor-id");
        return Reply{200, R"({"choices":[{"message":{"content":"ok"}}]})", {}};
    };
    AIRequest automaticRequest;
    automaticRequest.userPrompt = "test";
    check("unknown_new_server_model_usable", dynamicProvider.chat(automaticRequest).success);

    Settings wizardSettings;
    WizardDetector wizardDetector;
    ServiceLocator::registerService<ISettingsService>(&wizardSettings);
    ServiceLocator::registerService<IAIProviderDetector>(&wizardDetector);
    ServiceLocator::registerService<IAICredentialStore>(&credentials);
    AIConnectionWizardWidget wizard;
    auto* automatic = wizard.findChild<QCheckBox*>("aiConnectionAutoModel");
    auto* modelCombo = wizard.findChild<QComboBox*>("aiConnectionModel");
    auto* save = wizard.findChild<QPushButton*>("aiConnectionSave");
    automatic->setChecked(false);
    check("manual_model_editable_without_discovery", modelCombo->isEditable() && modelCombo->isEnabled() && modelCombo->count() == 0);
    modelCombo->setEditText("new-model-not-listed");
    wizard.findChild<QLineEdit*>("aiConnectionBaseUrl")->setText(server.base());
    wizard.findChild<QPushButton*>("accent")->click();
    QElapsedTimer wait;
    wait.start();
    while (!save->isEnabled() && wait.elapsed() < 3000) app.processEvents(QEventLoop::AllEvents, 10);
    check("manual_model_sent_to_detector", wizardDetector.received.model == "new-model-not-listed" && save->isEnabled());
    save->click();
    check("manual_model_saved_unchanged", wizardDetector.saved.recommendedModel == "new-model-not-listed");
    modelCombo->setEditText("another-new-model");
    check("model_edit_invalidates_previous_validation", !save->isEnabled());
    automatic->setChecked(true);
    wizard.findChild<QPushButton*>("accent")->click();
    wait.restart();
    while (!save->isEnabled() && wait.elapsed() < 3000) app.processEvents(QEventLoop::AllEvents, 10);
    check("automatic_discovery_not_pinned_to_previous_model", wizardDetector.received.model.isEmpty() && modelCombo->currentText() == "server-new-model");
    check("no_platform_visible_windows", visibleWindows() == 0);

    QJsonObject report{{"background", true}, {"platform_visible_top_level_windows", visibleWindows()},
        {"passed", static_cast<int>(checks.size()) - failed}, {"failed", failed}, {"checks", checks}};
    if (argc > 1) {
        QFile file(QString::fromLocal8Bit(argv[1]));
        if (!file.open(QIODevice::WriteOnly)) return 2;
        file.write(QJsonDocument(report).toJson());
    }
    qInfo("AI protocol contracts: %lld checks, %d failures; visible windows: %d", checks.size(), failed, visibleWindows());
    return failed ? 1 : 0;
}
