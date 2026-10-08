import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class VpFamilyPlaybackContracts(unittest.TestCase):
    def source(self) -> str:
        return (ROOT / "src/core/playback/PlaybackController.cpp").read_text(encoding="utf-8")

    def test_vp_family_uses_targeted_rgb_compatibility_path(self) -> None:
        source = self.source()
        for codec in ("vp3", "vp4", "vp5", "vp6", "vp6a", "vp6f", "vp7", "vp8", "vp9", "av1"):
            self.assertIn(f'"{codec}"', source)
        self.assertIn('tags.find("Video Codec")', source)
        self.assertIn('tlOpts.ioOptions["FFmpeg/YUVToRGB"] = "1";', source)
        self.assertIn('tlOpts.ioOptions["FFmpeg/ThreadCount"] = "4";', source)
        self.assertIn("if (hintedVpFamily)", source)
        self.assertIn("if (vpFamily && !hintedVpFamily)", source)

    def test_codec_hint_applies_rgb_options_before_first_timeline(self) -> None:
        source = self.source()
        hint_guard_index = source.index("if (hintedVpFamily)")
        option_index = source.index(
            'tlOpts.ioOptions["FFmpeg/YUVToRGB"] = "1";', hint_guard_index
        )
        timeline_index = source.index("auto timeline = tl::Timeline::create")
        self.assertLess(hint_guard_index, option_index)
        self.assertLess(option_index, timeline_index)

    def test_rgb_fallback_is_guarded_and_does_not_retry_hinted_vp(self) -> None:
        source = self.source()
        timeline_index = source.index("auto timeline = tl::Timeline::create")
        fallback_guard_index = source.index("if (vpFamily && !hintedVpFamily)")
        fallback_timeline_index = source.index(
            "timeline = tl::Timeline::create", fallback_guard_index
        )
        player_index = source.index("auto player = tl::Player::create", fallback_guard_index)
        self.assertGreater(fallback_guard_index, timeline_index)
        self.assertGreater(fallback_timeline_index, fallback_guard_index)
        self.assertLess(fallback_timeline_index, player_index)

    def test_tlrender_vp_decoders_use_slice_threading(self) -> None:
        patch = (ROOT / "cmake/patches/tlrender_vp_slice_threading.patch").read_text(encoding="utf-8")
        for codec in ("VP3", "VP4", "VP5", "VP6", "VP6A", "VP6F", "VP7", "VP8", "VP9"):
            self.assertIn(f"AV_CODEC_ID_{codec}", patch)
        self.assertIn("FF_THREAD_SLICE", patch)

        tlrender_source = ROOT.parent / "CGV/tlRender-main/lib/tlRender/IO/FFmpegReadVideo.cpp"
        if not tlrender_source.exists():
            self.skipTest("tlRender source checkout is not available")
        source = tlrender_source.read_text(encoding="utf-8")
        self.assertIn("case AV_CODEC_ID_VP9:", source)
        self.assertIn("thread_type = FF_THREAD_SLICE;", source)


if __name__ == "__main__":
    unittest.main()
