#include "services/component/ComponentPayloadStore.h"
#include <QCoreApplication>
#include <QThread>
#include <cstdio>

static QJsonObject identity()
{
    return {{"schema", 1}, {"ok", true}, {"obsVersion", "33.0.0"}, {"obsApi", 553648128},
        {"sourceCommit", "72e86655715d9a25d69ce446aa4dd44ba58e10d2"},
        {"manifestSHA256", QString(64, QLatin1Char('a'))}, {"capturePatch", 1}, {"wasapiPatch", 1}};
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc >= 2 && QByteArray(argv[1]) == "--check-runtime") {
        const QByteArray scenario = qgetenv("CGPLAY_COMPONENT_PROBE_SCENARIO");
        if (scenario == "sleep") QThread::msleep(5000);
        QJsonObject value = identity();
        if (scenario == "version") value["obsVersion"] = "32.0.0";
        if (scenario == "patch") value.remove("capturePatch");
        if (scenario == "digest") value["manifestSHA256"] = "not-a-digest";
        if (scenario == "api") value["obsApi"] = 1;
        if (scenario == "failure") value["ok"] = false;
        if (scenario == "stderr") {
            const QByteArray noise(32768, 'x');
            std::fwrite(noise.constData(), 1, noise.size(), stderr);
        }
        const QByteArray output = scenario == "json" ? QByteArray("invalid json") : QJsonDocument(value).toJson();
        std::fwrite(output.constData(), 1, output.size(), stdout);
        return scenario == "nonzero" ? 2 : 0;
    }
    if (argc < 2) return 2;
    const QString artifact = QString::fromLocal8Bit(argv[1]);
    QTemporaryDir fixture(QDir(artifact).filePath("runtime-fixture-XXXXXX"));
    if (!fixture.isValid()) return 3;
    if (!QFile::copy(QCoreApplication::applicationFilePath(), QDir(fixture.path()).filePath("CGPlayRecorder.exe"))) return 4;
    QJsonArray checks;
    const auto check = [&checks](const QString& name, bool passed, const QString& error = {}) {
        checks.append(QJsonObject{{"name", name}, {"passed", passed}, {"error", error}});
    };
    using namespace cgplay::component_payload_store;
    for (const QByteArray& scenario : {QByteArray("good"), QByteArray("version"), QByteArray("patch"), QByteArray("digest"),
            QByteArray("api"), QByteArray("failure"), QByteArray("stderr"), QByteArray("json"), QByteArray("nonzero")}) {
        qputenv("CGPLAY_COMPONENT_PROBE_SCENARIO", scenario);
        QString error;
        const bool accepted = validateRuntime("recording-runtime", fixture.path(), {}, {}, &error);
        const bool expected = scenario == "good" || scenario == "stderr";
        check(QString::fromLatin1(scenario), accepted == expected, error);
    }
    qputenv("CGPLAY_COMPONENT_PROBE_SCENARIO", "sleep");
    int calls = 0;
    QElapsedTimer timer; timer.start();
    QString error;
    const bool canceled = !validateRuntime("recording-runtime", fixture.path(), {}, [&calls] { return ++calls > 2; }, &error);
    check("cancel stops the probe promptly", canceled && timer.elapsed() < 1500, error);
    qunsetenv("CGPLAY_COMPONENT_PROBE_SCENARIO");
    if (argc >= 3) {
        error.clear();
        check("actual staged recorder manifest and modules", validateRuntime("recording-runtime", QString::fromLocal8Bit(argv[2]), {}, {}, &error), error);
    }
    int visible = 0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window, LPARAM count) -> BOOL {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(count);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&visible));
#endif
    bool passed = visible == 0;
    for (const auto& value : checks) passed = passed && value.toObject()["passed"].toBool();
    const QJsonObject report{{"backgroundMode", true}, {"nativeVisibleWindows", visible},
        {"checks", checks}, {"passed", passed}, {"actualRuntimeChecked", argc >= 3}};
    QFile output(QDir(artifact).filePath("report.json"));
    if (!output.open(QIODevice::WriteOnly)) return 5;
    output.write(QJsonDocument(report).toJson());
    return passed ? 0 : 1;
}
