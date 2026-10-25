"""Real Qt promise/watcher export lifecycle, with no visible desktop windows."""
from pathlib import Path
import json
import os
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
ARTIFACT = Path(os.environ.get('CGPLAY_EXPORT_GUI_ARTIFACTS', ROOT/'tests/artifacts/export_gui_1_0_8'))
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

RUNNER = r'''
#include "VideoExportTask.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
static int visible() {
    int count=0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND w,LPARAM data)->BOOL {
        DWORD pid=0; GetWindowThreadProcessId(w,&pid);
        if (pid==GetCurrentProcessId() && IsWindowVisible(w)) ++*reinterpret_cast<int*>(data);
        return TRUE;
    },reinterpret_cast<LPARAM>(&count));
#endif
    return count;
}
int main(int argc,char**argv) {
    QApplication app(argc,argv);
    if(argc!=2) return 2;
    const int fontId=QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    if(fontId>=0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).value(0),10));
    int beats=0,maxVisible=0;
    QTimer heartbeat;heartbeat.setInterval(10);
    QObject::connect(&heartbeat,&QTimer::timeout,[&]{++beats;maxVisible=qMax(maxVisible,visible());});
    heartbeat.start();
    auto until=[&](auto predicate,int milliseconds) {
        QElapsedTimer timer;timer.start();
        while(!predicate() && timer.elapsed()<milliseconds) {
            app.processEvents(QEventLoop::AllEvents,5);QThread::msleep(2);
            maxVisible=qMax(maxVisible,visible());
        }
        return predicate();
    };
    QJsonArray cases;
    bool ok=true;
    for(const QString mode : {QStringLiteral("complete"),QStringLiteral("cancel_button"),QStringLiteral("close"),QStringLiteral("destroy"),QStringLiteral("exception")}) {
        auto* parent=new QWidget;
        parent->setAttribute(Qt::WA_DontShowOnScreen);
        parent->show();
        auto done=std::make_shared<std::atomic_bool>(false);
        auto canceled=std::make_shared<std::atomic_bool>(false);
        int callbacks=0;bool receivedSuccess=false,receivedCancel=false;
        bool reentryRejected=true;
        auto reentered=std::make_shared<std::atomic_bool>(false);
        bool guiCallback=false;
        const int before=beats;
        const QString snapshot=QStringLiteral("original-source-and-color");
        cgplay::startVideoExportTask(parent,QStringLiteral("后台导出生命周期验证"),
            [done,canceled,mode,snapshot](const std::atomic_bool* stop,const std::function<void(double)>& progress) {
                if(mode==QStringLiteral("exception")) {done->store(true);throw std::runtime_error("injected export error");}
                for(int i=0;i<60;++i) {
                    progress(i/60.0);
                    if(stop->load()) {
                        canceled->store(true);
                        // Model slow encoder/process cleanup after the window
                        // has gone: it must neither block GUI nor touch widgets.
                        for(int j=0;j<30;++j){progress(.8);QThread::msleep(10);}
                        done->store(true);
                        return qMakePair(false,QStringLiteral("canceled"));
                    }
                    QThread::msleep(5);
                }
                progress(1.0);done->store(true);
                return qMakePair(snapshot==QStringLiteral("original-source-and-color"),QString{});
            },
            [&](const cgplay::VideoExportTaskResult& result,bool wasCanceled) {
                ++callbacks;receivedSuccess=result.first;receivedCancel=wasCanceled;
                guiCallback=QThread::currentThread()==app.thread();
            });
        if(mode==QStringLiteral("complete")) {
            reentryRejected=!cgplay::startVideoExportTask(parent,QStringLiteral("必须拒绝重复导出"),
                [reentered](const std::atomic_bool*,const std::function<void(double)>&) {
                    reentered->store(true);return qMakePair(true,QString{});
                }, [](const cgplay::VideoExportTaskResult&,bool) {});
            const QString screenshot=QFileInfo(QString::fromLocal8Bit(argv[1])).absolutePath()+"/export_progress_background.png";
            QTimer::singleShot(80,parent,[parent,screenshot] {
                if(auto* dialog=parent->findChild<QProgressDialog*>()) dialog->grab().save(screenshot);
            });
        }
        if(mode==QStringLiteral("cancel_button")) QTimer::singleShot(40,parent,[parent] {
            auto* dialog=parent->findChild<QProgressDialog*>();
            if(dialog) if(auto* button=dialog->findChild<QPushButton*>()) button->click();
        });
        if(mode==QStringLiteral("close")) QTimer::singleShot(40,parent,[parent]{parent->close();});
        if(mode==QStringLiteral("destroy")) QTimer::singleShot(40,[parent]{delete parent;});
        bool finished=until([&]{return done->load() && (mode==QStringLiteral("destroy") || callbacks==1);},2500);
        app.processEvents();
        bool passed=finished;
        if(mode==QStringLiteral("destroy")) passed=passed && callbacks==0 && canceled->load() && beats-before>=15;
        else if(mode==QStringLiteral("complete")) passed=passed && callbacks==1 && receivedSuccess && !receivedCancel && guiCallback && reentryRejected && !reentered->load() && !cgplay::isVideoExportTaskRunning(parent);
        else if(mode==QStringLiteral("exception")) passed=passed && callbacks==1 && !receivedSuccess && guiCallback;
        else passed=passed && callbacks==1 && !receivedSuccess && receivedCancel && canceled->load() && guiCallback && beats-before>=15;
        cases.append(QJsonObject{{"mode",mode},{"passed",passed},{"callbacks",callbacks},{"heartbeatTicks",beats-before},{"workerCanceled",canceled->load()},{"reentryRejected",reentryRejected}});
        ok=ok && passed;
        if(mode!=QStringLiteral("destroy")) delete parent;
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }
    QThreadPool::globalInstance()->waitForDone();
    const QJsonObject report{{"passed",ok && maxVisible==0},{"backgroundMode",true},
        {"visiblePlatformWindowsMax",maxVisible},{"cases",cases},{"poolDrained",true},
        {"screenshot","export_progress_background.png"}};
    QFile file(QString::fromLocal8Bit(argv[1]));
    if(!file.open(QIODevice::WriteOnly)) return 3;
    file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    return ok && maxVisible==0 ? 0 : 1;
}
'''


class ExportGuiSnapshotTests(unittest.TestCase):
    def test_real_async_export_close_cancel_progress_and_exception(self):
        ARTIFACT.mkdir(parents=True, exist_ok=True)
        cache=(ROOT/'build_win_full/CMakeCache.txt').read_text(encoding='utf-8')
        qt=Path(os.environ['CGPLAY_TEST_QT_ROOT']) if 'CGPLAY_TEST_QT_ROOT' in os.environ else Path(re.search(r'^Qt6Core_DIR:[^=]+=(.+)$',cache,re.M)[1]).parents[2].resolve()
        source=ARTIFACT/'source';source.mkdir(exist_ok=True)
        (source/'main.cpp').write_text(RUNNER,encoding='utf-8')
        (source/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.24)
project(ExportGuiLifecycle LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Concurrent)
add_executable(export_gui main.cpp)
target_include_directories(export_gui PRIVATE "{(ROOT/'src/features/annotation').as_posix()}")
target_compile_definitions(export_gui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(export_gui PRIVATE /utf-8)
target_link_libraries(export_gui PRIVATE Qt6::Core Qt6::Widgets Qt6::Concurrent)
''',encoding='utf-8')
        build=ARTIFACT/'build'
        env=os.environ.copy();env['QT_QPA_PLATFORM']='offscreen';env['PATH']=str(qt/'bin')+os.pathsep+env.get('PATH','')
        def run(name,args):
            with (ARTIFACT/(name+'.log')).open('w',encoding='utf-8') as log:
                result=subprocess.run(args,env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=NO_WINDOW,timeout=180)
            self.assertEqual(0,result.returncode,name+' failed; see '+str(ARTIFACT/(name+'.log')))
        run('configure',['cmake','-S',str(source),'-B',str(build),'-G','Visual Studio 17 2022','-A','x64',f'-DCMAKE_PREFIX_PATH={qt}'])
        run('build',['cmake','--build',str(build),'--config','Release','--parallel','2'])
        run('runtime',[str(build/'Release/export_gui.exe'),str(ARTIFACT/'runtime.json')])
        report=json.loads((ARTIFACT/'runtime.json').read_text(encoding='utf-8'))
        self.assertTrue(report['passed']);self.assertEqual(5,len(report['cases']))
        self.assertEqual(0,report['visiblePlatformWindowsMax'])

    def test_main_window_workers_capture_values_without_nested_wait_or_widgets(self):
        text=(ROOT/'src/ui/app/Application.cpp').read_text(encoding='utf-8-sig')
        self.assertEqual(2,text.count('startVideoExportTask(this,'))
        self.assertNotIn('QEventLoop exportLoop',text)
        for match in re.finditer(r'\[sourcePath, exportRuntime,[^\]]+\]\(const std::atomic_bool\* cancel',text):
            body=text[match.start():text.index('return qMakePair(',match.start())]
            self.assertNotIn('_p->',body);self.assertNotIn('progressDlg',body)
            self.assertIn('colorSnapshot',body)


if __name__=='__main__':
    unittest.main()
