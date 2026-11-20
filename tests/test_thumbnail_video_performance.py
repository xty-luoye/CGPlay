"""Real ffmpeg coverage for bounded video timeline-thumbnail extraction."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import textwrap
import tempfile
import unittest
from tests.native_probe_runtime import deploy_runtime


ROOT = Path(__file__).resolve().parents[1]
ARTIFACT = ROOT / "tests" / "artifacts" / "fix_review_20261101" / "remaining_regression" / "video_thumbnails"
QT = Path(os.environ.get("CGPLAY_TEST_QT_ROOT", "E:/rjkf/CGPlay-deps/QtClean/6.5.3/msvc2019_64"))
TL = Path(os.environ.get("CGPLAY_TEST_TL_ROOT", "E:/rj/CGV/tlRender-main/install-Release"))
FFMPEG = Path(os.environ.get(
    "CGPLAY_TEST_FFMPEG",
    str(ROOT / "build_win_full/bin/Release/ffmpeg.exe"),
))
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


PROXY = r'''
#include <windows.h>
#include <fstream>
#include <string>

int wmain(int argc, wchar_t** argv)
{
    {
        std::ofstream count("proxy_count.txt", std::ios::app);
        count << "1\n";
    }
    if (GetEnvironmentVariableW(L"CGPLAY_TEST_REJECT_CUDA", nullptr, 0)) {
        for (int i = 1; i < argc; ++i) {
            if (std::wstring(argv[i]) == L"-hwaccel") return 1;
        }
    }
    wchar_t realPath[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"CGPLAY_REAL_FFMPEG", realPath, 32768);
    if (!length || length >= 32768 || argc < 1) return 126;
    std::wstring command = L"\"" + std::wstring(realPath, length) + L"\"";
    for (int i = 1; i < argc; ++i) {
        command += L" \"";
        for (const wchar_t* p = argv[i]; *p; ++p) {
            if (*p == L'\"') command += L'\\';
            command += *p;
        }
        command += L"\"";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return 127;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
'''


RUNNER = r'''
#include "common/jobs/JobSystem.h"
#include "media/MediaProbe.h"
#include "media/ThumbnailService.h"
#include "media/ThumbnailDiskCache.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThreadPool>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace cgplay;

namespace cgplay {
bool MediaProbe::isStillImagePath(const QString&) { return false; }
bool MediaProbe::isVideoPath(const QString&) { return true; }
QString MediaProbe::locateFfmpeg() { return qEnvironmentVariable("CGPLAY_TEST_FFMPEG"); }
QString MediaProbe::locateFfprobe() { return qEnvironmentVariable("CGPLAY_TEST_FFPROBE"); }
double MediaProbe::parseFpsRatio(const QString& value)
{
    const QStringList parts = value.split(QLatin1Char('/'));
    bool okA = false;
    bool okB = false;
    const double a = parts.value(0).toDouble(&okA);
    const double b = parts.size() > 1 ? parts.value(1).toDouble(&okB) : 1.0;
    return okA && (parts.size() == 1 || okB) && b > 0.0 ? a / b : 0.0;
}
MediaInfo MediaProbe::probe(const QString&, double, JobContext*) { return {}; }
QVector<SequenceFrame> MediaProbe::collectSequenceFiles(const QString&) { return {}; }
int MediaInfo::effectiveFrameCount() const { return frameCount > 0 ? frameCount : 1; }
bool OcioManager::applyPreviewTransform(QImage&, const PreviewTransformSettings&) { return true; }
}

static QJsonObject report;
static const QString proxyCountPath = QStringLiteral("proxy_count.txt");

static int ffmpegStarts()
{
    QFile file(proxyCountPath);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    const QByteArray text = file.readAll();
    int count = 0;
    for (const char byte : text) {
        if (byte == '\n') ++count;
    }
    return count;
}

static bool commonChecks()
{
    int visible = 0;
#ifdef Q_OS_WIN
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId() && IsWindowVisible(window)) {
            ++*reinterpret_cast<int*>(data);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&visible));
#endif
    report["background"] = qEnvironmentVariable("QT_QPA_PLATFORM") == "offscreen";
    report["visiblePlatformWindows"] = visible;
    report["poolThreads"] = QThreadPool::globalInstance()->maxThreadCount();
    return report["background"].toBool() && visible == 0;
}

static bool batch(const QString& path, double fps = 24.0, int expectedStarts = 1)
{
    QFile::remove(proxyCountPath);
    QElapsedTimer timer;
    timer.start();
    const auto frames = ThumbnailService::buildTimelineThumbnails(
        path, 48, fps, 6, {}, nullptr);
    report["elapsedMs"] = timer.elapsed();
    report["frameCount"] = frames.size();
    report["ffmpegStarts"] = ffmpegStarts();
    bool valid = frames.size() == 6;
    for (const auto& frame : frames) {
        valid = valid && frame.image.size() == QSize(112, 64) && !frame.image.isNull();
    }
    return valid && report["ffmpegStarts"].toInt() == expectedStarts;
}

static bool trackColors(const QString& path, bool expectRed)
{
    const auto matches = [expectRed](const QImage& image) {
        if (image.isNull()) return false;
        const QColor pixel = image.pixelColor(image.width() / 2, image.height() / 2);
        return expectRed ? pixel.red() > 200 && pixel.blue() < 40 :
                           pixel.blue() > 200 && pixel.red() < 40;
    };
    const auto frames = ThumbnailService::buildTimelineThumbnails(path, 48, 24, 6, {}, nullptr);
    if (frames.size() != 6 || !matches(ThumbnailService::makePlaylistImage(path, {}))) return false;
    for (const auto& frame : frames) {
        if (!matches(frame.image)) return false;
    }
    return true;
}

static bool cacheReuse(const QString& path)
{
    QFile::remove(proxyCountPath);
    const auto first = ThumbnailService::buildTimelineThumbnails(path, 48, 24.0, 6, {}, nullptr);
    const int firstStarts = ffmpegStarts();
    QFile::remove(proxyCountPath);
    const auto second = ThumbnailService::buildTimelineThumbnails(path, 48, 24.0, 6, {}, nullptr);
    const int secondStarts = ffmpegStarts();
    report["firstFrameCount"] = first.size();
    report["secondFrameCount"] = second.size();
    report["firstFfmpegStarts"] = firstStarts;
    report["secondFfmpegStarts"] = secondStarts;
    // A fully cached request must work even if the external tools disappear.
    // Route an unexpected probe to the counting proxy as well.
    qputenv("CGPLAY_TEST_FFPROBE", qgetenv("CGPLAY_TEST_FFMPEG"));
    qunsetenv("CGPLAY_TEST_FFMPEG");
    QFile::remove(proxyCountPath);
    const auto offline = ThumbnailService::buildTimelineThumbnails(path, 48, 24.0, 6, {}, nullptr);
    report["cachedFramesWithoutTools"] = offline.size();
    report["warmToolStarts"] = ffmpegStarts();
    bool identical = first.size() == offline.size();
    for (int i = 0; identical && i < first.size(); ++i) {
        identical = first[i].image == offline[i].image;
    }
    return first.size() == 6 && second.size() == 6 && firstStarts == 1 &&
        secondStarts == 0 && identical && ffmpegStarts() == 0;
}

static bool canceled(const QString& path)
{
    const QDir temp(QDir::tempPath());
    const QStringList before = temp.entryList({QStringLiteral("cgplay_media_thumb_batch_*")},
                                               QDir::Dirs | QDir::NoDotAndDotDot);
    JobContext context;
    context.cancel();
    const auto frames = ThumbnailService::buildTimelineThumbnails(path, 48, 24.0, 6, {}, &context);
    const QStringList after = temp.entryList({QStringLiteral("cgplay_media_thumb_batch_*")},
                                              QDir::Dirs | QDir::NoDotAndDotDot);
    report["frameCount"] = frames.size();
    report["temporaryDirsBefore"] = before.size();
    report["temporaryDirsAfter"] = after.size();
    return frames.isEmpty() && before == after;
}

static bool longClipFallsBack(const QString& path, bool rejectHardware = false)
{
    QFile::remove(proxyCountPath);
    const auto frames = ThumbnailService::buildTimelineThumbnails(path, 1900, rejectHardware ? 60.0 : 24.0, 3, {}, nullptr);
    const int starts = ffmpegStarts();
    report["frameCount"] = frames.size();
    report["ffmpegStarts"] = starts;
    // totalFrames > the bounded batch threshold must use the legacy seek path.
    for (const auto& frame : frames) {
        if (frame.image.size() != QSize(112, 64)) return false;
    }
    return starts == (rejectHardware ? 4 : 3) && frames.size() == 3;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setProperty("cgplay.automationBackground", true);
    cgplay::ThumbnailDiskCache::Policy cachePolicy;
    cachePolicy.baseDirectory = qEnvironmentVariable("CGPLAY_TEST_CACHE_ROOT");
    if (!cachePolicy.baseDirectory.isEmpty()) {
        QString cacheError;
        if (!cgplay::ThumbnailDiskCache::prepare(cachePolicy, &cacheError)) {
            std::fprintf(stderr, "thumbnail cache setup failed: %s\\n", cacheError.toUtf8().constData());
            return 2;
        }
        cgplay::ThumbnailDiskCache::configure(cachePolicy);
    }
    const QString scenario = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    const QString path = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    const bool ok = scenario == "batch" ? batch(path) :
        scenario == "cuda-fallback" ? batch(path, 60.0, 2) :
        scenario == "track-first" ? trackColors(path, true) :
        scenario == "track-default" ? trackColors(path, false) :
        scenario == "cache" ? cacheReuse(path) :
        scenario == "cancel" ? canceled(path) :
        scenario == "long-cuda-fallback" ? longClipFallsBack(path, true) :
        scenario == "long" ? longClipFallsBack(path) : false;
    report["scenario"] = scenario;
    report["passed"] = ok && commonChecks();
    std::puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
    std::fflush(stdout);
    return report["passed"].toBool() ? 0 : 1;
}
'''


class ThumbnailVideoPerformance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        ARTIFACT.mkdir(parents=True, exist_ok=True)
        source = ARTIFACT / "source"
        build = ARTIFACT / "build"
        source.mkdir(parents=True, exist_ok=True)
        (source / "runner.cpp").write_text(RUNNER, encoding="utf-8")
        (source / "proxy.cpp").write_text(PROXY, encoding="utf-8")
        thumbnail = ROOT / "src/services/media/ThumbnailService.cpp"
        thumbnail_h = ROOT / "src/services/media/ThumbnailService.h"
        job_cpp = ROOT / "src/common/jobs/JobSystem.cpp"
        job_h = ROOT / "src/common/jobs/JobSystem.h"
        async_cpp = ROOT / "src/common/core/AsyncImageLoader.cpp"
        async_h = ROOT / "src/common/core/AsyncImageLoader.h"
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.21)\n"
            "project(CGPlayThumbnailVideoTest LANGUAGES CXX)\n"
            "set(CMAKE_CXX_STANDARD 17)\nset(CMAKE_AUTOMOC ON)\n"
            "find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)\n"
            "find_package(OpenEXR REQUIRED)\n"
            f'add_executable(video_thumbnail_runner runner.cpp "{thumbnail.as_posix()}" "{thumbnail_h.as_posix()}" '
            f'"{(ROOT / "src/services/media/ThumbnailDiskCache.cpp").as_posix()}" '
            f'"{(ROOT / "src/services/media/ThumbnailDiskCache.h").as_posix()}" '
            f'"{job_cpp.as_posix()}" "{job_h.as_posix()}" "{async_cpp.as_posix()}" "{async_h.as_posix()}")\n'
            "add_executable(ffmpeg_proxy proxy.cpp)\n"
            f'target_include_directories(video_thumbnail_runner PRIVATE "{(ROOT / "src/common").as_posix()}" '
            f'"{(ROOT / "src/common/core").as_posix()}" "{(ROOT / "src").as_posix()}" '
            f'"{(ROOT / "src/services").as_posix()}" "{(ROOT / "src/services/media").as_posix()}")\n'
            "target_compile_options(video_thumbnail_runner PRIVATE /utf-8)\n"
            "target_link_libraries(video_thumbnail_runner PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent OpenEXR::OpenEXR user32)\n",
            encoding="utf-8",
        )
        for name, command in (
            ("configure", ["cmake", "-S", str(source), "-B", str(build), "-G", "Visual Studio 17 2022",
                           "-A", "x64", f"-DCMAKE_PREFIX_PATH={QT.as_posix()};{TL.as_posix()}"]),
            ("build", ["cmake", "--build", str(build), "--config", "Release", "--parallel", "2"]),
        ):
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", creationflags=NO_WINDOW, timeout=180)
            (ARTIFACT / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode:
                raise AssertionError(f"{name} failed:\n{result.stdout[-5000:]}\n{result.stderr[-3000:]}")
        cls.runner = build / "Release" / "video_thumbnail_runner.exe"
        cls.proxy = build / "Release" / "ffmpeg_proxy.exe"
        cls.short = ARTIFACT / "short.mp4"
        cls.long = ARTIFACT / "long_path.mp4"
        cls.cache = ARTIFACT / f"cache_{os.getpid()}.mp4"
        result = subprocess.run([
            str(FFMPEG), "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi", "-i", "testsrc=size=320x180:rate=24",
            "-t", "2", "-c:v", "mpeg4", "-q:v", "5", str(cls.short),
        ], cwd=ARTIFACT, capture_output=True, text=True, encoding="utf-8", errors="replace",
            creationflags=NO_WINDOW, timeout=60)
        if result.returncode:
            raise AssertionError(f"fixture ffmpeg failed: {result.stderr}")
        result = subprocess.run([
            str(FFMPEG), "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi", "-i", "testsrc=size=64x36:rate=24",
            "-frames:v", "2000", "-c:v", "mpeg4", "-q:v", "8", str(cls.long),
        ], cwd=ARTIFACT, capture_output=True, text=True, encoding="utf-8", errors="replace",
            creationflags=NO_WINDOW, timeout=90)
        if result.returncode:
            raise AssertionError(f"long fixture ffmpeg failed: {result.stderr}")
        shutil.copy2(cls.short, cls.cache)

    def run_scenario(self, scenario, path, cache_root=None):
        deploy_runtime(self.runner, QT, (TL / "bin", ROOT / "build_win_full" / "bin" / "Release"))
        env = os.environ.copy()
        env["PATH"] = os.pathsep.join([str(QT / "bin"), str(TL / "bin"), env.get("PATH", "")])
        env["QT_QPA_PLATFORM"] = "offscreen"
        env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(QT / "plugins" / "platforms")
        env["QT_PLUGIN_PATH"] = str(QT / "plugins")
        env["CGPLAY_TEST_FFMPEG"] = str(self.proxy)
        env["CGPLAY_TEST_FFPROBE"] = str(FFMPEG.with_name("ffprobe.exe"))
        env["CGPLAY_REAL_FFMPEG"] = str(FFMPEG)
        if scenario in ("cuda-fallback", "long-cuda-fallback"):
            env["CGPLAY_TEST_REJECT_CUDA"] = "1"
        env["CGPLAY_TEST_CACHE_ROOT"] = str(cache_root or ARTIFACT / ("cache_root_" + scenario))
        result = subprocess.run([str(self.runner), scenario, str(path)], cwd=ARTIFACT, env=env,
                                capture_output=True, text=True, encoding="utf-8", errors="replace",
                                creationflags=NO_WINDOW, timeout=60)
        (ARTIFACT / f"{scenario}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(result.stdout.strip(), result.stderr)
        data = json.loads(result.stdout.strip().splitlines()[-1])
        (ARTIFACT / f"{scenario}.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        self.assertEqual(0, result.returncode, data)
        self.assertTrue(data["passed"], data)
        self.assertTrue(data["background"], data)
        self.assertEqual(0, data["visiblePlatformWindows"], data)
        return data

    def test_short_video_uses_one_bounded_ffmpeg_batch(self):
        self.run_scenario("batch", self.short)

    def test_8k_hevc_falls_back_to_software_when_cuda_is_unavailable(self):
        with tempfile.TemporaryDirectory(dir=ARTIFACT) as cache_root:
            self.run_scenario("cuda-fallback", ROOT / "tests/media/8k_60fps.mp4", cache_root)

    def test_4k_10bit_hevc_falls_back_when_cuda_is_unavailable(self):
        with tempfile.TemporaryDirectory(dir=ARTIFACT) as cache_root:
            self.run_scenario("cuda-fallback", ROOT / "tests/media/4k_60fps.mp4", cache_root)

    def test_long_hevc_retries_failed_hardware_once_per_job(self):
        with tempfile.TemporaryDirectory(dir=ARTIFACT) as folder:
            movie = Path(folder) / "long-hevc.mp4"
            subprocess.run([
                str(FFMPEG), "-v", "error", "-stream_loop", "3", "-i",
                str(ROOT / "tests/media/4k_60fps.mp4"), "-map", "0:v:0",
                "-c", "copy", str(movie),
            ], check=True, creationflags=NO_WINDOW, timeout=60)
            self.run_scenario("long-cuda-fallback", movie, Path(folder) / "cache")

    def test_multitrack_poster_and_timeline_follow_playback(self):
        for scenario, extension, disposition in (
            ("track-first", "mkv", "0"), ("track-default", "mp4", "default")
        ):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory(dir=ARTIFACT) as folder:
                path = Path(folder) / ("dual." + extension)
                subprocess.run([
                    str(FFMPEG), "-v", "error", "-f", "lavfi", "-i", "color=red:s=160x90:r=24",
                    "-f", "lavfi", "-i", "color=blue:s=320x180:r=24", "-t", "2",
                    "-map", "0:v", "-map", "1:v", "-c:v", "mpeg4",
                    "-disposition:v:0", "0", "-disposition:v:1", disposition, str(path),
                ], check=True, creationflags=NO_WINDOW, timeout=45)
                self.run_scenario(scenario, path, Path(folder) / "cache")

    def test_video_thumbnail_cache_reuses_batch_outputs(self):
        self.run_scenario("cache", self.cache)

    def test_canceled_batch_leaves_no_temporary_directory(self):
        self.run_scenario("cancel", self.long)

    def test_long_video_uses_bounded_seek_fallback(self):
        self.run_scenario("long", self.long)


if __name__ == "__main__":
    unittest.main(verbosity=2)
