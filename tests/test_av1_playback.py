import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class Av1PlaybackContracts(unittest.TestCase):
    def test_av1_uses_explicit_rgb_compatibility_path(self):
        source = (ROOT / "src/core/playback/PlaybackController.cpp").read_text(encoding="utf-8")
        self.assertIn("isAv1Codec", source)
        self.assertIn('codec == QStringLiteral("av1")', source)
        self.assertIn('codec == QStringLiteral("av01")', source)
        self.assertIn("const bool hintedAv1", source)
        self.assertIn("if (av1 && !hintedAv1)", source)
        self.assertIn('tlOpts.ioOptions["FFmpeg/YUVToRGB"] = "1";', source)

    def test_av1_has_non_black_h264_fallback_cache(self):
        source = (ROOT / "src/core/playback/PlaybackController.cpp").read_text(encoding="utf-8")
        self.assertIn("ensureAv1Cache", source)
        self.assertIn('QStringLiteral("playback.av1-transcode")', source)
        self.assertIn('QStringLiteral("libx264")', source)
        self.assertIn('QStringLiteral("yuv420p")', source)
        self.assertIn('QStringLiteral("AV1 playback ready")', source)

    def test_matrix_covers_high_resolution_mp4_and_ten_bit(self):
        matrix = (ROOT / "tools/run_player_format_matrix.ps1").read_text(encoding="utf-8")
        for sample in ("av1_mp4", "av1_mp4_10bit", "av1_mkv_10bit"):
            self.assertIn(f'Name="{sample}"', matrix)
        self.assertIn('Size="2560x1080"', matrix)
        self.assertIn('"yuv420p10le"', matrix)


if __name__ == "__main__":
    unittest.main()
