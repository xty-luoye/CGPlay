"""Execute the production export_media branch with real Qt/FFmpeg/PyAV."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

import av
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'tests/artifacts/fix_review_20261101/workbench_range'
QT=Path('E:/rjkf/CGPlay-deps/engine_migration_1_0_8/qt-6.11.3-msvc2022_64')
FFMPEG=ROOT/'build_win_full/bin/Release/ffmpeg.exe'


def run(args,name,env=None,data=None,ok=True):
    result=subprocess.run([str(v) for v in args],input=data,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
        env=env,creationflags=subprocess.CREATE_NO_WINDOW,timeout=180)
    (OUT/(name+'.log')).write_bytes(result.stdout)
    if ok and result.returncode:raise RuntimeError(result.stdout.decode(errors='replace'))
    return result


def function(text,signature):
    start=text.index(signature);brace=text.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    text=(ROOT/'src/plugins/codex/CodexPluginTools.cpp').read_text(encoding='utf-8')
    branch=function(text,'if (name == QStringLiteral("export_media"))')
    branch=branch.replace('IPlaybackService*','PlaybackStub*')
    sync=function((ROOT/'src/plugins/codex/CodexPluginSupport.cpp').read_text(encoding='utf-8'),'JobOutcome runSynchronousJob(')
    source=r'''
#include "common/core/SafeFileCopy.h"
#include "common/jobs/JobSystem.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QThreadPool>
#include <windows.h>
using namespace cgplay;
''' + sync + r'''
struct PlaybackStub {QString path;int count;double rate;
bool isValid()const{return true;}QString currentPath()const{return path;}
int totalFrames()const{return count;}double fps()const{return rate;}
bool hasInPoint()const{return false;}bool hasOutPoint()const{return false;}
int inPoint()const{return 0;}int outPoint()const{return count-1;}};
void appendMediaAssetRecord(const QString&,const QString&,const QJsonObject&){}
QString jobError(const JobOutcome& o,const QString& fallback){return o.errorMessage.isEmpty()?fallback:o.errorMessage;}
struct Tool:QObject{QJsonObject run(QJsonObject arguments){
QString name="export_media";
auto resolveWorkspacePath=[](const QString&p){return p;};
auto response=[](bool success,QJsonObject payload){payload["success"]=success;return payload;};
''' + branch + r'''
return {};}};
int main(int argc,char**argv){QCoreApplication app(argc,argv);Tool tool;
PlaybackStub playback{QString::fromLocal8Bit(argv[1]),QString::fromLocal8Bit(argv[4]).toInt(),QString::fromLocal8Bit(argv[3]).toDouble()};
app.setProperty("cgplay.codex.playbackService",QVariant::fromValue<qulonglong>(reinterpret_cast<qulonglong>(&playback)));
if(argc>7)QTimer::singleShot(1,&tool,[&]{for(auto* handle:tool.findChildren<JobHandle*>())handle->cancel();});
auto result=tool.run(QJsonObject{{"output",QString::fromLocal8Bit(argv[2])},{"startFrame",QString::fromLocal8Bit(argv[5]).toInt()},{"endFrame",QString::fromLocal8Bit(argv[6]).toInt()}});
QThreadPool::globalInstance()->waitForDone();
auto data=QJsonDocument(result).toJson(QJsonDocument::Compact);fwrite(data.constData(),1,data.size(),stdout);return 0;}
'''
    (OUT/'probe.cpp').write_text(source,encoding='utf-8')
    (OUT/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.21)
project(RangeRepair LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Concurrent)
add_executable(range_probe probe.cpp "{ROOT.as_posix()}/src/common/jobs/JobSystem.cpp" "{ROOT.as_posix()}/src/common/jobs/JobSystem.h")
target_include_directories(range_probe PRIVATE "{ROOT.as_posix()}/src")
target_compile_options(range_probe PRIVATE /utf-8)
target_link_libraries(range_probe PRIVATE Qt6::Core Qt6::Concurrent)
''',encoding='utf-8')
    run(['cmake','-S',OUT,'-B',OUT/'build','-G','Visual Studio 17 2022','-A','x64',f'-DCMAKE_PREFIX_PATH={QT}'],'configure')
    run(['cmake','--build',OUT/'build','--config','Release','--parallel','4'],'build')
    target=OUT/'build/Release'
    for name in ['Qt6Core.dll','Qt6Concurrent.dll']:
        dest=target/name
        if not dest.exists():os.link(QT/'bin'/name,dest)
    dest=target/'ffmpeg.exe'
    if not dest.exists():os.link(FFMPEG,dest)
    env=os.environ.copy();env['PATH']=str(target)+os.pathsep+env['PATH']
    raw=b''.join(bytes([i*2])*64*64*3 for i in range(100))
    source=OUT/'source.mp4'
    run([FFMPEG,'-v','error','-y','-f','rawvideo','-pix_fmt','rgb24','-s','64x64','-r','25','-i','pipe:0',
        '-c:v','libx264rgb','-crf','0','-g','100','-keyint_min','100','-sc_threshold','0','-bf','0',source],'source',data=raw)
    with_audio=OUT/'audio.mp4'
    run([FFMPEG,'-v','error','-y','-i',source,'-f','lavfi','-i','sine=frequency=997:sample_rate=48000:duration=4',
        '-c:v','copy','-c:a','aac',with_audio],'audio')
    rows=[]
    def call(src,dest,name,start=25,end=49,total=100,cancel=False):
        args=[target/'range_probe.exe',src,dest,25,total,start,end]
        if cancel:args.append('cancel')
        result=run(args,name,env)
        return json.loads(result.stdout)
    for ext,src in [('mkv',with_audio),('mp4',with_audio),('mov',source)]:
        dest=OUT/('range.'+ext);result=call(src,dest,ext);assert result['success'],result
        with av.open(str(dest)) as c:
            frames=list(c.decode(video=0));values=[float(f.to_ndarray(format='rgb24').mean()) for f in frames]
        assert len(frames)==25 and np.max(np.abs(np.array(values)-np.arange(25,50)*2))<=2,values
        if ext!='mov':
            with av.open(str(dest)) as c:
                samples=sum(f.samples for f in c.decode(audio=0));assert 45000<=samples<=51000,samples
        rows.append({'container':ext,'count':len(frames),'firstMean':values[0],'lastMean':values[-1]})
    for label,ext,end,cancel in [('incomplete','mp4',120,False),('bad-format','unsupported',49,False),('cancel','mkv',49,True)]:
        dest=OUT/(label+'.'+ext);dest.write_bytes(b'old target')
        result=call(source,dest,label,end=end,total=140,cancel=cancel)
        assert not result['success'] and dest.read_bytes()==b'old target',result
    old=hashlib.sha256(source.read_bytes()).hexdigest()
    assert not call(source,source,'same-source')['success']
    assert hashlib.sha256(source.read_bytes()).hexdigest()==old
    alias=OUT/'hardlink.mp4'
    if alias.exists():alias.unlink()
    os.link(source,alias)
    assert call(source,alias,'hardlink')['success']
    assert hashlib.sha256(source.read_bytes()).hexdigest()==old
    assert not list(OUT.glob('.cgplay-tool-export-*'))
    result={'passed':True,'backgroundMode':True,'encodedRanges':rows,'failedTargetPreserved':3,
            'sourceAliasPreserved':True,'temporaryDirectoriesCleaned':True,'productionToolBranch':True}
    (OUT/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))


if __name__=='__main__':main()
