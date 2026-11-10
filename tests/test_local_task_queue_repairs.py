"""R37/R89: real Qt files and production queue methods, isolated AI transport."""
import json
import os
from pathlib import Path
import subprocess
import unittest
import uuid

from tests.native_probe_runtime import deploy_runtime
from tests.test_review_metadata_repairs import COMMON, QT, function

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tests/artifacts/fix_review_20261101/stage11'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

STUBS = r'''
#include "plugins/codex/LocalTaskStore.h"
#include <QDateTime>
#include <QCryptographicHash>
#include <QFileSystemWatcher>
#include <QProcess>
#include <QTimer>
#include <QHash>
#include <QSet>
#include <utility>
using namespace cgplay;
QString fixtureRoot;
QStringList events;
struct Workspace { void appendWorkbenchEvent(QString,QString s){events.append(s);} };
struct QInputDialog {static QString action; static int calls;
static QString getItem(Workspace*,QString,QString,QStringList items,int,bool,bool*ok){*ok=true;return ++calls==1?items.first():action;}};
QString QInputDialog::action;int QInputDialog::calls=0;
struct JobHandle{void cancel(){}};struct JobContext{void cancel(){}};
struct Session{bool isRunning(){return false;}void stop(){}};
class CodexPlugin: public QObject {public:
enum class ProtocolState{Ready,TurnInProgress,Stopping,Disabled,Failed};
bool _initialized=true,_restartAfterStop=false,submissionOk=true;
QTimer *_localTaskTimer=nullptr,*_terminalTimeoutTimer=nullptr;QFileSystemWatcher*_localFileWatcher=nullptr;QProcess*_terminalProcess=nullptr;
QStringList _suspendedFileWatchPaths,_suspendedDirectoryWatchPaths;QHash<QString,JobHandle*> _asyncToolJobs;QSet<JobContext*> _activeNetworkJobs;QHash<QString,int> _pendingRequests,_pendingThreadSettings,_pendingThreadIds;
QString _threadId="thread",_activeTurnId,_runningLocalTaskId,_runningLocalTaskThreadId,_runningLocalTaskTurnId,_localTaskFinalStatus,_localTaskFinalDetail,_lastError;
Session _session;Workspace value;Workspace* _workspace=&value;ProtocolState _protocolState=ProtocolState::Ready;int submissions=0,interrupts=0;
void setProtocolState(ProtocolState s){_protocolState=s;}
bool submitPrompt(const QString&,QString*error){++submissions;if(!submissionOk){*error="injected transport failure";return false;}_protocolState=ProtocolState::TurnInProgress;return true;}
bool interruptTurn(QString*){++interrupts;return true;}
void turnCompleted(QJsonObject){}void reportProtocolError(QString){}
void shutdown();bool updateLocalTaskStatus(const QString&,const QString&,const QString& detail={});void processLocalTaskQueue();
void finishLocalTask(const QString&,const QString&);bool flushLocalTaskStatus();
void complete(QJsonObject params){COMPLETION}
void manage(){auto*workspace=_workspace;MANAGEMENT}
};
'''

MAIN = r'''
QString queuePath(){return QDir(fixtureRoot).filePath("cgplay_local_tasks.jsonl");}
QString status(QString id){QJsonArray a;QString e;localTaskStore::read(queuePath(),&a,&e);for(auto v:a)if(v.toObject()["id"].toString()==id)return v.toObject()["status"].toString();return {};}
void seed(std::initializer_list<std::pair<const char*,const char*>> items){QJsonArray a;for(auto p:items)a.append(QJsonObject{{"id",p.first},{"status",p.second},{"objective","fixture"}});QString e;localTaskStore::write(queuePath(),a,&e);}
void bind(CodexPlugin&p,QString id="turn-A"){p._activeTurnId=id;p._runningLocalTaskTurnId=id;}
void completed(CodexPlugin&p,QString id="turn-A",QString thread="thread"){p.complete({{"threadId",thread},{"turn",QJsonObject{{"id",id},{"status","completed"}}}});}
int main(int argc,char**argv){QCoreApplication app(argc,argv);fixtureRoot=QString::fromUtf8(argv[1]);QDir().mkpath(fixtureRoot);QJsonObject result;
seed({{"A","queued"},{"B","queued"}});CodexPlugin p;p.processLocalTaskQueue();p.processLocalTaskQueue();
result["twoPollsOneDispatch"]=p.submissions==1&&status("A")=="running"&&status("B")=="queued";
bind(p);completed(p,"unrelated");completed(p,"turn-A","other-thread");
result["unrelatedCompletionIgnored"]=p._runningLocalTaskId=="A"&&status("A")=="running";
completed(p);result["completionPersists"]=status("A")=="completed"&&p._runningLocalTaskId.isEmpty();
p.processLocalTaskQueue();p.shutdown();result["shutdownClosesTask"]=status("B")=="cancelled"&&p._runningLocalTaskId.isEmpty();
seed({{"C","queued"}});p._initialized=true;p._threadId="thread";p._protocolState=CodexPlugin::ProtocolState::Ready;p.processLocalTaskQueue();
result["reactivationDispatchesNext"]=p.submissions==3&&status("C")=="running";p.shutdown();
seed({{"orphan","running"},{"new","queued"}});CodexPlugin fresh;fresh.processLocalTaskQueue();
result["orphanRecovered"]=status("orphan")=="failed"&&status("new")=="running"&&fresh.submissions==1;fresh.shutdown();
seed({{"locked","queued"}});CodexPlugin locked;QFile reader(queuePath());reader.open(QIODevice::ReadOnly);locked.processLocalTaskQueue();
result["failedCommitPreventsDispatch"]=locked.submissions==0&&status("locked")=="queued"&&!locked._lastError.isEmpty();reader.close();locked.processLocalTaskQueue();bind(locked);
reader.open(QIODevice::ReadOnly);completed(locked);result["completionRetainedForRetry"]=locked._runningLocalTaskId=="locked"&&locked._localTaskFinalStatus=="completed"&&status("locked")=="running";
reader.close();locked.processLocalTaskQueue();result["completionRetryDoesNotRedispatch"]=locked._runningLocalTaskId.isEmpty()&&status("locked")=="completed"&&locked.submissions==1;
seed({{"cancel","queued"},{"other","queued"}});CodexPlugin cancelled;cancelled.processLocalTaskQueue();bind(cancelled);
QInputDialog::calls=0;QInputDialog::action="Cancel";cancelled.manage();
result["managementCancelPersistsAndInterrupts"]=status("cancel")=="cancelled"&&cancelled.interrupts==1&&status("other")=="queued";
completed(cancelled);result["lateCompletionPreservesCancel"]=status("cancel")=="cancelled"&&cancelled._runningLocalTaskId.isEmpty();
QInputDialog::calls=0;QInputDialog::action="Retry";cancelled.manage();result["managementRetryPersists"]=status("cancel")=="queued";
QInputDialog::calls=0;QInputDialog::action="Delete";cancelled.manage();result["managementDeletePersists"]=status("cancel").isEmpty()&&status("other")=="queued";
seed({{"reject","queued"}});CodexPlugin reject;reject.submissionOk=false;reject.processLocalTaskQueue();result["transportFailureClosesTask"]=status("reject")=="failed"&&reject._runningLocalTaskId.isEmpty();
QFile corrupt(queuePath());corrupt.open(QIODevice::WriteOnly);corrupt.write("{broken");corrupt.close();CodexPlugin invalid;invalid.processLocalTaskQueue();corrupt.open(QIODevice::ReadOnly);
result["invalidQueuePreserved"]=invalid.submissions==0&&corrupt.readAll()=="{broken";corrupt.close();
seed({});QString scheduledPath=QDir(fixtureRoot).filePath("codex_local_scheduled_tasks.jsonl");QString error;
localTaskStore::write(scheduledPath,QJsonArray{QJsonObject{{"id","scheduled"},{"objective","fixture"},{"status","queued"},{"runAt",QDateTime::currentDateTimeUtc().addSecs(-60).toString(Qt::ISODate)}}},&error);
QFile scheduledReader(scheduledPath);scheduledReader.open(QIODevice::ReadOnly);CodexPlugin scheduled;scheduled.processLocalTaskQueue();
result["partialScheduleCommitDoesNotDispatch"]=scheduled.submissions==0&&status("scheduled")=="queued";scheduledReader.close();scheduled.processLocalTaskQueue();scheduled.processLocalTaskQueue();
QJsonArray tasks;localTaskStore::read(queuePath(),&tasks,&error);result["scheduledRetryIsIdempotent"]=tasks.size()==1&&scheduled.submissions==1&&status("scheduled")=="running";
return finish(result);}
'''


class LocalTaskQueueRepairs(unittest.TestCase):
    def test_persistence_lifecycle_and_queue_management(self):
        OUT.mkdir(parents=True, exist_ok=True)
        product = (ROOT / 'src/plugins/codex/CodexPlugin.cpp').read_text(encoding='utf-8-sig')
        workspace = (ROOT / 'src/plugins/codex/CodexPluginWorkspace.cpp').read_text(encoding='utf-8-sig')
        a = product.index('    const QJsonObject turn = params.value', product.index('void CodexPlugin::handleNotification'))
        completion = product[a:product.index('\n}\n', a)]
        a = workspace.index('    connect(workspace, &CodexAgentWorkspace::localTaskManageRequested')
        a = workspace.index('\n', a) + 1
        management = workspace[a:workspace.index('\n    });', a)]
        methods = '\n'.join(function(product, signature) for signature in (
            'void CodexPlugin::shutdown(', 'bool CodexPlugin::updateLocalTaskStatus(',
            'void CodexPlugin::finishLocalTask(', 'bool CodexPlugin::flushLocalTaskStatus(',
            'void CodexPlugin::processLocalTaskQueue('))
        code = COMMON + STUBS.replace('COMPLETION', completion).replace('MANAGEMENT', management) + methods + MAIN
        code = code.replace('QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)', 'fixtureRoot')
        (OUT / 'queue_probe.cpp').write_text(code, encoding='utf-8')
        cmake = f'''cmake_minimum_required(VERSION 3.21)
project(LocalTaskQueueRepairs LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Qt6 REQUIRED COMPONENTS Core)
add_executable(queue_probe queue_probe.cpp)
target_include_directories(queue_probe PRIVATE "{ROOT.as_posix()}/src")
target_compile_options(queue_probe PRIVATE /utf-8 /Zc:__cplusplus /permissive-)
target_link_libraries(queue_probe PRIVATE Qt6::Core user32)
'''
        (OUT / 'CMakeLists.txt').write_text(cmake, encoding='utf-8')
        for name, command in [('configure', ['cmake', '-S', str(OUT), '-B', str(OUT / 'build'), '-G', 'Visual Studio 17 2022', '-A', 'x64', f'-DCMAKE_PREFIX_PATH={QT.as_posix()}']),
                              ('build', ['cmake', '--build', str(OUT / 'build'), '--config', 'Release', '--parallel', '4'])]:
            result = subprocess.run(command, capture_output=True, creationflags=NO_WINDOW, timeout=120)
            (OUT / (name + '.log')).write_bytes(result.stdout + result.stderr)
            self.assertEqual(0, result.returncode, (result.stdout + result.stderr).decode(errors='replace')[-4500:])
        exe = OUT / 'build/Release/queue_probe.exe'
        deploy_runtime(exe, QT, (ROOT / 'build_win_full/bin/Release',))
        fixture = OUT / ('fixture_' + uuid.uuid4().hex)
        result = subprocess.run([str(exe), str(fixture)], capture_output=True, creationflags=NO_WINDOW, timeout=30,
                                env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))
        (OUT / 'queue_probe.log').write_bytes(result.stdout + result.stderr)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        report = json.loads(result.stdout.decode().splitlines()[-1])
        (OUT / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        self.assertTrue(report['passed'], report)


if __name__ == '__main__':
    unittest.main()
