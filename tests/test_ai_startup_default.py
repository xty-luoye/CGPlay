"""Run the production AI dock setup with a local plugin stub and real Qt widgets.

The extracted setup code is compiled unchanged. Only the external AI services
are replaced, so these checks never read credentials or contact a provider.
"""
from pathlib import Path
import json
import os
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / "tests/artifacts/fullscreen_ai_20261008/ai_startup"
QT = Path(os.environ.get("CGPLAY_TEST_QT_ROOT", "C:/QtClean/6.5.3/msvc2019_64"))
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

FIXTURE = r'''
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDockWidget>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

struct IAIProviderManager {};
struct IAIWorkflowService {};
struct IAICredentialStore {};
struct IPlaybackService {};
struct IEventBus {};
struct IAnnotationService {};
struct AnnotationManager {};
struct Settings {
    QMap<QString, QVariant> values;
    void setValue(const QString& key, const QVariant& value) { values[key] = value; }
};
struct ServiceLocator { template<class T> static T* getService() { return nullptr; } };
static int legacyConstructions = 0;
class AIAgentWorkspace : public QWidget {
public:
    AIAgentWorkspace(IAIProviderManager*, IAIWorkflowService*, IPlaybackService*,
        IAICredentialStore*, Settings*, IEventBus*, IAnnotationService*, QWidget* parent)
        : QWidget(parent) { ++legacyConstructions; }
};
static QDockWidget* createThemedDockWidget(const QString& title, QWidget* parent) { return new QDockWidget(title, parent); }
static QColor dialogSurfaceColor() { return QColor("#222222"); }
static QColor appColorProperty(const char*) { return QColor("#888888"); }
static QColor compositeOpaque(const QColor& color, const QColor&) { return color; }
static IAnnotationService* resolveAnnotationService(IAnnotationService* service, AnnotationManager*) { return service; }
struct Private {
    QDockWidget* aiDock = nullptr;
    QPointer<AIAgentWorkspace> aiWorkspace;
    std::shared_ptr<Settings> windowSettings, userSettings;
    std::shared_ptr<IPlaybackService> playbackCtrl;
    IAnnotationService* annotationService = nullptr;
};
class MainWindow : public QMainWindow {
public:
    std::unique_ptr<Private> _p = std::make_unique<Private>();
    std::unique_ptr<AnnotationManager> _annoMgr;
    void _setupAIAgentWorkspace();
    void _applyAdaptiveSidePanelLayout(bool) {}
};
// PRODUCTION_LEGACY_SETUP

class FixtureWorkspace : public QWidget {
    Q_OBJECT
public:
    FixtureWorkspace() { setObjectName("CodexAgentWorkspace"); }
signals:
    void closeRequested();
};
class FixturePlugin : public QObject {
    Q_OBJECT
public:
    int constructions = 0;
    QPointer<QWidget> workspace;
    Q_INVOKABLE QWidget* workspaceWidget() {
        if (!workspace) { ++constructions; workspace = new FixtureWorkspace; }
        return workspace;
    }
};
class PluginManager {
public:
    FixturePlugin plugin;
    QObject* pluginObject(const QString&) { return &plugin; }
};
class FixtureApplication : public QApplication {
public:
    using QApplication::QApplication;
    ~FixtureApplication() { if (_mainWindow) _mainWindow->close(); _mainWindow.reset(); }
    std::unique_ptr<MainWindow> _mainWindow;
    std::unique_ptr<PluginManager> _pluginManager = std::make_unique<PluginManager>();
    std::shared_ptr<Settings> _windowSettings = std::make_shared<Settings>();
    bool _dumpRuntimeMode = false, _componentCheckMode = false;
    bool _qwenAsrProviderSmokeMode = false, _codexWorkbenchSmokeMode = false;
    void setup(bool legacyOnly) {
        _windowSettings->setValue("ai/workspaceVisible", true);
        _windowSettings->setValue("codex/workspaceVisible", true);
        _mainWindow = std::make_unique<MainWindow>();
        _mainWindow->setAttribute(Qt::WA_DontShowOnScreen);
        _mainWindow->setAttribute(Qt::WA_ShowWithoutActivating);
        _mainWindow->resize(1400, 800);
        _mainWindow->setCentralWidget(new QWidget);
        _mainWindow->menuBar()->addMenu(QStringLiteral("视图"));
        _mainWindow->menuBar()->addMenu(QStringLiteral("窗口"));
        _mainWindow->_p->windowSettings = _windowSettings;
        _mainWindow->_setupAIAgentWorkspace();
        if (legacyOnly) return;
        // Simulate an old visible AI layout before the Runtime installs Codex.
        _mainWindow->_p->aiDock->show();
        // PRODUCTION_CODEX_SETUP
    }
};

static QJsonObject report;
static int maximumVisible = 0;
static void sampleVisibility() {
    int visible = 0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(data);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&visible));
#endif
    maximumVisible = qMax(maximumVisible, visible);
}
static void pump() {
    for (int i = 0; i < 8; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        sampleVisibility();
    }
}
#define CHECK(condition) do { if (!(condition)) { report["failure"] = #condition; report["line"] = __LINE__; return false; } } while (false)
static bool run(FixtureApplication& app, const QString& scenario) {
    const bool legacyOnly = scenario == "legacy-only";
    app.setup(legacyOnly);
    auto* aiDock = app._mainWindow->_p->aiDock;
    auto* codexDock = app._mainWindow->findChild<QDockWidget*>("CodexAgentWorkspaceDock");
    auto& plugin = app._pluginManager->plugin;
    app._mainWindow->show(); pump();
    CHECK(aiDock->isHidden());
    CHECK(legacyConstructions == 0);
    if (legacyOnly) {
        aiDock->toggleViewAction()->trigger(); pump();
        CHECK(!aiDock->isHidden() && legacyConstructions == 1);
        auto* workspace = aiDock->widget();
        aiDock->toggleViewAction()->trigger(); pump();
        aiDock->toggleViewAction()->trigger(); pump();
        CHECK(!aiDock->isHidden() && aiDock->widget() == workspace && legacyConstructions == 1);
        return true;
    }
    CHECK(codexDock && codexDock->isHidden() && plugin.constructions == 0);
    CHECK(!app._windowSettings->values.value("codex/workspaceVisible").toBool());
    if (scenario == "startup-old-visible") return true;
    auto* action = codexDock->toggleViewAction();
    CHECK(action && !action->isChecked());
    if (scenario == "close-before-load") {
        action->trigger(); action->trigger(); pump();
        CHECK(codexDock->isHidden() && plugin.constructions == 0);
        return true;
    }
    if (scenario == "close-after-load-request") {
        action->trigger();
        auto* loadButton = codexDock->findChild<QPushButton*>("CodexLazyLoadButton");
        CHECK(loadButton);
        loadButton->click();
        codexDock->hide(); pump();
        CHECK(codexDock->isHidden() && plugin.constructions == 0 && loadButton->isEnabled());
        return true;
    }
    if (scenario == "destroy-before-load") {
        action->trigger(); app._mainWindow->close(); app._mainWindow.reset(); pump();
        CHECK(plugin.constructions == 0);
        return true;
    }
    action->trigger(); pump();
    CHECK(!codexDock->isHidden() && plugin.constructions == 1);
    CHECK(aiDock->isHidden());
    QWidget* workspace = codexDock->widget();
    CHECK(workspace && workspace->objectName() == "CodexAgentWorkspace");
    CHECK(app._windowSettings->values.value("codex/workspaceVisible").toBool());
    action->trigger(); pump();
    CHECK(codexDock->isHidden());
    CHECK(!app._windowSettings->values.value("codex/workspaceVisible").toBool());
    action->trigger(); pump();
    CHECK(!codexDock->isHidden() && plugin.constructions == 1 && codexDock->widget() == workspace);
    return true;
}
int main(int argc, char** argv) {
    FixtureApplication app(argc, argv);
    app.setProperty("cgplay.automationBackground", true);
    const bool passed = run(app, app.arguments().value(1));
    sampleVisibility();
    report["background"] = true;
    report["visiblePlatformWindows"] = maximumVisible;
    report["passed"] = passed && maximumVisible == 0;
    report["codexConstructions"] = app._pluginManager->plugin.constructions;
    report["legacyConstructions"] = legacyConstructions;
    printf("%s\n", QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
    return passed && maximumVisible == 0 ? 0 : 1;
}
#include "runner.moc"
'''


class AIStartupDefaultTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source, build = ART / "source", ART / "build"
        source.mkdir(parents=True, exist_ok=True)
        application = (ROOT / "src/ui/app/Application.cpp").read_text(encoding="utf-8")
        legacy = application.split("void MainWindow::_setupAIAgentWorkspace()", 1)[1].split(
            "void MainWindow::_setupMenuBar()", 1)[0]
        runtime = (ROOT / "src/ui/app/ApplicationRuntime.cpp").read_text(encoding="utf-8")
        codex = runtime.split("    const bool suppressCodexWorkspace =", 1)[1].split(
            '    recordStartupPhase("workspaces");', 1)[0]
        runner = FIXTURE.replace("// PRODUCTION_LEGACY_SETUP", "void MainWindow::_setupAIAgentWorkspace()" + legacy)
        runner = runner.replace("// PRODUCTION_CODEX_SETUP", "const bool suppressCodexWorkspace =" + codex)
        (source / "runner.cpp").write_text(runner, encoding="utf-8")
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.21)\nproject(AIStartupDefault LANGUAGES CXX)\n'
            'set(CMAKE_CXX_STANDARD 17)\nset(CMAKE_AUTOMOC ON)\n'
            'find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)\n'
            'add_executable(ai_startup_runner runner.cpp)\n'
            'target_compile_options(ai_startup_runner PRIVATE /utf-8)\n'
            'target_link_libraries(ai_startup_runner PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets user32)\n',
            encoding="utf-8")
        commands = [
            ("configure", ["cmake", "-S", str(source), "-B", str(build), "-G", "Visual Studio 17 2022", "-A", "x64", f"-DCMAKE_PREFIX_PATH={QT}"]),
            ("build", ["cmake", "--build", str(build), "--config", "Release", "--parallel", "2"]),
        ]
        for name, command in commands:
            result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=NO_WINDOW, timeout=180)
            (ART / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode:
                raise AssertionError(result.stdout[-5000:] + result.stderr[-1500:])
        cls.runner = build / "Release/ai_startup_runner.exe"

    def scenario(self, name):
        env = os.environ.copy()
        env["PATH"] = str(QT / "bin") + os.pathsep + env.get("PATH", "")
        env["QT_QPA_PLATFORM"] = "offscreen"
        result = subprocess.run([str(self.runner), name], cwd=ART, env=env, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", creationflags=NO_WINDOW, timeout=30)
        (ART / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(result.stdout.strip(), result.stderr)
        data = json.loads(result.stdout.strip().splitlines()[-1])
        (ART / f"{name}.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        self.assertEqual(0, result.returncode, data)
        self.assertTrue(data["background"])
        self.assertEqual(0, data["visiblePlatformWindows"])

    def test_old_visible_settings_do_not_open_or_construct_workspaces(self):
        self.scenario("startup-old-visible")

    def test_explicit_open_close_reopen_reuses_workspace(self):
        self.scenario("open-close-reopen")

    def test_rapid_close_cancels_queued_construction_and_reopen(self):
        self.scenario("close-before-load")

    def test_destroying_owner_cancels_queued_construction(self):
        self.scenario("destroy-before-load")

    def test_closing_after_load_request_cancels_queued_plugin_call(self):
        self.scenario("close-after-load-request")

    def test_legacy_ai_stays_lazy_without_codex_plugin(self):
        self.scenario("legacy-only")

    def test_startup_layout_does_not_restore_saved_ai_visibility(self):
        source = (ROOT / "src/ui/app/ApplicationInput.cpp").read_text(encoding="utf-8")
        restore = source.split("void MainWindow::_restoreState()", 1)[1].split("void MainWindow::_saveState()", 1)[0]
        self.assertNotIn('_p->aiDock->show()', restore)
        self.assertNotIn('value(QStringLiteral("ai/workspaceVisible")', restore)
        media = (ROOT / "src/ui/app/ApplicationMedia.cpp").read_text(encoding="utf-8")
        startup = media.split("void MainWindow::_restoreAuxDocksAfterShow()", 1)[1]
        startup = startup.split("void MainWindow::_applyAdaptiveSidePanelLayout", 1)[0]
        self.assertNotIn('value(QStringLiteral("ai/workspaceVisible")', startup)
        self.assertNotIn('setVisible(workspace.panels.value(QStringLiteral("codex")).visible)', startup)


if __name__ == "__main__":
    unittest.main()
