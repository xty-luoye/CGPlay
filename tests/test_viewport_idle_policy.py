"""Real TlViewport hidden/paused cadence with a separate native playback clock.

The windows QPA backend is retained for real OpenGL, while WA_DontShowOnScreen
keeps the probe off the desktop. Every phase samples native visible windows.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT
ART = ROOT / "tests/artifacts/migration_1_0_8_20261025/performance_validation/viewport"
QT = Path(os.environ.get("CGPLAY_TEST_QT_ROOT", "E:/rjkf/CGPlay-deps/QtClean/6.5.3/msvc2019_64"))
BASELINE = False
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

RUNNER = r'''
#include "ui/viewer/TlViewport.h"
#include <tlRender/Timeline/Init.h>
#include <tlRender/Timeline/Timeline.h>
#include <tlRender/Timeline/Player.h>
#include <tlRender/Timeline/System.h>
#include <ftk/Core/Context.h>
#include <ftk/UI/Style.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSurfaceFormat>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace cgplay;
static int maxVisible = 0;
static std::vector<double> heartbeatSamples;
static int visibleWindows() {
    int count = 0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window, LPARAM out) -> BOOL {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(out);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&count));
#endif
    maxVisible = std::max(maxVisible, count); return count;
}
static double processCpuMs() {
#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER k, u; k.LowPart=kernel.dwLowDateTime; k.HighPart=kernel.dwHighDateTime;
        u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;
        return static_cast<double>(k.QuadPart+u.QuadPart)/10000.0;
    }
#endif
    return 0;
}
static void pump(int milliseconds) {
    QElapsedTimer elapsed; elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        visibleWindows(); QThread::msleep(1);
    }
}
static bool waitUntil(const std::function<bool()>& ready, int timeout=2000) {
    QElapsedTimer elapsed;elapsed.start();
    while (!ready() && elapsed.elapsed()<timeout) pump(1);
    return ready();
}
static QJsonObject phase(TlViewport& viewport, const std::shared_ptr<tl::Player>& player, int milliseconds) {
    viewport.resetRenderStats();
    const double beforeCpu=processCpuMs();
    const double beforeTime=player->getCurrentTime().to_seconds();
    const auto heartbeatStart=heartbeatSamples.size();
    QElapsedTimer elapsed; elapsed.start();
    pump(milliseconds);
    QJsonObject result=viewport.benchmarkDiagnostics();
    result["durationMs"]=elapsed.elapsed();
    result["cpuMs"]=processCpuMs()-beforeCpu;
    result["mediaDeltaSeconds"]=player->getCurrentTime().to_seconds()-beforeTime;
    result["logicalVisibleObserved"]=viewport.isVisible();
    result["nativeVisibleWindows"]=visibleWindows();
    result["renderFrames"]=viewport.renderFrameCount();
    result["openGLValid"]=viewport.isValid();
    std::vector<double> phaseBeats(heartbeatSamples.begin()+heartbeatStart,heartbeatSamples.end());
    std::sort(phaseBeats.begin(),phaseBeats.end());
    result["heartbeatMaxMs"]=phaseBeats.empty()?0.0:phaseBeats.back();
    result["heartbeatP95Ms"]=phaseBeats.empty()?0.0:phaseBeats[static_cast<size_t>(.95*(phaseBeats.size()-1))];
    return result;
}
static int currentVideoFrame(const std::shared_ptr<tl::Player>& player) {
    const auto& video=player->getCurrentVideo();
    if (video.empty() || video.front().layers.empty() || !video.front().layers.front().image) return -1;
    return static_cast<int>(std::llround(video.front().time.value_rescaled_to(24.0)));
}
static bool hasVariation(const QImage& image) {
    if (image.isNull()) return false;
    const QRgb first=image.pixel(0,0)&0x00ffffff;
    for(int y=0;y<image.height();y+=std::max(1,image.height()/20))
        for(int x=0;x<image.width();x+=std::max(1,image.width()/20))
            if((image.pixel(x,y)&0x00ffffff)!=first) return true;
    return false;
}
static QJsonObject capture(QWidget& host,TlViewport& viewport,const QString& label) {
    QJsonObject result{{"backgroundOnly",true},{"desktopPresentationValidated",false}};
    QImage native;
#ifdef Q_OS_WIN
    result["PrintWindowAttempted"]=true;
    const HWND hwnd=reinterpret_cast<HWND>(host.winId());
    RECT rect{};GetClientRect(hwnd,&rect);
    HDC dc=GetDC(hwnd),memory=CreateCompatibleDC(dc);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=rect.right;info.bmiHeader.biHeight=-rect.bottom;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    BOOL printed=FALSE;
    if(bitmap&&bits) {
        const auto previous=SelectObject(memory,bitmap);
        printed=PrintWindow(hwnd,memory,3); // PW_CLIENTONLY | PW_RENDERFULLCONTENT
        if(printed)native=QImage(static_cast<uchar*>(bits),rect.right,rect.bottom,QImage::Format_RGB32).copy();
        SelectObject(memory,previous);DeleteObject(bitmap);
    }
    DeleteDC(memory);ReleaseDC(hwnd,dc);
    result["PrintWindowSucceeded"]=static_cast<bool>(printed);
    result["PrintWindowHasVariation"]=hasVariation(native);
    if(!native.isNull()) {const QString path=label+"_printwindow.png";result["PrintWindowPath"]=path;result["PrintWindowSaved"]=native.save(path);}
#endif
    if(native.isNull()||!hasVariation(native)) {
        const QImage framebuffer=viewport.grabFramebuffer();
        const QString path=label+"_framebuffer.png";
        result["method"]="grabFramebuffer";result["path"]=path;
        result["saved"]=!framebuffer.isNull()&&framebuffer.save(path);
        result["hasVariation"]=hasVariation(framebuffer);
        result["fallbackReason"]="Hidden native PrintWindow failed or returned a uniform image; this is an offscreen OpenGL image only.";
    } else {result["method"]="PrintWindow";result["path"]=label+"_printwindow.png";result["saved"]=true;result["hasVariation"]=true;}
    visibleWindows();return result;
}
int main(int argc,char**argv) {
    QSurfaceFormat format;format.setVersion(4,1);format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);format.setStencilBufferSize(8);format.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(format);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc,argv);
    app.setProperty("cgplay.automationBackground",true);
    const bool baseline=app.arguments().contains("--baseline");
    QJsonObject report;
    QElapsedTimer heartbeatClock;heartbeatClock.start();
    QTimer heartbeat;heartbeat.setInterval(10);
    QObject::connect(&heartbeat,&QTimer::timeout,[&]{heartbeatSamples.push_back(heartbeatClock.nsecsElapsed()/1e6);heartbeatClock.restart();visibleWindows();});
    heartbeat.start();
    auto context=ftk::Context::create();tl::init(context);
    auto style=ftk::Style::create(context);
    QWidget host;host.setAttribute(Qt::WA_DontShowOnScreen);host.setAttribute(Qt::WA_ShowWithoutActivating);
    host.resize(480,270);
    auto* layout=new QVBoxLayout(&host);layout->setContentsMargins(0,0,0,0);
    auto* viewport=new TlViewport(context,style,&host);layout->addWidget(viewport);
    auto timeline=tl::Timeline::create(context,ftk::Path(app.arguments().at(1).toStdString()));
    auto player=tl::Player::create(context,timeline);
    player->setMute(true);player->setLoop(tl::Loop::Loop);
    viewport->setPlayer(player);
    // This is deliberately independent of the display timer under test.
    int clockTicks=0;QTimer systemTick;systemTick.setTimerType(Qt::PreciseTimer);systemTick.setInterval(5);
    QObject::connect(&systemTick,&QTimer::timeout,[&]{context->getSystem<tl::System>()->tick();++clockTicks;});systemTick.start();
    host.show();pump(600);
    const bool firstFrame=waitUntil([&]{return currentVideoFrame(player)>=0;});
    report["firstFrameReady"]=firstFrame;
    if(app.arguments().contains("--multi-window")) {
        bool passed=firstFrame;
        for(int count:{2,4}) {
            std::vector<std::unique_ptr<QWidget>> extraHosts;
            std::vector<TlViewport*> displays{viewport};
            for(int i=1;i<count;++i) {
                auto extra=std::make_unique<QWidget>();
                extra->setAttribute(Qt::WA_DontShowOnScreen);extra->setAttribute(Qt::WA_ShowWithoutActivating);extra->resize(480,270);
                auto* box=new QVBoxLayout(extra.get());box->setContentsMargins(0,0,0,0);
                auto* display=new TlViewport(context,style,extra.get());box->addWidget(display);display->setPlayer(player);
                displays.push_back(display);extra->show();extraHosts.push_back(std::move(extra));
            }
            host.show();pump(600);
            auto measure=[&] {
                for(auto* display:displays) display->resetRenderStats();
                const double before=processCpuMs();pump(1200);QJsonArray timers;
                for(auto* display:displays) {timers.append(display->benchmarkDiagnostics()["timer_tick_count"]);passed=passed&&display->isValid();}
                return QJsonObject{{"cpuMs",processCpuMs()-before},{"timerTicks",timers},{"durationMs",1200}};
            };
            QJsonObject result;result["visiblePaused"]=measure();
            host.hide();for(auto& extra:extraHosts) extra->hide();pump(350);
            result["hiddenPaused"]=measure();
            if(!baseline) {
                for(const auto& ticks:result["visiblePaused"].toObject()["timerTicks"].toArray())passed=passed&&ticks.toDouble()<=18;
                for(const auto& ticks:result["hiddenPaused"].toObject()["timerTicks"].toArray())passed=passed&&ticks.toDouble()<=4;
            }
            report[QString::number(count)+"Windows"]=result;
            for(size_t i=1;i<displays.size();++i)displays[i]->clearPlayer();
            extraHosts.clear();pump(50);
        }
        viewport->clearPlayer();player.reset();timeline.reset();systemTick.stop();heartbeat.stop();
        report["scope"]="Independent native OpenGL viewports share a paused player to isolate display-only multiwindow overhead; not full-player or desktop presentation measurements.";
        report["backgroundMode"]=true;report["visiblePlatformWindowsMax"]=maxVisible;report["baseline"]=baseline;report["passed"]=passed&&maxVisible==0;
        std::puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData());return passed&&maxVisible==0?0:1;
    }
    report["pausedVisible"]=phase(*viewport,player,1200);
    player->setPlayback(tl::Playback::Forward);pump(350);
    report["playingVisible"]=phase(*viewport,player,1200);
    host.hide();pump(350);
    report["playingHidden"]=phase(*viewport,player,1600);
    host.showMinimized();pump(350);
    report["playingMinimized"]=phase(*viewport,player,1200);
    viewport->resetRenderStats();QElapsedTimer restored;restored.start();host.showNormal();
    const bool restorePaint=waitUntil([&]{return viewport->renderFrameCount()>0;},300);
    report["restorePaintReady"]=restorePaint;report["restorePaintMs"]=restored.elapsed();
    report["restoreDiagnostics"]=viewport->benchmarkDiagnostics();
    report["restoreCapture"]=capture(host,*viewport,"restore");
    player->setPlayback(tl::Playback::Stop);pump(500);
    // A far-away seek waits for the real asynchronous image, then checks the
    // paused display wake instead of waking merely when the command is issued.
    viewport->resetRenderStats();QElapsedTimer seek;seek.start();player->seek(OTIO_NS::RationalTime(12,24));
    const bool seekReady=waitUntil([&]{return currentVideoFrame(player)==12;});
    const qint64 decodeReadyMs=seek.elapsed();
    const bool seekPaint=waitUntil([&]{return viewport->renderFrameCount()>0;},300);
    report["seekFrameReady"]=seekReady;report["seekPaintReady"]=seekPaint;
    report["seekDecodeMs"]=decodeReadyMs;report["seekPaintMs"]=seek.elapsed();
    report["seekDiagnostics"]=viewport->benchmarkDiagnostics();
    report["seekCapture"]=capture(host,*viewport,"seek");
    pump(500);
    const auto beforeSettingsTick=viewport->benchmarkDiagnostics()["timer_tick_count"].toDouble();
    waitUntil([&]{return viewport->benchmarkDiagnostics()["timer_tick_count"].toDouble()>beforeSettingsTick;},300);
    viewport->resetRenderStats();QElapsedTimer settingsWake;settingsWake.start();
    tl::DisplayOptions display;display.color.enabled=true;display.color.brightness=ftk::V3F(.5F,.5F,.5F);
    viewport->setDisplayOptions({display,display});
    report["settingsPaintReady"]=waitUntil([&]{return viewport->renderFrameCount()>0;},300);
    report["settingsPaintMs"]=settingsWake.elapsed();
    report["settingsDiagnostics"]=viewport->benchmarkDiagnostics();
    // A stop/clear/queued restore must not leave an observer referencing player.
    host.hide();player->seek(OTIO_NS::RationalTime(24,24));
    viewport->clearPlayer();player.reset();timeline.reset();host.show();pump(100);
    systemTick.stop();heartbeat.stop();
    std::sort(heartbeatSamples.begin(),heartbeatSamples.end());
    auto percentile=[&](double p){return heartbeatSamples.empty()?0.0:heartbeatSamples[std::min(heartbeatSamples.size()-1,static_cast<size_t>(p*(heartbeatSamples.size()-1)))];};
    report["heartbeatMaxMs"]=heartbeatSamples.empty()?0.0:heartbeatSamples.back();report["heartbeatP95Ms"]=percentile(.95);
    report["clockTicks"]=clockTicks;report["backgroundMode"]=app.property("cgplay.automationBackground").toBool();
    report["visiblePlatformWindowsMax"]=maxVisible;report["baseline"]=baseline;
    report["swapInterval"]=format.swapInterval();
    report["scope"]="Real TlViewport/native tlRender; platform-hidden OpenGL widget. Internal timer/render data are not desktop presentation measurements.";
    const auto paused=report["pausedVisible"].toObject(),hidden=report["playingHidden"].toObject(),minimized=report["playingMinimized"].toObject();
    bool ok=firstFrame && maxVisible==0 && hidden["mediaDeltaSeconds"].toDouble()>.8 && seekReady;
    if (!baseline) {
        ok=ok && paused["timer_tick_count"].toDouble()<=18 && hidden["timer_tick_count"].toDouble()<=4;
        ok=ok && paused["current_timer_interval_ms"].toInt()>=80 && hidden["current_timer_interval_ms"].toInt()>=250;
        ok=ok && minimized["timer_tick_count"].toDouble()<=4 && minimized["mediaDeltaSeconds"].toDouble()>.8;
        ok=ok && restorePaint && seekPaint && report["restorePaintMs"].toDouble()<150;
        ok=ok && report["seekDiagnostics"].toObject()["paused_frame_wake_count"].toDouble()>=1;
        ok=ok && report["settingsPaintReady"].toBool() && report["settingsPaintMs"].toDouble()<75;
        ok=ok && report["restoreCapture"].toObject()["saved"].toBool() && report["seekCapture"].toObject()["saved"].toBool();
    }
    report["passed"]=ok;
    std::puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
    return ok?0:1;
}
'''


def _sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _native_link_libraries():
    """Use exactly the checked-in build's existing Release link closure."""
    project = ROOT / "build_win_full/src/CGPlay.vcxproj"
    namespace = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
    tree = ET.parse(project)
    group = next(node for node in tree.findall("m:ItemDefinitionGroup", namespace)
                 if "Release|x64" in node.attrib.get("Condition", ""))
    raw = group.find("m:Link/m:AdditionalDependencies", namespace).text
    result = []
    for value in raw.split(";"):
        if not value or value.startswith("%(") or "Qt6" in value or "CGPlayTheme" in value:
            continue
        if "\\" in value or "/" in value:
            path = Path(value)
            if not path.is_absolute():
                path = project.parent / path
            result.append(str(path.resolve()))
        else:
            result.append(value)
    return result


class ViewportIdlePolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source, build = ART / "source", ART / "build"
        source.mkdir(parents=True, exist_ok=True)
        (source / "runner.cpp").write_text(RUNNER, encoding="utf-8")
        paths = [SOURCE / "src/ui/viewer/TlViewport.cpp", SOURCE / "src/ui/viewer/TlViewport.h",
                 SOURCE / "src/common/core/GPUUploadQueue.cpp", SOURCE / "src/common/core/GPUUploadQueue.h"]
        upstream = Path("E:/rj/CGV/tlRender-main")
        native = upstream / "install-Release"
        includes = [SOURCE / "src", SOURCE / "src/common", upstream / "lib", native / "include",
                    Path("E:/rjkf/CGPlay-deps/CGV_install/include"), native / "include/Imath",
                    native / "include/SDL2", native / "include/opentimelineio", native / "include/opentime"]
        libraries = _native_link_libraries()
        cmake = (
            "cmake_minimum_required(VERSION 3.21)\nproject(ViewportPolicyProbe LANGUAGES CXX)\n"
            "set(CMAKE_CXX_STANDARD 17)\nset(CMAKE_AUTOMOC ON)\n"
            "find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Concurrent Network)\n"
            "add_executable(viewport_policy_probe runner.cpp\n" +
            "\n".join(f'"{path.as_posix()}"' for path in paths) + ")\n"
            "target_include_directories(viewport_policy_probe PRIVATE " +
            " ".join(f'"{path.as_posix()}"' for path in includes) + ")\n"
            "target_compile_options(viewport_policy_probe PRIVATE /utf-8 /bigobj)\n"
            "target_compile_definitions(viewport_policy_probe PRIVATE TL_STATIC FTK_STATIC OTIO_STATIC OPENTIME_STATIC "
            "CGPLAY_HAS_TLRENDER=1 CGPLAY_HAS_FTK=1 NOMINMAX FTK_API_GL_4_1 OpenColorIO_SKIP_IMPORTS FTK_SDL2 FTK_NFD)\n"
            "target_link_libraries(viewport_policy_probe PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL "
            "Qt6::OpenGLWidgets Qt6::Concurrent Qt6::Network " +
            " ".join(f'"{Path(value).as_posix()}"' for value in libraries) + ")\n"
        )
        (source / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
        for name, command in (
            ("configure", ["cmake", "-S", str(source), "-B", str(build), "-G", "Visual Studio 17 2022", "-A", "x64", f"-DCMAKE_PREFIX_PATH={QT.as_posix()}"]),
            ("build", ["cmake", "--build", str(build), "--config", "Release", "--parallel", "2"]),
        ):
            process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
                                     errors="replace", creationflags=NO_WINDOW, timeout=240)
            (ART / f"{name}.log").write_text(process.stdout + process.stderr, encoding="utf-8")
            if process.returncode:
                raise AssertionError(f"{name}:\n{process.stdout[-6000:]}\n{process.stderr[-2500:]}")
        cls.runner = build / "Release/viewport_policy_probe.exe"
        cls.source_hashes = {str(path): _sha(path) for path in paths}
        cls.fixture = ART / "fixture.mp4"
        ffmpeg = ROOT / "build_win_full/bin/Release/ffmpeg.exe"
        process = subprocess.run([str(ffmpeg), "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                                  "testsrc2=size=160x90:rate=24", "-t", "8", "-c:v", "libx264", "-pix_fmt", "yuv420p",
                                  "-g", "24", "-y", str(cls.fixture)], cwd=ROOT, capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", creationflags=NO_WINDOW, timeout=30)
        if process.returncode:
            raise AssertionError(process.stderr)

    def test_real_native_hidden_paused_restore_and_frame_wake(self):
        env = dict(os.environ)
        env["QT_QPA_PLATFORM"] = "windows"
        env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(QT / "plugins/platforms")
        env["PATH"] = os.pathsep.join([str(QT / "bin"), str(ROOT / "build_win_full/bin/Release"),
                                       "E:/rj/CGV/tlRender-main/install-Release/bin", env.get("PATH", "")])
        command = [str(self.runner), str(self.fixture)] + (["--baseline"] if BASELINE else [])
        process = subprocess.run(command, cwd=ART, env=env, capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", creationflags=NO_WINDOW, timeout=30)
        (ART / "run.log").write_text(process.stdout + process.stderr, encoding="utf-8")
        lines = [line for line in process.stdout.splitlines() if line.startswith('{"')]
        self.assertTrue(lines, process.stdout[-3000:] + process.stderr[-3000:])
        result = json.loads(lines[-1])
        result.update(sourceSha256=self.source_hashes, exeSha256=_sha(self.runner), fixtureSha256=_sha(self.fixture))
        (ART / "report.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        self.assertEqual(0, process.returncode, result)
        self.assertTrue(result["passed"], result)
        self.assertTrue(result["backgroundMode"], result)
        self.assertEqual(0, result["visiblePlatformWindowsMax"], result)

    def test_two_and_four_native_viewports_idle_cpu_and_polling(self):
        env = dict(os.environ, QT_QPA_PLATFORM="windows", QT_QPA_PLATFORM_PLUGIN_PATH=str(QT / "plugins/platforms"))
        env["PATH"] = os.pathsep.join([str(QT / "bin"), str(ROOT / "build_win_full/bin/Release"),
                                       "E:/rj/CGV/tlRender-main/install-Release/bin", env.get("PATH", "")])
        command = [str(self.runner), str(self.fixture), "--multi-window"] + (["--baseline"] if BASELINE else [])
        process = subprocess.run(command, cwd=ART, env=env, capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", creationflags=NO_WINDOW, timeout=30)
        (ART / "multiwindow.log").write_text(process.stdout + process.stderr, encoding="utf-8")
        lines = [line for line in process.stdout.splitlines() if line.startswith('{"')]
        self.assertTrue(lines, process.stdout[-3000:] + process.stderr[-3000:])
        result = json.loads(lines[-1])
        result.update(sourceSha256=self.source_hashes, exeSha256=_sha(self.runner), fixtureSha256=_sha(self.fixture))
        (ART / "multiwindow.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        self.assertEqual(0, process.returncode, result)
        self.assertTrue(result["passed"], result)
        self.assertEqual(0, result["visiblePlatformWindowsMax"], result)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--baseline-root", type=Path)
    parser.add_argument("--artifact-root", type=Path)
    args, remaining = parser.parse_known_args()
    if args.baseline_root:
        SOURCE = args.baseline_root.resolve()
        BASELINE = True
        ART = ART / "baseline"
    if args.artifact_root:
        ART = args.artifact_root.resolve()
    unittest.main(argv=[__file__, *remaining], verbosity=2)
