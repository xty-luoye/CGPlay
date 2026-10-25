"""Compile real SystemPerformanceMonitor; background heartbeat/lifetime evidence."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = ROOT
ART = ROOT / "tests/artifacts/migration_1_0_8_20261025/performance_validation/monitor"
QT = Path(os.environ.get("CGPLAY_TEST_QT_ROOT", "E:/rjkf/CGPlay-deps/QtClean/6.5.3/msvc2019_64"))
BASELINE = False
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

RUNNER = r'''
#include "SystemPerformanceMonitor.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cstdio>
#include <vector>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif
using namespace cgplay;
static int visibleWindows() {
 int visible=0;
#ifdef Q_OS_WIN
 EnumWindows([](HWND w,LPARAM p)->BOOL {DWORD pid=0;GetWindowThreadProcessId(w,&pid);
  if(pid==GetCurrentProcessId()&&IsWindowVisible(w))++*reinterpret_cast<int*>(p);return TRUE;},reinterpret_cast<LPARAM>(&visible));
#endif
 return visible;
}
int main(int argc,char** argv) {
 QCoreApplication app(argc,argv); app.setProperty("cgplay.automationBackground",true);
 const QString scenario=argc>1?QString::fromLocal8Bit(argv[1]):QStringLiteral("native");
 QJsonObject report;std::vector<double> gaps;QElapsedTimer clock;clock.start();qint64 previous=clock.nsecsElapsed();
 QTimer heartbeat;heartbeat.setTimerType(Qt::PreciseTimer);heartbeat.setInterval(5);
 int samples=0,offGuiSignals=0,visible=0;double stopMs=-1,deleteMs=-1;bool stopped=false;int samplesAtStop=0;qint64 firstSampleMs=-1;
 auto* monitor=new SystemPerformanceMonitor;
 QObject::connect(monitor,&SystemPerformanceMonitor::sampleReady,&app,[&](double,double,double){
  ++samples;if(firstSampleMs<0)firstSampleMs=clock.elapsed();if(QThread::currentThread()!=app.thread())++offGuiSignals;
 });
 QObject::connect(&heartbeat,&QTimer::timeout,&app,[&]{const auto now=clock.nsecsElapsed();gaps.push_back((now-previous)/1e6);previous=now;visible=std::max(visible,visibleWindows());});heartbeat.start();
 monitor->start(250);
 if(scenario=="mailbox") QTimer::singleShot(900,&app,[]{QThread::msleep(900);});
 if(scenario=="stop"||scenario=="destroy"||scenario=="restart") {
  QTimer::singleShot(350,&app,[&]{QElapsedTimer elapsed;elapsed.start();monitor->stop();stopMs=elapsed.nsecsElapsed()/1e6;stopped=true;samplesAtStop=samples;
   if(scenario=="destroy"){elapsed.restart();delete monitor;monitor=nullptr;deleteMs=elapsed.nsecsElapsed()/1e6;}
   if(scenario=="restart") QTimer::singleShot(10,&app,[&]{stopped=false;monitor->start(250);});
  });
 }
 QTimer::singleShot(scenario=="drain-live"?350:2100,&app,[&]{
  heartbeat.stop();QJsonObject diagnostics;
#ifndef BASELINE_MONITOR
  if(monitor)diagnostics=monitor->diagnostics();
#endif
  QElapsedTimer drain;drain.start();
#ifndef BASELINE_MONITOR
  if(scenario=="drain-live") {
   SystemPerformanceMonitor::shutdownWorkers();
   monitor->start(250);
   report["workersAfterRestartDuringShutdown"]=SystemPerformanceMonitor::activeWorkerCount();
  }
#endif
  if(monitor){monitor->stop();delete monitor;monitor=nullptr;}
#ifndef BASELINE_MONITOR
  SystemPerformanceMonitor::shutdownWorkers();
  report["remainingWorkers"]=SystemPerformanceMonitor::activeWorkerCount();
#endif
  report["drainMs"]=drain.nsecsElapsed()/1e6;
  std::sort(gaps.begin(),gaps.end());
  auto percentile=[&](double p){return gaps.empty()?0.0:gaps[std::min(gaps.size()-1,static_cast<size_t>(p*(gaps.size()-1)))];};
  report["scenario"]=scenario;report["backgroundMode"]=true;report["visiblePlatformWindowsMax"]=visible;
  report["intentionalGuiStall"]=scenario=="mailbox";
  report["heartbeatCount"]=static_cast<int>(gaps.size());report["heartbeatMaxMs"]=gaps.empty()?0.0:gaps.back();
  report["heartbeatP95Ms"]=percentile(.95);report["heartbeatP99Ms"]=percentile(.99);
  report["samples"]=samples;report["firstSampleMs"]=static_cast<double>(firstSampleMs);report["signalsOffGui"]=offGuiSignals;report["stopMs"]=stopMs;report["deleteMs"]=deleteMs;
  report["samplesAfterStop"]=stopped?samples-samplesAtStop:0;report["diagnostics"]=diagnostics;
  std::puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData());std::fflush(stdout);app.quit();
 });
 return app.exec();
}
'''


class PerformanceMonitorRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.art = ART / ("baseline" if BASELINE else "current")
        source = cls.art / "source"
        build = cls.art / "build"
        source.mkdir(parents=True, exist_ok=True)
        original = SOURCE_ROOT / "src/common/core/SystemPerformanceMonitor.cpp"
        raw = original.read_text(encoding="utf-8")
        if BASELINE:
            anchor = "void SystemPerformanceMonitor::_sample()\n{"
        else:
            anchor = "SampleValues collectSample(SamplerCounters& counters)\n{"
        if raw.count(anchor) != 1:
            raise AssertionError(f"Controlled delay anchor missing: {anchor}")
        # This fixture-only instrumented copy keeps production free of test env
        # switches while exercising the real sampler and its actual thread.
        raw = raw.replace(anchor, anchor + '\n    if (qEnvironmentVariableIntValue("CGPLAY_TEST_PERF_DELAY_MS") > 0)\n        std::this_thread::sleep_for(std::chrono::milliseconds(qEnvironmentVariableIntValue("CGPLAY_TEST_PERF_DELAY_MS")));', 1)
        raw = '#include <thread>\n#include <chrono>\n' + raw
        (source / "SystemPerformanceMonitor.cpp").write_text(raw, encoding="utf-8")
        (source / "SystemPerformanceMonitor.h").write_bytes((SOURCE_ROOT / "src/common/core/SystemPerformanceMonitor.h").read_bytes())
        (source / "runner.cpp").write_text(RUNNER, encoding="utf-8")
        (cls.art / "source_identity.json").write_text(json.dumps({
            "source": str(original), "sourceSha256": hashlib.sha256(original.read_bytes()).hexdigest(),
            "controlledDelayInFixtureCopyOnly": True,
        }, indent=2), encoding="utf-8")
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.21)\nproject(PerfMonitorContract LANGUAGES CXX)\n"
            "set(CMAKE_CXX_STANDARD 17)\nset(CMAKE_AUTOMOC ON)\nfind_package(Qt6 REQUIRED COMPONENTS Core)\n"
            "add_executable(perf_monitor_runner runner.cpp SystemPerformanceMonitor.cpp SystemPerformanceMonitor.h)\n"
            "target_compile_options(perf_monitor_runner PRIVATE /utf-8)\n"
            + ("target_compile_definitions(perf_monitor_runner PRIVATE BASELINE_MONITOR)\n" if BASELINE else "")
            + "target_link_libraries(perf_monitor_runner PRIVATE Qt6::Core pdh psapi user32)\n", encoding="utf-8")
        for name, command in (
            ("configure", ["cmake", "-S", str(source), "-B", str(build), "-G", "Visual Studio 17 2022", "-A", "x64", f"-DCMAKE_PREFIX_PATH={QT.as_posix()}"]),
            ("build", ["cmake", "--build", str(build), "--config", "Release", "--parallel", "2"]),
        ):
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=NO_WINDOW, timeout=180)
            (cls.art / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode:
                raise AssertionError(f"{name}: {result.stdout[-5000:]}\n{result.stderr[-2000:]}")
        cls.runner = build / "Release/perf_monitor_runner.exe"

    def scenario(self, name, delay):
        env = dict(os.environ, CGPLAY_TEST_PERF_DELAY_MS=str(delay))
        env["PATH"] = str(QT / "bin") + os.pathsep + env.get("PATH", "")
        result = subprocess.run([str(self.runner), name], cwd=self.art, env=env, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", creationflags=NO_WINDOW, timeout=25)
        (self.art / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        report = json.loads(result.stdout.strip().splitlines()[-1])
        report["runnerSha256"] = hashlib.sha256(self.runner.read_bytes()).hexdigest()
        (self.art / f"{name}.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        self.assertTrue(report["backgroundMode"])
        self.assertEqual(0, report["visiblePlatformWindowsMax"])
        self.assertEqual(0, report["signalsOffGui"])
        if not BASELINE:
            self.assertEqual(0, report["remainingWorkers"])
        return report

    def test_native_pdh_runs_without_window(self):
        report = self.scenario("native", 0)
        self.assertGreaterEqual(report["samples"], 1)
        if not BASELINE:
            self.assertNotEqual(report["diagnostics"]["samplerThreadId"], report["diagnostics"]["guiThreadId"])

    def test_slow_sampling_keeps_gui_heartbeat_responsive(self):
        report = self.scenario("delayed", 300)
        if BASELINE:
            self.assertGreaterEqual(report["heartbeatMaxMs"], 280)
        else:
            self.assertLess(report["heartbeatMaxMs"], 150)
            self.assertEqual(1, report["diagnostics"]["maxConcurrentSamples"])
            self.assertLessEqual(report["diagnostics"]["pendingNotifications"], 1)

    def test_stop_is_nonblocking_and_discards_inflight_result(self):
        if BASELINE:
            self.skipTest("Baseline only measures original native and delayed GUI sampling")
        report = self.scenario("stop", 500)
        self.assertLess(report["stopMs"], 50)
        self.assertEqual(0, report["samplesAfterStop"])

    def test_destroy_consumer_does_not_wait_for_native_sample(self):
        if BASELINE:
            self.skipTest("New worker lifetime contract")
        report = self.scenario("destroy", 500)
        self.assertLess(report["deleteMs"], 50)
        self.assertLess(report["heartbeatMaxMs"], 150)

    def test_stop_restart_has_no_overlapping_or_stale_samples(self):
        if BASELINE:
            self.skipTest("New worker lifetime contract")
        report = self.scenario("restart", 300)
        self.assertGreaterEqual(report["samples"], 1)
        self.assertGreaterEqual(report["firstSampleMs"], 1050)
        self.assertLess(report["heartbeatMaxMs"], 150)
        self.assertEqual(1, report["diagnostics"]["maxConcurrentSamples"])

    def test_app_drain_is_safe_with_live_facade_and_cannot_restart(self):
        if BASELINE:
            self.skipTest("New process-level drain contract")
        report = self.scenario("drain-live", 300)
        self.assertTrue(report["diagnostics"]["inFlight"])
        self.assertGreaterEqual(report["drainMs"], 100)
        self.assertEqual(0, report["workersAfterRestartDuringShutdown"])

    def test_slow_consumer_coalesces_pending_snapshots(self):
        if BASELINE:
            self.skipTest("New latest-mailbox contract")
        report = self.scenario("mailbox", 0)
        self.assertTrue(report["intentionalGuiStall"])
        self.assertGreaterEqual(report["diagnostics"]["supersededSamples"], 2)
        self.assertLessEqual(report["diagnostics"]["pendingNotifications"], 1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--baseline-root", type=Path)
    args, remaining = parser.parse_known_args()
    if args.baseline_root:
        SOURCE_ROOT = args.baseline_root.resolve()
        BASELINE = True
    unittest.main(argv=[__file__, *remaining], verbosity=2)
