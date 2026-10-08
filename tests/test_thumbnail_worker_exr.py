"""Real EXR thumbnails must finish inside a one-thread shared worker pool."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = ROOT
ARTIFACT = ROOT / "tests/artifacts/player_optimize_20260912/exr_worker"
QT = Path(os.environ.get("CGPLAY_TEST_QT_ROOT", "C:/QtClean/6.5.3/msvc2019_64"))
EXR = Path("C:/Users/1/Desktop/CGV/tlRender-main/install-Release")
BASELINE = False
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

RUNNER = r'''
#include "core/AsyncImageLoader.h"
#include "common/jobs/JobSystem.h"
#include "media/MediaProbe.h"
#include "media/ThumbnailService.h"
#include <OpenEXR/ImfRgbaFile.h>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QThreadPool>
#include <QWidget>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace cgplay;
static std::atomic<int> workerCalls{0}, uiCalls{0}, externalCalls{0};
static QJsonObject report;
namespace cgplay {
bool MediaProbe::isStillImagePath(const QString& path) { return path.endsWith(".exr") || path.endsWith(".png"); }
QVector<SequenceFrame> MediaProbe::collectSequenceFiles(const QString&) { return {}; }
QString MediaProbe::locateFfmpeg() { ++externalCalls; return {}; }
MediaInfo MediaProbe::probe(const QString&, double, JobContext*) { ++externalCalls; return {}; }
int MediaInfo::effectiveFrameCount() const { return frameCount > 0 ? frameCount : 1; }
// This suite tests decoding/scheduling, not color-management configurations.
bool OcioManager::applyPreviewTransform(QImage&, const PreviewTransformSettings&) { return true; }
}
#define CHECK(x) do { if (!(x)) { report["failure"] = #x; report["line"] = __LINE__; return false; } } while (false)
static bool waitUntil(const std::function<bool()>& condition, int timeout = 1500) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return condition();
}
static void markThread() {
    ++workerCalls;
    if (QThread::currentThread() == qApp->thread()) ++uiCalls;
}
static bool decode(const QString& path) {
    int completed = 0;
    bool valid = true;
    QElapsedTimer timer; timer.start();
    for (int i = 0; i < 3; ++i) {
        auto* job = JobRunner::start("test.exr-thumb", qApp, 3000, [path](JobContext& context) {
            markThread();
            const QImage image = ThumbnailService::makePlaylistImage(path, {}, nullptr, &context);
            return JobOutcome::success(QVariant::fromValue(image));
        });
        QObject::connect(job, &JobHandle::finished, qApp, [&](const JobOutcome& outcome) {
            const QImage image = outcome.value.value<QImage>();
            valid = valid && outcome.succeeded() && image.size() == QSize(160, 96);
            if (!image.isNull()) {
                const QColor pixel = image.pixelColor(80, 48);
                valid = valid && std::abs(pixel.red() - 128) <= 1 &&
                    std::abs(pixel.green() - 64) <= 1 && std::abs(pixel.blue() - 191) <= 1;
            }
            ++completed;
        });
    }
    const bool ready = waitUntil([&] { return completed == 3; });
    report["elapsedMs"] = timer.elapsed(); report["completed"] = completed;
    CHECK(ready && valid);
    CHECK(workerCalls == 3 && uiCalls == 0 && externalCalls == 0);
    return true;
}
static bool cancellation(const QString& path) {
    bool done = false;
    bool valid = false;
    auto* job = JobRunner::start("test.canceled-exr-thumb", qApp, 3000, [path](JobContext&) {
        markThread();
        JobContext canceled; canceled.cancel();
        const auto canceledImage = ThumbnailService::makePlaylistImage(path, {}, nullptr, &canceled);
        JobContext timedOut(1); QThread::msleep(5);
        const auto expiredImage = ThumbnailService::makePlaylistImage(path, {}, nullptr, &timedOut);
        return JobOutcome::success(canceledImage.isNull() && expiredImage.isNull());
    });
    QObject::connect(job, &JobHandle::finished, qApp, [&](const JobOutcome& outcome) {
        done = true; valid = outcome.succeeded() && outcome.value.toBool();
    });
    CHECK(waitUntil([&] { return done; }));
    CHECK(valid && uiCalls == 0 && externalCalls == 0);
    return true;
}
static bool asyncCompatibility(const QString& path) {
    AsyncImageLoader loader;
    ImageLoadRequest request; request.filePath = path; request.frame = 71;
    auto future = loader.loadAsync(request);
    CHECK(waitUntil([&] { return future.isFinished(); }));
    const auto result = future.result();
    CHECK(result.success && result.frame == 71 && result.filePath == path);
    CHECK(result.origWidth == 8 && result.origHeight == 8);
    CHECK(result.image.format() == QImage::Format_RGBA64);
    const QRgba64 pixel = result.image.pixelColor(0, 0).rgba64();
    CHECK(pixel.red() == 32767 && pixel.green() == 16383 && pixel.blue() == 49151);
    return true;
}
static bool corrupt(const QString& path) {
    const QString corruptPath = QFileInfo(path).dir().filePath("corrupt.exr");
    QFile file(corruptPath); CHECK(file.open(QIODevice::WriteOnly)); file.write("not-exr"); file.close();
    bool done = false, valid = false;
    auto* job = JobRunner::start("test.bad-exr-thumb", qApp, 3000, [corruptPath](JobContext& context) {
        markThread();
        return JobOutcome::success(ThumbnailService::makePlaylistImage(corruptPath, {}, nullptr, &context).isNull());
    });
    QObject::connect(job, &JobHandle::finished, qApp, [&](const JobOutcome& outcome) {
        done = true; valid = outcome.succeeded() && outcome.value.toBool();
    });
    CHECK(waitUntil([&] { return done; }));
    CHECK(valid && uiCalls == 0 && externalCalls == 0);
    return true;
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setProperty("cgplay.automationBackground", true);
    QThreadPool::globalInstance()->setMaxThreadCount(1);
    const QString path = QDir::current().filePath("fixture.exr");
    {
        Imf::Rgba pixels[64];
        for (auto& pixel : pixels) pixel = Imf::Rgba(0.5f, 0.25f, 0.75f, 1.0f);
        Imf::RgbaOutputFile output(path.toStdString().c_str(), 8, 8, Imf::WRITE_RGBA);
        output.setFrameBuffer(pixels, 1, 8); output.writePixels(8);
    }
    const QString scenario = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    const bool ok = scenario == "decode" ? decode(path) : scenario == "cancel" ? cancellation(path) :
        scenario == "async" ? asyncCompatibility(path) : scenario == "corrupt" && corrupt(path);
    int visible = 0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        DWORD process = 0; GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(data);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&visible));
#endif
    report["scenario"] = scenario; report["passed"] = ok;
    report["background"] = qEnvironmentVariable("QT_QPA_PLATFORM") == "offscreen";
    report["visiblePlatformWindows"] = visible;
    report["poolThreads"] = QThreadPool::globalInstance()->maxThreadCount();
    report["workerCalls"] = workerCalls.load(); report["uiCalls"] = uiCalls.load();
    report["externalCalls"] = externalCalls.load();
    std::puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
    std::fflush(stdout);
    // A regressed nested worker cannot be joined safely here: emit failure and
    // terminate this disposable test process before its queued loader expires.
    if (!ok) std::_Exit(1);
    QThreadPool::globalInstance()->waitForDone();
    return visible == 0 ? 0 : 1;
}
'''


class ThumbnailWorkerExr(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source, build = ARTIFACT / "source", ARTIFACT / "build"
        source.mkdir(parents=True, exist_ok=True)
        (source / "runner.cpp").write_text(RUNNER, encoding="utf-8")
        paths = [SOURCE_ROOT / "src/common/core/AsyncImageLoader.cpp",
                 SOURCE_ROOT / "src/common/core/AsyncImageLoader.h",
                 SOURCE_ROOT / "src/services/media/ThumbnailService.cpp",
                 ROOT / "src/common/jobs/JobSystem.cpp", ROOT / "src/common/jobs/JobSystem.h"]
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.21)\nproject(ThumbnailExrTest LANGUAGES CXX)\n"
            "set(CMAKE_CXX_STANDARD 17)\nset(CMAKE_AUTOMOC ON)\n"
            "find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)\nfind_package(OpenEXR REQUIRED)\n"
            "add_executable(exr_thumbnail_runner runner.cpp\n" +
            "\n".join(f'"{p.as_posix()}"' for p in paths) + ")\n" +
            f'target_include_directories(exr_thumbnail_runner PRIVATE "{SOURCE_ROOT.as_posix()}/src/common" "{ROOT.as_posix()}/src/common" "{ROOT.as_posix()}/src/common/core" "{ROOT.as_posix()}/src" "{ROOT.as_posix()}/src/services" "{ROOT.as_posix()}/src/services/media")\n' +
            "target_compile_options(exr_thumbnail_runner PRIVATE /utf-8)\n"
            "target_link_libraries(exr_thumbnail_runner PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent OpenEXR::OpenEXR user32)\n",
            encoding="utf-8")
        for name, command in (
            ("configure", ["cmake", "-S", str(source), "-B", str(build), "-G", "Visual Studio 17 2022",
                           "-A", "x64", f"-DCMAKE_PREFIX_PATH={QT.as_posix()};{EXR.as_posix()}"]),
            ("build", ["cmake", "--build", str(build), "--config", "Release", "--parallel", "2"]),
        ):
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
                                    errors="replace", creationflags=NO_WINDOW, timeout=180)
            (ARTIFACT / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode:
                raise AssertionError(f"{name} failed:\n{result.stdout[-5000:]}\n{result.stderr[-2000:]}")
        cls.runner = build / "Release/exr_thumbnail_runner.exe"

    def run_scenario(self, scenario):
        if BASELINE and scenario != "decode":
            self.skipTest("Baseline only demonstrates the one-worker nested EXR starvation")
        env = os.environ.copy()
        env["PATH"] = os.pathsep.join([str(QT / "bin"), str(EXR / "bin"), env.get("PATH", "")])
        env["QT_QPA_PLATFORM"] = "offscreen"
        result = subprocess.run([str(self.runner), scenario], cwd=ARTIFACT, env=env,
                                capture_output=True, text=True, encoding="utf-8", errors="replace",
                                creationflags=NO_WINDOW, timeout=15)
        (ARTIFACT / f"{scenario}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(result.stdout.strip(), result.stderr)
        data = json.loads(result.stdout.strip().splitlines()[-1])
        (ARTIFACT / f"{scenario}.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        evidence_root = ROOT / "tests/artifacts/player_optimize_20260912/exr_worker"
        baseline_report, current_report = evidence_root / "baseline/decode.json", evidence_root / "decode.json"
        if scenario == "decode" and baseline_report.exists() and current_report.exists():
            (evidence_root / "comparison.json").write_text(json.dumps({
                "scope": "Real ThumbnailService, AsyncImageLoader and 8x8 EXR; shared Qt worker pool limited to one thread; OCIO is bypassed",
                "before": json.loads(baseline_report.read_text(encoding="utf-8")),
                "after": json.loads(current_report.read_text(encoding="utf-8")),
            }, indent=2) + "\n", encoding="utf-8")
        self.assertTrue(data["background"])
        self.assertEqual(0, data["visiblePlatformWindows"])
        self.assertEqual(1, data["poolThreads"])
        self.assertEqual(0, data["uiCalls"])
        self.assertEqual(0, data["externalCalls"])
        if BASELINE:
            self.assertEqual(1, result.returncode, data)
            self.assertFalse(data["passed"], data)
            self.assertEqual(0, data["completed"], data)
        else:
            self.assertEqual(0, result.returncode, data)
            self.assertTrue(data["passed"], data)

    def test_real_exr_thumbnails_finish_on_single_worker(self):
        self.run_scenario("decode")

    def test_precanceled_and_expired_requests_return_empty(self):
        self.run_scenario("cancel")

    def test_existing_async_api_preserves_rgba64_pixels(self):
        self.run_scenario("async")

    def test_corrupt_exr_returns_without_stalling(self):
        self.run_scenario("corrupt")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--baseline-root", type=Path)
    args, remaining = parser.parse_known_args()
    if args.baseline_root:
        SOURCE_ROOT = args.baseline_root.resolve()
        BASELINE = True
        ARTIFACT = ARTIFACT / "baseline"
    unittest.main(argv=[__file__, *remaining], verbosity=2)
