#include "services/component/ComponentManifestPolicy.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 2) return 2;
    QFile source(QString::fromLocal8Bit(argv[1]));
    if (!source.open(QIODevice::ReadOnly)) return 2;
    QJsonObject bundled = QJsonDocument::fromJson(source.readAll()).object();
    using namespace cgplay::component_manifest_policy;
    QJsonArray checks;
    const auto check = [&checks](const char* name, bool passed) {
        checks.append(QJsonObject{{"name", name}, {"passed", passed}});
    };
    const QString github = "https://github.com/xty-luoye/CGPlay/releases/latest/download/manifest.json";
    check("shipped default uses GitHub only", sourceUrls(bundled, {}) == QStringList{github});
    auto defaults = bundled["components"].toObject();
    defaults["recording-contract-fixture"] = QJsonObject{{"version", "local"}, {"installSubdir", "runtime/recorder"}};
    bundled["components"] = defaults;
    QJsonObject cached{{"manifestUrl", "https://cgplay-app.netlify.app/manifest.json"},
        {"manifestUrls", QJsonArray{"https://cgplay-app.netlify.app/manifest.json"}},
        {"app", QJsonObject{{"version", "1.0.7.01"}}},
        {"components", QJsonObject{{"tools", QJsonObject{{"version", "remote-2"}, {"url", "https://example.invalid/tools.zip"}}}}}};
    const auto merged = mergePayload(cached, bundled);
    const auto components = merged["components"].toObject();
    check("old cache cannot remove new bundled component", components.contains("recording-contract-fixture"));
    check("old cache retains bundled core", components.contains("core"));
    check("remote component update remains selected", components["tools"].toObject()["version"].toString() == "remote-2");
    check("remote app version remains payload", merged["app"].toObject()["version"].toString() == "1.0.7.01");
    check("old cache cannot redirect routing", sourceUrls(bundled, {}) == QStringList{github});
    check("explicit override is authoritative", sourceUrls(bundled, "  http://127.0.0.1/manifest.json  ") == QStringList{"http://127.0.0.1/manifest.json"});
    check("clearing override returns shipped routing", sourceUrls(bundled, " ") == QStringList{github});
    QJsonObject aliases{{"manifestUrl", " a "}, {"manifestUrls", QJsonArray{"a", "b", "", " b "}}};
    check("source aliases are unique and ordered", sourceUrls(aliases, {}) == QStringList{"a", "b"});
    check("missing bundled source does not trust cached source", sourceUrls({}, {}).isEmpty());
    int visible = 0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(value);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&visible));
#endif
    bool passed = visible == 0;
    for (const auto& value : checks) passed = passed && value.toObject()["passed"].toBool();
    const QJsonObject report{{"backgroundMode", true}, {"nativeVisibleWindows", visible},
        {"checks", checks}, {"passed", passed}};
    const QByteArray data = QJsonDocument(report).toJson();
    std::fwrite(data.constData(), 1, data.size(), stdout);
    return passed ? 0 : 1;
}
