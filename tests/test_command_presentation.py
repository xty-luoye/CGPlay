"""Compile the actual Qt presentation helper; no application or settings writes."""

from pathlib import Path
import json
import os
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / "tests/artifacts/settings_controls_ui_20260912/command_presentation"
QT = Path("C:/QtClean/6.5.3/msvc2019_64")
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

RUNNER = r'''
#include "ui/app/CommandPresentation.h"
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace cgplay;
QJsonObject report;
#define CHECK(x) do { if (!(x)) { report["failure"] = #x; report["line"] = __LINE__; return false; } } while (false)
static bool run(const QString& scenario, const QString& manifest) {
 if (scenario == "default") {
  QToolButton button; button.setText("icon"); button.setFocusPolicy(Qt::NoFocus);
  button.setShortcut(QKeySequence("Ctrl+F9")); button.setFixedSize(32,32);
  int clicks=0; QObject::connect(&button,&QToolButton::clicked,[&]{++clicks;});
  applyCommandPresentation(&button,QStringLiteral("下一帧"),"Right");
  CHECK(button.toolTip().contains("Right")); CHECK(button.accessibleName()==QStringLiteral("下一帧"));
  CHECK(button.accessibleDescription()==button.toolTip()); CHECK(button.statusTip()==button.toolTip());
  CHECK(button.focusPolicy()==Qt::NoFocus && button.size()==QSize(32,32));
  CHECK(button.shortcut()==QKeySequence("Ctrl+F9") && button.text()=="icon" && clicks==0);
  button.click(); CHECK(clicks==1);
 } else if (scenario == "rebind-clear") {
  QPushButton button;
  applyCommandPresentation(&button,QStringLiteral("全屏"),"F11");
  applyCommandPresentation(&button,QStringLiteral("全屏"),"Ctrl+Alt+F");
  CHECK(button.toolTip().contains(QKeySequence("Ctrl+Alt+F").toString(QKeySequence::NativeText)));
  CHECK(!button.toolTip().contains("F11"));
  applyCommandPresentation(&button,QStringLiteral("全屏"),{});
  CHECK(button.toolTip().contains(QStringLiteral("快捷键：未设置")));
  CHECK(!button.toolTip().contains("F11") && !button.toolTip().contains("Ctrl"));
  CHECK(button.shortcut().isEmpty());
 } else if (scenario == "state") {
  QToolButton button; button.setText("play-icon");
  applyCommandPresentation(&button,QStringLiteral("播放/暂停"),"Space");
  updateCommandPresentationState(&button,QStringLiteral("暂停"),QStringLiteral("当前正在播放"));
  CHECK(button.accessibleName()==QStringLiteral("暂停") && button.toolTip().contains("Space"));
  applyCommandPresentation(&button,QStringLiteral("播放/暂停"),"Alt+P");
  CHECK(button.accessibleName()==QStringLiteral("暂停") && button.toolTip().contains("Alt+P"));
  CHECK(!button.toolTip().contains("Space"));
  CHECK(button.toolTip().contains(QStringLiteral("当前正在播放")) && button.text()=="play-icon");
  updateCommandPresentationState(&button,QStringLiteral("播放"));
  CHECK(button.accessibleName()==QStringLiteral("播放") && !button.toolTip().contains(QStringLiteral("当前正在播放")));
 } else if (scenario == "enabled-check") {
  QWidget parent; QToolButton button(&parent); button.setCheckable(true);
  int toggles=0; QObject::connect(&button,&QToolButton::toggled,[&](bool){++toggles;});
  applyCommandPresentation(&button,QStringLiteral("静音"),"M");
  CHECK(!button.isChecked() && toggles==0);
  button.setChecked(true); CHECK(toggles==1 && button.toolTip().contains(QStringLiteral("当前：已选中")));
  parent.setEnabled(false);
  CHECK(!button.isEnabled() && button.toolTip().contains(QStringLiteral("当前不可用")));
  applyCommandPresentation(&button,QStringLiteral("静音"),{});
  CHECK(!button.isEnabled() && button.isChecked() && toggles==1);
  parent.setEnabled(true); CHECK(!button.toolTip().contains(QStringLiteral("当前不可用")));
  {QSignalBlocker blocker(&button); button.setChecked(false); refreshCommandPresentation(&button);}
  CHECK(toggles==1 && button.toolTip().contains(QStringLiteral("当前：未选中")));
 } else if (scenario == "action") {
  QAction action; action.setText("Mute"); action.setShortcut(QKeySequence("M")); action.setCheckable(true);
  int fired=0; QObject::connect(&action,&QAction::triggered,[&](bool){++fired;});
  applyCommandPresentation(&action,QStringLiteral("静音"),"Alt+M");
  CHECK(action.shortcut()==QKeySequence("M") && action.text()=="Mute" && fired==0 && !action.isChecked());
  action.trigger(); CHECK(fired==1 && action.isChecked());
  CHECK(action.toolTip().contains(QStringLiteral("当前：已选中")));
  action.setEnabled(false); CHECK(action.toolTip().contains(QStringLiteral("当前不可用")));
  applyCommandPresentation(&action,QStringLiteral("静音"),{});
  CHECK(!action.isEnabled() && action.isChecked() && fired==1);
  CHECK(action.toolTip().contains(QStringLiteral("快捷键：未设置")) && !action.toolTip().contains("Alt+M"));
  action.setEnabled(true); CHECK(!action.toolTip().contains(QStringLiteral("当前不可用")));
 } else if (scenario == "idempotent") {
  QToolButton button; button.setCheckable(true); QAction action;
  for(int i=0;i<100;++i) {applyCommandPresentation(&button,"Button","Ctrl+P");applyCommandPresentation(&action,"Action","Ctrl+P");}
  CHECK(button.children().size()==1);
  CHECK(button.toolTip().count(QStringLiteral("快捷键："))==1);
  CHECK(action.toolTip().count(QStringLiteral("快捷键："))==1);
  button.setChecked(true); action.setCheckable(true); action.setChecked(true);
  CHECK(button.toolTip().count(QStringLiteral("当前：已选中"))==1);
  CHECK(action.toolTip().count(QStringLiteral("当前：已选中"))==1);
 } else if (scenario == "standalone") {
  QToolButton button;
  updateCommandPresentationState(&button,QStringLiteral("暂停"));
  CHECK(button.accessibleName()==QStringLiteral("暂停"));
  CHECK(!button.toolTip().contains(QStringLiteral("快捷键")));
  applyCommandPresentation(&button,QStringLiteral("播放/暂停"),"Space");
  CHECK(button.accessibleName()==QStringLiteral("暂停") && button.toolTip().contains("Space"));
 } else if (scenario == "registry-shape") {
  QFile file(manifest); CHECK(file.open(QIODevice::ReadOnly));
  const auto commands=QJsonDocument::fromJson(file.readAll()).object()["commands"].toObject();
  CHECK(!commands.isEmpty());
  for (auto it=commands.begin();it!=commands.end();++it) {
   QToolButton button; button.setProperty("commandId",it.key());
   applyCommandPresentation(&button,it.key(),{});
   CHECK(!button.accessibleName().isEmpty() && button.property("commandId").toString()==it.key());
   CHECK(button.toolTip().contains(QStringLiteral("快捷键：未设置")));
  }
  report["manifestCommandsExercised"]=commands.size();
 } else return false;
 return true;
}
int main(int argc,char** argv) {
 QApplication app(argc,argv); app.setProperty("cgplay.automationBackground",true);
 const bool passed=run(app.arguments().value(1),app.arguments().value(2));
 app.processEvents(); int qtVisible=0,nativeVisible=0;
 for(auto* widget:QApplication::topLevelWidgets()) if(widget->isVisible()&&!widget->testAttribute(Qt::WA_DontShowOnScreen))++qtVisible;
#ifdef Q_OS_WIN
 EnumWindows([](HWND w,LPARAM p)->BOOL{DWORD pid=0;GetWindowThreadProcessId(w,&pid);if(pid==GetCurrentProcessId()&&IsWindowVisible(w))++*reinterpret_cast<int*>(p);return TRUE;},reinterpret_cast<LPARAM>(&nativeVisible));
#endif
 report["passed"]=passed;report["background"]=app.property("cgplay.automationBackground").toBool();
 report["qtPlatform"]=app.platformName();report["visibleQtWindows"]=qtVisible;report["visiblePlatformWindows"]=nativeVisible;
 printf("%s\n",QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
 return passed&&qtVisible==0&&nativeVisible==0?0:1;
}
'''


class CommandPresentationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source, build = ART / "source", ART / "build"
        source.mkdir(parents=True, exist_ok=True)
        (source / "runner.cpp").write_text(RUNNER, encoding="utf-8")
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.21)\nproject(CommandPresentationTest LANGUAGES CXX)\n'
            'set(CMAKE_CXX_STANDARD 17)\nfind_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)\n'
            'add_executable(command_presentation runner.cpp)\n'
            f'target_include_directories(command_presentation PRIVATE "{ROOT.as_posix()}/src")\n'
            'target_compile_options(command_presentation PRIVATE /utf-8)\n'
            'target_link_libraries(command_presentation PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets user32)\n',
            encoding="utf-8")
        for name, command in (
            ("configure", ["cmake", "-S", str(source), "-B", str(build), "-G", "Visual Studio 17 2022", "-A", "x64", f"-DCMAKE_PREFIX_PATH={QT}"]),
            ("build", ["cmake", "--build", str(build), "--config", "Release", "--parallel", "1"]),
        ):
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", creationflags=NO_WINDOW, timeout=180)
            (ART / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode:
                raise AssertionError(result.stdout[-6000:] + result.stderr[-2000:])
        cls.runner = build / "Release/command_presentation.exe"

    def scenario(self, name):
        env = os.environ.copy()
        env["PATH"] = str(QT / "bin") + os.pathsep + env.get("PATH", "")
        env["QT_QPA_PLATFORM"] = "offscreen"
        result = subprocess.run([str(self.runner), name, str(ROOT / "tests/automation_coverage_manifest.json")],
                                cwd=ART, env=env, capture_output=True, text=True, encoding="utf-8",
                                errors="replace", creationflags=NO_WINDOW, timeout=20)
        (ART / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        data = json.loads(result.stdout.strip().splitlines()[-1])
        (ART / f"{name}.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        self.assertEqual(0, result.returncode, data)
        self.assertTrue(data["passed"])
        self.assertTrue(data["background"])
        self.assertEqual("offscreen", data["qtPlatform"])
        self.assertEqual(0, data["visibleQtWindows"])
        self.assertEqual(0, data["visiblePlatformWindows"])

    def test_default_hint_accessibility_and_unchanged_dispatch(self): self.scenario("default")
    def test_rebound_and_cleared_keys_replace_old_hint(self): self.scenario("rebind-clear")
    def test_dynamic_state_keeps_effective_shortcut(self): self.scenario("state")
    def test_disabled_and_checked_state_are_observed_not_changed(self): self.scenario("enabled-check")
    def test_actions_preserve_binding_and_trigger(self): self.scenario("action")
    def test_refresh_is_idempotent(self): self.scenario("idempotent")
    def test_unresolved_standalone_control_has_no_fake_binding(self): self.scenario("standalone")
    def test_formatter_handles_every_manifest_command(self): self.scenario("registry-shape")


if __name__ == "__main__":
    unittest.main(verbosity=2)
