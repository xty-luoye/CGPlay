import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing function: {signature}")
    brace = source.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing body: {signature}")
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:index + 1]
    raise AssertionError(f"unterminated body: {signature}")


class MediaCloseReleaseContractTests(unittest.TestCase):
    def test_secondary_window_releases_timeline_and_player_before_emit(self):
        source = (ROOT / "src/ui/app/SecondaryWindow.cpp").read_text(encoding="utf-8")
        body = _function_body(source, "void SecondaryWindow::closeEvent")
        self.assertIn("_timeline->setMediaPath({})", body)
        self.assertIn("_playbackCtrl->closeFile()", body)
        self.assertLess(
            body.index("_timeline->setMediaPath({})"),
            body.index("_playbackCtrl->closeFile()"),
        )
        self.assertLess(body.index("_playbackCtrl->closeFile()"), body.index("Q_EMIT closed"))

    def test_main_window_releases_current_media_before_state_save(self):
        source = (ROOT / "src/ui/app/ApplicationInput.cpp").read_text(encoding="utf-8")
        body = _function_body(source, "void MainWindow::closeEvent")
        self.assertIn("_closeCurrentMedia();", body)
        self.assertLess(body.index("_closeCurrentMedia();"), body.index("_saveState();"))

    def test_player_smoke_closes_main_window_before_quit(self):
        source = (ROOT / "src/ui/app/ApplicationRuntimeAutomation.cpp").read_text(encoding="utf-8")
        body = _function_body(source, "int Application::_runPlayerSmokeTest")
        self.assertIn("_mainWindow->close();", body)
        self.assertIn("QCoreApplication::processEvents(QEventLoop::AllEvents, 500);", body)
        self.assertLess(body.index("_mainWindow->close();"), body.rindex("quit();"))


if __name__ == "__main__":
    unittest.main()
