"""Run the R18/R21/R34/R36/R39/R87/R94/R95 regression probes offscreen.

Usage: python -B tests/test_review_repair_contracts.py
Run through the repository background supervisor for visible-window evidence.
"""
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "tests/artifacts/fix_review_20261101/regression"
QT = Path(os.environ.get("CGPLAY_TEST_QT", "E:/rjkf/CGPlay-deps/engine_migration_1_0_8/qt-6.11.3-msvc2022_64"))


def function(path, marker):
    source = (ROOT / path).read_text(encoding="utf-8")
    start = source.index(marker)
    return source[start:source.index("\n}", start) + 2]


def run(command, name, env=None):
    result = subprocess.run([str(x) for x in command], cwd=ROOT, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    (OUTPUT / (name + ".log")).write_bytes(result.stdout)
    if result.returncode:
        raise RuntimeError(f"{name} failed: {result.returncode}; see {OUTPUT / (name + '.log')}")


def prepare():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    directory = function("src/plugins/codex/CodexPluginSupport.cpp", "bool copyDirectoryTree(")
    download = function("src/services/component/DownloadService.cpp", "JobOutcome runBlockingDownloadJob(")
    session = function("src/core/session/SessionManager.cpp", "bool SessionManager::save(").replace("SessionManager::save", "SessionHarness::save")
    json_export = function("src/features/annotation/ReviewExport.cpp", "bool ReviewExport::exportJson(")
    html_export = function("src/features/annotation/ReviewExport.cpp", "bool ReviewExport::exportHtml(")
    core = r'''#include <QApplication>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QProgressDialog>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <QDebug>
#include "common/core/SafeFileCopy.h"
#include "common/jobs/JobSystem.h"
#include "features/annotation/AnnotationItem.h"
#include <cstdio>
#include <stdexcept>
using namespace cgplay;
bool shortWrite=false,failCommit=false;
struct ControlledSaveFile : QSaveFile {
 using QSaveFile::QSaveFile;
 qint64 write(const QByteArray& data){return QSaveFile::write(data.constData(),shortWrite?qMin(qsizetype(5),data.size()):data.size());}
 bool commit(){if(failCommit){cancelWriting();return false;}return QSaveFile::commit();}
};
#define QSaveFile ControlledSaveFile
#include "features/annotation/AnnotationStorage.cpp"
class ReviewExport { public: static bool exportJson(const QVector<AnnotationItem>&,const QString&);static bool exportHtml(const QVector<AnnotationItem>&,const QString&,const QString&);};
JSON_EXPORT
HTML_EXPORT
struct QMessageBox {static void warning(void*,QString,QString){}};
struct SessionHarness {
 bool _pendingRestore=false,dirty=true,_preserveRecovery=true,recovery=true;QString _sessionPath;void* _mainWindow=nullptr;int saved=0;
 QJsonObject _serialize(){return QJsonObject{{"test",123}};}bool saveAs(){return false;}
 void _setDirty(bool value){dirty=value;}void clearRecovery(){recovery=false;}void sessionSaved(QString){++saved;}
 static QString tr(const char*s){return QString::fromUtf8(s);}bool save(const QString&);
};
SESSION
#undef QSaveFile
DIRECTORY
constexpr int kDownloadTotalTimeoutMs=5000;
DOWNLOAD
QJsonObject checks;
void check(bool passed,const char*name){checks[name]=passed;if(!passed)throw std::runtime_error(name);}
void put(QString path,QByteArray data){QFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(data)!=data.size())throw std::runtime_error("fixture");}
QByteArray get(QString path){QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};return f.readAll();}
int main(int argc,char**argv){QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
QString base=QString::fromLocal8Bit(argv[1]);QDir().mkpath(base);
QString src=base+"/source.bin",dst=base+"/destination.bin",error;put(src,"source");put(dst,"old");
check(copyFileAtomically(src,src,&error)&&get(src)=="source","same_file_preserved");
check(copyFileAtomically(src,base+"/../fixture/source.bin",&error)&&get(src)=="source","normalized_alias_preserved");
check(!copyFileAtomically(base+"/missing",dst,&error)&&get(dst)=="old","missing_source_retains_destination");
check(copyFileAtomically(src,dst,&error)&&get(dst)=="source","overwrite_commits_complete_copy");
QDir().mkpath(base+"/skill");put(base+"/skill/SKILL.md","skill");
check(!copyDirectoryTree(base+"/skill",base+"/skill",&error)&&get(base+"/skill/SKILL.md")=="skill","skill_self_copy_rejected");
check(!copyDirectoryTree(base+"/skill",base+"/skill/nested",&error)&&!QFileInfo::exists(base+"/skill/nested"),"skill_nested_copy_rejected_before_mutation");
check(copyDirectoryTree(base+"/skill",base+"/other",&error)&&get(base+"/other/SKILL.md")=="skill","normal_skill_install");
QVector<AnnotationItem> annotations(1);annotations[0].comment="original";QString storage=base+"/review.json";
check(AnnotationStorage::save(annotations,storage),"annotation_normal_save");QByteArray original=get(storage);annotations[0].comment="changed";
shortWrite=true;check(!AnnotationStorage::save(annotations,storage)&&get(storage)==original,"annotation_short_write_preserves_original");shortWrite=false;
failCommit=true;check(!AnnotationStorage::save(annotations,storage)&&get(storage)==original,"annotation_failed_commit_preserves_original");failCommit=false;
check(AnnotationStorage::load(storage).first().comment=="original","annotation_round_trip_after_failed_save");
for(int format=0;format<2;++format){QString path=base+(format?"/report.html":"/report.json");put(path,"existing report");shortWrite=true;
bool ok=format?ReviewExport::exportHtml(annotations,path,"test"):ReviewExport::exportJson(annotations,path);
check(!ok&&get(path)=="existing report",format?"html_short_write_preserves_original":"json_short_write_preserves_original");shortWrite=false;
check(format?ReviewExport::exportHtml(annotations,path,"test"):ReviewExport::exportJson(annotations,path),format?"html_save":"json_save");}
SessionHarness session;QString path=base+"/session.cgplay";put(path,"existing session");shortWrite=true;
check(!session.save(path)&&get(path)=="existing session"&&session.dirty&&session.recovery&&session.saved==0,"session_short_write_preserves_dirty_and_recovery");shortWrite=false;
failCommit=true;check(!session.save(path)&&get(path)=="existing session"&&session.dirty&&session.recovery,"session_commit_failure_preserves_original");failCommit=false;
check(session.save(path)&&!session.dirty&&!session.recovery&&session.saved==1,"session_success_clears_state_after_commit");
auto* parent=new QWidget;QPointer<QWidget> parentGuard(parent);bool cancellationObserved=false;
QTimer::singleShot(40,parent,[parent]{delete parent;});
runBlockingDownloadJob("regression.download","fixture",parent,[&](JobContext&ctx){for(int i=0;i<40;++i){if(ctx.isCancellationRequested()){cancellationObserved=true;break;}QThread::msleep(5);}return JobOutcome::success();});
check(!parentGuard&&cancellationObserved,"download_parent_destruction_cancels_without_invalid_access");
auto bytes=QJsonDocument(QJsonObject{{"checks",checks},{"actualQtAndJobRunner",true},{"faultBoundary","QSaveFile short write and commit"}}).toJson();fwrite(bytes.constData(),1,bytes.size(),stdout);return 0;}
'''
    for key, value in {"JSON_EXPORT": json_export, "HTML_EXPORT": html_export,
                       "SESSION": session, "DIRECTORY": directory, "DOWNLOAD": download}.items():
        core = core.replace(key, value)
    (OUTPUT / "core.cpp").write_text(core, encoding="utf-8")
    source = (ROOT / "src/plugins/codex/CodexAgentWorkspace.cpp").read_text(encoding="utf-8")
    browser = source[source.index("struct BrowserScriptResult"):source.index("\nQColor mixColors")]
    (OUTPUT / "browser.cpp").write_text(r'''#include <QApplication>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QWebEngineProfile>
#include <QEventLoop>
#include <QTimer>
#include <QPointer>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <memory>
#include <cstdio>
''' + browser + r'''
int main(int argc,char**argv){QApplication app(argc,argv);QWebEngineView view;view.resize(400,160);view.show();
QEventLoop loaded;QObject::connect(view.page(),&QWebEnginePage::loadFinished,&loaded,&QEventLoop::quit);view.setHtml("<button id='b' onclick='const end=performance.now()+12000;while(performance.now()<end){}'>test</button>");QTimer::singleShot(20000,&loaded,&QEventLoop::quit);loaded.exec();
auto fast=runBrowserScript(view.page(),"'ready'");if(!fast.completed||fast.value!="ready")return 2;
QElapsedTimer time;time.start();auto late=runBrowserScript(view.page(),"document.querySelector('#b').click();'late'");qint64 elapsed=time.elapsed();
QEventLoop tail;QTimer::singleShot(4000,&tail,&QEventLoop::quit);tail.exec();
auto after=runBrowserScript(view.page(),"'after'");view.grab().save(QString::fromLocal8Bit(argv[1])+"/browser_offscreen.png");
auto* destroyed=new QWebEnginePage;QTimer::singleShot(30,&app,[destroyed]{delete destroyed;});auto gone=runBrowserScript(destroyed,"const end=performance.now()+12000;while(performance.now()<end){};'late'");
QJsonObject result{{"fastResult",fast.value},{"timedOut",!late.completed},{"timeoutMs",elapsed},{"rendererWorksAfterLateCallback",after.completed&&after.value=="after"},{"pageDestructionReturned",true},{"offscreenScreenshot","browser_offscreen.png"}};
auto bytes=QJsonDocument(result).toJson();fwrite(bytes.constData(),1,bytes.size(),stdout);return !late.completed&&elapsed>=9000&&after.value=="after"?0:3;}
''', encoding="utf-8")
    # Compile the real orchestrator/session in an isolated settings location.
    (OUTPUT / "agent.cpp").write_text(r'''#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <QPointer>
#include <QSharedPointer>
#include "services/agent/CodexAppServerSession.h"
#define private public
#include "plugins/codex/LocalCodexOrchestrator.h"
#undef private
static QString fixture;
class FixturePaths {public:static constexpr auto AppLocalDataLocation=QStandardPaths::AppLocalDataLocation;static QString writableLocation(QStandardPaths::StandardLocation){return fixture;}};
#define QStandardPaths FixturePaths
#include "plugins/codex/LocalCodexOrchestrator.cpp"
#undef QStandardPaths
#include <cstdio>
int main(int argc,char**argv){QCoreApplication app(argc,argv);fixture=QString::fromLocal8Bit(argv[1])+"/agent";QDir().mkpath(fixture);
cgplay::LocalCodexOrchestrator orchestrator;auto agent=QSharedPointer<cgplay::LocalCodexOrchestrator::Agent>::create();
agent->id="synthetic";agent->taskId="fixture";agent->worktree=fixture;agent->session=std::make_unique<cgplay::CodexAppServerSession>();
auto* raw=agent.get();auto* session=agent->session.get();QPointer<cgplay::CodexAppServerSession> guard(session);
QObject::connect(session,&cgplay::CodexAppServerSession::jsonRpcLineReceived,&orchestrator,[&](QByteArray line){orchestrator.handleLine(raw,line,"","","","");});
orchestrator._agents.insert(raw->id,agent);agent.clear();session->jsonRpcLineReceived("{\"method\":\"turn/completed\"}");
bool alive=bool(guard),removed=orchestrator._agents.isEmpty();
QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);bool destroyed=guard.isNull();
auto bytes=QJsonDocument(QJsonObject{{"sessionAliveUntilSignalReturns",alive},{"agentRemoved",removed},{"sessionDestroyedAfterDeferredDelete",destroyed}}).toJson();fwrite(bytes.constData(),1,bytes.size(),stdout);return alive&&removed&&destroyed?0:2;}
''', encoding="utf-8")
    tools = (ROOT / "src/plugins/codex/CodexPluginTools.cpp").read_text(encoding="utf-8")
    start = tools.index("        auto* multipart =", tools.index('if (kind == QStringLiteral("imageEditing"))'))
    attachment = tools[start:tools.index("        QNetworkAccessManager network;", start)]
    (OUTPUT / "attachment.cpp").write_text(r'''#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QNetworkRequest>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
QJsonObject buildAttachment(QString path){QFileInfo source(path);QByteArray key="fixture";QString model="fixture",prompt="test";
auto result=[](bool ok,QJsonObject){return QJsonObject{{"ok",ok}};};
''' + attachment + r'''
QPointer<QFile> observed(imageFile);bool alive=observed&&imageFile->isOpen();QObject owner;multipart->setParent(&owner);delete multipart;
return QJsonObject{{"attachmentOpenBeforeCleanup",alive},{"attachmentDestroyedByMultipart",observed.isNull()}};}
int main(int argc,char**argv){QCoreApplication app(argc,argv);QString path=QString::fromLocal8Bit(argv[1])+"/source.bin";auto result=buildAttachment(path);auto bytes=QJsonDocument(result).toJson();fwrite(bytes.constData(),1,bytes.size(),stdout);return result["attachmentOpenBeforeCleanup"].toBool()&&result["attachmentDestroyedByMultipart"].toBool()?0:2;}
''', encoding="utf-8")
    cmake = f'''cmake_minimum_required(VERSION 3.21)
project(ReviewRepairs LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Concurrent Network WebEngineWidgets)
add_executable(core core.cpp "{ROOT.as_posix()}/src/common/jobs/JobSystem.cpp" "{ROOT.as_posix()}/src/common/jobs/JobSystem.h")
target_include_directories(core PRIVATE "{ROOT.as_posix()}/src")
target_compile_options(core PRIVATE /utf-8)
target_link_libraries(core PRIVATE Qt6::Core Qt6::Widgets Qt6::Concurrent)
add_executable(browser browser.cpp)
target_compile_options(browser PRIVATE /utf-8)
target_link_libraries(browser PRIVATE Qt6::Widgets Qt6::WebEngineWidgets)
add_executable(agent agent.cpp "{ROOT.as_posix()}/src/plugins/codex/LocalCodexOrchestrator.h" "{ROOT.as_posix()}/src/services/agent/CodexAppServerSession.cpp" "{ROOT.as_posix()}/src/services/agent/CodexAppServerSession.h" "{ROOT.as_posix()}/src/common/jobs/JobSystem.cpp" "{ROOT.as_posix()}/src/common/jobs/JobSystem.h")
target_include_directories(agent PRIVATE "{ROOT.as_posix()}/src")
target_compile_options(agent PRIVATE /utf-8)
target_link_libraries(agent PRIVATE Qt6::Core Qt6::Concurrent)
add_executable(attachment attachment.cpp)
target_link_libraries(attachment PRIVATE Qt6::Core Qt6::Network)
'''
    (OUTPUT / "CMakeLists.txt").write_text(cmake, encoding="utf-8")


def main():
    prepare()
    run(["cmake", "-S", OUTPUT, "-B", OUTPUT / "build", "-G", "Visual Studio 17 2022", "-A", "x64", f"-DCMAKE_PREFIX_PATH={QT}"], "configure")
    run(["cmake", "--build", OUTPUT / "build", "--config", "Release", "--parallel", "4"], "build")
    target = OUTPUT / "build/Release"
    for dll in (QT / "bin").glob("*.dll"):
        if not dll.name.lower().endswith("d.dll"):
            dest = target / dll.name
            if not dest.exists():
                os.link(dll, dest)
    env = os.environ.copy()
    env.update({"QT_QPA_PLATFORM": "offscreen", "QT_PLUGIN_PATH": str(QT / "plugins"),
                "QTWEBENGINE_RESOURCES_PATH": str(QT / "resources"),
                "QTWEBENGINE_LOCALES_PATH": str(QT / "translations/qtwebengine_locales"),
                "QTWEBENGINEPROCESS_PATH": str(QT / "bin/QtWebEngineProcess.exe"),
                "QTWEBENGINE_CHROMIUM_FLAGS": "--disable-gpu --disable-background-timer-throttling"})
    fixture = OUTPUT / "fixture"
    fixture.mkdir(exist_ok=True)
    run([target / "core.exe", fixture], "core_result", env)
    run([target / "browser.exe", OUTPUT], "browser_result", env)
    run([target / "agent.exe", fixture], "agent_result", env)
    run([target / "attachment.exe", fixture], "attachment_result", env)
    (OUTPUT / "result.json").write_text(json.dumps({"passed": True, "backgroundMode": True,
        "coreLog": "core_result.log", "browserLog": "browser_result.log",
        "scope": "extracted production functions with actual Qt; storage failures injected"}, indent=2) + "\n", encoding="utf-8")
    print("Native repair regression probes passed")


if __name__ == "__main__":
    main()
