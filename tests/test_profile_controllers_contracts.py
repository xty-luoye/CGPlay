"""Compile and exercise the typed settings controller layer."""

from __future__ import annotations

import os
import subprocess
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ARTIFACT = ROOT / "tests" / "artifacts" / "profile_controllers_20260726"
CONTROLLER_DIR = ROOT / "src" / "services" / "settings" / "controllers"
PROFILE_DIR = ROOT / "src" / "common" / "settings" / "profiles"
SERVICE_DIR = ROOT / "src" / "services" / "settings"


class ProfileControllerStaticContracts(unittest.TestCase):
    def test_controllers_are_ui_free_and_delegate_persistence(self):
        expected_calls = {
            "AppearanceController": ("loadAppearance", "saveAppearance"),
            "WorkspaceController": ("loadWorkspace", "saveWorkspace"),
            "ToolbarController": ("loadToolbar", "saveToolbar"),
        }
        for controller, calls in expected_calls.items():
            header = (CONTROLLER_DIR / f"{controller}.h").read_text(encoding="utf-8")
            source = (CONTROLLER_DIR / f"{controller}.cpp").read_text(encoding="utf-8")
            self.assertIn(f"class {controller} final", header)
            for api in ("load(", "validate(", "save(", "reset("):
                self.assertIn(api, header)
            self.assertNotIn("QWidget", header + source)
            self.assertNotIn("QSaveFile", header + source)
            self.assertNotIn("QJsonDocument", header + source)
            for call in calls:
                self.assertIn(f"_service.{call}", source)

    def test_command_reference_validation_is_explicit(self):
        appearance = (CONTROLLER_DIR / "AppearanceController.cpp").read_text(encoding="utf-8")
        toolbar = (CONTROLLER_DIR / "ToolbarController.cpp").read_text(encoding="utf-8")
        self.assertIn("_knownStyleTargets.contains", appearance)
        self.assertIn("profile.validate(_knownCommands", toolbar)
        self.assertIn("without a command catalog", toolbar)


class ProfileControllerCompiledContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        ARTIFACT.mkdir(parents=True, exist_ok=True)
        source_dir = ARTIFACT / "contract_source"
        build_dir = ARTIFACT / "contract_build"
        source_dir.mkdir(exist_ok=True)
        runner = source_dir / "profile_controller_runner.cpp"
        cmake = source_dir / "CMakeLists.txt"
        runner.write_text(
            textwrap.dedent(
                r'''
                #include "services/settings/controllers/AppearanceController.h"
                #include "services/settings/controllers/ToolbarController.h"
                #include "services/settings/controllers/WorkspaceController.h"
                #include "services/settings/SettingsProfileService.h"
                #include <QCoreApplication>
                #include <QFile>
                #include <QTemporaryDir>

                using namespace cgplay;
                #define REQUIRE(x) do { if (!(x)) return __LINE__; } while (false)

                CustomToolbarButtonProfile customButton(const QString& command) {
                    CustomToolbarButtonProfile button;
                    button.id = "custom.test";
                    button.name = "Test";
                    button.commands = {command};
                    return button;
                }

                int main(int argc, char** argv) {
                    QCoreApplication app(argc, argv);
                    QTemporaryDir temp;
                    REQUIRE(temp.isValid());
                    SettingsProfileService service(temp.path());
                    QString error;
                    bool recovered = false;

                    AppearanceController appearance(service, {"playback.toggle", "custom.test"});
                    REQUIRE(!appearance.load("", &recovered, &error));
                    AppearanceProfile appearanceValue = AppearanceProfile::defaults();
                    appearanceValue.themeName = "Studio";
                    appearanceValue.buttonStyles.insert("playback.toggle", ButtonStyleProfile{});
                    REQUIRE(appearance.save("Studio", appearanceValue, &error));
                    REQUIRE(appearance.hasCurrentProfile());
                    REQUIRE(appearance.currentProfile().themeName == "Studio");
                    AppearanceProfile badAppearance = appearanceValue;
                    badAppearance.buttonStyles.insert("missing.command", ButtonStyleProfile{});
                    REQUIRE(!appearance.save("Studio", badAppearance, &error));
                    REQUIRE(!appearance.currentProfile().buttonStyles.contains("missing.command"));
                    REQUIRE(service.saveAppearance("Studio", badAppearance, &error));
                    REQUIRE(!appearance.load("Studio", &recovered, &error));
                    REQUIRE(!appearance.currentProfile().buttonStyles.contains("missing.command"));
                    REQUIRE(service.saveAppearance("Studio", appearanceValue, &error));
                    REQUIRE(appearance.load("Studio", &recovered, &error));
                    REQUIRE(error.isEmpty());
                    REQUIRE(!recovered);
                    REQUIRE(appearance.reset("Studio", &error));
                    REQUIRE(appearance.currentProfile().buttonStyles.isEmpty());

                    WorkspaceController workspace(service);
                    REQUIRE(!workspace.reset("", &error));
                    WorkspaceProfile workspaceValue = WorkspaceProfile::defaults();
                    workspaceValue.name = "Review";
                    workspaceValue.translationEnabled = true;
                    REQUIRE(workspace.save(workspaceValue, &error));
                    WorkspaceProfile invalidWorkspace = workspaceValue;
                    invalidWorkspace.panels["playlist"].size = -1;
                    REQUIRE(!workspace.save(invalidWorkspace, &error));
                    REQUIRE(workspace.currentProfile().translationEnabled);
                    workspaceValue.translationEnabled = false;
                    REQUIRE(workspace.save(workspaceValue, &error));
                    workspaceValue.translationEnabled = true;
                    REQUIRE(workspace.save(workspaceValue, &error));
                    QFile damaged(service.workspacePath("Review"));
                    REQUIRE(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
                    REQUIRE(damaged.write("not-json") > 0);
                    damaged.close();
                    REQUIRE(workspace.load("Review", &recovered, &error));
                    REQUIRE(recovered);
                    REQUIRE(!workspace.currentProfile().translationEnabled);
                    REQUIRE(workspace.reset("Review", &error));
                    REQUIRE(workspace.currentProfile().name == "Review");
                    REQUIRE(workspace.currentProfile().panels.value("playlist").size == 280);

                    ToolbarProfile toolbarValue;
                    toolbarValue.customButtons.append(customButton("playback.toggle"));
                    ToolbarController toolbar(service, {"playback.toggle", "annotation.create"});
                    REQUIRE(toolbar.save(toolbarValue, &error));
                    REQUIRE(toolbar.load(&recovered, &error));
                    REQUIRE(toolbar.currentProfile().customButtons.size() == 1);
                    ToolbarProfile badToolbar;
                    badToolbar.customButtons.append(customButton("missing.command"));
                    REQUIRE(!toolbar.save(badToolbar, &error));
                    REQUIRE(toolbar.currentProfile().customButtons.first().commands.first()
                            == "playback.toggle");
                    REQUIRE(service.saveToolbar(badToolbar, &error));
                    REQUIRE(!toolbar.load(&recovered, &error));
                    REQUIRE(toolbar.currentProfile().customButtons.first().commands.first()
                            == "playback.toggle");
                    REQUIRE(service.saveToolbar(toolbarValue, &error));
                    ToolbarController noCatalog(service);
                    REQUIRE(!noCatalog.validate(toolbarValue, &error));
                    REQUIRE(noCatalog.reset(&error));
                    REQUIRE(error.isEmpty());
                    REQUIRE(noCatalog.currentProfile().customButtons.isEmpty());
                    REQUIRE(toolbar.reset(&error));
                    REQUIRE(toolbar.currentProfile().customButtons.isEmpty());
                    return 0;
                }
                '''
            ),
            encoding="utf-8",
        )
        sources = [
            PROFILE_DIR / "AppearanceProfile.cpp",
            PROFILE_DIR / "WorkspaceProfile.cpp",
            PROFILE_DIR / "ToolbarProfile.cpp",
            SERVICE_DIR / "SettingsProfileService.cpp",
            CONTROLLER_DIR / "AppearanceController.cpp",
            CONTROLLER_DIR / "WorkspaceController.cpp",
            CONTROLLER_DIR / "ToolbarController.cpp",
        ]
        cmake.write_text(
            "cmake_minimum_required(VERSION 3.21)\n"
            "project(CGPlayProfileControllerContracts LANGUAGES CXX)\n"
            "set(CMAKE_CXX_STANDARD 17)\n"
            "find_package(Qt6 REQUIRED COMPONENTS Core Gui)\n"
            "add_executable(profile_controller_runner profile_controller_runner.cpp\n"
            + "\n".join(f'  "{path.as_posix()}"' for path in sources)
            + "\n)\n"
            + f'target_include_directories(profile_controller_runner PRIVATE "{(ROOT / "src").as_posix()}")\n'
            + "target_link_libraries(profile_controller_runner PRIVATE Qt6::Core Qt6::Gui)\n",
            encoding="utf-8",
        )
        configure = subprocess.run(
            ["cmake", "-S", str(source_dir), "-B", str(build_dir),
             "-G", "Visual Studio 17 2022", "-A", "x64",
             "-DCMAKE_PREFIX_PATH=C:/QtClean/6.5.3/msvc2019_64"],
            cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        (ARTIFACT / "configure.log").write_text(
            configure.stdout + configure.stderr, encoding="utf-8"
        )
        if configure.returncode != 0:
            raise AssertionError(f"Profile controller configure failed: {configure.stderr[-2000:]}")
        build = subprocess.run(
            ["cmake", "--build", str(build_dir), "--config", "Release"],
            cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        (ARTIFACT / "build.log").write_text(build.stdout + build.stderr, encoding="utf-8")
        if build.returncode != 0:
            raise AssertionError(f"Profile controller build failed: {build.stderr[-2000:]}")
        cls.runner = build_dir / "Release" / "profile_controller_runner.exe"

    def test_load_validate_save_reset_and_recovery(self):
        environment = os.environ.copy()
        environment["PATH"] = "C:/QtClean/6.5.3/msvc2019_64/bin;" + environment.get("PATH", "")
        result = subprocess.run(
            [str(self.runner)], cwd=ARTIFACT, capture_output=True, text=True,
            encoding="utf-8", errors="replace", env=environment,
        )
        (ARTIFACT / "runner.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
