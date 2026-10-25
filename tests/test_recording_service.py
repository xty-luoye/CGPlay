"""Compile real QtCore service and exercise synthetic recording/publish/close."""
from pathlib import Path
import json,os,subprocess,unittest
import av

ROOT=Path(__file__).resolve().parents[1]
OUT=Path(os.environ.get('CGPLAY_RECORDING_FRONTEND_ARTIFACTS', ROOT/'tests/artifacts/migration_1_0_8_20261025/recording_frontend/service'))
QT=Path(os.environ.get('CGPLAY_TEST_QT_ROOT','E:/rjkf/CGPlay-deps/QtClean/6.5.3/msvc2019_64'))
DEPS=Path('E:/rjkf/CGPlay-deps/obs-33.0.0')
PACKAGE=ROOT/'build_win_full/package/full/CGPlay'

CPP=r'''
#include "RecordingService.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <windows.h>
using namespace cgplay;
int main(int argc,char** argv) {
 QCoreApplication app(argc,argv);
 app.setProperty("cgplay.automationBackground",true); app.setProperty("cgplay.recordingSmoke",true);
 app.setProperty("cgplay.recordingHelperStopBudgetMs",3000);
 QString out=QString::fromUtf8(argv[1]); QDir().mkpath(out); app.setProperty("cgplay.recordingArtifactRoot",out);
 RecordingRuntimeSnapshot runtime;
 runtime.recorderRoot=QString::fromUtf8(argv[2]); runtime.helperExecutable=QDir(runtime.recorderRoot).filePath("CGPlayRecorder.exe");
 runtime.pythonExecutable=QString::fromUtf8(argv[3]); runtime.pythonHome=QFileInfo(runtime.pythonExecutable).absolutePath();
 runtime.toolsRoot=QString::fromUtf8(argv[4]); runtime.finalizerScript=QString::fromUtf8(argv[5]);
 runtime.ffmpegExecutable=QString::fromUtf8(argv[6]); runtime.ffprobeExecutable=QString::fromUtf8(argv[7]);
 runtime.pythonEnvironment=QProcessEnvironment::systemEnvironment();
 RecordingService service(&app),peer(&app); RecordingStartRequest reusable; QJsonArray events; QString failure,descriptor; int phase=0,pulses=0,closeCount=0; qint64 previousDuration=0; bool nativeCounterMonotonic=true,waitedThroughSave=false,globalBusyObserved=false,globalIdleObserved=false,pendingStartRefused=false,deliveredStartAccepted=false;
 QElapsedTimer elapsed,pulseClock; elapsed.start(); pulseClock.start(); qint64 maxGap=0;
 QTimer pulse; pulse.setInterval(10); QObject::connect(&pulse,&QTimer::timeout,&app,[&]{++pulses;maxGap=qMax(maxGap,pulseClock.restart());}); pulse.start();
 auto fail=[&](const QString& error){if(failure.isEmpty())failure=error;RecordingService::shutdownAndDrainAll(1000);app.exit(1);};
 QObject::connect(&service,&RecordingService::catalogReady,&app,[&](const QJsonObject& catalog){
   if(!catalog.value("error").toString().isEmpty()||catalog.value("monitors").toArray().size()!=1){fail("catalog-failed");return;}
   RecordingStartRequest request;request.runtime=runtime;request.synthetic=true;request.finalOutputPath=QDir(out).filePath("service.mp4");
   request.helperPlan={{"source","synthetic"},{"width",320},{"height",180},{"fps",30},{"systemEnabled",true},{"micEnabled",true}};
   reusable=request;phase=1;QString error;if(!service.start(request,&error))fail(error);
 });
 QObject::connect(&service,&RecordingService::stateChanged,&app,[&](const QJsonObject& value){
   events.append(value);const QString state=value.value("state").toString();
   if(phase<=5&&(state=="Recording"||state=="Paused"||state=="Stopping")){qint64 duration=value.value("durationNs").toVariant().toLongLong();if(duration<previousDuration)nativeCounterMonotonic=false;previousDuration=duration;}
   if(service.isBusy()&&!peer.isBusy())globalBusyObserved=RecordingService::hasActiveSessions();
   if(state=="Recording"&&phase==1){phase=2;QTimer::singleShot(800,&app,[&]{service.setPaused(true);});}
   else if(state=="Paused"&&phase==2){phase=3;service.setAudio({{"systemGain",.25},{"micMute",true}});QTimer::singleShot(150,&app,[&]{service.setPaused(false);});}
   else if(state=="Recording"&&phase==3){phase=4;QTimer::singleShot(800,&app,[&]{service.stop();});}
   else if(state=="Finalizing"&&phase==4){phase=5;QElapsedTimer call;call.start();service.requestStopForClose();if(call.elapsed()>100)fail("blocking-close-request");
     QTimer::singleShot(3300,&app,[&]{waitedThroughSave=service.isBusy()&&service.status().value("state").toString()=="Finalizing"&&closeCount==0;if(!waitedThroughSave)fail("close-or-stop-budget-killed-slow-finalizer");});}
 });
 QObject::connect(&service,&RecordingService::finished,&app,[&](const QJsonObject& value){
   if(phase==9){if(value.value("state").toString()!="Failed")fail("cancelled-new-prepare-state");else app.quit();return;}
   if(value.value("state").toString()!="Completed"){fail(value.value("error").toString());return;}
   if(phase==5){descriptor=value.value("sessionDescriptorPath").toString();if(descriptor.isEmpty())fail("missing-descriptor");phase=6;}
   else if(phase==7){phase=8;service.requestStopForClose();auto next=reusable;next.finalOutputPath=QDir(out).filePath("after-close.mp4");QString error;pendingStartRefused=!service.start(next,&error);if(!pendingStartRefused)fail("new-session-crossed-pending-close");}
 });
 QObject::connect(&service,&RecordingService::closeReady,&app,[&]{
   ++closeCount;if(service.isBusy()){fail("close-ready-while-busy");return;}
   if(phase==6){phase=7;RecordingRecoveryRequest request;request.runtime=runtime;request.sessionDescriptorPath=descriptor;request.finalOutputPath=QDir(out).filePath("recovered.mp4");QString error;if(!service.recover(request,&error))fail(error);}
   else if(phase==8){globalIdleObserved=!RecordingService::hasActiveSessions();if(pulses<150||maxGap>750||!waitedThroughSave||!globalBusyObserved||!globalIdleObserved||!pendingStartRefused||!nativeCounterMonotonic){fail("event-loop-or-global-busy-contract");return;}auto next=reusable;next.finalOutputPath=QDir(out).filePath("after-close.mp4");QString error;phase=9;deliveredStartAccepted=service.start(next,&error);if(!deliveredStartAccepted)fail(error);else service.stop();}
 });
 QTimer::singleShot(45000,&app,[&]{fail("service-contract-timeout");});
 service.refreshCatalog(runtime,true);
 int code=app.exec();
 QJsonObject report{{"backgroundMode",true},{"passed",code==0},{"phase",phase},{"error",failure},{"events",events},{"pulses",pulses},{"maxPulseGapMs",maxGap},{"nativeCounterMonotonic",nativeCounterMonotonic},{"waitedThroughSlowSave",waitedThroughSave},{"globalBusyObserved",globalBusyObserved},{"globalIdleObserved",globalIdleObserved},{"pendingStartRefused",pendingStartRefused},{"deliveredStartAccepted",deliveredStartAccepted},{"closeReadyCount",closeCount},{"sessionDescriptorPath",descriptor}};
 QFile file(QDir(out).filePath("service-report.json"));file.open(QIODevice::WriteOnly);file.write(QJsonDocument(report).toJson());
 return code;
}
'''

class RecordingServiceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True,exist_ok=True)
        cls.source=OUT/'native';cls.source.mkdir(exist_ok=True)
        (cls.source/'main.cpp').write_text(CPP,encoding='utf-8')
        (cls.source/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.21)
project(CGPlayRecordingServiceContracts LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core)
add_executable(recording_service_probe main.cpp "${CGPLAY_SOURCE_ROOT}/src/services/recording/RecordingService.cpp" "${CGPLAY_SOURCE_ROOT}/src/services/recording/RecordingService.h")
target_include_directories(recording_service_probe PRIVATE "${CGPLAY_SOURCE_ROOT}/src/services/recording")
target_link_libraries(recording_service_probe PRIVATE Qt6::Core)
target_compile_definitions(recording_service_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
target_compile_options(recording_service_probe PRIVATE /utf-8)
endif()
''',encoding='utf-8')
        cls.build=DEPS/'build/qt-service-contracts'/OUT.name
        cls.environment=os.environ.copy();cls.environment['PATH']=str(QT/'bin')+os.pathsep+cls.environment.get('PATH','')
        cls.call('cmake','-S',cls.source,'-B',cls.build,'-G','Visual Studio 17 2022','-A','x64','-DCMAKE_PREFIX_PATH='+str(QT),'-DCGPLAY_SOURCE_ROOT='+str(ROOT))
        cls.call('cmake','--build',cls.build,'--config','Release','--parallel','2')

    @classmethod
    def call(cls,*arguments):
        with (OUT/'commands.log').open('ab') as log:
            subprocess.run([str(argument) for argument in arguments],cwd=ROOT,env=cls.environment,stdout=log,stderr=log,
                           check=True,creationflags=subprocess.CREATE_NO_WINDOW,timeout=120)

    def test_real_qt_service_record_pause_slow_publish_recover_and_async_close(self):
        case=OUT/'trip';case.mkdir(exist_ok=True)
        for name in ('service.mp4','recovered.mp4'):
            if (case/name).exists():self.fail('Use a fresh artifact directory; recording targets are never replaced')
        wrapper=OUT/'slow_finalizer.py'
        script=str(ROOT/'tools/cgplay/export/recording_finalize.py')
        wrapper.write_text("import sys,json,time,runpy\nfrom pathlib import Path\nr=json.loads(Path(sys.argv[sys.argv.index('--request')+1]).read_text(encoding='utf-8'))\nif r.get('operation') in ('finalize','recover'):time.sleep(4.2)\nrunpy.run_path("+repr(script)+",run_name='__main__')\n",encoding='utf-8')
        self.call(self.build/'Release/recording_service_probe.exe',case,DEPS/'stage',PACKAGE/'runtime/python/python.exe',ROOT/'tools',wrapper,PACKAGE/'ffmpeg.exe',PACKAGE/'ffprobe.exe')
        value=json.loads((case/'service-report.json').read_text(encoding='utf-8'))
        self.assertTrue(value['passed'],value);self.assertTrue(value['waitedThroughSlowSave']);self.assertGreater(value['pulses'],150)
        for name in ('service.mp4','recovered.mp4'):
            with av.open(str(case/name)) as video:
                self.assertGreater(sum(1 for frame in video.decode(video=0)),20)

if __name__=='__main__':unittest.main()
