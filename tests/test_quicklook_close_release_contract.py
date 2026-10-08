from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PREVIEW_CPP = ROOT / "src" / "plugins" / "quicklook" / "PreviewWindow.cpp"
PREVIEW_H = ROOT / "src" / "plugins" / "quicklook" / "PreviewWindow.h"


class QuickLookCloseReleaseContractTests(unittest.TestCase):
    def test_close_preview_releases_player_and_invalidates_queued_open(self):
        source = PREVIEW_CPP.read_text(encoding="utf-8")

        close_start = source.index("void PreviewWindow::closePreview()")
        close_end = source.index("void PreviewWindow::setStatusText", close_start)
        close_body = source[close_start:close_end]
        self.assertIn("++_openGeneration", close_body)
        self.assertIn("_playback->closeFile()", close_body)
        self.assertNotIn("_playback->stop()", close_body)

        open_start = source.index("void PreviewWindow::openFile")
        open_end = source.index("void PreviewWindow::togglePlayback", open_start)
        open_body = source[open_start:open_end]
        self.assertIn("const quint64 openGeneration = ++_openGeneration", open_body)
        self.assertIn("openGeneration != _openGeneration", open_body)

    def test_preview_window_owns_generation_guard(self):
        header = PREVIEW_H.read_text(encoding="utf-8")
        self.assertIn("quint64 _openGeneration = 0;", header)


if __name__ == "__main__":
    unittest.main()
