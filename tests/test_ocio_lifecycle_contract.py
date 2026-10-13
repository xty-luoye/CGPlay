from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class OcioLifecycleContractTests(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_alpha_action_resolves_rebuilt_viewer_at_use_time(self) -> None:
        source = self.read("src/plugins/ocio/OcioPlugin.cpp")
        self.assertIn("_resolveViewerObject()", source)
        self.assertIn("_bindAlphaActionToViewer()", source)
        self.assertNotIn("[viewer](bool checked)", source)

    def test_exr_ocio_override_is_cleared_when_opening_other_media(self) -> None:
        manager = self.read("src/services/ocio/OcioManager.cpp")
        media = self.read("src/ui/app/ApplicationMedia.cpp")
        self.assertIn("exrOverrideActive", manager)
        self.assertIn("void OcioManager::clearExrSceneLinearDefaults()", manager)
        self.assertIn("clearExrSceneLinearDefaults()", media)
        reset = media.split("void MainWindow::_resetMediaSession", 1)[1].split("void MainWindow::_closeCurrentMedia", 1)[0]
        self.assertIn("clearExrSceneLinearDefaults()", reset)
        self.assertIn("++_p->mediaGenerationId", reset)
        self.assertIn("generation != _p->mediaGenerationId", media)
        self.assertIn("_p->currentPath != path", media)
        self.assertIn("!_p->playbackCtrl->isValid()", media)
        self.assertIn("_resetMediaSession({}, {});", media)

    def test_subtitle_delayed_work_is_media_scoped(self) -> None:
        subtitles = self.read("src/ui/app/ApplicationSubtitles.cpp")
        enhancement = self.read("src/ui/app/ApplicationEnhancement.cpp")
        self.assertIn("QTimer::singleShot(std::max(0, delayMs), this, [this, generation]", subtitles)
        self.assertIn("QTimer::singleShot(kSubtitleRefinementIdleRetryMs, this, [this, mediaPath, generation]", subtitles)
        self.assertIn("QTimer::singleShot(2000, this, [this, mediaPath, requestedGeneration]", subtitles)
        self.assertIn("subtitleRefinementWatcher != watcherPtr", subtitles)
        self.assertIn("if (requestedGeneration != _p->mediaGenerationId) return;", subtitles)
        self.assertIn("if (mediaGenerationId != _p->mediaGenerationId) return;", enhancement)
        self.assertIn("if (requestedMediaGenerationId != _p->mediaGenerationId) return;", enhancement)


if __name__ == "__main__":
    unittest.main()
