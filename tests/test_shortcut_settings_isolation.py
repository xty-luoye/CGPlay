import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class ShortcutSettingsIsolationContracts(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_default_frame_shortcuts_are_bidirectional(self) -> None:
        app = self.read("src/ui/app/Application.cpp")
        self.assertIn('QStringLiteral("playback.previousFrame")', app)
        self.assertIn('QStringLiteral("skip-back"), QStringLiteral("Left")', app)
        self.assertIn('QStringLiteral("playback.nextFrame")', app)
        self.assertIn('QStringLiteral("skip-forward"), QStringLiteral("Right")', app)

    def test_automation_namespace_is_opt_in_and_sanitized(self) -> None:
        runtime = self.read("src/ui/app/ApplicationRuntime.cpp")
        self.assertIn('arg == QStringLiteral("--automation-settings-namespace")', runtime)
        self.assertIn("!_automationSettingsNamespace.isEmpty() && isAutomationMode()", runtime)
        self.assertIn('QStringLiteral("CGPlayAutomation_%1")', runtime)
        self.assertIn('QStringLiteral("[^A-Za-z0-9_.-]")', runtime)

    def test_customization_acceptance_never_targets_real_settings(self) -> None:
        script = self.read("tools/run_customization_acceptance.py")
        self.assertIn('SETTINGS_NAMESPACE = "CustomizationAcceptance"', script)
        self.assertIn('REG_PATH = rf"Software\\CGPlay\\{SETTINGS_APPLICATION}"', script)
        self.assertIn('["--automation-settings-namespace", SETTINGS_NAMESPACE, *args]', script)
        self.assertNotIn('REG_PATH = r"Software\\CGPlay\\CGPlay"', script)


if __name__ == "__main__":
    unittest.main()
