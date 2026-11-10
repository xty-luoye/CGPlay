"""R17/R59: bounded pool sequence decoding and real FFmpeg short-video thumbnail."""
import os
import subprocess
import unittest
from tests.native_review_probe import build_and_run
from tests.test_review_metadata_repairs import ROOT

OUT = ROOT / 'tests/artifacts/fix_review_20261101/stage18'
POOL = r'''
#include "media/ThumbnailService.h"
#include "media/ThumbnailDiskCache.h"
#include "media/MediaProbe.h"
#include "common/jobs/JobSystem.h"
#include <QImage>
#include <QElapsedTimer>
#include <QThreadPool>
#include <QThread>
#include <QtConcurrent>
namespace cgplay {
bool MediaProbe::isStillImagePath(const QString&){return true;}
QString MediaProbe::locateFfmpeg(){return {};}
MediaInfo MediaProbe::probe(const QString&,double,JobContext*){return {};}
int MediaInfo::effectiveFrameCount()const{return 3;}
QVector<SequenceFrame> MediaProbe::collectSequenceFiles(const QString&p){QVector<SequenceFrame> files;
for(int i=1;i<=3;++i)files.push_back({i,QFileInfo(p).dir().filePath(QString("image.%1.png").arg(i,4,10,QLatin1Char('0')))});return files;}
bool OcioManager::applyPreviewTransform(QImage&,const PreviewTransformSettings&){return true;}
}
int main(int argc,char**argv){QCoreApplication app(argc,argv);QJsonObject report;using namespace cgplay;
const QDir dir(QString::fromUtf8(argv[1]));for(int i=1;i<=3;++i){QImage image(32,24,QImage::Format_RGB32);image.fill(qRgb(20*i,40*i,60*i));if(!image.save(dir.filePath(QString("image.%1.png").arg(i,4,10,QLatin1Char('0')))))return 2;}
const QString first=dir.filePath("image.0001.png");auto*pool=QThreadPool::globalInstance();
for(int threads:{1,2,3}){pool->setMaxThreadCount(threads);QList<QFuture<QVector<ThumbnailFrame>>>futures;
for(int n=0;n<threads;++n)futures.push_back(QtConcurrent::run([first]{JobContext job(5000);return ThumbnailService::buildTimelineThumbnails(first,3,24,3,{},&job);}));
QElapsedTimer timer;timer.start();bool done=false;while(timer.elapsed()<6000){done=true;for(const auto&future:futures)done=done&&future.isFinished();if(done)break;QCoreApplication::processEvents();QThread::msleep(1);}
if(!done){report[QString("pool%1").arg(threads)]=false;puts(QJsonDocument(report).toJson(QJsonDocument::Compact));fflush(stdout);ExitProcess(3);}
bool ok=true;for(auto&future:futures){const auto frames=future.result();ok=ok&&frames.size()==3;for(int i=0;i<frames.size();++i)ok=ok&&frames[i].frame==i&&!frames[i].image.isNull();}
report[QString("pool%1AllConcurrentRequestsComplete").arg(threads)]=ok;report[QString("pool%1ElapsedMs").arg(threads)]=timer.elapsed();pool->waitForDone();}
return finish(report);}
'''

SHELL = r'''
#include "shell/CGPlayThumbnailProvider.cpp"
int main(int argc,char**argv){QCoreApplication app(argc,argv);const QDir dir(QString::fromUtf8(argv[1]));QJsonObject report;
for(const QString name:{QString("short"),QString("long")}){std::vector<BYTE>data;bool ok=renderFrame(dir.filePath(name+".mp4").toStdWString(),128,&data);
QImage image=QImage::fromData(data.data(),static_cast<int>(data.size()),"JPG");report[name+"VideoProducesValidFrame"]=ok&&!image.isNull()&&image.size()==QSize(128,128);}
std::vector<BYTE>missing;report["missingSourceStillFails"]=!renderFrame(dir.filePath("missing.mp4").toStdWString(),128,&missing);
return finish(report);}
'''


class ThumbnailScanRepairs(unittest.TestCase):
    def test_bounded_pool_real_sequence_decode(self):
        build_and_run(OUT / 'pool', POOL, sources=[
            'src/services/media/ThumbnailService.cpp', 'src/services/media/ThumbnailDiskCache.cpp',
            'src/common/jobs/JobSystem.cpp', 'src/common/jobs/JobSystem.h',
            'src/common/core/AsyncImageLoader.cpp', 'src/common/core/AsyncImageLoader.h'])

    def test_short_shell_video_thumbnail(self):
        directory = OUT / 'shell'; fixture = directory / 'fixture'; fixture.mkdir(parents=True, exist_ok=True)
        ffmpeg = ROOT / 'build_win_full/bin/Release/ffmpeg.exe'
        binary = directory / 'build/Release'; binary.mkdir(parents=True, exist_ok=True)
        if not (binary / 'ffmpeg.exe').exists():
            os.link(ffmpeg, binary / 'ffmpeg.exe')
        for name, count in [('short', 2), ('long', 10)]:
            result = subprocess.run([str(ffmpeg), '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi',
                                     '-i', 'testsrc2=size=32x32:rate=10', '-frames:v', str(count), '-c:v', 'libx264', str(fixture / (name + '.mp4'))],
                                    capture_output=True, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0), timeout=15)
            self.assertEqual(0, result.returncode, result.stderr)
        build_and_run(directory, '\n#include <QImage>\n' + SHELL,
                      extra_cmake='target_link_libraries(probe PRIVATE shlwapi ole32 windowscodecs gdi32)\n')


if __name__ == '__main__':
    unittest.main()
