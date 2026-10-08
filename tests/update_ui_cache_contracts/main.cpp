// The two GUI methods under test are extracted unchanged from Application.cpp.
// Only the network/install boundary is stubbed; no real installer is launched.
#include "UpdateService.h"
#include <QApplication>
#include <QAbstractButton>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QtConcurrent>
#include <algorithm>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace cgplay {
std::atomic<int> downloads{0};
int launchAttempts = 0;
QString testDirectory;
UpdateService& UpdateService::instance() { static UpdateService service; return service; }
UpdateCheckResult UpdateCheckResult::fromJson(const QJsonObject& json) {
    UpdateCheckResult value;
    value.updateAvailable = json.value("updateAvailable").toBool();
    value.remoteVersion = json.value("remoteVersion").toString();
    value.sha256 = json.value("sha256").toString();
    value.installerSize = json.value("installerSize").toInteger();
    return value;
}
UpdateInstallResult UpdateService::downloadInstaller(const UpdateCheckResult& release,
    const std::shared_ptr<UpdateTransferState>&, const QUrl&, const QString&) {
    const int number = ++downloads;
    const QString path = testDirectory + QStringLiteral("/verified-%1.exe").arg(number);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write("fixture") != 7) return {};
    file.close();
    return {true, path, release.sha256, {}};
}
bool UpdateService::launchInstaller(const UpdateInstallResult&, QString* error) {
    ++launchAttempts;
    *error = QStringLiteral("Test boundary: installer execution is disabled.");
    return false;
}

class MainWindow : public QMainWindow {
public:
    struct Private {
        bool updateClosing = false;
        bool updateDownloadInProgress = false;
        QJsonObject latestUpdateResult;
        std::shared_ptr<UpdateTransferState> updateDownloadState;
        QString verifiedUpdateVersion;
        std::shared_ptr<UpdateInstallResult> verifiedUpdateInstaller;
    };
    std::unique_ptr<Private> _p = std::make_unique<Private>();
    int refreshes = 0;
    void _checkForUpdates(bool, bool = true) { ++refreshes; }
    bool _downloadAndLaunchInstaller(const QString& targetVersion);
    void _offerDownloadedUpdate(UpdateInstallResult installer);
};
#include "update_methods.inc"
} // namespace cgplay

int visibleWindows() {
    int count = 0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window, LPARAM target) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(target);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&count));
#endif
    return count;
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    if (argc != 2) return 2;
    cgplay::testDirectory = QString::fromLocal8Bit(argv[1]);
    cgplay::MainWindow window;
    QJsonArray results;
    int failed = 0;
    int offers = 0;
    bool installNext = false;
    QTimer answer;
    QObject::connect(&answer, &QTimer::timeout, &app, [&] {
        for (QWidget* widget : app.topLevelWidgets()) {
            auto* box = qobject_cast<QMessageBox*>(widget);
            if (!box || !box->isVisible()) continue;
            if (box->windowTitle() == QStringLiteral("更新已准备好")) {
                ++offers;
                const auto role = installNext ? QMessageBox::AcceptRole : QMessageBox::RejectRole;
                installNext = false;
                for (auto* button : box->buttons()) {
                    if (box->buttonRole(button) == role) { button->click(); break; }
                }
            } else if (!box->buttons().isEmpty()) {
                box->buttons().first()->click();
            }
        }
    });
    answer.start(1);
    const auto waitForOffer = [&](int expected) {
        QElapsedTimer timer;
        timer.start();
        while (offers < expected && timer.elapsed() < 3000) app.processEvents(QEventLoop::AllEvents, 10);
    };
    const auto check = [&](const QString& name, bool pass) {
        pass = pass && visibleWindows() == 0;
        if (!pass) ++failed;
        results.append(QJsonObject{{"name", name}, {"pass", pass}});
    };
    auto& metadata = window._p->latestUpdateResult;
    metadata = {{"updateAvailable", true}, {"remoteVersion", "1.0.7.12"},
        {"installerSize", 7}, {"sha256", QString(64, 'a')}};
    window._downloadAndLaunchInstaller("1.0.7.12");
    waitForOffer(1);
    check("initial download offers confirmation", cgplay::downloads == 1 && offers == 1 && cgplay::launchAttempts == 0);
    check("verified result retained after later", window._p->verifiedUpdateVersion == "1.0.7.12" && window._p->verifiedUpdateInstaller);
    window._downloadAndLaunchInstaller("1.0.7.12");
    check("same version later repeat does not create worker", cgplay::downloads == 1 && offers == 2 && cgplay::launchAttempts == 0);
    window._downloadAndLaunchInstaller("1.0.7.12");
    check("repeated deferral still requires confirmation", cgplay::downloads == 1 && offers == 3 && cgplay::launchAttempts == 0);

    // Delete only the exact test-owned fixture to simulate temporary-file cleanup.
    QFile::remove(window._p->verifiedUpdateInstaller->installerPath);
    window._downloadAndLaunchInstaller("1.0.7.12");
    waitForOffer(4);
    check("missing cached file starts a new download", cgplay::downloads == 2 && offers == 4);
    metadata["remoteVersion"] = "1.0.7.13";
    window._downloadAndLaunchInstaller("1.0.7.13");
    waitForOffer(5);
    check("new version never reuses old installer", cgplay::downloads == 3 && offers == 5 && window._p->verifiedUpdateVersion == "1.0.7.13");
    metadata["sha256"] = QString(64, 'b');
    window._downloadAndLaunchInstaller("1.0.7.13");
    waitForOffer(6);
    check("changed release digest invalidates cached installer", cgplay::downloads == 4 && offers == 6);
    installNext = true;
    window._downloadAndLaunchInstaller("1.0.7.13");
    check("cached install still goes through launch boundary after confirmation", cgplay::downloads == 4 && offers == 7 && cgplay::launchAttempts == 1);
    window._p->updateClosing = true;
    check("closed window does not reopen install prompt", !window._downloadAndLaunchInstaller("1.0.7.13") && offers == 7);

    const QJsonObject report{{"background", true}, {"platform_visible_top_level_windows", visibleWindows()},
        {"passed", static_cast<int>(results.size()) - failed}, {"failed", failed},
        {"installerLaunchExecuted", false}, {"downloads", cgplay::downloads.load()}, {"results", results}};
    QFile output(cgplay::testDirectory + "/report.json");
    if (!output.open(QIODevice::WriteOnly)) return 3;
    output.write(QJsonDocument(report).toJson());
    return failed ? 1 : 0;
}
