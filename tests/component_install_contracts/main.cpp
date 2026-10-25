// Draft test: compile after integration against the actual production helper.
// Test output is deliberately restricted to the path supplied on the command line.
#include "services/component/ComponentPayloadStore.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTextStream>

using namespace cgplay::component_payload_store;
static bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
static QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
static int visibleWindowCount()
{
    int count=0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window,LPARAM context)->BOOL {
        DWORD pid=0;
        GetWindowThreadProcessId(window,&pid);
        if(pid==GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(context);
        return TRUE;
    },reinterpret_cast<LPARAM>(&count));
#endif
    return count;
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc,argv);
    if (argc != 2) return 2;
    const QString artifacts = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
    if (!QDir().mkpath(artifacts)) return 3;
    QTemporaryDir root(QDir(artifacts).filePath(QStringLiteral("payload-test-XXXXXX")));
    root.setAutoRemove(false);
    const QString applicationRoot = QDir(root.path()).filePath(QStringLiteral("app"));
    const QString components = QDir(applicationRoot).filePath(QStringLiteral("components"));
    const QString extracted = QDir(root.path()).filePath(QStringLiteral("extracted"));
    QDir().mkpath(components);
    writeFile(QDir(applicationRoot).filePath(QStringLiteral("a.txt")), "old-a");
    writeFile(QDir(applicationRoot).filePath(QStringLiteral("b.dll")), "old-b");
    writeFile(QDir(extracted).filePath(QStringLiteral("a.txt")), "new-a");
    writeFile(QDir(extracted).filePath(QStringLiteral("b.dll")), "new-b");
    QJsonArray assertions;
    int failures = 0;
    const auto check = [&](const QString& id, bool pass) {
        assertions.append(QJsonObject{{QStringLiteral("id"),id},{QStringLiteral("passed"),pass}});
        if (!pass) ++failures;
    };
#ifdef Q_OS_WIN
    const QString lockedPath = QDir(applicationRoot).filePath(QStringLiteral("b.dll"));
    const HANDLE locked = CreateFileW(reinterpret_cast<LPCWSTR>(lockedPath.utf16()),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    check(QStringLiteral("old-dll-locked"),locked != INVALID_HANDLE_VALUE);
#endif
    QJsonObject candidate;
    QString error;
    check(QStringLiteral("stage-while-old-dll-locked"), prepare(extracted,components,QStringLiteral("fixture"),QStringLiteral("2"),
        {QStringLiteral("a.txt"),QStringLiteral("b.dll")},&candidate,&error));
    const QString selected = resolvePublishedRoot(components,QStringLiteral("fixture"),candidate.value(QStringLiteral("payloadRoot")).toString());
    check(QStringLiteral("new-complete-root"),readFile(QDir(selected).filePath(QStringLiteral("a.txt")))=="new-a" &&
        readFile(QDir(selected).filePath(QStringLiteral("b.dll")))=="new-b");
    check(QStringLiteral("application-root-untouched"),readFile(QDir(applicationRoot).filePath(QStringLiteral("a.txt")))=="old-a");
#ifdef Q_OS_WIN
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
#endif
    check(QStringLiteral("old-dll-unchanged"),readFile(QDir(applicationRoot).filePath(QStringLiteral("b.dll")))=="old-b");
    QJsonObject rejected;
    check(QStringLiteral("missing-required-no-publish"),!prepare(extracted,components,QStringLiteral("fixture"),QStringLiteral("3"),
        {QStringLiteral("missing.bin")},&rejected,&error) && rejected.isEmpty());
    check(QStringLiteral("canceled-no-publish"),!prepare(extracted,components,QStringLiteral("fixture"),QStringLiteral("3"),
        {QStringLiteral("a.txt")},&rejected,&error,[](){return true;}) && rejected.isEmpty());
    check(QStringLiteral("core-cannot-live-install"),!prepare(extracted,components,QStringLiteral("core"),QStringLiteral("3"),
        {QStringLiteral("a.txt")},&rejected,&error));
    check(QStringLiteral("pointer-traversal-rejected"),resolvePublishedRoot(components,QStringLiteral("fixture"),QStringLiteral("../app")).isEmpty());
    check(QStringLiteral("wrong-id-pointer-rejected"),resolvePublishedRoot(components,QStringLiteral("other"),candidate.value(QStringLiteral("payloadRoot")).toString()).isEmpty());
    const QString tools = QDir(root.path()).filePath(QStringLiteral("tools"));
    writeFile(QDir(tools).filePath(QStringLiteral("cgplay/export/export_engine.py")),"print('ok')");
    QJsonObject toolsCandidate;
    check(QStringLiteral("unique-cgplay-child-not-stripped"),prepare(tools,components,QStringLiteral("tools-fixture"),QStringLiteral("1"),
        {QStringLiteral("cgplay/export/export_engine.py")},&toolsCandidate,&error));
    const QString statePath=QDir(components).filePath(QStringLiteral("installed-components.json"));
    const QJsonObject oldRecord{{QStringLiteral("version"),QStringLiteral("1")}};
    const QJsonObject oldComponents{{QStringLiteral("fixture"),oldRecord}};
    const QJsonObject oldState{{QStringLiteral("components"),oldComponents}};
    check(QStringLiteral("legacy-state-compatible"),writeState(statePath,oldState,&error));
    const QByteArray previous=readFile(statePath);
#ifdef Q_OS_WIN
    const HANDLE stateLock=CreateFileW(reinterpret_cast<LPCWSTR>(statePath.utf16()),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    const QJsonObject nextRecord{{QStringLiteral("version"),QStringLiteral("2")},
        {QStringLiteral("payloadRoot"),candidate.value(QStringLiteral("payloadRoot"))}};
    const QJsonObject nextComponents{{QStringLiteral("fixture"),nextRecord}};
    const QJsonObject nextState{{QStringLiteral("components"),nextComponents}};
    check(QStringLiteral("state-locked-commit-rejected"),stateLock!=INVALID_HANDLE_VALUE && !writeState(statePath,nextState,&error));
    if(stateLock!=INVALID_HANDLE_VALUE) CloseHandle(stateLock);
    check(QStringLiteral("failed-state-preserves-old"),readFile(statePath)==previous);
    check(QStringLiteral("atomic-state-success"),writeState(statePath,nextState,&error));
    const auto committed=QJsonDocument::fromJson(readFile(statePath)).object();
    check(QStringLiteral("pointer-selects-complete-root"),committed==nextState && !selected.isEmpty());
#endif
    const int visibleWindows=visibleWindowCount();
    check(QStringLiteral("no-platform-visible-windows"),visibleWindows==0);
    const QJsonObject report{{QStringLiteral("passed"),failures==0},{QStringLiteral("backgroundMode"),true},
        {QStringLiteral("visiblePlatformWindows"),visibleWindows},
        {QStringLiteral("syntheticOnly"),true},{QStringLiteral("assertions"),assertions},
        {QStringLiteral("failures"),failures},{QStringLiteral("testRoot"),root.path()}};
    writeFile(QDir(artifacts).filePath(QStringLiteral("payload-store-report.json")),QJsonDocument(report).toJson(QJsonDocument::Indented));
    QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Compact) << '\n';
    return failures==0 ? 0 : 1;
}
