"""Exercise real playlist/probe/jobs with a deterministic slow ffprobe process."""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
ARTIFACT = Path(os.environ.get("CGPLAY_PLAYLIST_TEST_ARTIFACT",
    str(ROOT / "tests/artifacts/playlist_async_contract")))
QT = Path(os.environ.get("CGPLAY_TEST_QT_ROOT", "C:/QtClean/6.5.3/msvc2019_64"))
TL = Path(os.environ.get("CGPLAY_TEST_TL_ROOT", os.environ.get("TLRENDER_INSTALL_DIR",
    str(ROOT.parent / "CGV/tlRender-main/install-Release"))))
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

RUNNER = r'''
#include "features/playlist/PlaylistModel.h"
#include "common/jobs/JobSystem.h"
#include "media/MediaProbe.h"
#include "media/ThumbnailService.h"
#include "otio/OtioExporter.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListView>
#include <QMimeData>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <atomic>
#include <functional>
#include <cstdio>
#include <windows.h>
using namespace cgplay;
static std::atomic<int> active{0}, peak{0}, canceled{0}, uiImages{0}, thumbDelay{300};
static QJsonObject report;
#define CHECK(x) do { if (!(x)) {report["failure"] = #x; report["line"] = __LINE__; return false;} } while(false)
namespace cgplay {
QImage ThumbnailService::makePlaylistImage(const QString&, const OcioManager::PreviewTransformSettings& settings,
                                           const MediaInfo*, JobContext* job) {
    if (QThread::currentThread() == qApp->thread()) ++uiImages;
    const int count = ++active;
    int old = peak.load(); while(count > old && !peak.compare_exchange_weak(old,count)) {}
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < thumbDelay) {
        if (job && job->shouldStop()) {
            // Cancellation must not free a dispatch slot before the worker exits.
            QThread::msleep(60); ++canceled; --active; return {};
        }
        QThread::msleep(5);
    }
    QImage image(16,16,QImage::Format_RGB32);image.fill(settings.enabled ? Qt::green : Qt::red);
    --active;return image;
}
}
static bool waitUntil(const std::function<bool()>& check,int timeout=6500) {
    QElapsedTimer timer;timer.start();
    while(!check() && timer.elapsed()<timeout){QCoreApplication::processEvents(QEventLoop::AllEvents,10);QThread::msleep(2);}
    return check();
}
static QString path(int n){return QDir::current().filePath(QString("d%1.mp4").arg(n));}
static int probeCount(){QFile f("probe_calls.txt");return f.open(QIODevice::ReadOnly)?f.readAll().count('\n'):0;}
static bool ready(const PlaylistModel& model){for(int i=0;i<model.shotCount();++i)if(model.shotAt(i).width<=0)return false;return true;}
static bool allThumbs(const PlaylistModel& model){for(int i=0;i<model.shotCount();++i)if(model.shotAt(i).thumbnail.isNull())return false;return true;}
static MediaInfo metadata(const QString& p){MediaInfo m;m.path=p;m.name=QFileInfo(p).baseName();m.width=64;m.height=36;m.fps=24;m.frameCount=48;m.lastFrame=47;m.exists=true;return m;}
static bool drop() {
    PlaylistModel model;int beats=0;QTimer heart;heart.setInterval(10);QObject::connect(&heart,&QTimer::timeout,[&]{++beats;});heart.start();
    QMimeData data;data.setUrls({QUrl::fromLocalFile(path(2)),QUrl::fromLocalFile(path(0)),QUrl::fromLocalFile(path(2)),QUrl::fromLocalFile(path(1))});
    QElapsedTimer timer;timer.start();CHECK(model.dropMimeData(&data,Qt::CopyAction,-1,0,{}));report["dropReturnMs"]=timer.elapsed();
    CHECK(timer.elapsed()<100);CHECK(model.shotCount()==4);CHECK(model.shotAt(0).path==path(2)&&model.shotAt(1).path==path(0)&&model.shotAt(2).path==path(2));
    model.setStatusColor(0,Qt::blue);CHECK(waitUntil([&]{return ready(model);}));
    CHECK(probeCount()==3);CHECK(model.shotAt(0).width==96&&model.shotAt(1).width==64&&model.shotAt(3).width==80);
    CHECK(model.statusColorAt(0)==QColor(Qt::blue));CHECK(beats>=100);CHECK(waitUntil([&]{return allThumbs(model);}));
    report["heartbeats"]=beats;report["probeCount"]=probeCount();
    QListView view;view.setModel(&model);view.resize(400,220);QImage capture(view.size(),QImage::Format_ARGB32);capture.fill(Qt::white);view.render(&capture);CHECK(capture.save("drop.png"));
    return true;
}
static bool removedAndSorted() {
    PlaylistModel model;model.addPaths({path(2),path(0),path(1)});
    CHECK(waitUntil([]{return probeCount()>0;},1000));model.setCurrentIndex(0);model.removeShotAt(0);model.addPath(path(3));model.moveUp(2);model.sort(false);
    CHECK(model.shotCount()==3);CHECK(model.shotAt(0).path==path(3));model.setCurrentIndex(0);model.removeShotAt(0);
    CHECK(waitUntil([&]{return ready(model);}));CHECK(model.shotCount()==2);
    CHECK(model.shotAt(0).path==path(1)&&model.shotAt(0).width==80);CHECK(model.shotAt(1).path==path(0)&&model.shotAt(1).width==64);
    CHECK(waitUntil([&]{return model.findChildren<JobHandle*>().isEmpty();}));CHECK(model.shotCount()==2);
    return true;
}
static bool clearReadd() {
    PlaylistModel model;model.addPath(path(0));CHECK(waitUntil([]{return probeCount()>0;},1000));
    model.clear();model.addPath(path(0));model.setStatusColor(0,Qt::green);
    CHECK(waitUntil([&]{return ready(model);}));CHECK(model.shotCount()==1&&model.shotAt(0).width==64&&model.statusColorAt(0)==QColor(Qt::green));
    CHECK(probeCount()==2);CHECK(waitUntil([&]{return model.findChildren<JobHandle*>().isEmpty();}));return true;
}
static bool restore() {
    QJsonArray shots;for(const QString& p:{path(0),path(0),path(1),QDir::current().filePath("missing.mp4")})shots.append(QJsonObject{{"path",p},{"name","saved"},{"width",5},{"height",5},{"fps",30.0},{"first_frame",3},{"last_frame",4},{"track_name","V2"},{"status_color","#ff0000ff"}});
    PlaylistModel model;QElapsedTimer timer;timer.start();model.fromJson({{"current_index",2},{"shots",shots}});report["restoreReturnMs"]=timer.elapsed();
    CHECK(timer.elapsed()<100&&model.shotCount()==4&&model.currentIndex()==2);CHECK(model.shotAt(0).width==5&&model.shotAt(0).name=="saved");
    CHECK(waitUntil([&]{return model.shotAt(2).width==80;}));CHECK(probeCount()==2);CHECK(model.shotAt(0).fps==30&&model.shotAt(0).trackName=="V2"&&model.statusColorAt(0)==QColor(Qt::blue));
    CHECK(model.shotAt(3).width==5&&model.shotAt(3).name=="saved");CHECK(waitUntil([&]{return model.findChildren<JobHandle*>().isEmpty();}));return true;
}
static bool queue() {
    PlaylistModel model;thumbDelay=500;for(int i=0;i<12;++i)model.addPath(path(i),metadata(path(i)));
    CHECK(waitUntil([]{return active==2;}));bool sentinel=false;QElapsedTimer timer;timer.start();
    auto* job=JobRunner::start("sentinel",qApp,1000,[](JobContext&){return JobOutcome::success();});QObject::connect(job,&JobHandle::finished,qApp,[&](const JobOutcome&){sentinel=true;});
    CHECK(waitUntil([&]{return sentinel;},250));report["sentinelMs"]=timer.elapsed();
    OcioManager::PreviewTransformSettings settings;settings.enabled=true;model.rebuildThumbnails(settings);model.rebuildThumbnails({});
    CHECK(waitUntil([&]{return allThumbs(model);},9000));CHECK(peak<=2&&canceled>=2);
    for(int i=0;i<model.shotCount();++i)CHECK(model.shotAt(i).thumbnail.pixmap(16,16).toImage().pixelColor(0,0)==QColor(Qt::red));
    model.clear();CHECK(waitUntil([&]{return model.findChildren<JobHandle*>().isEmpty();}));CHECK(active==0);return true;
}
static bool queuedCompletion() {
    PlaylistModel model;thumbDelay=100;model.addPath(path(0),metadata(path(0)));
    CHECK(waitUntil([]{return active==1;}));QThread::msleep(160);CHECK(active==0);
    // The worker has finished but its GUI completion is still queued.
    model.removeShotAt(0);model.addPath(path(0),metadata(path(0)));QCoreApplication::processEvents();
    CHECK(model.shotCount()==1&&model.shotAt(0).thumbnail.isNull());CHECK(waitUntil([&]{return allThumbs(model);}));return true;
}
static bool destroyed() {
    auto* model=new PlaylistModel;model->addPath(path(0));CHECK(waitUntil([]{return probeCount()>0;},1000));delete model;
    CHECK(waitUntil([]{return QThreadPool::globalInstance()->activeThreadCount()==0;},3000));CHECK(active==0);return true;
}
static bool pendingPersistence() {
    PlaylistModel model;model.addPath(path(0));CHECK(!model.shotAt(0).isMetadataReady()&&model.shotAt(0).fps==0);
    auto snapshot=model.toJson();auto item=snapshot["shots"].toArray()[0].toObject();CHECK(!item["metadata_ready"].toBool()&&item["fps"].toDouble()==0);
    model.clear();model.fromJson(snapshot);CHECK(!model.shotAt(0).isMetadataReady());
    CHECK(waitUntil([&]{return model.shotAt(0).isMetadataReady();}));CHECK(model.shotAt(0).fps==30);CHECK(model.toJson()["shots"].toArray()[0].toObject()["metadata_ready"].toBool());
    model.clear();model.addPath(path(1),25);snapshot=model.toJson();CHECK(snapshot["shots"].toArray()[0].toObject()["fps"].toDouble()==25);
    model.clear();model.fromJson(snapshot);CHECK(waitUntil([&]{return model.shotAt(0).isMetadataReady();}));CHECK(model.shotAt(0).fps==25);return true;
}
static bool confirmedMetadataWins() {
    PlaylistModel model;model.addPaths({path(0),path(0)});CHECK(waitUntil([]{return probeCount()>0;},1000));
    auto confirmed=metadata(path(0));confirmed.width=200;confirmed.frameCount=101;confirmed.lastFrame=100;
    model.updateMetadata(0,confirmed,25);CHECK(model.shotAt(0).isMetadataReady()&&model.shotAt(0).fps==25);
    CHECK(waitUntil([&]{return model.shotAt(1).isMetadataReady();}));
    CHECK(model.shotAt(0).width==200&&model.shotAt(0).fps==25&&model.shotAt(0).frameCount()==101);
    CHECK(model.shotAt(1).width==64&&model.shotAt(1).fps==30);CHECK(probeCount()==1);return true;
}
static bool exportGuard() {
    const QString destination=QDir::current().filePath("existing.otio");{QFile f(destination);CHECK(f.open(QIODevice::WriteOnly));f.write("keep-original");}
    auto originalIntact=[&]{QFile f(destination);return f.open(QIODevice::ReadOnly)&&f.readAll()=="keep-original";};
    PlaylistModel model;model.addPath(path(0));OtioExporter exporter;
    CHECK(!exporter.exportToFile(destination,&model));CHECK(!exporter.errorString().isEmpty()&&originalIntact());
    CHECK(!exporter.exportShotsToFile(destination,{model.shotAt(0)}));CHECK(originalIntact());
    CHECK(waitUntil([&]{return model.shotAt(0).isMetadataReady();}));const bool exported=exporter.exportToFile(destination,&model);report["exportError"]=exporter.errorString();CHECK(exported);CHECK(exporter.errorString().isEmpty());
    QFile file(destination);CHECK(file.open(QIODevice::ReadOnly));const auto document=QJsonDocument::fromJson(file.readAll()).object();file.close();
    const auto clip=document["tracks"].toObject()["children"].toArray()[0].toObject()["children"].toArray()[0].toObject();
    const auto duration=clip["source_range"].toObject()["duration"].toObject();CHECK(duration["value"].toDouble()==48&&duration["rate"].toDouble()==30);
    const auto bytes=QJsonDocument(document).toJson();model.clear();model.addPath(QDir::current().filePath("missing.mp4"));
    CHECK(waitUntil([&]{return model.findChildren<JobHandle*>().isEmpty();}));CHECK(!model.shotAt(0).isMetadataReady());CHECK(!exporter.exportToFile(destination,&model));
    CHECK(file.open(QIODevice::ReadOnly));CHECK(QJsonDocument::fromJson(file.readAll()).toJson()==bytes);file.close();
    PlaylistModel known;ShotItem shot;shot.path=path(1);shot.name="otio-input";shot.fps=25;shot.lastFrame=99;known.addShot(shot);
    CHECK(known.shotAt(0).isMetadataReady());CHECK(exporter.exportToFile(destination,&known));return true;
}
int main(int argc,char**argv){
    QApplication app(argc,argv);app.setProperty("cgplay.automationBackground",true);QThreadPool::globalInstance()->setMaxThreadCount(4);
    const int fontId=QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    const auto families=QFontDatabase::applicationFontFamilies(fontId);if(!families.isEmpty())app.setFont(QFont(families.first(),10));
    for(int i=0;i<12;++i){QFile f(path(i));f.open(QIODevice::WriteOnly);f.write("fixture");}QFile::remove("probe_calls.txt");
    const QString scenario=argc>1?QString::fromLocal8Bit(argv[1]):QString();
    const bool ok=scenario=="drop"?drop():scenario=="remove-sort"?removedAndSorted():scenario=="clear-readd"?clearReadd():scenario=="restore"?restore():scenario=="queue"?queue():scenario=="queued-completion"?queuedCompletion():scenario=="destroy"?destroyed():scenario=="pending-persistence"?pendingPersistence():scenario=="confirmed-metadata"?confirmedMetadataWins():scenario=="export-guard"&&exportGuard();
    QThreadPool::globalInstance()->waitForDone();int visible=0;EnumWindows([](HWND h,LPARAM p)->BOOL{DWORD pid=0;GetWindowThreadProcessId(h,&pid);if(pid==GetCurrentProcessId()&&IsWindowVisible(h))++*reinterpret_cast<int*>(p);return TRUE;},reinterpret_cast<LPARAM>(&visible));
    report["scenario"]=scenario;report["passed"]=ok&&visible==0&&uiImages==0;report["background"]=qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen";report["visiblePlatformWindows"]=visible;report["thumbnailPeak"]=peak.load();report["thumbnailCanceled"]=canceled.load();report["uiImages"]=uiImages.load();
    std::puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData());return report["passed"].toBool()?0:1;
}
'''

PROBE = r'''
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <string>
int main(int argc,char**argv){const std::string p=argc>1?argv[argc-1]:"";{std::ofstream log("probe_calls.txt",std::ios::app);log<<p<<'\n';}
Sleep(1200);const auto slash=p.find_last_of("/\\");const int n=std::atoi(p.substr(slash+2).c_str());
std::printf("{\"streams\":[{\"index\":0,\"codec_type\":\"video\",\"width\":%d,\"height\":36,\"codec_name\":\"h264\",\"pix_fmt\":\"yuv420p\",\"avg_frame_rate\":\"30/1\",\"r_frame_rate\":\"30/1\",\"nb_frames\":\"48\",\"duration\":\"1.6\"}],\"format\":{\"format_name\":\"mp4\"}}\n",64+n*16);return 0;}
'''


class PlaylistAsyncContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source, build = ARTIFACT / "source", ARTIFACT / "build"
        source.mkdir(parents=True, exist_ok=True)
        stubs = {
            "component/ComponentManager.h": '#pragma once\n#include <QString>\nnamespace cgplay { class ComponentManager { public: static ComponentManager& instance(){static ComponentManager c;return c;} QString componentExecutablePath(const QString&,const QString&) const{return qEnvironmentVariable("CGPLAY_AUDIT_FFPROBE");} }; }',
            "annotation/ReviewExport.h": '#pragma once\n#include <QString>\nnamespace cgplay { class ReviewExport { public: static QString locateFfmpeg(){return {};} }; }',
            "core/AsyncImageLoader.h": '#pragma once\n#include <QString>\n#include <vector>\nnamespace cgplay { class AsyncImageLoader { public: static bool probeEXR(const QString&,int&,int&,std::vector<QString>* = nullptr){return false;} }; }',
        }
        for relative, content in stubs.items():
            target = source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="utf-8")
        (source / "runner.cpp").write_text(RUNNER, encoding="utf-8")
        (source / "probe.cpp").write_text(PROBE, encoding="utf-8")
        paths = ["src/features/playlist/PlaylistModel.cpp", "src/features/playlist/PlaylistModel.h",
                 "src/common/jobs/JobSystem.cpp", "src/common/jobs/JobSystem.h", "src/services/media/MediaProbe.cpp",
                 "src/features/otio/OtioExporter.cpp"]
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.21)\nproject(PlaylistAsyncTest LANGUAGES CXX)\n"
            "set(CMAKE_CXX_STANDARD 17)\nset(CMAKE_AUTOMOC ON)\nfind_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)\n"
            "find_package(OpenTimelineIO REQUIRED)\n"
            "add_executable(delayed_ffprobe probe.cpp)\nadd_executable(playlist_async_runner runner.cpp\n" +
            "\n".join(f'"{(ROOT / p).as_posix()}"' for p in paths) + ")\n" +
            f'target_include_directories(playlist_async_runner PRIVATE "{source.as_posix()}" "{ROOT.as_posix()}/src" "{ROOT.as_posix()}/src/features" "{ROOT.as_posix()}/src/services" "{ROOT.as_posix()}/src/common")\n' +
            "target_compile_options(playlist_async_runner PRIVATE /utf-8)\n"
            "target_link_libraries(playlist_async_runner PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent OTIO::opentimelineio user32)\n", encoding="utf-8")
        for name, command in (
            ("configure", ["cmake", "-S", str(source), "-B", str(build), "-G", "Visual Studio 17 2022", "-A", "x64", f"-DCMAKE_PREFIX_PATH={QT.as_posix()};{TL.as_posix()}"]),
            ("build", ["cmake", "--build", str(build), "--config", "Release", "--parallel", "2"]),
        ):
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
                                    errors="replace", creationflags=NO_WINDOW, timeout=180)
            (ARTIFACT / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode:
                raise AssertionError(f"{name} failed:\n{result.stdout[-5000:]}\n{result.stderr[-1000:]}")
        cls.runner = build / "Release/playlist_async_runner.exe"
        cls.probe = build / "Release/delayed_ffprobe.exe"

    def scenario(self, name):
        work = ARTIFACT / name
        work.mkdir(exist_ok=True)
        env = os.environ.copy()
        env["PATH"] = os.pathsep.join([str(QT / "bin"), str(TL / "bin"), env.get("PATH", "")])
        env["QT_QPA_PLATFORM"] = "offscreen"
        env["CGPLAY_AUDIT_FFPROBE"] = str(self.probe)
        result = subprocess.run([str(self.runner), name], cwd=work, env=env, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", creationflags=NO_WINDOW, timeout=30)
        (work / "output.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(result.stdout.strip(), result.stderr)
        data = json.loads(result.stdout.strip().splitlines()[-1])
        (work / "evidence.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        self.assertEqual(0, result.returncode, data)
        self.assertTrue(data["passed"], data)
        self.assertTrue(data["background"], data)
        self.assertEqual(0, data["visiblePlatformWindows"], data)
        self.assertLessEqual(data["thumbnailPeak"], 2, data)

    def test_slow_drop_remains_responsive_and_deduplicates_probe(self): self.scenario("drop")
    def test_remove_current_and_sort_never_resurrect_or_misapply_rows(self): self.scenario("remove-sort")
    def test_clear_and_readd_same_path_discards_canceled_metadata(self): self.scenario("clear-readd")
    def test_restore_returns_saved_rows_then_refreshes_metadata(self): self.scenario("restore")
    def test_thumbnail_queue_leaves_workers_available_and_caps_rebuilds(self): self.scenario("queue")
    def test_removed_row_rejects_already_queued_thumbnail_success(self): self.scenario("queued-completion")
    def test_destruction_cancels_pending_metadata(self): self.scenario("destroy")
    def test_pending_session_preserves_unknown_and_explicit_fps(self): self.scenario("pending-persistence")
    def test_confirmed_metadata_and_fps_reject_late_probe(self): self.scenario("confirmed-metadata")
    def test_otio_never_writes_pending_or_failed_placeholder_rows(self): self.scenario("export-guard")


if __name__ == "__main__":
    unittest.main(verbosity=2)
