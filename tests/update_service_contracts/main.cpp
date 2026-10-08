#include "UpdateService.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <future>
#include <thread>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
using namespace cgplay;

QByteArray fixtureBytes() { return QByteArray("MZ-CGPlay-update-contract\n").repeated(4096); }
QJsonObject fixtureRelease(const QString& version = QStringLiteral("1.0.7.12"))
{
    const auto bytes = fixtureBytes();
    const QString name = QStringLiteral("CGPlay_Setup_%1_full.exe").arg(version);
    return {{"draft", false}, {"prerelease", false}, {"tag_name", "v" + version}, {"body", "Release notes"},
        {"assets", QJsonArray{QJsonObject{{"name", name}, {"state", "uploaded"}, {"size", bytes.size()},
            {"digest", "sha256:" + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
            {"browser_download_url", QStringLiteral("https://github.com/xty-luoye/CGPlay/releases/download/v%1/%2").arg(version, name)}}}}};
}
UpdateCheckResult parse(const QJsonObject& release, const QString& current = QStringLiteral("1.0.7.9"))
{ return UpdateService::parseRelease(QJsonDocument(release).toJson(), current); }
int visibleWindows()
{
    int count = 0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(parameter);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&count));
#endif
    return count;
}
template<class F> auto worker(F&& fn) { return std::async(std::launch::async, std::forward<F>(fn)).get(); }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 4 && QByteArray(argv[1]) == "--live-check") {
        const QString current = QString::fromLocal8Bit(argv[2]);
        const auto result = worker([&] {
            return UpdateService::instance().checkForUpdates(current, std::make_shared<UpdateTransferState>());
        });
        auto report = result.toJson();
        report["background"] = true;
        report["visiblePlatformWindows"] = visibleWindows();
        report["installerDownloadExecuted"] = false;
        QFile file(QString::fromLocal8Bit(argv[3]));
        if (!file.open(QIODevice::WriteOnly)) return 3;
        file.write(QJsonDocument(report).toJson());
        return result.refreshed && visibleWindows() == 0 ? 0 : 1;
    }
    if (argc != 3) return 2;
    const QString base = QString::fromLocal8Bit(argv[1]);
    const QDir out(QString::fromLocal8Bit(argv[2]));
    QDir().mkpath(out.absolutePath());
    QJsonArray results;
    int fail = 0;
    const auto check = [&](const QString& name, bool pass, const QJsonObject& details = QJsonObject{}) {
        const int windows = visibleWindows();
        pass = pass && windows == 0;
        if (!pass) ++fail;
        results.append(QJsonObject{{"name", name}, {"pass", pass}, {"background", true},
            {"visiblePlatformWindows", windows}, {"details", details}});
    };
    const auto valid = fixtureRelease();
    auto release = parse(valid);
    check("Numeric four-component version ordering .9 to .12", release.refreshed && release.updateAvailable);
    check("Same version", parse(valid, "1.0.7.12").refreshed && !parse(valid, "1.0.7.12").updateAvailable);
    check("Older remote version", parse(valid, "1.0.7.13").refreshed && !parse(valid, "1.0.7.13").updateAvailable);
    check("Three/four component equality", !parse(fixtureRelease("1.0.7.0"), "1.0.7").updateAvailable);
    check("Three component upgrade", parse(fixtureRelease("1.0.8"), "1.0.7.99").updateAvailable);
    check("Malformed JSON", !UpdateService::parseRelease("not-json", "1.0.7.9").refreshed);
    check("Non-object JSON", !UpdateService::parseRelease("[]", "1.0.7.9").refreshed);
    for (const QString& invalid : {QString("1.2"), QString("1.0.7.12-beta"), QString("1.0.7.12.1"),
            QString("18446744073709551616.0.1"), QString("1.0.7.$(test)")})
        check("Invalid version " + invalid, !parse(fixtureRelease(invalid)).refreshed);
    auto modified = valid;
    modified["draft"] = true;
    check("Draft rejected", !parse(modified).refreshed);
    modified = valid; modified["prerelease"] = true;
    check("Prerelease rejected", !parse(modified).refreshed);
    modified = valid; modified.remove("draft");
    check("Missing publication state rejected", !parse(modified).refreshed);
    modified = valid; modified["assets"] = QJsonArray{};
    check("Newer version missing installer fails", !parse(modified).refreshed && !parse(modified).error.isEmpty());
    check("Current version needs no new installer", parse(modified, "1.0.7.12").refreshed);
    const auto asset = valid["assets"].toArray().first().toObject();
    const auto badAsset = [&](const QString& key, const QJsonValue& value) {
        auto a = asset; a[key] = value;
        auto doc = valid; doc["assets"] = QJsonArray{a};
        return parse(doc);
    };
    for (const QString& key : {QString("state"), QString("name"), QString("digest")})
        check("Invalid asset " + key, !badAsset(key, "invalid").refreshed);
    check("Short hash", !badAsset("digest", "sha256:abc").refreshed);
    check("Non-hex hash", !badAsset("digest", "sha256:" + QString(64, 'z')).refreshed);
    for (double size : {-1., 0., 0.5, 4294967297.})
        check("Invalid size " + QString::number(size), !badAsset("size", size).refreshed);
    check("4 GiB maximum accepted", badAsset("size", 4294967296.).updateAvailable);
    for (const QString& url : {QString("http://github.com/xty-luoye/CGPlay/releases/download/v1.0.7.12/CGPlay_Setup_1.0.7.12_full.exe"),
            QString("https://github.com.evil.test/xty-luoye/CGPlay/releases/download/v1.0.7.12/CGPlay_Setup_1.0.7.12_full.exe"),
            QString("https://github.com@evil.test/CGPlay_Setup_1.0.7.12_full.exe"),
            asset["browser_download_url"].toString() + "?x=1", asset["browser_download_url"].toString() + "#x",
            QString("file:///C:/bad.exe"), QString("https://github.com/other/CGPlay/releases/download/v1.0.7.12/CGPlay_Setup_1.0.7.12_full.exe")})
        check("Untrusted installer URL rejected", !badAsset("browser_download_url", url).refreshed, {{"url", url}});
    modified = valid; modified["assets"] = QJsonArray{asset, asset};
    check("Duplicate matching assets rejected", !parse(modified).refreshed);
    check("Value serialization", UpdateCheckResult::fromJson(release.toJson()).toJson() == release.toJson());

    auto& service = UpdateService::instance();
    auto state = std::make_shared<UpdateTransferState>();
    auto live = worker([&] { return service.checkForUpdates("1.0.7.9", state, QUrl(base + "/release")); });
    check("Local HTTP metadata on worker", live.refreshed && live.updateAvailable);
    const auto badMetadata = worker([&] { return service.checkForUpdates("1.0.7.9", state, QUrl(base + "/metadata-large")); });
    check("Metadata response bounded", !badMetadata.refreshed && !badMetadata.error.isEmpty());
    const auto statusFailure = worker([&] { return service.checkForUpdates("1.0.7.9", state, QUrl(base + "/not-found")); });
    check("HTTP failure", !statusFailure.refreshed && !statusFailure.error.isEmpty());
    auto download = worker([&] { return service.downloadInstaller(live, state, QUrl(base + "/installer"), out.absolutePath()); });
    QFile downloaded(download.installerPath);
    const bool opened = downloaded.open(QIODevice::ReadOnly);
    check("Successful download size and hash", download.downloaded && opened && downloaded.readAll() == fixtureBytes() &&
        state->received.load() == fixtureBytes().size());
    downloaded.close();
    const auto redirect = worker([&] { return service.downloadInstaller(live, state, QUrl(base + "/redirect"), out.absolutePath()); });
    check("Redirect and unique directory", redirect.downloaded && QFileInfo(redirect.installerPath).absolutePath() != QFileInfo(download.installerPath).absolutePath());
    const auto directories = [&] { return out.entryList({"CGPlay-update-*"}, QDir::Dirs | QDir::NoDotAndDotDot).size(); };
    for (const QString& path : {QString("/corrupt"), QString("/truncate"), QString("/oversize"), QString("/redirect-loop")}) {
        const auto before = directories();
        const auto failed = worker([&] { return service.downloadInstaller(live, state, QUrl(base + path), out.absolutePath()); });
        check("Failure cleans temporary files " + path, !failed.downloaded && !failed.error.isEmpty() && directories() == before,
            {{"error", failed.error}});
    }
    const auto beforeCancel = directories();
    state->cancelled = true;
    const auto preCancelled = worker([&] { return service.downloadInstaller(live, state, QUrl(base + "/installer"), out.absolutePath()); });
    check("Pre-cancelled download", !preCancelled.downloaded && directories() == beforeCancel);
    state->cancelled = false;
    QElapsedTimer cancelClock; cancelClock.start();
    std::thread cancel([&] { std::this_thread::sleep_for(std::chrono::milliseconds(125)); state->cancelled = true; });
    const auto midCancelled = worker([&] { return service.downloadInstaller(live, state, QUrl(base + "/slow"), out.absolutePath()); });
    cancel.join();
    check("In-flight cancellation bounded and cleaned", !midCancelled.downloaded && directories() == beforeCancel && cancelClock.elapsed() < 1500,
        {{"elapsedMs", double(cancelClock.elapsed())}});
    state->cancelled = false;
    std::thread cancelCheck([&] { std::this_thread::sleep_for(std::chrono::milliseconds(125)); state->cancelled = true; });
    const auto cancelledCheck = worker([&] { return service.checkForUpdates("1.0.7.9", state, QUrl(base + "/slow")); });
    cancelCheck.join();
    check("Check request cancellation", !cancelledCheck.refreshed && !cancelledCheck.error.isEmpty());
    auto invalidRelease = live; invalidRelease.installerUrl = "https://evil.test/a.exe";
    check("Download revalidates metadata", !service.downloadInstaller(invalidRelease, {}, {}, out.absolutePath()).downloaded);

    UpdateInstallResult scriptInstaller = download;
    scriptInstaller.installerPath = out.filePath("path with spaces and 'quote' & $(expression)/installer.exe");
    const QString script = UpdateService::installerLaunchScript(scriptInstaller, 12345);
    check("Installer script fixed wait PID", script.contains("Get-Process -Id 12345") && !script.contains("-Name CGPlay"));
    check("Timeout never starts installer", script.contains("Wait-Process -Timeout 120 -ErrorAction Stop } catch { exit 1 }") &&
        script.indexOf("Wait-Process") < script.indexOf("Get-FileHash") && script.indexOf("Get-FileHash") < script.indexOf("Start-Process"));
    check("PowerShell path single quote escaping", script.contains("''quote'' & $(expression)") && script.contains("Get-FileHash -LiteralPath '") &&
        script.contains("Start-Process -FilePath '") && !script.contains("cmd.exe"));
    check("Hash rechecked after process exit", script.contains(download.sha256.toUpper()) && script.contains("if ($actual -cne '") && script.contains("{ exit 1 }"));
    check("Invalid installer cannot produce launch script", UpdateService::installerLaunchScript({}, 12345).isEmpty() &&
        UpdateService::installerLaunchScript(download, 0).isEmpty());
    QFile scriptFile(out.filePath("installer_launch_script.ps1"));
    if (scriptFile.open(QIODevice::WriteOnly)) scriptFile.write(script.toUtf8());
    // Deliberately do not call launchInstaller or run the generated script.
    QJsonObject report{{"background", true}, {"visiblePlatformWindows", visibleWindows()},
        {"results", results}, {"passed", results.size() - fail}, {"failed", fail},
        {"installerLaunchExecuted", false}};
    QFile reportFile(out.filePath("report.json"));
    if (!reportFile.open(QIODevice::WriteOnly)) return 3;
    reportFile.write(QJsonDocument(report).toJson());
    return fail ? 1 : 0;
}
