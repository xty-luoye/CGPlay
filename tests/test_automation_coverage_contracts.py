import json
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
COMMAND_PREFIXES = ("playback", "audio", "view", "translation", "annotation", "compare", "codex", "export", "workspace")


def registered_commands() -> dict[str, str]:
    source = (ROOT / "src/ui/app/Application.cpp").read_text(encoding="utf-8")
    load = source.split("void MainWindow::_loadHostExtensionContributions()", 1)[1]
    load = load.split("void MainWindow::_applyMenuContributions", 1)[0]
    commands: dict[str, str] = {}
    prefix = "(?:" + "|".join(COMMAND_PREFIXES) + ")"
    pattern = rf'\{{QStringLiteral\("({prefix}\.[^\"]+)"\)([^\n]*)'
    for match in re.finditer(pattern, load):
        values = re.findall(r'QStringLiteral\("([^"]*)"\)', match.group(0))
        shortcut = re.search(
            r'\{\},\s*\{\},\s*(?:QStringLiteral\("[^"]*"\)|\{\}),\s*QStringLiteral\("([^"]*)"\)',
            match.group(0),
        )
        commands[values[0]] = shortcut.group(1) if shortcut else ""

    for plugin in (ROOT / "src/plugins").glob("**/*Plugin.cpp"):
        text = plugin.read_text(encoding="utf-8")
        pattern = r'CommandDescriptor\s*\{\s*QStringLiteral\("([^"]+)"\)(.*?)\n\s*\}'
        for match in re.finditer(pattern, text, re.DOTALL):
            values = re.findall(r'QStringLiteral\("([^"]*)"\)', match.group(0))
            commands[values[0]] = values[5] if len(values) >= 6 else ""
    return commands


class AutomationCoverageContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = json.loads((ROOT / "tests/automation_coverage_manifest.json").read_text(encoding="utf-8"))
        cls.registered = registered_commands()

    def test_manifest_policy_is_strict(self) -> None:
        self.assertEqual(self.manifest["policy"], {
            "requireEveryRegisteredCommand": True,
            "requireEveryDefaultShortcut": True,
            "allowManual": False,
        })

    def test_every_registered_command_has_automated_evidence(self) -> None:
        coverage = self.manifest["commands"]
        self.assertEqual(set(self.registered), set(coverage))
        for command_id, item in coverage.items():
            self.assertTrue(item.get("test", "").strip(), command_id)
            self.assertNotIn("manual", item["test"].lower(), command_id)

    def test_every_default_shortcut_has_a_shortcut_test(self) -> None:
        coverage = self.manifest["commands"]
        missing = {command_id: shortcut for command_id, shortcut in self.registered.items()
                   if shortcut and not coverage[command_id].get("shortcutTest", "").strip()}
        self.assertEqual({}, missing)
        for command_id, shortcut in self.registered.items():
            if shortcut:
                self.assertIn(shortcut.casefold(), coverage[command_id]["shortcutTest"].casefold(), command_id)

    def test_player_smoke_rejects_manual_and_uncovered_features(self) -> None:
        automation = (ROOT / "src/ui/app/MainWindowAutomation.cpp").read_text(encoding="utf-8")
        launcher = (ROOT / "tools/player_function_smoke.py").read_text(encoding="utf-8")
        self.assertNotRegex(automation, r'\{\s*"manual"\s*,\s*[1-9]')
        self.assertIn('"manual", 0', automation)
        self.assertIn('report["coverage"]', automation)
        self.assertIn('uncoveredCommands', automation)
        self.assertIn('uncoveredShortcuts', automation)
        self.assertIn('summary.get("manual", 0) == 0', launcher)
        self.assertIn('coverage.get("complete") is True', launcher)
        powershell = (ROOT / "tools/run_player_smoke_one_click.ps1").read_text(encoding="utf-8")
        self.assertIn("$ManualCount -ne 0", powershell)
        self.assertIn("-not $CoverageComplete", powershell)


if __name__ == "__main__":
    unittest.main()
