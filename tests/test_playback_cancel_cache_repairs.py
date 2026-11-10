"""R03/R06/R10/R11/R14 targeted current-source runtime regressions."""
import subprocess
import unittest
from tests.native_review_probe import build_and_run
from tests.test_review_metadata_repairs import ROOT,function
from tests.product_object_probe import build_object_probe
OUT=ROOT/'tests/artifacts/fix_review_20261101/stage29'
FLAGS=subprocess.CREATE_NO_WINDOW

def fixture_video():
    media=OUT/'media';media.mkdir(exist_ok=True)
    path=media/'120fps.mp4'
    if not path.is_file():
        p=subprocess.run([str(ROOT/'build_win_full/bin/Release/ffmpeg.exe'),'-v','error','-y','-f','lavfi','-i','testsrc2=size=160x90:rate=120','-t','8','-c:v','libx264','-preset','ultrafast',str(path)],capture_output=True,creationflags=FLAGS,timeout=30)
        if p.returncode:raise AssertionError(p.stderr.decode(errors='replace'))
    return path

class PlaybackRepairs(unittest.TestCase):
 def test_real_tl_cadence(self):
    main=(ROOT/'tests/review_repair_probes/cadence.cpp').read_text(encoding='utf-8')
    r=build_object_probe(OUT/'product_cadence',main,arguments=[fixture_video()])
    self.assertEqual(0,r['visiblePlatformWindowsMax'])
    self.assertEqual(8,r['120']['current_timer_interval_ms'])
    self.assertEqual(166,r['6']['current_timer_interval_ms'])
    self.assertGreater(r['120']['timer_tick_count'],160)
    self.assertGreater(r['120']['mediaDeltaSeconds'],1.2)
    self.assertGreater(r['120']['renderFrames'],110)
    self.assertGreaterEqual(r['6']['timer_tick_count'],7)
    self.assertLess(r['6']['timer_tick_count'],20)
    self.assertEqual(120,r['60']['actualSpeed'])
    self.assertEqual(8,r['60']['current_timer_interval_ms'])

 def test_concurrent_av1_cache(self):
    text=(ROOT/'src/core/playback/PlaybackController.cpp').read_text(encoding='utf-8-sig')
    funcs='\n'.join(function(text,s) for s in ('bool hasUsableVideoStream(','QString uniqueTranscodeTempPath(','JobOutcome ensureAv1Cache('))
    main=r'''
#include "common/jobs/JobSystem.h"
#include <QLockFile>
#include <QUuid>
#include <QThread>
#include <thread>
using namespace cgplay;
QString tools,cache;
QString locateBundledTool(const QString&name){return QDir(tools).filePath(name);}
QString av1CachePath(const QString&){return cache;}
FUNCTIONS
int main(int argc,char**argv){QCoreApplication app(argc,argv);QDir dir(QString::fromUtf8(argv[1]));tools=QString::fromUtf8(argv[2]);cache=dir.filePath("cache.mp4");QFile::remove(cache);QString media=QString::fromUtf8(argv[3]);JobOutcome outcomes[2];std::thread a([&]{JobContext c(30000);outcomes[0]=ensureAv1Cache(c,media);});std::thread b([&]{JobContext c(30000);outcomes[1]=ensureAv1Cache(c,media);});a.join();b.join();JobContext verify;QJsonObject result{{"bothConcurrentRequestsSucceed",outcomes[0].succeeded()&&outcomes[1].succeeded()},{"cachePlayable",hasUsableVideoStream(verify,locateBundledTool("ffprobe.exe"),cache)},{"noPrivateTempsRemain",dir.entryList({"*.tmp.mp4"},QDir::Files).isEmpty()}};QFile f(cache);f.open(QIODevice::ReadOnly);QByteArray before=f.readAll();f.close();JobContext reuse;auto reused=ensureAv1Cache(reuse,media);f.open(QIODevice::ReadOnly);result["reusePreservesCache"]=reused.succeeded()&&before==f.readAll();f.close();
QString validCache=cache;cache=dir.filePath("cancel.mp4");QLockFile blocker(cache+".lock");bool locked=blocker.tryLock();JobContext cancel(30000);JobOutcome canceled;std::thread waiter([&]{canceled=ensureAv1Cache(cancel,media);});QThread::msleep(200);cancel.cancel();waiter.join();blocker.unlock();result["waitingRequestCanCancel"]=locked&&canceled.state==JobState::Canceled&&!QFileInfo::exists(cache);return finish(result);}
'''
    tools=ROOT/'build_win_full/bin/Release'
    build_and_run(OUT/'cache',main.replace('FUNCTIONS',funcs),['src/common/jobs/JobSystem.cpp'],arguments=[tools,fixture_video()])

 def test_recording_failed_prepare_cleanup(self):
    main=(ROOT/'tests/review_repair_probes/recording_cleanup.cpp').read_text(encoding='utf-8')
    tools=ROOT/'build_win_full/bin/Release'
    recorder=next((ROOT/'build_win_full').rglob('CGPlayRecorder.exe')).parent
    build_and_run(OUT/'recording',main,['src/services/recording/RecordingService.cpp'],arguments=[recorder,tools/'runtime/python/python.exe',ROOT/'tools',tools/'ffmpeg.exe',tools/'ffprobe.exe'],timeout=40)

 def test_partial_cache_retries_and_skips(self):
    text=(ROOT/'src/ui/app/ApplicationSubtitles.cpp').read_text(encoding='utf-8-sig');start=text.index('        const QFileInfo partialInfo(partialPath);');end=text.index('\n    }\n    if (_p->subtitleSession.state().generatedSubtitleCues.isEmpty()',start);code=text[start:end]
    # Compile the exact production signature/read/merge branch; external reads
    # and UI state are deterministic counters, not a full playback claim.
    code=code.replace('_p->subtitleSession.state()','state').replace('_p->currentPath','media').replace('_p->translationPlaybackMode','mode').replace('TranslationPlaybackStrategy::highQualityModeId()','QStringLiteral("high-quality")').replace('SubtitleGenerationService::readSubtitleFile','readCues').replace('SubtitleGenerationService::mediaFingerprint','fingerprint')
    main=r'''
#include "ai/SubtitleGenerationService.h"
#include <QDateTime>
#include <QDebug>
using namespace cgplay;
struct State{bool generatedSubtitlePartialSignatureValid=false,generatedSubtitleEnhancedLoaded=false;QString generatedSubtitlePartialPath,generatedSubtitlePath,generatedSubtitleMediaFingerprint;qint64 generatedSubtitlePartialSize=-1,generatedSubtitlePartialModifiedMs=-1;QVector<GeneratedSubtitleCue> generatedSubtitleCues;}state;
bool ready=false;int checks=0,reads=0,merges=0;QString media="movie",mode="normal";
bool generatedSubtitleCacheMatchesCurrentMedia(const QString&,const QString&,double,QString*){++checks;return ready;}
QVector<GeneratedSubtitleCue> readCues(const QString&){++reads;return {GeneratedSubtitleCue{}};}
int scrubGeneratedSubtitleCrossMediaLeaks(QVector<GeneratedSubtitleCue>*,QString,QString,QString*){return 0;}
QString companionSubtitleSourceTextForMedia(const QString&){return {};}
QString fingerprint(const QString&){return "fingerprint";}
void mergeGeneratedSubtitleCues(QVector<GeneratedSubtitleCue>*to,const QVector<GeneratedSubtitleCue>&from,bool){++merges;*to=from;}
void refresh(QString partialPath){CODE}
void write(QString path,QByteArray bytes){QFile f(path);f.open(QIODevice::WriteOnly);f.write(bytes);}
int main(int argc,char**argv){QCoreApplication app(argc,argv);QDir dir(QString::fromUtf8(argv[1]));QString path=dir.filePath("partial.vtt");write(path,"first");refresh(path);bool pending=!state.generatedSubtitlePartialSignatureValid&&reads==0;ready=true;refresh(path);bool retried=state.generatedSubtitlePartialSignatureValid&&reads==1&&merges==1;for(int i=0;i<2400;++i)refresh(path);bool skipped=reads==1&&merges==1&&checks==2;write(path,"replacement larger");refresh(path);bool changed=reads==2&&merges==2;QString second=dir.filePath("second.vtt");write(second,"replacement larger");refresh(second);bool moved=reads==3;QFile::remove(second);refresh(second);bool cleared=!state.generatedSubtitlePartialSignatureValid;QJsonObject result{{"sidecarNotReadyRetried",pending&&retried},{"unchanged2400TicksNoIOOrMerge",skipped},{"sizeChangeRefreshes",changed},{"pathChangeRefreshes",moved},{"missingFileInvalidatesSignature",cleared}};return finish(result);}
'''
    build_and_run(OUT/'partial',main.replace('CODE',code))

if __name__=='__main__':unittest.main()
