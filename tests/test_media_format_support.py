#!/usr/bin/env python3
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class MediaFormatSupportContracts(unittest.TestCase):
    def test_common_video_extensions_have_one_source_of_truth(self) -> None:
        probe = read("src/services/media/MediaProbe.cpp")
        for extension in (
            "mp4", "mov", "m4v", "mxf", "avi", "mkv", "webm", "flv",
            "wmv", "asf", "ts", "m2ts", "mts", "mpg", "mpeg", "vob",
            "ogv", "3gp", "3g2", "rmvb", "wtv", "dv", "y4m", "ivf",
        ):
            self.assertIn(f'QStringLiteral("{extension}")', probe)

        self.assertIn("MediaProbe::mediaFileDialogFilter()", read("src/ui/app/Application.cpp"))
        self.assertIn("MediaProbe::mediaFileDialogFilter()", read("src/features/playlist/PlaylistPanel.cpp"))
        self.assertIn("MediaProbe::isVideoPath(path)", read("src/features/playlist/PlaylistModel.cpp"))

    def test_ocio_scene_linear_is_default_for_unlabelled_exr(self) -> None:
        ocio = read("src/services/ocio/OcioManager.cpp")
        self.assertIn('QString input   = "scene_linear";', ocio)
        self.assertIn('Keep the scene-linear role as the safe default for EXR', ocio)
        self.assertNotIn('csName.contains("ACEScg", Qt::CaseInsensitive)', ocio)
        self.assertIn("applyExrSceneLinearDefaults", ocio)
        self.assertIn("_p->userEnabled = true", ocio)
        self.assertIn('scene_linear', ocio)
        self.assertIn('config.ocio', ocio)
        self.assertIn('getRoleColorSpace(OCIO::ROLE_SCENE_LINEAR)', ocio)
        self.assertIn('view.compare(QStringLiteral("sRGB"), Qt::CaseInsensitive)', ocio)
        self.assertIn('4 * sizeof(float)', ocio)
        self.assertIn('rgba.bytesPerLine()', ocio)
        stubs = read("src/common/core/TlStubs.cpp")
        self.assertIn('enabled == o.enabled', stubs)
        self.assertIn('channels == o.channels', stubs)
        media = read("src/ui/app/ApplicationMedia.cpp")
        application = read("src/ui/app/Application.cpp")
        self.assertIn('incomingSuffix == QStringLiteral("exr")', media)
        self.assertIn("applyExrSceneLinearDefaults", media)
        self.assertIn('QStringLiteral("View: %1")', application)
        self.assertIn('cgplay.automationBackground', media)

    def test_float_exr_alpha_does_not_multiply_scene_rgb(self) -> None:
        tlrender = Path(r"C:\Users\1\Desktop\CGV\tlRender-main\lib\tlRender\GL\RenderVideo.cpp")
        if not tlrender.exists():
            self.skipTest("tlRender source tree is not present")
        source = tlrender.read_text(encoding="utf-8")
        self.assertIn("normalizeFloatAlpha", source)
        self.assertIn("ImageType::RGBA_F16", source)
        self.assertIn("ImageType::RGBA_F32", source)
        self.assertIn("options.alphaBlend = ftk::AlphaBlend::None", source)

    def test_tlrender_alias_patch_is_reproducible(self) -> None:
        patch = read("cmake/patches/tlrender_ffmpeg_common_extensions.patch")
        for extension in (".wmv", ".asf", ".ts", ".m2ts", ".mts", ".mpg", ".mpeg", ".vob"):
            self.assertIn(f'"{extension}"', patch)
        build = read("build_release.bat")
        self.assertIn("apply_tlrender_patches.ps1", build)

    def test_vp_hint_precedes_first_timeline_creation(self) -> None:
        playback = read("src/core/playback/PlaybackController.cpp")
        create_player = playback.index("void PlaybackController::_createPlayer")
        hint = playback.index("const bool hintedVpFamily", create_player)
        first_timeline = playback.index("auto timeline = tl::Timeline::create", create_player)
        self.assertLess(hint, first_timeline)
        self.assertIn("vpFamily && !hintedVpFamily", playback)

    def test_video_open_does_not_scan_numbered_sequence_directory(self) -> None:
        timeline = read("src/features/timeline/TimelineWidget.cpp")
        self.assertIn("MediaProbe::isStillImagePath(path)\n        ? MediaProbe::collectSequenceFiles(path)", timeline)
        self.assertIn("kThumbnailStartDelayMs = 500", timeline)
        self.assertIn("kThumbnailStartDelayMs = 500", read("src/features/playlist/PlaylistModel.cpp"))

    def test_png_encoded_mov_keeps_video_container_identity(self) -> None:
        probe = read("src/services/media/MediaProbe.h")
        source = read("src/services/media/MediaProbe.cpp")
        playback = read("src/core/playback/PlaybackController.cpp")
        enhancement = read("src/ui/app/ApplicationEnhancement.cpp")
        performance = read("src/ui/app/PerformanceService.cpp")
        matrix = read("tools/run_player_format_matrix.ps1")

        self.assertIn("QString containerFormat;", probe)
        self.assertIn('format=format_name,format_long_name', source)
        self.assertIn("QString MediaInfo::codecDisplayText() const", source)
        self.assertIn('QStringLiteral(" / ")', source)
        self.assertIn("bool isPngEncodedMov", playback)
        self.assertIn('return codec == QStringLiteral("png");', playback)
        self.assertIn('QStringLiteral("playback.png-mov-transcode")', playback)
        self.assertIn("media.codecDisplayText()", enhancement)
        self.assertIn("mediaInfo.codecDisplayText()", performance)
        self.assertIn('Name="png_mov"', matrix)

    def test_png_still_and_png_sequence_paths_remain_image_paths(self) -> None:
        probe = read("src/services/media/MediaProbe.cpp")
        timeline = read("src/features/timeline/TimelineWidget.cpp")
        thumbnail = read("src/services/media/ThumbnailService.cpp")
        self.assertIn('ext == "png"', probe)
        self.assertIn("MediaProbe::isStillImagePath(path)\n        ? MediaProbe::collectSequenceFiles(path)", timeline)
        self.assertIn("if (MediaProbe::isStillImagePath(path))", thumbnail)

    def test_normal_startup_keeps_codex_and_legacy_ai_lazy(self) -> None:
        runtime = read("src/ui/app/ApplicationRuntime.cpp")
        self.assertIn("eagerCodexWorkspace = _codexWorkbenchSmokeMode", runtime)
        self.assertIn("CodexLazyLoadButton", runtime)
        self.assertIn("QTimer::singleShot(0, loadButton", runtime)
        self.assertIn("if (codexDock->isVisible() && loadButton->isEnabled()) loadButton->click();", runtime)
        self.assertIn('QStringLiteral("main.ready")', runtime)

        application = read("src/ui/app/Application.cpp")
        self.assertIn("const auto ensureWorkspace", application)
        self.assertNotIn("codexUnavailable", application)
        self.assertIn("_p->aiDock->hide();", application)

    def test_startup_benchmark_is_isolated_and_has_latency_gates(self) -> None:
        runtime = read("src/ui/app/ApplicationRuntime.cpp")
        self.assertIn("return _backgroundAutomationMode ||", runtime)

        benchmark = read("tools/run_startup_benchmark.ps1")
        self.assertIn('"--automation-settings-namespace"', benchmark)
        self.assertIn("MaxP50Ms", benchmark)
        self.assertIn("MaxP95Ms", benchmark)
        self.assertIn("performancePass", benchmark)

    def test_format_matrix_covers_wmv3_vc1_and_safe_rejection(self) -> None:
        matrix = read("tools/run_player_format_matrix.ps1")
        self.assertIn("buggyDMOdecoding.wmv", matrix)
        self.assertIn("wmv3.avi", matrix)
        self.assertIn("glitch-ffvc1.avi", matrix)
        self.assertIn("SafeRejectPass", matrix)
        self.assertIn('$Open.message -eq "player invalid"', matrix)
        self.assertEqual(matrix.count("ExpectedSha256="), 3)
        self.assertIn("$MediaHashPass -and $SafeRejectPass", matrix)
        self.assertIn("ForEach-Object { [string]$_.name }", matrix)


if __name__ == "__main__":
    unittest.main()
