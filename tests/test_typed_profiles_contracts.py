"""Compile and exercise the typed appearance/workspace/toolbar profile layer."""

from __future__ import annotations

import subprocess
import textwrap
import unittest
import os
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ARTIFACT = ROOT / "tests" / "artifacts" / "architecture_profiles_20260726"
PROFILE_DIR = ROOT / "src" / "common" / "settings" / "profiles"
SERVICE_DIR = ROOT / "src" / "services" / "settings"


class TypedProfileStaticContracts(unittest.TestCase):
    def test_versioned_profile_apis_exist(self):
        for name, version in (
            ("AppearanceProfile", 1),
            ("WorkspaceProfile", 2),
            ("ToolbarProfile", 1),
        ):
            header = (PROFILE_DIR / f"{name}.h").read_text(encoding="utf-8")
            self.assertIn(f"class {name}", header)
            self.assertIn(f"SchemaVersion = {version}", header)
            for api in ("defaults()", "migrate(", "fromJson(", "toJson()", "validate("):
                self.assertIn(api, header)

    def test_package_field_ownership_matches_released_contract(self):
        source = (SERVICE_DIR / "SettingsProfileService.cpp").read_text(encoding="utf-8")
        for field in (
            "theme", "shortcuts", "toolbar", "buttonStyles", "input",
            "mouseBindings", "gamepadBindings", "workspace", "workspacePreset",
        ):
            self.assertIn(f'QStringLiteral("{field}")', source)
        for part in (
            "theme.json", "shortcuts.json", "toolbar.json", "button_styles.json",
            "input.json", "mouse_bindings.json", "gamepad_bindings.json",
            "workspace.json", "metadata.json",
        ):
            self.assertIn(f'QStringLiteral("{part}")', source)

    def test_persistence_is_atomic_and_recovers_backup(self):
        source = (SERVICE_DIR / "SettingsProfileService.cpp").read_text(encoding="utf-8")
        self.assertIn("QSaveFile file(path)", source)
        self.assertIn('path + QStringLiteral(".bak")', source)
        self.assertIn('QStringLiteral(".corrupt-")', source)
        self.assertIn("restored the last valid backup", source)
        self.assertIn("isFutureVersionError", source)

    def test_legacy_aliases_and_current_bounds_are_explicit(self):
        appearance = (PROFILE_DIR / "AppearanceProfile.cpp").read_text(encoding="utf-8")
        workspace = (PROFILE_DIR / "WorkspaceProfile.cpp").read_text(encoding="utf-8")
        toolbar = (PROFILE_DIR / "ToolbarProfile.cpp").read_text(encoding="utf-8")
        for old, new in (
            ("background", "backgroundColor"), ("panel", "panelColor"),
            ("toolbar", "toolbarColor"), ("name", "themeName"),
        ):
            self.assertIn(f'QStringLiteral("{old}")', appearance)
            self.assertIn(f'QStringLiteral("{new}")', appearance)
        self.assertIn("inRange(windowOpacity, 40, 100)", appearance)
        self.assertIn("inRange(buttonOpacity, 30, 100)", appearance)
        self.assertIn('QStringLiteral("preset")', workspace)
        self.assertIn("Version 0 accepted a boolean shorthand", toolbar)


class TypedProfileCompiledContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        ARTIFACT.mkdir(parents=True, exist_ok=True)
        source_dir = ARTIFACT / "contract_source"
        build_dir = ARTIFACT / "contract_build"
        source_dir.mkdir(exist_ok=True)
        runner = source_dir / "profile_contract_runner.cpp"
        cmake = source_dir / "CMakeLists.txt"
        runner.write_text(
            textwrap.dedent(
                r'''
                #include "common/settings/profiles/AppearanceProfile.h"
                #include "common/settings/profiles/ToolbarProfile.h"
                #include "common/settings/profiles/WorkspaceProfile.h"
                #include "services/settings/SettingsProfileService.h"
                #include <QCoreApplication>
                #include <QFile>
                #include <QJsonArray>
                #include <QJsonDocument>
                #include <QTemporaryDir>

                using namespace cgplay;
                #define REQUIRE(x) do { if (!(x)) return __LINE__; } while (false)

                int main(int argc, char** argv) {
                    QCoreApplication app(argc, argv);
                    QString error;
                    bool valid = false;

                    QJsonObject legacyTheme{{"mode", "custom"}, {"background", "#112233"},
                        {"panel", "#223344"}, {"windowOpacity", 40}, {"buttonOpacity", 30}};
                    AppearanceProfile appearance = AppearanceProfile::fromJson(legacyTheme, &valid, &error);
                    REQUIRE(valid);
                    REQUIRE(appearance.backgroundColor == "#112233");
                    REQUIRE(appearance.panelColor == "#223344");
                    REQUIRE(AppearanceProfile::fromJson(appearance.toJson(), &valid, &error).toJson()
                            == appearance.toJson());
                    QJsonObject futureTheme{{"version", AppearanceProfile::SchemaVersion + 1}};
                    AppearanceProfile::fromJson(futureTheme, &valid, &error);
                    REQUIRE(!valid);

                    QJsonObject legacyWorkspace{{"version", 1}, {"preset", "Legacy"},
                        {"panels", QJsonObject{{"playlist", QJsonObject{{"visible", true},
                            {"area", "left"}, {"size", 240}}}}}};
                    WorkspaceProfile workspace = WorkspaceProfile::fromJson(legacyWorkspace, &valid, &error);
                    REQUIRE(valid && workspace.name == "Legacy");
                    REQUIRE(workspace.toJson().value("version").toInt() == WorkspaceProfile::SchemaVersion);

                    QJsonObject toolbarJson{{"legacy.annotation", true},
                        {"customButtons", QJsonArray{QJsonObject{{"id", "custom.capture"},
                            {"name", "Capture"}, {"commands", QJsonArray{"codex.captureFrame"}}}}}};
                    ToolbarProfile toolbar = ToolbarProfile::fromJson(toolbarJson, &valid, &error);
                    REQUIRE(valid && toolbar.items.value("legacy.annotation").visible);
                    REQUIRE(toolbar.validate(QSet<QString>{"codex.captureFrame"}, &error));
                    REQUIRE(!toolbar.validate(QSet<QString>{"playback.toggle"}, &error));

                    SettingsProfilePackage package = SettingsProfilePackage::defaults();
                    package.appearance = appearance;
                    package.workspace = workspace;
                    package.workspacePreset = workspace.name;
                    package.toolbar = toolbar;
                    package.input.insert("mouseWheel", "zoom");
                    REQUIRE(package.validate(QSet<QString>{"codex.captureFrame"}, &error));
                    const auto parts = package.toParts("2026-07-26T00:00:00Z");
                    REQUIRE(parts.size() == 9);
                    SettingsProfilePackage restored = SettingsProfilePackage::fromParts(
                        parts, QSet<QString>{"codex.captureFrame"}, &valid, &error);
                    REQUIRE(valid && restored.workspacePreset == "Legacy");
                    REQUIRE(restored.input.value("mouseWheel").toString() == "zoom");

                    QTemporaryDir temp;
                    REQUIRE(temp.isValid());
                    SettingsProfileService service(temp.path());
                    WorkspaceProfile first = workspace;
                    first.name = "Recovery";
                    first.translationEnabled = false;
                    REQUIRE(service.saveWorkspace(first, &error));
                    WorkspaceProfile second = first;
                    second.translationEnabled = true;
                    REQUIRE(service.saveWorkspace(second, &error));
                    QFile damaged(service.workspacePath("Recovery"));
                    REQUIRE(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
                    REQUIRE(damaged.write("not-json") > 0);
                    damaged.close();
                    bool recoveredFlag = false;
                    WorkspaceProfile recovered = service.loadWorkspace("Recovery", &recoveredFlag, &error);
                    REQUIRE(recoveredFlag);
                    REQUIRE(!recovered.translationEnabled);
                    REQUIRE(QFile::exists(service.workspacePath("Recovery")));

                    AppearanceProfile clean = AppearanceProfile::defaults();
                    REQUIRE(service.saveAppearance("Future", clean, &error));
                    QFile futureFile(service.appearancePath("Future"));
                    REQUIRE(futureFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
                    const QByteArray futureData = QJsonDocument(
                        QJsonObject{{"version", AppearanceProfile::SchemaVersion + 1}}).toJson();
                    REQUIRE(futureFile.write(futureData) == futureData.size());
                    futureFile.close();
                    recoveredFlag = true;
                    service.loadAppearance("Future", &recoveredFlag, &error);
                    REQUIRE(!recoveredFlag);
                    REQUIRE(futureFile.open(QIODevice::ReadOnly));
                    REQUIRE(QJsonDocument::fromJson(futureFile.readAll()).object().value("version").toInt()
                            == AppearanceProfile::SchemaVersion + 1);
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
        ]
        cmake.write_text(
            "cmake_minimum_required(VERSION 3.21)\n"
            "project(CGPlayTypedProfileContracts LANGUAGES CXX)\n"
            "set(CMAKE_CXX_STANDARD 17)\n"
            "find_package(Qt6 REQUIRED COMPONENTS Core Gui)\n"
            "add_executable(profile_contract_runner profile_contract_runner.cpp\n"
            + "\n".join(f'  "{path.as_posix()}"' for path in sources)
            + "\n)\n"
            + f'target_include_directories(profile_contract_runner PRIVATE "{(ROOT / "src").as_posix()}")\n'
            + "target_link_libraries(profile_contract_runner PRIVATE Qt6::Core Qt6::Gui)\n",
            encoding="utf-8",
        )
        configure = subprocess.run(
            ["cmake", "-S", str(source_dir), "-B", str(build_dir),
             "-G", "Visual Studio 17 2022", "-A", "x64",
             "-DCMAKE_PREFIX_PATH=C:/QtClean/6.5.3/msvc2019_64"],
            cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        (ARTIFACT / "configure.log").write_text(configure.stdout + configure.stderr, encoding="utf-8")
        if configure.returncode != 0:
            raise AssertionError(f"Typed profile configure failed: {configure.stderr[-2000:]}")
        build = subprocess.run(
            ["cmake", "--build", str(build_dir), "--config", "Release"],
            cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        (ARTIFACT / "build.log").write_text(build.stdout + build.stderr, encoding="utf-8")
        if build.returncode != 0:
            raise AssertionError(f"Typed profile build failed: {build.stderr[-2000:]}")
        cls.runner = build_dir / "Release" / "profile_contract_runner.exe"

    def test_compiled_roundtrip_validation_migration_and_recovery(self):
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
