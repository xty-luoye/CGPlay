"""Exercise production update UI dispatch with a deterministic, offline worker."""
from pathlib import Path
import json
import os
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / "tests/artifacts/fullscreen_ai_20261008/update_ui"
QT = Path(os.environ.get("CGPLAY_TEST_QT_ROOT", "C:/QtClean/6.5.3/msvc2019_64"))
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

FIXTURE = r'''
#include "UpdateService.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QMainWindow>
#include <QStatusBar>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
namespace cgplay {
static std::atomic_int calls{0}, activeWorkers{0};
static std::atomic_bool releaseWorkers{false}, workerOnGuiThread{false};
static int presented = 0, applied = 0;
static bool callbackOffGuiThread = false;
UpdateService& UpdateService::instance() { static UpdateService instance; return instance; }
QUrl UpdateService::latestReleaseUrl() { return QUrl("http://127.0.0.1/unused"); }
UpdateCheckResult UpdateService::checkForUpdates(const QString& version,
    const std::shared_ptr<UpdateTransferState>& state, const QUrl&) {
    ++calls; ++activeWorkers;
    if (QThread::currentThread() == qApp->thread()) workerOnGuiThread = true;
    QElapsedTimer elapsed; elapsed.start();
    while (!state->cancelled.load() && !releaseWorkers.load() && elapsed.elapsed() < 4000) QThread::msleep(1);
    UpdateCheckResult result; result.currentVersion = version; result.remoteVersion = "1.0.7.99";
    result.refreshed = !state->cancelled.load(); result.updateAvailable = result.refreshed;
    --activeWorkers; return result;
}
QJsonObject UpdateCheckResult::toJson() const {
    return {{"currentVersion", currentVersion}, {"remoteVersion", remoteVersion}, {"updateAvailable", updateAvailable}};
}
struct Private {
    bool updateCheckInProgress = false, updateCheckInteractive = false, updateDownloadInProgress = false;
    bool updateClosing = false;
    QJsonObject latestUpdateResult;
    std::shared_ptr<UpdateTransferState> updateCheckState, updateDownloadState;
};
class MainWindow : public QMainWindow {
public:
    std::unique_ptr<Private> _p = std::make_unique<Private>();
    void _checkForUpdates(bool interactive, bool refreshRemote = true);
    void _presentUpdateResult(const QJsonObject&) {
        ++presented; callbackOffGuiThread |= QThread::currentThread() != qApp->thread();
    }
    void _applyBackgroundUpdateResult(const QJsonObject& result) {
        ++applied; _p->latestUpdateResult = result;
        callbackOffGuiThread |= QThread::currentThread() != qApp->thread();
    }
    void closeUpdates() {
        // PRODUCTION_CLOSE_UPDATES
    }
};
// PRODUCTION_CHECK_UPDATES
static QJsonObject report;
static int visiblePlatformWindows = 0;
static void pump() {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    int visible = 0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(data);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&visible));
#endif
    visiblePlatformWindows = qMax(visiblePlatformWindows, visible);
    QThread::msleep(1);
}
template<class Predicate> static bool until(Predicate predicate) {
    QElapsedTimer elapsed; elapsed.start();
    while (!predicate() && elapsed.elapsed() < 2000) pump();
    return predicate();
}
#define CHECK(condition) do { if (!(condition)) { report["failure"] = #condition; report["line"] = __LINE__; releaseWorkers = true; return false; } } while (false)
static bool run(const QString& scenario) {
    auto window = std::make_unique<MainWindow>();
    window->setAttribute(Qt::WA_DontShowOnScreen);
    window->setAttribute(Qt::WA_ShowWithoutActivating);
    window->_checkForUpdates(false);
    CHECK(until([] { return calls.load() == 1; }));
    const auto original = window->_p->updateCheckState;
    if (scenario == "duplicate-interactive") {
        window->_checkForUpdates(true); window->_checkForUpdates(true);
        CHECK(calls.load() == 1 && window->_p->updateCheckInteractive);
    } else {
        original->cancelled = true;
        // This call deliberately precedes delivery of the canceled future's finished signal.
        window->_checkForUpdates(true);
        if (scenario == "cancel-then-manual") {
            CHECK(until([] { return calls.load() == 2; }));
            CHECK(window->_p->updateCheckState != original && !window->_p->updateCheckState->cancelled.load());
        } else {
            window->closeUpdates();
            window->_checkForUpdates(true);
            if (scenario == "destroy-while-cancelling") window.reset();
            CHECK(until([] { return activeWorkers.load() == 0; }));
            for (int i = 0; i < 10; ++i) pump();
            CHECK(calls.load() == 1 && applied == 0 && presented == 0);
            return true;
        }
    }
    releaseWorkers = true;
    CHECK(until([&] { return !window->_p->updateCheckInProgress; }));
    CHECK(applied == 1 && presented == 1);
    CHECK(!workerOnGuiThread.load() && !callbackOffGuiThread);
    return true;
}
} // namespace cgplay
int main(int argc, char** argv) {
    QApplication app(argc, argv); app.setApplicationVersion("1.0.7.11");
    app.setProperty("cgplay.automationBackground", true);
    const bool passed = cgplay::run(app.arguments().value(1));
    cgplay::releaseWorkers = true;
    cgplay::report["passed"] = passed && cgplay::visiblePlatformWindows == 0;
    cgplay::report["background"] = true;
    cgplay::report["visiblePlatformWindows"] = cgplay::visiblePlatformWindows;
    cgplay::report["workerCalls"] = cgplay::calls.load();
    cgplay::report["appliedResults"] = cgplay::applied;
    cgplay::report["presentedResults"] = cgplay::presented;
    cgplay::report["workerOnGuiThread"] = cgplay::workerOnGuiThread.load();
    cgplay::report["callbackOffGuiThread"] = cgplay::callbackOffGuiThread;
    printf("%s\n", QJsonDocument(cgplay::report).toJson(QJsonDocument::Compact).constData());
    return passed && cgplay::visiblePlatformWindows == 0 ? 0 : 1;
}
'''


class UpdateUILifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source, build = ART / "source", ART / "build"
        source.mkdir(parents=True, exist_ok=True)
        application = (ROOT / "src/ui/app/Application.cpp").read_text(encoding="utf-8")
        check = "void MainWindow::_checkForUpdates" + application.split("void MainWindow::_checkForUpdates", 1)[1].split(
            "void MainWindow::_presentUpdateResult", 1)[0]
        window_input = (ROOT / "src/ui/app/ApplicationInput.cpp").read_text(encoding="utf-8")
        close = window_input.split("void MainWindow::closeEvent(QCloseEvent* event)\n{", 1)[1].split(
            "    if (_p->subtitleRefinementCancelRequested)", 1)[0]
        runner = FIXTURE.replace("// PRODUCTION_CHECK_UPDATES", check).replace("// PRODUCTION_CLOSE_UPDATES", close)
        (source / "runner.cpp").write_text(runner, encoding="utf-8")
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.21)\nproject(UpdateUILifecycle LANGUAGES CXX)\n'
            'set(CMAKE_CXX_STANDARD 17)\nfind_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)\n'
            'add_executable(update_ui_runner runner.cpp)\n'
            f'target_include_directories(update_ui_runner PRIVATE "{ROOT.as_posix()}/src/ui/app")\n'
            'target_compile_options(update_ui_runner PRIVATE /utf-8)\n'
            'target_link_libraries(update_ui_runner PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent user32)\n', encoding="utf-8")
        for name, command in [
            ("configure", ["cmake", "-S", str(source), "-B", str(build), "-G", "Visual Studio 17 2022", "-A", "x64", f"-DCMAKE_PREFIX_PATH={QT}"]),
            ("build", ["cmake", "--build", str(build), "--config", "Release", "--parallel", "2"]),
        ]:
            result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=NO_WINDOW, timeout=180)
            (ART / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode:
                raise AssertionError(result.stdout[-5000:] + result.stderr[-1500:])
        cls.runner = build / "Release/update_ui_runner.exe"

    def scenario(self, name):
        env = os.environ.copy()
        env["PATH"] = str(QT / "bin") + os.pathsep + env.get("PATH", "")
        env["QT_QPA_PLATFORM"] = "offscreen"
        result = subprocess.run([str(self.runner), name], cwd=ART, env=env, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", creationflags=NO_WINDOW, timeout=15)
        (ART / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(result.stdout.strip(), f"exit={result.returncode}: {result.stderr}")
        data = json.loads(result.stdout.strip().splitlines()[-1])
        (ART / f"{name}.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        self.assertEqual(0, result.returncode, data)
        self.assertTrue(data["background"])
        self.assertEqual(0, data["visiblePlatformWindows"])
        self.assertFalse(data["workerOnGuiThread"])
        self.assertFalse(data["callbackOffGuiThread"])

    def test_manual_check_survives_canceled_automatic_check(self):
        self.scenario("cancel-then-manual")

    def test_duplicate_clicks_share_one_check_and_one_prompt(self):
        self.scenario("duplicate-interactive")

    def test_closed_window_neither_retries_nor_starts_a_new_check(self):
        self.scenario("close-while-cancelling")

    def test_destroyed_window_does_not_receive_completion_callbacks(self):
        self.scenario("destroy-while-cancelling")


if __name__ == "__main__":
    unittest.main()
