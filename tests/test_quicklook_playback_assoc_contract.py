from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class QuickLookPlaybackAssociationContractTests(unittest.TestCase):
    def test_space_hook_preserves_explorer_text_input_and_uses_shared_video_list(self):
        app = (ROOT / "src/plugins/quicklook/QuickLookApp.cpp").read_text(encoding="utf-8")
        selection = (ROOT / "src/plugins/quicklook/ExplorerSelection.cpp").read_text(encoding="utf-8")
        self.assertIn("isExplorerTextInputFocused()", app)
        self.assertIn("explorerTextInput", app)
        self.assertIn("MediaProbe::videoExtensions()", app)
        self.assertIn("GetGUIThreadInfo", selection)
        self.assertIn('QStringLiteral("Edit")', selection)

    def test_quicklook_autoplay_waits_for_player_ready(self):
        header = (ROOT / "src/plugins/quicklook/PreviewWindow.h").read_text(encoding="utf-8")
        source = (ROOT / "src/plugins/quicklook/PreviewWindow.cpp").read_text(encoding="utf-8")
        self.assertIn("bool _autoPlayPending = false;", header)
        self.assertIn("playerReady", source)
        self.assertIn("_autoPlayPending", source)
        self.assertIn("_playback->isValid()", source)

    def test_main_window_autoplay_is_bound_to_video_open(self):
        private = (ROOT / "src/ui/app/ApplicationPrivate.h").read_text(encoding="utf-8")
        media = (ROOT / "src/ui/app/ApplicationMedia.cpp").read_text(encoding="utf-8")
        app = (ROOT / "src/ui/app/Application.cpp").read_text(encoding="utf-8")
        self.assertIn("bool autoPlayPending = false;", private)
        self.assertIn("_p->autoPlayPending = mediaInfo.isVideo", media)
        self.assertIn("PlaybackServiceSignals::playerReady", app)
        self.assertIn("_p->playbackCtrl->play()", app)
        automation = (ROOT / "src/ui/app/MainWindowAutomationMedia.cpp").read_text(encoding="utf-8")
        self.assertIn("autoPlayPass", automation)
        self.assertIn("playbackState() != 0", automation)

    def test_installer_registers_default_apps_and_video_extensions(self):
        installer = (ROOT / "installer/CGPlay_Installer.nsi").read_text(encoding="utf-8")
        self.assertIn('"Software\\RegisteredApplications" "CGPlay"', installer)
        self.assertIn('"Software\\Classes\\CGPlay.Video\\shell\\open\\command"', installer)
        self.assertIn("!insertmacro RegisterVideoExtension mp4", installer)
        self.assertIn("!insertmacro RegisterVideoExtension mov", installer)
        self.assertIn("!insertmacro RegisterVideoExtension mkv", installer)
        self.assertIn("PerceivedType", installer)
        self.assertIn("OpenWithProgids", installer)


if __name__ == "__main__":
    unittest.main()
