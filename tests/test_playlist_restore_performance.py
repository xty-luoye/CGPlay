"""Exercise the real playlist model and job runner with slow counted media services."""

from __future__ import annotations

import json
import argparse
import os
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
ARTIFACT = ROOT / "tests/artifacts/player_optimize_20260912/playlist_restore"
MODEL_ROOT = ROOT
BASELINE = False
QT = Path(os.environ.get("CGPLAY_TEST_QT_ROOT", "C:/QtClean/6.5.3/msvc2019_64"))
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

RUNNER = r'''
#include "features/playlist/PlaylistModel.h"
#include "common/jobs/JobSystem.h"
#include "media/MediaProbe.h"
#include "media/ThumbnailService.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>
#include <atomic>
#include <cstdio>
#include <functional>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace cgplay;
static std::atomic<int> probes{0}, images{0}, syncIcons{0}, uiDecodes{0};
static std::atomic<int> canceled{0}, badMetadata{0}, active{0}, delayMs{100};
static QJsonObject report;
#define CHECK(x) do { if (!(x)) { report["failure"] = #x; report["line"] = __LINE__; return false; } } while (false)

namespace cgplay {
int MediaInfo::effectiveFrameCount() const { return frameCount > 0 ? frameCount : lastFrame - firstFrame + 1; }
bool MediaProbe::isStillImagePath(const QString& path) { return path.endsWith(".png"); }
bool MediaProbe::isVideoPath(const QString& path) { return path.endsWith(".mp4"); }
MediaInfo MediaProbe::probe(const QString& path, double fpsOverride, JobContext*) {
    ++probes;
    QThread::msleep(5);
    MediaInfo info;
    info.path = path; info.name = "fresh-" + QFileInfo(path).baseName();
    info.exists = true; info.isVideo = true; info.width = 640; info.height = 360;
    info.firstFrame = 0; info.lastFrame = 95; info.frameCount = 96;
    info.fps = fpsOverride > 0 ? fpsOverride : 24; info.formatLabel = "MP4";
    return info;
}
QImage ThumbnailService::makePlaylistImage(const QString&, const OcioManager::PreviewTransformSettings& settings,
                                           const MediaInfo* info, JobContext* job) {
    ++images; ++active;
    if (QThread::currentThread() == qApp->thread()) ++uiDecodes;
    if (!info || info->width != 640 || info->height != 360 || info->frameCount != 96) ++badMetadata;
    const int wait = delayMs.load();
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < wait) {
        if (job && job->isCancellationRequested()) { ++canceled; --active; return {}; }
        QThread::msleep(5);
    }
    QImage image(16, 16, QImage::Format_RGB32);
    image.fill(settings.enabled ? Qt::green : Qt::red);
    --active;
    return image;
}
QIcon ThumbnailService::makePlaylistIcon(const QString& path, const OcioManager::PreviewTransformSettings& settings,
                                         const MediaInfo* info, JobContext* job) {
    ++syncIcons;
    const MediaInfo local = info ? MediaInfo{} : MediaProbe::probe(path);
    return QIcon(QPixmap::fromImage(makePlaylistImage(path, settings, info ? info : &local, job)));
}
}

static bool waitUntil(const std::function<bool()>& condition, int timeout = 4000) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return condition();
}
static void pump(int milliseconds) { waitUntil([] { return false; }, milliseconds); }
static QJsonObject session(const QStringList& paths) {
    QJsonArray shots;
    for (const auto& path : paths) shots.append(QJsonObject{
        {"path", path}, {"name", "saved-name"}, {"width", 1}, {"height", 1},
        {"fps", 30.0}, {"first_frame", 3}, {"last_frame", 4},
        {"track_kind", "Video"}, {"track_name", "V2"}, {"status_color", "#ffaa6600"}});
    return {{"current_index", 0}, {"shots", shots}};
}
static QColor colorAt(const PlaylistModel& model, int row) {
    return model.shotAt(row).thumbnail.pixmap(16, 16).toImage().pixelColor(8, 8);
}

static bool restore(const QString& a, const QString& b, const QString& missing) {
    delayMs = 350;
    PlaylistModel model;
    QElapsedTimer timer; timer.start();
    model.fromJson(session({a, a, b, missing}));
    report["restoreMs"] = timer.elapsed();
    CHECK(probes == 3);
    CHECK(images == 0 && syncIcons == 0);
    CHECK(timer.elapsed() < 250); // Three 5ms probes, no 350ms thumbnail decodes.
    CHECK(model.shotCount() == 4 && model.currentIndex() == 0);
    CHECK(model.shotAt(0).width == 640 && model.shotAt(0).height == 360);
    CHECK(model.shotAt(0).fps == 30 && model.shotAt(0).frameCount() == 96);
    CHECK(model.shotAt(0).name == "fresh-a");
    CHECK(model.shotAt(0).trackName == "V2" && model.shotAt(0).statusColor == QColor("#ffaa6600"));
    CHECK(model.shotAt(3).name == "saved-name" && model.shotAt(3).width == 1);
    int heartbeats = 0;
    QTimer heartbeat; heartbeat.setInterval(10);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; }); heartbeat.start();
    CHECK(waitUntil([&] { return !model.shotAt(0).thumbnail.isNull() &&
        !model.shotAt(1).thumbnail.isNull() && !model.shotAt(2).thumbnail.isNull(); }));
    CHECK(images == 2 && probes == 3 && uiDecodes == 0 && badMetadata == 0);
    CHECK(model.shotAt(3).thumbnail.isNull());
    CHECK(heartbeats >= 10);
    CHECK(colorAt(model, 0) == Qt::red && colorAt(model, 1) == Qt::red);
    report["heartbeats"] = heartbeats;
    const auto saved = model.toJson()["shots"].toArray();
    CHECK(saved.size() == 4 && saved[0].toObject()["track_name"] == "V2");
    CHECK(waitUntil([&] { return model.findChildren<JobHandle*>().isEmpty(); }));
    return true;
}
static bool queuedReset(const QString& a) {
    PlaylistModel model;
    // Exercise both addPath's queued work and a reset to the exact same path.
    model.addPath(a);
    pump(300);
    model.fromJson(session({a}));
    pump(300);
    CHECK(images == 0); // The older 500ms callback must not act on the new list.
    CHECK(waitUntil([&] { return !model.shotAt(0).thumbnail.isNull(); }));
    CHECK(images == 1 && probes == 2 && uiDecodes == 0);
    model.fromJson(session({a}));
    model.clear();
    pump(600);
    CHECK(images == 1 && model.shotCount() == 0);
    return true;
}
static bool runningReset(const QString& a) {
    delayMs = 2000;
    PlaylistModel model;
    model.fromJson(session({a}));
    CHECK(waitUntil([] { return active == 1; }));
    model.fromJson(session({a}));
    CHECK(waitUntil([] { return canceled == 1; }, 1000));
    CHECK(model.shotAt(0).thumbnail.isNull());
    delayMs = 100;
    CHECK(waitUntil([&] { return !model.shotAt(0).thumbnail.isNull(); }));
    CHECK(images == 2 && probes == 2 && badMetadata == 0 && uiDecodes == 0);
    CHECK(waitUntil([&] { return model.findChildren<JobHandle*>().isEmpty(); }));
    return true;
}
static bool rebuild(const QString& a) {
    PlaylistModel model;
    model.fromJson(session({a, a}));
    CHECK(waitUntil([&] { return !model.shotAt(1).thumbnail.isNull(); }));
    CHECK(images == 1);
    OcioManager::PreviewTransformSettings settings; settings.enabled = true;
    model.rebuildThumbnails(settings);
    CHECK(waitUntil([&] { return !model.shotAt(1).thumbnail.isNull(); }));
    CHECK(images == 2 && probes == 2);
    CHECK(colorAt(model, 0) == Qt::green && colorAt(model, 1) == Qt::green);
    model.rebuildThumbnails(settings);
    pump(100);
    CHECK(images == 2 && uiDecodes == 0 && badMetadata == 0);
    return true;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setProperty("cgplay.automationBackground", true);
    QThreadPool::globalInstance()->setMaxThreadCount(4);
    const QString root = QDir::current().filePath("fixtures");
    QDir().mkpath(root);
    const QString a = root + "/a.mp4", b = root + "/b.mp4", missing = root + "/missing.mp4";
    for (const QString& path : {a, b}) { QFile file(path); if (!file.open(QIODevice::WriteOnly)) return 2; file.write("test"); }
    const QString scenario = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    const bool ok = scenario == "restore" ? restore(a, b, missing) :
        scenario == "queued-reset" ? queuedReset(a) :
        scenario == "running-reset" ? runningReset(a) : scenario == "rebuild" && rebuild(a);
    QThreadPool::globalInstance()->waitForDone();
    int visible = 0;
    for (auto* widget : QApplication::topLevelWidgets()) if (widget->isVisible() &&
        !widget->testAttribute(Qt::WA_DontShowOnScreen)) ++visible;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        DWORD process = 0; GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId() && IsWindowVisible(window)) ++*reinterpret_cast<int*>(data);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&visible));
#endif
    report["scenario"] = scenario; report["passed"] = ok && visible == 0;
    report["background"] = qEnvironmentVariable("QT_QPA_PLATFORM") == "offscreen";
    report["visiblePlatformWindows"] = visible;
    report["probeCalls"] = probes.load(); report["decodeCalls"] = images.load();
    report["uiDecodeCalls"] = uiDecodes.load(); report["syncIconCalls"] = syncIcons.load();
    report["canceled"] = canceled.load(); report["badMetadata"] = badMetadata.load();
    const auto json = QJsonDocument(report).toJson(QJsonDocument::Compact);
    std::puts(json.constData());
    return ok && visible == 0 ? 0 : 1;
}
'''


class PlaylistRestorePerformance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = ARTIFACT / "source"
        build = ARTIFACT / "build"
        source.mkdir(parents=True, exist_ok=True)
        (source / "runner.cpp").write_text(RUNNER, encoding="utf-8")
        paths = [MODEL_ROOT / "src/features/playlist/PlaylistModel.cpp",
                 MODEL_ROOT / "src/features/playlist/PlaylistModel.h",
                 ROOT / "src/common/jobs/JobSystem.cpp", ROOT / "src/common/jobs/JobSystem.h"]
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.21)\nproject(PlaylistRestoreTest LANGUAGES CXX)\n"
            "set(CMAKE_CXX_STANDARD 17)\nset(CMAKE_AUTOMOC ON)\n"
            "find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)\n"
            "add_executable(playlist_restore_runner runner.cpp\n" +
            "\n".join(f'"{p.as_posix()}"' for p in paths) + ")\n" +
            f'target_include_directories(playlist_restore_runner PRIVATE "{MODEL_ROOT.as_posix()}/src" "{ROOT.as_posix()}/src" "{ROOT.as_posix()}/src/services")\n' +
            "target_compile_options(playlist_restore_runner PRIVATE /utf-8)\n"
            "target_link_libraries(playlist_restore_runner PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent user32)\n",
            encoding="utf-8")
        for name, command in (
            ("configure", ["cmake", "-S", str(source), "-B", str(build), "-G", "Visual Studio 17 2022",
                           "-A", "x64", f"-DCMAKE_PREFIX_PATH={QT.as_posix()}"]),
            ("build", ["cmake", "--build", str(build), "--config", "Release", "--parallel", "2"]),
        ):
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", creationflags=NO_WINDOW, timeout=180)
            (ARTIFACT / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode:
                raise AssertionError(f"{name} failed:\n{result.stdout[-6000:]}\n{result.stderr[-2000:]}")
        cls.runner = build / "Release/playlist_restore_runner.exe"

    def run_scenario(self, scenario):
        env = os.environ.copy()
        env["PATH"] = str(QT / "bin") + os.pathsep + env.get("PATH", "")
        env["QT_QPA_PLATFORM"] = "offscreen"
        result = subprocess.run([str(self.runner), scenario], cwd=ARTIFACT, env=env,
                                capture_output=True, text=True, encoding="utf-8", errors="replace",
                                creationflags=NO_WINDOW, timeout=20)
        (ARTIFACT / f"{scenario}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        data = json.loads(result.stdout.strip().splitlines()[-1])
        (ARTIFACT / f"{scenario}.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        evidence_root = ROOT / "tests/artifacts/player_optimize_20260912/playlist_restore"
        baseline_report = evidence_root / "baseline/restore.json"
        current_report = evidence_root / "restore.json"
        if scenario == "restore" and baseline_report.exists() and current_report.exists():
            comparison = {
                "scope": "Real PlaylistModel and JobRunner; controlled 5ms probe / 350ms decode stubs, not real media or startup timing",
                "before": json.loads(baseline_report.read_text(encoding="utf-8")),
                "after": json.loads(current_report.read_text(encoding="utf-8")),
            }
            (evidence_root / "comparison.json").write_text(
                json.dumps(comparison, indent=2) + "\n", encoding="utf-8")
        if BASELINE:
            self.assertEqual(1, result.returncode, data)
            self.assertFalse(data["passed"], data)
            self.assertTrue(data["background"], data)
            self.assertEqual(0, data["visiblePlatformWindows"], data)
            self.assertEqual(6, data["probeCalls"], data)
            self.assertEqual(3, data["uiDecodeCalls"], data)
            self.assertEqual(3, data["syncIconCalls"], data)
            self.assertGreaterEqual(data["restoreMs"], 1000, data)
            return
        self.assertEqual(0, result.returncode, data)
        self.assertTrue(data["passed"], data)
        self.assertTrue(data["background"], data)
        self.assertEqual(0, data["visiblePlatformWindows"], data)
        self.assertEqual(0, data["uiDecodeCalls"], data)

    def test_restore_probes_once_and_decodes_off_ui_with_duplicates(self):
        self.run_scenario("restore")

    def test_reset_discards_old_queued_request_even_for_same_path(self):
        if BASELINE:
            self.skipTest("Baseline records the controlled synchronous restore regression only")
        self.run_scenario("queued-reset")

    def test_reset_cancels_inflight_decode_and_rejects_stale_result(self):
        if BASELINE:
            self.skipTest("Baseline records the controlled synchronous restore regression only")
        self.run_scenario("running-reset")

    def test_rebuild_updates_duplicates_and_reuses_unchanged_settings(self):
        if BASELINE:
            self.skipTest("Baseline records the controlled synchronous restore regression only")
        self.run_scenario("rebuild")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--baseline-root", type=Path)
    args, remaining = parser.parse_known_args()
    if args.baseline_root:
        MODEL_ROOT = args.baseline_root.resolve()
        BASELINE = True
        ARTIFACT = ARTIFACT / "baseline"
    unittest.main(argv=[__file__, *remaining], verbosity=2)
