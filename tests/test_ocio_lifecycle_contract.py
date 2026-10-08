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
        self.assertIn("incomingSuffix != QStringLiteral(\"exr\")", media)


if __name__ == "__main__":
    unittest.main()
