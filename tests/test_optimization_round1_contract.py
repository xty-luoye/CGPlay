"""Regression contracts for the first playback optimization round."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class OptimizationRound1ContractTests(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_thumbnail_cache_includes_file_identity(self) -> None:
        source = self.read("src/services/media/ThumbnailService.cpp")
        self.assertIn("fileInfo.size()", source)
        self.assertIn("fileInfo.lastModified().toMSecsSinceEpoch()", source)

    def test_video_thumbnail_processes_are_bounded(self) -> None:
        source = self.read("src/services/media/ThumbnailService.cpp")
        self.assertIn("QSemaphore", source)
        self.assertIn("QSemaphore thumbnailProcessSlots(2)", source)
        self.assertIn("thumbnailProcessSlots.release()", source)

    def test_status_bar_is_compact_at_minimum_window(self) -> None:
        source = self.read("src/ui/app/Application.cpp")
        self.assertIn("primary ? 76 : 58", source)
        self.assertIn("primary ? 104 : 92", source)
        self.assertIn('font-size:10px', source)
        self.assertIn("lblMeter->setMinimumWidth(145)", source)


if __name__ == "__main__":
    unittest.main()
