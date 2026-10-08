"""Compile real MediaProbe/JobSystem; deterministic subprocess and real video checks."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT
ART = ROOT / "tests/artifacts/player_speed_settings_20260912/media_probe"
QT = Path("C:/QtClean/6.5.3/msvc2019_64")
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
BASELINE = False

PROBE = r'''
#include <QCoreApplication>
#include <QFile>
#include <QThread>
#include <cstdio>
int main(int argc, char** argv) {
 QCoreApplication app(argc, argv);
 QFile count(qEnvironmentVariable("TEST_PROBE_COUNT")); count.open(QIODevice::Append); count.write("1\n"); count.close();
 QFile file(app.arguments().last()); file.open(QIODevice::ReadOnly); const auto data=file.readAll();
 if(data.startsWith('S')) QThread::msleep(200);
 if(data.startsWith('!')) return 2;
 const int width=data.startsWith('B')?640:320;
 printf("{\"format\":{\"format_name\":\"mov,mp4\",\"bit_rate\":\"1234567\"},\"streams\":[{\"codec_type\":\"video\",\"codec_name\":\"h264\",\"pix_fmt\":\"yuv420p\",\"width\":%d,\"height\":180,\"avg_frame_rate\":\"24/1\",\"nb_frames\":\"48\",\"duration\":\"2.0\",\"bit_rate\":\"1200000\"},{\"codec_type\":\"audio\"},{\"codec_type\":\"subtitle\"}]}\n",width);
}
'''
RUNNER = r'''
#include "media/MediaProbe.h"
#include "common/jobs/JobSystem.h"
#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <future>
#include <vector>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace cgplay;
QJsonObject report;
#define CHECK(x) do {if(!(x)){report["failure"]=#x;report["line"]=__LINE__;return false;}}while(false)
static void write(const QString& path, const QByteArray& data) {QFile f(path);f.open(QIODevice::WriteOnly);f.write(data);}
static int calls(){QFile f(qEnvironmentVariable("TEST_PROBE_COUNT"));f.open(QIODevice::ReadOnly);return f.readAll().count('\n');}
static bool run(const QString& scenario, bool baseline) {
 const QString path="fixture.mp4";write(path,"A");
 if(scenario=="repeat" || scenario=="real"){
  const QString media=scenario=="real"?qEnvironmentVariable("TEST_REAL_MEDIA"):path;
  QElapsedTimer timer;timer.start();const auto a=MediaProbe::probe(media);const auto cold=timer.nsecsElapsed();
  timer.restart();const auto b=MediaProbe::probe(media);const auto warm=timer.nsecsElapsed();
  report["coldMs"]=cold/1e6;report["warmMs"]=warm/1e6;
  CHECK(a.error.isEmpty()&&a.width>0&&a.width==b.width&&a.fps==b.fps&&a.codecName==b.codecName);
  CHECK(b.isVideo&&!b.isStillImage&&b.containerFormat=="mp4");
  if(scenario=="repeat")CHECK(b.bitrateBitsPerSecond==1234567&&b.bitrateText()=="1.23 Mbps"&&b.bitrateText("mbps")=="1.23 Mbps"&&b.bitrateText("kbps")=="1235 kbps");
  if(scenario=="real")CHECK(b.bitrateBitsPerSecond>0&&b.bitrateText()!="--");
  if(scenario=="repeat")CHECK(calls()==(baseline?2:1));
 }else if(scenario=="invalidation"){
  CHECK(MediaProbe::probe(path).width==320);write(path,"BBBB");CHECK(MediaProbe::probe(path).width==640);
  write(path,"AAAA");QFile f(path);CHECK(f.open(QIODevice::ReadWrite));
  CHECK(f.setFileTime(QDateTime::currentDateTimeUtc().addSecs(2),QFileDevice::FileModificationTime));f.close();
  CHECK(MediaProbe::probe(path).width==320);CHECK(calls()==3);
  CHECK(QFile::remove(path));CHECK(!MediaProbe::probe(path).exists);CHECK(calls()==3);
 }else if(scenario=="sidecars"){
  CHECK(!MediaProbe::probe(path).hasExternalSubtitles);write("fixture.en.srt","subtitle");
  const auto with=MediaProbe::probe(path);CHECK(with.hasExternalSubtitles&&with.externalSubtitlePaths.size()==1);
  CHECK(QFile::remove("fixture.en.srt"));CHECK(!MediaProbe::probe(path).hasExternalSubtitles);
  write("fixture.source.srt","generated");CHECK(!MediaProbe::probe(path).hasExternalSubtitles);CHECK(calls()==1);
 }else if(scenario=="fps"){
  const auto a=MediaProbe::probe(path,30);const auto b=MediaProbe::probe(path);const auto c=MediaProbe::probe(path,60);
  CHECK(a.fps==30&&b.fps==24&&c.fps==60&&b.audioStreamCount==1&&b.subtitleStreamCount==1);CHECK(calls()==1);
 }else if(scenario=="failure"){
  write(path,"!");CHECK(!MediaProbe::probe(path).hasRealDimensions);CHECK(!MediaProbe::probe(path).hasRealDimensions);CHECK(calls()==2);
  write(path,"AA");CHECK(MediaProbe::probe(path).width==320);CHECK(calls()==3);
 }else if(scenario=="cancel"){
  JobContext canceled;canceled.cancel();CHECK(!MediaProbe::probe(path,0,&canceled).error.isEmpty());CHECK(calls()==0);
  CHECK(MediaProbe::probe(path).width==320);CHECK(calls()==1);
  JobContext expired(1);QThread::msleep(5);CHECK(!MediaProbe::probe(path,0,&expired).error.isEmpty());CHECK(calls()==1);
  write(path,"S");JobContext pending;std::thread worker([&]{QThread::msleep(30);pending.cancel();});
  const auto failed=MediaProbe::probe(path,0,&pending);worker.join();CHECK(!failed.error.isEmpty());
  const int before=calls();CHECK(MediaProbe::probe(path).width==320);CHECK(calls()==before+1);
 }else if(scenario=="concurrent"){
  std::vector<std::future<MediaInfo>> tasks;for(int i=0;i<8;++i)tasks.push_back(std::async(std::launch::async,[&]{return MediaProbe::probe(path);}));
  for(auto& task:tasks){const auto info=task.get();CHECK(info.error.isEmpty()&&info.width==320);}
  const int before=calls();CHECK(MediaProbe::probe(path).width==320);CHECK(calls()==before);CHECK(before>=1&&before<=8);
 }else if(scenario=="bounded"){
  for(int i=0;i<140;++i){const auto p=QString("clip%1.mp4").arg(i);write(p,"A");CHECK(MediaProbe::probe(p).width==320);}
  CHECK(calls()==140);CHECK(MediaProbe::probe("clip0.mp4").width==320);CHECK(calls()==141);
 }else if(scenario=="still"){
  QImage image(7,9,QImage::Format_RGB32);image.fill(Qt::red);CHECK(image.save("still.png"));
  const auto a=MediaProbe::probe("still.png");CHECK(a.isStillImage&&!a.isVideo&&a.width==7&&a.height==9);
  QImage changed(11,12,QImage::Format_RGB32);changed.fill(Qt::green);CHECK(changed.save("still.png"));
  CHECK(MediaProbe::probe("still.png").width==11);CHECK(calls()==0);
 }else if(scenario=="replaced-during-probe"){
  write(path,"S");std::thread writer([&]{QThread::msleep(90);write(path,"BBBB");});
  const auto original=MediaProbe::probe(path);writer.join();CHECK(original.width==320);
  CHECK(MediaProbe::probe(path).width==640);CHECK(calls()==2);
 }else return false;
 return true;
}
int main(int argc,char** argv){
 QApplication app(argc,argv);app.setProperty("cgplay.automationBackground",true);
 const bool passed=run(app.arguments().value(1),app.arguments().contains("baseline"));int visible=0;
#ifdef Q_OS_WIN
 EnumWindows([](HWND w,LPARAM p)->BOOL{DWORD pid=0;GetWindowThreadProcessId(w,&pid);if(pid==GetCurrentProcessId()&&IsWindowVisible(w))++*reinterpret_cast<int*>(p);return TRUE;},reinterpret_cast<LPARAM>(&visible));
#endif
 report["passed"]=passed;report["background"]=true;report["visiblePlatformWindows"]=visible;report["probeCalls"]=calls();
 printf("%s\n",QJsonDocument(report).toJson(QJsonDocument::Compact).constData());return passed&&visible==0?0:1;
}
'''

class MediaProbeCacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source, build = ART / "source", ART / "build"
        source.mkdir(parents=True, exist_ok=True)
        headers = {
            "annotation/ReviewExport.h": '#pragma once\n#include <QString>\nnamespace cgplay { class ReviewExport { public: static QString locateFfmpeg(){return {};} }; }\n',
            "component/ComponentManager.h": '#pragma once\n#include <QString>\nnamespace cgplay { class ComponentManager { public: static ComponentManager& instance(){static ComponentManager x;return x;} QString componentExecutablePath(const QString&, const QString&) const {return qEnvironmentVariable("TEST_FFPROBE");} }; }\n',
            "core/AsyncImageLoader.h": '#pragma once\n#include <QString>\nnamespace cgplay {class AsyncImageLoader {public: static bool probeEXR(const QString&,int&,int&){return false;}};}\n',
        }
        for name, text in headers.items():
            file = source / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(text, encoding="utf-8")
        (source / "runner.cpp").write_text(RUNNER, encoding="utf-8")
        (source / "probe.cpp").write_text(PROBE, encoding="utf-8")
        paths = [SOURCE / "src/services/media/MediaProbe.cpp", ROOT / "src/common/jobs/JobSystem.cpp", ROOT / "src/common/jobs/JobSystem.h"]
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.21)\nproject(ProbeCacheTest LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 17)\nset(CMAKE_AUTOMOC ON)\nfind_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)\n'
            'add_executable(probe_fixture probe.cpp)\ntarget_link_libraries(probe_fixture PRIVATE Qt6::Core)\n'
            'add_executable(probe_runner runner.cpp\n' + '\n'.join(f'"{p.as_posix()}"' for p in paths) + ')\n'
            f'target_include_directories(probe_runner PRIVATE "{source.as_posix()}" "{ROOT.as_posix()}/src" "{ROOT.as_posix()}/src/common" "{ROOT.as_posix()}/src/services" "{ROOT.as_posix()}/src/services/media")\n'
            'target_compile_options(probe_runner PRIVATE /utf-8)\ntarget_link_libraries(probe_runner PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent user32)\n', encoding="utf-8")
        for name, command in [("configure", ["cmake", "-S", str(source), "-B", str(build), "-G", "Visual Studio 17 2022", "-A", "x64", f"-DCMAKE_PREFIX_PATH={QT}"]),
                              ("build", ["cmake", "--build", str(build), "--config", "Release", "--parallel", "2"])]:
            result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=NO_WINDOW, timeout=180)
            (ART / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode:
                raise AssertionError(result.stdout[-5000:] + result.stderr[-1500:])
        cls.runner = build / "Release/probe_runner.exe"
        cls.probe = build / "Release/probe_fixture.exe"

    def scenario(self, name):
        if BASELINE and name not in ("repeat", "real"):
            self.skipTest("Before/after timing only")
        cwd = ART / name
        cwd.mkdir(parents=True, exist_ok=True)
        # All disposable inputs are reset within this scenario's fixture directory.
        for file in cwd.glob("*"):
            if file.is_file() and file.suffix in (".mp4", ".srt", ".png", ".count"):
                file.unlink()
        env = os.environ.copy()
        env["PATH"] = str(QT / "bin") + os.pathsep + env.get("PATH", "")
        env["QT_QPA_PLATFORM"] = "offscreen"
        env["TEST_FFPROBE"] = str(ROOT / "build_win_full/runtime/ffmpeg/ffprobe.exe") if name == "real" else str(self.probe)
        env["TEST_PROBE_COUNT"] = str(cwd / "probe.count")
        env["TEST_REAL_MEDIA"] = str(ROOT / "tests/media/1080p_h264.mp4")
        command = [str(self.runner), name] + (["baseline"] if BASELINE else [])
        result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=NO_WINDOW, timeout=30)
        (cwd / "run.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        data = json.loads(result.stdout.strip().splitlines()[-1])
        (cwd / "result.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        self.assertEqual(0, result.returncode, data)
        self.assertTrue(data["passed"])
        self.assertTrue(data["background"])
        self.assertEqual(0, data["visiblePlatformWindows"])

    def test_repeat(self): self.scenario("repeat")
    def test_real_video(self): self.scenario("real")
    def test_file_invalidation(self): self.scenario("invalidation")
    def test_sidecars(self): self.scenario("sidecars")
    def test_fps_override(self): self.scenario("fps")
    def test_failed_probe_retry(self): self.scenario("failure")
    def test_cancellation(self): self.scenario("cancel")
    def test_concurrent_readers(self): self.scenario("concurrent")
    def test_capacity(self): self.scenario("bounded")
    def test_stills_not_cached(self): self.scenario("still")
    def test_replaced_during_probe(self): self.scenario("replaced-during-probe")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--baseline-root", type=Path)
    args, extra = parser.parse_known_args()
    if args.baseline_root:
        SOURCE = args.baseline_root.resolve()
        BASELINE = True
        ART /= "baseline"
    unittest.main(argv=[__file__, *extra], verbosity=2)
