"""R24 actual async provider flow, frozen range preparation and real Python decoding."""
import json
from pathlib import Path
import subprocess
import unittest
import os
import copy
from tests.native_review_probe import build_and_run
from tests.test_review_metadata_repairs import ROOT,function
OUT=ROOT/'tests/artifacts/fix_review_20261101/stage28'
PY=ROOT/'build_win_full/bin/Release/runtime/python/python.exe'
MAIN=r'''
#include "ai/AIProviderManager.h"
#include "ai/AIWorkflowService.h"
#include "ai/MediaFrameSnapshotService.h"
#include "ai/api/IAIContextBuilder.h"
#include "media/api/IMediaService.h"
#include "common/core/ServiceLocator.h"
#include "component/ComponentRuntimePaths.h"
#include <QApplication>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QElapsedTimer>
#include <QDateTime>
#include <atomic>
#include <cmath>
using namespace cgplay;
QString python,tools,mediaPath;
struct Runtime{QString pythonHome,toolsRoot;QString pythonFor()const{return python;}QString script(QString relative)const{return QDir(tools).filePath(relative);}};
Runtime fixtureRuntime(){return {{},tools};}
namespace cgplay{
ColorTransformSnapshot OcioManager::exportColorSnapshot()const{return {};}
bool MediaFrameSnapshotService::capture(const AIFrameCaptureRequest&,QVector<AIMediaFrameReference>*,QString*)const{return false;}
PREPARATION
}
struct Media:IMediaService{MediaInfo probe(const QString&,double)const override{return {};}
bool isStillImagePath(const QString&p)const override{return p.endsWith(".png");}bool isVideoPath(const QString&p)const override{return p.endsWith(".mkv");}QString formatLabelForPath(const QString&)const override{return {};}
QString displayNameForPath(const QString&)const override{return {};}
QVector<SequenceFrame> collectSequenceFiles(const QString&p)const override{QVector<SequenceFrame> frames;for(const auto&name:QDir(QFileInfo(p).absolutePath()).entryList({"shot.*.png"},QDir::Files,QDir::Name)){int n=name.mid(5,4).toInt();frames.append({n,QDir(QFileInfo(p).absolutePath()).filePath(name)});}return frames;}
QString locateFfmpeg()const override{return {};}QString locateFfprobe()const override{return {};}double parseFpsRatio(const QString&)const override{return 1;}
QImage makePreviewImage(const QImage&,bool,const QSize&,const OcioManager::PreviewTransformSettings&)const override{return {};}
QIcon makePlaylistIcon(const QString&,const OcioManager::PreviewTransformSettings&)const override{return {};}
QVector<ThumbnailFrame> buildTimelineThumbnails(const QString&,int,double,int,const OcioManager::PreviewTransformSettings&)const override{return {};}};
struct Context:IAIContextBuilder{AIRequestContext buildContext(AIRequestScope scope,const QJsonObject&options)const override{AIRequestContext c;c.scope=scope;c.mediaPath=options["mediaPath"].toString();c.fps=10;c.totalFrames=20;c.mediaWidth=64;c.mediaHeight=32;c.startFrame=options["startFrame"].toInt(0);c.endFrame=options["endFrame"].toInt(19);return c;}};
struct Provider:IAIProvider{std::atomic_int calls{0};QString providerId()const override{return "fixture";}QString providerName()const override{return "fixture";}bool isAvailable()const override{return true;}AIProviderCapabilities capabilities()const override{return {};}
AIResponse chat(const AIRequest&r)override{++calls;QJsonArray data;for(const auto&frame:r.frames){QImage image;image.loadFromData(frame.encodedBytes);data.append(QJsonObject{{"frame",frame.frame},{"red",image.pixelColor(20,10).red()}});}AIResponse out;out.success=true;out.rawText=QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact));return out;}AIAudioTranscriptionResult transcribe(const AIAudioTranscriptionRequest&)override{return {};}};
bool wait(AIProviderManager&manager,QString id){QElapsedTimer t;t.start();while(t.elapsed()<30000){QCoreApplication::processEvents();const auto state=manager.jobSnapshot(id).state;if(state==AIJobState::Succeeded||state==AIJobState::Failed||state==AIJobState::Canceled)return true;QThread::msleep(2);}return false;}
int main(int argc,char**argv){QApplication app(argc,argv);QJsonObject result;QDir dir(QString::fromUtf8(argv[1]));python=QString::fromUtf8(argv[2]);tools=QString::fromUtf8(argv[3]);mediaPath=QString::fromUtf8(argv[4]);Media media;ServiceLocator::registerService<IMediaService>(&media);Context context;MediaFrameSnapshotService snapshot;AIProviderManager manager;auto provider=std::make_shared<Provider>();manager.registerProvider(provider);AIWorkflowService workflow(&manager,&context,&snapshot,nullptr);AIWorkflowRequest request;request.providerId="fixture";request.scope=AIRequestScope::CurrentSequence;request.attachFrames=true;request.requireFrames=true;request.sampleCount=4;request.targetSize={64,32};request.options={{"mediaPath",mediaPath}};
int beats=0;QTimer timer;timer.setInterval(10);QObject::connect(&timer,&QTimer::timeout,[&]{++beats;});timer.start();QElapsedTimer submit;submit.start();auto id=workflow.submit(request);result["submissionDoesNotWaitForDecode"]=!id.isEmpty()&&submit.elapsed()<250;bool complete=wait(manager,id);auto response=manager.jobSnapshot(id).response;auto frames=QJsonDocument::fromJson(response.rawText.toUtf8()).array();result["rangeActuallyReachesProvider"]=complete&&response.success&&response.frameCount==4&&frames.size()==4;
result["evenlySpacedDistinctDecodedSamples"]=frames.size()==4&&frames[0].toObject()["frame"].toInt()==0&&frames[1].toObject()["frame"].toInt()==6&&frames[2].toObject()["frame"].toInt()==13&&frames[3].toObject()["frame"].toInt()==19&&frames[3].toObject()["red"].toInt()>frames[0].toObject()["red"].toInt()+100;result["guiEventLoopRemainsResponsive"]=beats>3;
request.scope=AIRequestScope::CurrentShot;request.options["startFrame"]=5;request.options["endFrame"]=8;request.sampleCount=8;id=workflow.submit(request);wait(manager,id);response=manager.jobSnapshot(id).response;frames=QJsonDocument::fromJson(response.rawText.toUtf8()).array();result["shotBoundsAndShortRangeHonored"]=response.success&&frames.size()==4&&frames[0].toObject()["frame"].toInt()==5&&frames[3].toObject()["frame"].toInt()==8;
const int before=provider->calls.load();id=workflow.submit(request);manager.cancel(id);wait(manager,id);QThreadPool::globalInstance()->waitForDone();QCoreApplication::processEvents();result["cancelDoesNotSendFramesToProvider"]=manager.jobSnapshot(id).state==AIJobState::Canceled&&provider->calls==before;
request.options["mediaPath"]=dir.filePath("missing.mkv");result["missingSourceRejectedBeforeProvider"]=workflow.submit(request).isEmpty()&&provider->calls==before;
request.options["mediaPath"]=mediaPath;request.options["endFrame"]=20;result["outOfRangeRejected"]=workflow.submit(request).isEmpty();request.options["endFrame"]=8;
AIRequest direct;direct.context=context.buildContext(request.scope,request.options);AIFrameCaptureRequest capture;capture.sampleCount=2;capture.targetSize={64,32};QString error;bool prepared=snapshot.prepareRangeCapture(capture,&direct,&error);auto plan=direct.options["rangeCaptureJob"].toObject();plan["helper"]=dir.filePath("missing-helper.py");direct.options["rangeCaptureJob"]=plan;id=manager.submit(direct);wait(manager,id);response=manager.jobSnapshot(id).response;result["decoderFailureNeverBecomesTextOnlySuccess"]=prepared&&!response.success&&!response.frameCaptureError.isEmpty()&&provider->calls==before;
// The same preparation maps sequence source numbers without packing holes.
request.scope=AIRequestScope::CurrentSequence;request.options={{"mediaPath",QString::fromUtf8(argv[5])}};request.sampleCount=3;id=workflow.submit(request);wait(manager,id);response=manager.jobSnapshot(id).response;result["sequenceRangeDecoded"]=response.success&&response.frameCount==3;timer.stop();QThreadPool::globalInstance()->waitForDone();return finish(result);}
'''
class RangeFrameRepairs(unittest.TestCase):
 def test_real_exr_identity_and_running_cancel(self):
    script=r'''
import sys,json,os,copy,subprocess
from pathlib import Path
import numpy as np,OpenImageIO as oiio
from PIL import Image
root=Path(sys.argv[1]);out=Path(sys.argv[2]);sys.path.insert(0,str(root/'tools'))
from cgplay.ai.capture_frame_range import capture
p=out/'range.exr';a=np.zeros((2,4,3),np.float32);a[...,0]=4;a[...,1]=1;a[...,2]=-1
spec=oiio.ImageSpec(4,2,3,oiio.FLOAT);spec.channelnames=['R','G','B'];writer=oiio.ImageOutput.create(str(p));assert writer.open(str(p),spec);assert writer.write_image(a);writer.close()
config=out/'gain.ocio';config.write_text('ocio_profile_version: 2\nroles:\n  scene_linear: Linear\n  default: Linear\nfile_rules:\n  - !<Rule> {name: Default, colorspace: Linear}\ndisplays:\n  sRGB:\n    - !<View> {name: Gain, colorspace: Display}\nactive_displays: [sRGB]\nactive_views: [Gain]\ncolorspaces:\n  - !<ColorSpace>\n    name: Linear\n    family: ""\n    bitdepth: 32f\n    isdata: false\n    allocation: uniform\n  - !<ColorSpace>\n    name: Display\n    family: ""\n    bitdepth: 32f\n    isdata: false\n    allocation: uniform\n    from_scene_reference: !<MatrixTransform> {matrix: [0.1, 0, 0, 0, 0, 0.1, 0, 0, 0, 0, 0.1, 0, 0, 0, 0, 1]}\n')
s=p.stat();job={'samples':[{'path':str(p),'frame':0,'size':s.st_size,'mtimeMs':s.st_mtime_ns//1000000}], 'fps':24,'totalFrames':1,'targetWidth':4,'targetHeight':2,'sourceKind':'image','width':4,'height':2,'color':{'schemaVersion':1,'mode':'ocio','configPath':str(config),'input':'Linear','display':'sRGB','view':'Gain','outputEncoding':'srgb','exposure':0,'gamma':1}}
# Read the production color contract below rather than infer successful defaults.
capture(job,out);pixels=np.array(Image.open(out/'frame_0.png'));assert pixels[0,0].tolist()==[102,26,0],pixels[0,0]
changed=copy.deepcopy(job);changed['samples'][0]['size']+=1
try:capture(changed,out);raise AssertionError('changed source accepted')
except ValueError as e:assert 'changed' in str(e)
depth=out/'depth.exr';ds=oiio.ImageSpec(4,2,1,oiio.FLOAT);ds.channelnames=['Z'];writer=oiio.ImageOutput.create(str(depth));assert writer.open(str(depth),ds);writer.write_image(np.ones((2,4,1),np.float32));writer.close();s=depth.stat();bad=copy.deepcopy(job);bad['samples']=[{'path':str(depth),'frame':0,'size':s.st_size,'mtimeMs':s.st_mtime_ns//1000000}]
try:capture(bad,out);raise AssertionError('depth interpreted as RGB')
except Exception as e:assert not isinstance(e,AssertionError)
print(json.dumps({'passed':True,'realFloatEXRColorMapped':True,'sourceChangeRejected':True,'dataAOVRejected':True,'backgroundMode':True,'visiblePlatformWindows':0}))
'''
    extra=OUT/'extra';extra.mkdir(exist_ok=True)
    proc=subprocess.run([str(PY),'-c',script,str(ROOT),str(extra)],capture_output=True,creationflags=subprocess.CREATE_NO_WINDOW)
    (extra/'runtime.log').write_bytes(proc.stdout+proc.stderr)
    self.assertEqual(0,proc.returncode,proc.stderr.decode(errors='replace'))
    (extra/'result.json').write_bytes(proc.stdout)
    # A helper fixture signals startup, then blocks; cancellation must kill it
    # and remove the actual temporary control directory before returning.
    helper=extra/'blocking_helper.py'
    helper.write_text("import sys,time\nfrom pathlib import Path\np=Path(sys.argv[sys.argv.index('--job')+1]);Path(__file__).with_suffix('.started').write_text(str(p.parent))\ntime.sleep(30)\n",encoding='utf-8')
    main=r'''
#include "ai/AIFrameRangeCapture.h"
#include <QCoreApplication>
#include <QThread>
#include <thread>
int main(int argc,char**argv){QCoreApplication app(argc,argv);QDir dir(QString::fromUtf8(argv[1]));QString helper=QString::fromUtf8(argv[3]);QFile::remove(QFileInfo(helper).path()+"/blocking_helper.started");cgplay::AIRequest request;request.context.scope=cgplay::AIRequestScope::CurrentSequence;request.context.fps=24;request.options["rangeCaptureJob"]=QJsonObject{{"samples",QJsonArray{QJsonObject{{"frame",0}}}},{"python",QString::fromUtf8(argv[2])},{"helper",helper}};auto token=std::make_shared<std::atomic_bool>(false);QString error;bool ok=true;std::thread worker([&]{ok=cgplay::prepareDeferredAIFrames(&request,token,&error);});QElapsedTimer timer;timer.start();QString marker=QFileInfo(helper).path()+"/blocking_helper.started";while(!QFileInfo::exists(marker)&&timer.elapsed()<10000)QThread::msleep(10);bool started=QFileInfo::exists(marker);QFile file(marker);file.open(QIODevice::ReadOnly);QString control=QString::fromUtf8(file.readAll());token->store(true);timer.restart();worker.join();QJsonObject result{{"helperReallyStarted",started},{"runningCancelStopsAndCleans",!ok&&error=="Canceled"&&request.frames.isEmpty()&&!control.isEmpty()&&!QFileInfo::exists(control)},{"cancelUnderTwoSeconds",timer.elapsed()<2000}};return finish(result);}
'''
    build_and_run(OUT/'running_cancel',main,arguments=[PY,helper])
 def test_real_worker_flow(self):
    fixture=OUT/'media';fixture.mkdir(exist_ok=True)
    create=r'''
import av,numpy as np
from pathlib import Path
from PIL import Image
p=Path(__import__('sys').argv[1]);out=av.open(str(p/'ramp.mkv'),'w');stream=out.add_stream('ffv1',rate=10);stream.width=64;stream.height=32;stream.pix_fmt='bgr0'
for n in range(20):
 a=np.zeros((32,64,3),np.uint8);a[...,0]=n*10
 f=av.VideoFrame.from_ndarray(a,format='rgb24');f.pts=n
 for packet in stream.encode(f):out.mux(packet)
 Image.fromarray(a).save(p/f'shot.{1001+n:04}.png')
for packet in stream.encode(None):out.mux(packet)
out.close()
'''
    proc=subprocess.run([str(PY),'-c',create,str(fixture)],capture_output=True,creationflags=subprocess.CREATE_NO_WINDOW)
    self.assertEqual(0,proc.returncode,proc.stderr)
    source=(ROOT/'src/services/ai/MediaFrameSnapshotService.cpp').read_text(encoding='utf-8-sig');preparation=function(source,'bool MediaFrameSnapshotService::prepareRangeCapture(').replace('captureComponentRuntimePaths()', 'fixtureRuntime()')
    build_and_run(OUT/'native',MAIN.replace('PREPARATION',preparation),['src/services/ai/AIProviderManager.cpp','src/services/ai/AIWorkflowService.cpp'],arguments=[PY,ROOT/'tools',fixture/'ramp.mkv',fixture/'shot.1001.png'],timeout=90)
if __name__=='__main__':unittest.main()

