"""Draft runtime coverage for atomic thumbnails and completed-job disposal."""
from pathlib import Path
import json
import os
import re
import subprocess
from tests.native_probe_runtime import deploy_runtime
import unittest

ROOT = Path(__file__).resolve().parents[1]
ARTIFACT = Path(os.environ.get('CGPLAY_SMALL_FIX_ARTIFACT_DIR', ROOT/'tests/artifacts/fix_review_20261101/remaining_regression'))/'thumbnail_jobs'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

PROXY = r'''
#include <windows.h>
#include <fstream>
#include <iterator>
#include <string>
int wmain(int argc,wchar_t**argv) {
    if(argc<2) return 2;
    wchar_t ready[32768]{},jpeg[32768]{},counter[32768]{};
    GetEnvironmentVariableW(L"CGPLAY_RACE_READY",ready,32768);
    GetEnvironmentVariableW(L"CGPLAY_RACE_JPEG",jpeg,32768);
    GetEnvironmentVariableW(L"CGPLAY_RACE_COUNTER",counter,32768);
    {std::ofstream count(counter,std::ios::app); count<<"1\n";}
    std::ifstream input(jpeg,std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(input)),{});
    if(data.size()<16) return 3;
    std::ofstream output(argv[argc-1],std::ios::binary|std::ios::trunc);
    output.write(data.data(),4); output.flush();
    {std::ofstream flag(ready); flag<<"ready";}
    Sleep(650);
    output.write(data.data()+4,data.size()-4); output.flush();
    return 0;
}
'''

RUNNER = r'''
#include "common/jobs/JobSystem.h"
#include "media/MediaProbe.h"
#include "media/ThumbnailService.h"
#include "media/ThumbnailDiskCache.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QThreadPool>
#include <QEvent>
#include <QtConcurrent>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace cgplay;
namespace cgplay {
bool MediaProbe::isStillImagePath(const QString&) {return false;}
bool MediaProbe::isVideoPath(const QString&) {return true;}
QString MediaProbe::locateFfmpeg() {return qEnvironmentVariable("CGPLAY_TEST_FFMPEG");}
QString MediaProbe::locateFfprobe() {return {};}
double MediaProbe::parseFpsRatio(const QString&) {return 24.0;}
MediaInfo MediaProbe::probe(const QString&,double,JobContext*) {return {};}
QVector<SequenceFrame> MediaProbe::collectSequenceFiles(const QString&) {return {};}
int MediaInfo::effectiveFrameCount() const {return frameCount>0?frameCount:1;}
bool OcioManager::applyPreviewTransform(QImage&,const PreviewTransformSettings&) {return true;}
}
static int visibleWindows() {
    int result=0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window,LPARAM data)->BOOL {
        DWORD pid=0; GetWindowThreadProcessId(window,&pid);
        if(pid==GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(data);
        return TRUE;
    },reinterpret_cast<LPARAM>(&result));
#endif
    return result;
}
static int starts() {
    QFile input(qEnvironmentVariable("CGPLAY_RACE_COUNTER"));
    return input.open(QIODevice::ReadOnly)?input.readAll().count('\n'):0;
}
static bool ready() {
    QElapsedTimer wait; wait.start();
    while(!QFileInfo::exists(qEnvironmentVariable("CGPLAY_RACE_READY")) && wait.elapsed()<5000) QThread::msleep(5);
    return QFileInfo::exists(qEnvironmentVariable("CGPLAY_RACE_READY"));
}
int main(int argc,char**argv) {
    QApplication app(argc,argv);
    app.setProperty("cgplay.automationBackground",true);
    QThreadPool::globalInstance()->setMaxThreadCount(4);
    QImage fixture(112,64,QImage::Format_RGB32); fixture.fill(Qt::green);
    if(!fixture.save(qEnvironmentVariable("CGPLAY_RACE_JPEG"),"JPG")) return 2;
    const QDir folder(QString::fromLocal8Bit(argv[1]));
    auto diskPolicy=ThumbnailDiskCache::policy();
    diskPolicy.baseDirectory=folder.filePath("thumbnail-cache");
    ThumbnailDiskCache::configure(diskPolicy);
    QJsonArray cases;
    auto thumbnails=[&](const QString& path,JobContext* context) {
        return ThumbnailService::buildTimelineThumbnails(path,1900,24.0,1,{},context);
    };
    auto runRace=[&](const QString& name,bool cancelProducer,bool cancelConsumer=false) {
        QFile::remove(qEnvironmentVariable("CGPLAY_RACE_READY"));
        QFile::remove(qEnvironmentVariable("CGPLAY_RACE_COUNTER"));
        const QString media=folder.filePath(name+".mp4");
        {QFile f(media); if(!f.open(QIODevice::WriteOnly)) return false; f.write(name.toUtf8());}
        JobContext producerContext(9000),consumerContext(9000);
        auto first=QtConcurrent::run([&]{return thumbnails(media,&producerContext);});
        if(!ready()) {producerContext.cancel();first.waitForFinished();return false;}
        auto second=QtConcurrent::run([&]{return thumbnails(media,&consumerContext);});
        if(cancelProducer) {QThread::msleep(30);producerContext.cancel();}
        if(cancelConsumer) {QThread::msleep(30);consumerContext.cancel();}
        first.waitForFinished();second.waitForFinished();
        const auto after=thumbnails(media,nullptr);
        const bool passed=(cancelConsumer?second.result().isEmpty():second.result().size()==1) && after.size()==1 &&
            (cancelProducer?first.result().isEmpty():first.result().size()==1) &&
            starts()==(cancelProducer?2:1);
        cases.push_back(QJsonObject{{"name",name},{"passed",passed},{"first",first.result().size()},
            {"second",second.result().size()},{"cached",after.size()},{"encoderStarts",starts()}});
        return passed;
    };
    bool ok=runRace("same_key_waits_for_complete",false);
    ok=runRace("canceled_producer_surviving_consumer_retries",true)&&ok;
    ok=runRace("canceled_consumer_does_not_cancel_producer",false,true)&&ok;
    QFile::remove(qEnvironmentVariable("CGPLAY_RACE_COUNTER"));
    const QString retimed=folder.filePath("retimed.mp4");
    {QFile media(retimed);media.open(QIODevice::WriteOnly);media.write("retimed");}
    const auto at24=ThumbnailService::buildTimelineThumbnails(retimed,1900,24.0,2,{},nullptr);
    const auto at30=ThumbnailService::buildTimelineThumbnails(retimed,1900,30.0,2,{},nullptr);
    const bool fpsKey=at24.size()==2&&at30.size()==2&&starts()==4;
    ok=fpsKey&&ok;
    cases.push_back(QJsonObject{{"name","FPS override has distinct timestamp cache"},{"passed",fpsKey},{"encoderStarts",starts()}});
    QObject owner;
    int completed=0,canceled=0;
    for(int i=0;i<40;++i) {
        auto* job=JobRunner::start("completed-disposal",&owner,1000,[](JobContext& context){
            QThread::msleep(10);return context.shouldStop()?JobOutcome::canceled():JobOutcome::success();
        });
        QObject::connect(job,&JobHandle::finished,&owner,[&,job](const JobOutcome& outcome){
            job->deleteLater();++completed;if(outcome.state==JobState::Canceled)++canceled;
        });
        if(i%2==0)job->cancel();
    }
    QElapsedTimer wait;wait.start();
    while(completed<40 && wait.elapsed()<5000) {app.processEvents();QThread::msleep(1);}
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    const int remaining=owner.findChildren<JobHandle*>().size();
    const bool disposed=completed==40 && canceled>=20 && remaining==0;
    cases.push_back(QJsonObject{{"name","completed_handles_disposed"},{"passed",disposed},
        {"completed",completed},{"canceled",canceled},{"remaining",remaining}});
    const int visible=visibleWindows();
    const bool background=qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen";
    QJsonObject report{{"cases",cases},{"backgroundMode",background},{"visiblePlatformWindows",visible},
        {"passed",ok&&disposed&&background&&visible==0}};
    std::puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
    return report["passed"].toBool()?0:1;
}
'''

def cmake_value(key):
    cache=(ROOT/'build_win_full/CMakeCache.txt').read_text(encoding='utf-8')
    match=re.search(r'^'+re.escape(key)+r':[^=]*=(.+)$',cache,re.MULTILINE)
    if not match: raise AssertionError('Missing '+key+'; set CGPLAY_TEST_QT_ROOT and CGPLAY_TEST_TL_ROOT')
    return match.group(1).strip()

class ThumbnailJobLifecycleRuntime(unittest.TestCase):
    def test_real_thumbnail_concurrency_and_job_disposal(self):
        ARTIFACT.mkdir(parents=True,exist_ok=True)
        source=ARTIFACT/'source';build=ARTIFACT/'build';source.mkdir(exist_ok=True)
        qt=Path(os.environ.get('CGPLAY_TEST_QT_ROOT') or Path(cmake_value('Qt6_DIR')).parents[2])
        tl=Path(os.environ.get('CGPLAY_TEST_TL_ROOT') or Path(cmake_value('OpenEXR_DIR')).parents[2])
        (source/'runner.cpp').write_text(RUNNER,encoding='utf-8')
        (source/'proxy.cpp').write_text(PROXY,encoding='utf-8')
        files=['src/services/media/ThumbnailService.cpp','src/services/media/ThumbnailService.h',
               'src/common/jobs/JobSystem.cpp','src/common/jobs/JobSystem.h',
               'src/services/media/ThumbnailDiskCache.cpp','src/services/media/ThumbnailDiskCache.h',
               'src/common/core/AsyncImageLoader.cpp','src/common/core/AsyncImageLoader.h']
        text='cmake_minimum_required(VERSION 3.21)\nproject(ThumbnailJobs LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 17)\nset(CMAKE_AUTOMOC ON)\n'
        text+='find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)\nfind_package(OpenEXR REQUIRED)\n'
        text+='add_executable(thumbnail_jobs runner.cpp '+' '.join('"'+(ROOT/f).as_posix()+'"' for f in files)+')\n'
        text+='add_executable(delayed_encoder proxy.cpp)\n'
        text+='target_include_directories(thumbnail_jobs PRIVATE '+' '.join('"'+(ROOT/f).as_posix()+'"' for f in ['src','src/common','src/common/core','src/services','src/services/media'])+')\n'
        text+='target_compile_options(thumbnail_jobs PRIVATE /utf-8)\ntarget_link_libraries(thumbnail_jobs PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent OpenEXR::OpenEXR user32)\n'
        (source/'CMakeLists.txt').write_text(text,encoding='utf-8')
        for name,args in (
            ('configure',['cmake','-S',str(source),'-B',str(build),'-G','Visual Studio 17 2022','-A','x64','-DCMAKE_PREFIX_PATH='+qt.as_posix()+';'+tl.as_posix()]),
            ('build',['cmake','--build',str(build),'--config','Release','--parallel','2'])):
            result=subprocess.run(args,capture_output=True,creationflags=NO_WINDOW,timeout=180)
            (ARTIFACT/(name+'.log')).write_bytes(result.stdout+result.stderr)
            self.assertEqual(result.returncode,0,(result.stdout+result.stderr)[-5000:])
        run=ARTIFACT/('run_'+str(os.getpid()));run.mkdir(exist_ok=True)
        temporary=run/'temp';temporary.mkdir(exist_ok=True)
        env=os.environ.copy()
        env.update(QT_QPA_PLATFORM='offscreen',CGPLAY_TEST_FFMPEG=str(build/'Release/delayed_encoder.exe'),
            CGPLAY_RACE_READY=str(run/'ready.flag'),CGPLAY_RACE_JPEG=str(run/'complete.jpg'),
            CGPLAY_RACE_COUNTER=str(run/'starts.txt'),TMP=str(temporary),TEMP=str(temporary))
        env['PATH']=str(qt/'bin')+';'+str(tl/'bin')+';'+env.get('PATH','')
        env['QT_QPA_PLATFORM_PLUGIN_PATH']=str(qt/'plugins'/'platforms')
        env['QT_PLUGIN_PATH']=str(qt/'plugins')
        deploy_runtime(build/'Release/thumbnail_jobs.exe', qt,
                       (tl/'bin', ROOT/'build_win_full'/'bin'/'Release'))
        result=subprocess.run([str(build/'Release/thumbnail_jobs.exe'),str(run)],env=env,capture_output=True,creationflags=NO_WINDOW,timeout=30)
        (ARTIFACT/'runtime.log').write_bytes(result.stdout+result.stderr)
        self.assertEqual(result.returncode,0,(result.stdout+result.stderr)[-5000:])
        report=json.loads(result.stdout.decode('utf-8').strip().splitlines()[-1])
        (ARTIFACT/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        self.assertTrue(report['passed'])
        self.assertEqual(report['visiblePlatformWindows'],0)
        self.assertTrue(report['backgroundMode'])
        self.assertEqual(len(report['cases']),5)
        self.assertTrue(all(case['passed'] for case in report['cases']))

    def test_all_three_real_callers_dispose_before_early_returns(self):
        media=(ROOT/'src/ui/app/ApplicationMedia.cpp').read_text(encoding='utf-8')
        marker='[this, job, path, sequenceFpsOverride, generation, mediaInfo](const JobOutcome& outcome) {'
        self.assertTrue(media.split(marker,1)[1].lstrip().startswith('job->deleteLater();'))
        playback=(ROOT/'src/core/playback/PlaybackController.cpp').read_text(encoding='utf-8')
        marker='[this, job, path, sourcePath, sequenceFpsOverride, generation](const JobOutcome& outcome) {'
        parts=playback.split(marker)
        self.assertEqual(len(parts),3)
        self.assertTrue(all(part.lstrip().startswith('job->deleteLater();') for part in parts[1:]))

if __name__=='__main__': unittest.main()
