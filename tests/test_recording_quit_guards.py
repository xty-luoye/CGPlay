"""Production GUI quit guards with fake installer and recorder boundary objects."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
from tests.native_probe_runtime import deploy_runtime
import unittest

ROOT=Path(__file__).resolve().parents[1]
OUT=Path(os.environ.get('CGPLAY_RECORDING_QUIT_ARTIFACTS',ROOT/'tests/artifacts/fix_review_20261101/remaining_regression/recording_quit_guards'))
QT=Path(os.environ.get('CGPLAY_TEST_QT_ROOT','E:/rjkf/CGPlay-deps/QtClean/6.5.3/msvc2019_64'))

FIXTURE=r'''
#include <QApplication>
#include <QMainWindow>
#include <QCloseEvent>
#include <QStatusBar>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <memory>
#include <windows.h>
namespace cgplay {
static bool active=false, becameBusy=false, chooseInstall=true, launchSucceeds=true, deferred=false;
static int prompts=0, launched=0, closed=0, quit=0, notices=0, warnings=0;
struct UpdateInstallResult { QString installerPath="unused-test-installer.exe"; };
struct RecordingService { static bool hasActiveSessions() { return active; } };
struct UpdateService { static bool launchInstaller(const UpdateInstallResult&,QString*) { ++launched; return launchSucceeds; } };
struct QCoreApplication { static void quit() { ++cgplay::quit; } };
struct QMessageBox {
 enum {Information,NoButton,AcceptRole,RejectRole}; struct Button{} yes,no;
 QMessageBox(int,const QString&,const QString&,int,QWidget*) { ++prompts; }
 Button* addButton(const QString&,int role) { return role==AcceptRole?&yes:&no; }
 void setDetailedText(const QString&) {}
 void exec() { if(becameBusy)active=true; }
 Button* clickedButton() { return chooseInstall?&yes:&no; }
 static void information(QWidget*,const QString&,const QString&) { ++notices; }
 static void warning(QWidget*,const QString&,const QString&) { ++warnings; }
};
struct SubtitleState { bool subtitleGenerationBusy=false; };
struct SubtitleSession { SubtitleState value; SubtitleState& state() { return value; } };
struct Private { bool updateClosing=false; SubtitleSession subtitleSession; };
class MainWindow: public QMainWindow {
public:
 std::unique_ptr<Private> _p=std::make_unique<Private>();
 bool close() { ++closed; return true; }
 bool _deferCloseForRecording() { return deferred; }
 void _offerDownloadedUpdate(UpdateInstallResult);
 void closeEvent(QCloseEvent*) override;
};
// UPDATE_METHOD
// CLOSE_GUARD
}
int main(int argc,char**argv) {
 QApplication application(argc,argv);
 if(argc!=2)return 2;
 QJsonArray checks;
 const auto check=[&](const char*name,bool pass) { checks.append(QJsonObject{{"name",name},{"passed",pass}}); };
 using namespace cgplay;
 const auto reset=[] { active=becameBusy=deferred=false;chooseInstall=launchSucceeds=true;prompts=launched=closed=quit=notices=warnings=0; };
 MainWindow window;window.setAttribute(Qt::WA_DontShowOnScreen);window.setAttribute(Qt::WA_ShowWithoutActivating);
 reset();active=true;window._offerDownloadedUpdate({});
 check("active recording blocks installer launcher and global exit",launched==0&&closed==0&&quit==0&&notices==1);
 reset();becameBusy=true;window._offerDownloadedUpdate({});
 check("recording started during confirmation is rechecked",launched==0&&closed==0&&quit==0&&notices==1);
 reset();window._offerDownloadedUpdate({});
 check("idle update installation still launches then exits",launched==1&&closed==1&&quit==1);
 reset();chooseInstall=false;window._offerDownloadedUpdate({});
 check("later install does not launch or exit",launched==0&&closed==0&&quit==0);
 reset();launchSucceeds=false;window._offerDownloadedUpdate({});
 check("launcher failure does not close or exit",launched==1&&closed==0&&quit==0&&warnings==1);
 reset();window._p->updateClosing=true;window._offerDownloadedUpdate({});
 check("closing window does not show install prompt",prompts==0&&launched==0&&quit==0);window._p->updateClosing=false;
 reset();active=true;window._p->subtitleSession.value.subtitleGenerationBusy=true;QCloseEvent busy;
 window.closeEvent(&busy);
 check("other recording prevents subtitle global-exit branch",!busy.isAccepted()&&!window._p->updateClosing&&!window.statusBar()->currentMessage().isEmpty());
 reset();window._p->subtitleSession.value.subtitleGenerationBusy=true;QCloseEvent idle;window.closeEvent(&idle);
 check("no recording leaves legacy subtitle close branch reachable",idle.isAccepted());
 reset();active=true;window._p->subtitleSession.value.subtitleGenerationBusy=false;QCloseEvent other;window.closeEvent(&other);
 check("ordinary unrelated window close stays allowed",other.isAccepted());
 reset();active=true;deferred=true;QCloseEvent own;window.closeEvent(&own);
 check("own recording still defers close",!own.isAccepted());
 int visible=0;
 EnumWindows([](HWND handle,LPARAM pointer)->BOOL { DWORD pid=0;GetWindowThreadProcessId(handle,&pid);
  if(pid==GetCurrentProcessId()&&IsWindowVisible(handle))++*reinterpret_cast<int*>(pointer);return TRUE;
 },reinterpret_cast<LPARAM>(&visible));
 bool passed=visible==0;for(const auto&value:checks)passed=passed&&value.toObject()["passed"].toBool();
 QFile file(QString::fromLocal8Bit(argv[1]));if(!file.open(QIODevice::WriteOnly))return 3;
 file.write(QJsonDocument(QJsonObject{{"passed",passed},{"backgroundMode",true},{"visiblePlatformWindows",visible},
 {"actualInstallerLaunched",false},{"actualUserCapture",false},{"checks",checks}}).toJson());
 return passed?0:1;
}
'''


class RecordingQuitGuards(unittest.TestCase):
    def test_update_and_subtitle_global_exit_preserve_active_recordings(self):
        OUT.mkdir(parents=True,exist_ok=True)
        application=(ROOT/'src/ui/app/Application.cpp').read_text(encoding='utf-8-sig')
        inputs=(ROOT/'src/ui/app/ApplicationInput.cpp').read_text(encoding='utf-8-sig')
        update=application[application.index('void MainWindow::_offerDownloadedUpdate('):application.index('void MainWindow::_setUpdateStatusBadge(')]
        close=inputs[inputs.index('void MainWindow::closeEvent('):]
        close=close[:close.index('    _p->updateClosing = true;')]+'    event->accept();\n}\n'
        (OUT/'runner.cpp').write_text(FIXTURE.replace('// UPDATE_METHOD',update).replace('// CLOSE_GUARD',close),encoding='utf-8')
        cmake='''cmake_minimum_required(VERSION 3.21)
project(RecordingQuitGuards LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
add_executable(recording_quit_guards runner.cpp)
target_link_libraries(recording_quit_guards PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets user32)
target_compile_options(recording_quit_guards PRIVATE /utf-8)
'''
        (OUT/'CMakeLists.txt').write_text(cmake,encoding='utf-8')
        build=OUT/'build'
        environment=os.environ.copy();environment['PATH']=str(QT/'bin')+os.pathsep+str(ROOT/'build_win_full'/'bin'/'Release')+os.pathsep+environment.get('PATH','')
        environment['QT_QPA_PLATFORM']='offscreen'
        environment['QT_QPA_PLATFORM_PLUGIN_PATH']=str(QT/'plugins'/'platforms')
        environment['QT_PLUGIN_PATH']=str(QT/'plugins')
        commands=[['cmake','-S',OUT,'-B',build,'-G','Visual Studio 17 2022','-A','x64',f'-DCMAKE_PREFIX_PATH={QT}'],
            ['cmake','--build',build,'--config','Release','--parallel','2'],
            [build/'Release/recording_quit_guards.exe',OUT/'report.json']]
        with (OUT/'commands.log').open('wb') as log:
            for index, command in enumerate(commands):
                subprocess.run([str(v) for v in command],cwd=ROOT,env=environment,check=True,
                    stdout=log,stderr=log,creationflags=subprocess.CREATE_NO_WINDOW,timeout=180)
                if index == 1:
                    deploy_runtime(build/'Release/recording_quit_guards.exe', QT,
                                   (ROOT/'build_win_full'/'bin'/'Release',))
        report=json.loads((OUT/'report.json').read_text(encoding='utf-8'))
        report['productionExtracts']={'updateSHA256':hashlib.sha256(update.encode()).hexdigest(),
            'closeGateSHA256':hashlib.sha256(close.encode()).hexdigest()}
        (OUT/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        self.assertTrue(report['passed'],report)
        self.assertEqual(0,report['visiblePlatformWindows'])
        self.assertEqual(10,len(report['checks']))


if __name__=='__main__':unittest.main()
