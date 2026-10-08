"""Contracts for extracting and presenting video file bitrate metadata."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MediaBitrateContracts(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_probe_requests_format_and_stream_bitrate(self) -> None:
        header = self.read("src/services/media/MediaProbe.h")
        source = self.read("src/services/media/MediaProbe.cpp")
        self.assertIn("qint64 bitrateBitsPerSecond", header)
        self.assertIn("QString bitrateText(const QString& unit", header)
        self.assertIn("stream=index,codec_type,width,height,codec_name,pix_fmt,avg_frame_rate,r_frame_rate,nb_frames,duration,bit_rate", source)
        self.assertIn("format=format_name,format_long_name,bit_rate", source)
        self.assertIn("bitrateBitsPerSecond", source)
        self.assertIn("format.value(QStringLiteral(\"bit_rate\"))", source)
        self.assertIn("stream.value(QStringLiteral(\"bit_rate\"))", source)

    def test_display_has_stable_units_and_unknown_fallback(self) -> None:
        source = self.read("src/services/media/MediaProbe.cpp")
        self.assertIn("QString MediaInfo::bitrateText(const QString& unit) const", source)
        self.assertIn('QStringLiteral("--")', source)
        self.assertIn('QStringLiteral("%1 Mbps")', source)
        self.assertIn('QStringLiteral("%1 kbps")', source)
        self.assertIn('normalizedUnit == QStringLiteral("mbps")', source)
        self.assertIn('normalizedUnit != QStringLiteral("kbps")', source)

    def test_status_bar_updates_and_resets_bitrate(self) -> None:
        performance = self.read("src/ui/app/PerformanceService.h") + self.read("src/ui/app/PerformanceService.cpp")
        application = self.read("src/ui/app/Application.cpp")
        media = self.read("src/ui/app/ApplicationMedia.cpp")
        automation = self.read("src/ui/app/MainWindowAutomationMedia.cpp")
        self.assertIn("QLabel* bitrate", performance)
        self.assertIn('QStringLiteral("码率: %1")', performance)
        self.assertIn('QStringLiteral("码率: --")', performance)
        self.assertIn("lblBitrate", application)
        self.assertIn("mediaInfo.bitrateText(bitrateUnit)", media)
        self.assertIn('playback/bitrateUnit', application)
        self.assertIn('SettingsBitrateUnit', self.read("src/ui/app/ApplicationSettings.cpp"))
        self.assertIn('setBitrateDisplayUnit', performance)
        self.assertIn('bitrateDisplayUnit()', performance)
        self.assertIn('Settings bitrate unit display and persistence', self.read("src/ui/app/MainWindowAutomationSettings.cpp"))
        self.assertIn("bitrateLabelPresent", automation)

    def test_background_coverage_registers_media_bitrate_feature(self) -> None:
        manifest = self.read("tests/automation_coverage_manifest.json")
        self.assertIn('"media.bitrateInfo"', manifest)
        self.assertIn("player-smoke:open media bitrate label", manifest)


if __name__ == "__main__":
    unittest.main()
