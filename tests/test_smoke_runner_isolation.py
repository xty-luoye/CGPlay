import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def load_python_runner():
    path = ROOT / "tools/player_function_smoke.py"
    spec = importlib.util.spec_from_file_location("player_function_smoke", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SmokeRunnerIsolationContracts(unittest.TestCase):
    def test_python_runner_uses_isolated_deterministic_namespace(self) -> None:
        runner = load_python_runner()
        first = runner.settings_namespace(ROOT / "tests/artifacts/run one/report.json", pid=1234)
        second = runner.settings_namespace(ROOT / "tests/artifacts/run one/report.json", pid=1234)
        other = runner.settings_namespace(ROOT / "tests/artifacts/run two/report.json", pid=1234)

        self.assertEqual(first, second)
        self.assertNotEqual(first, other)
        self.assertLessEqual(len(first), 64)
        self.assertTrue(first.startswith("player_smoke_1234_"))
        self.assertNotIn(" ", first)

    def test_both_launchers_pass_background_and_namespace(self) -> None:
        python = (ROOT / "tools/player_function_smoke.py").read_text(encoding="utf-8")
        powershell = (ROOT / "tools/run_player_smoke_one_click.ps1").read_text(encoding="utf-8")

        for source in (python, powershell):
            self.assertIn("--automation-background", source)
            self.assertIn("--automation-settings-namespace", source)
            self.assertIn("visiblePlatformWindows", source)
        self.assertIn("settings_namespace(args.output)", python)
        self.assertIn("New-SettingsNamespace -ReportPath $ReportPath", powershell)

    def test_launchers_do_not_target_normal_cgplay_settings(self) -> None:
        for relative in ("tools/player_function_smoke.py", "tools/run_player_smoke_one_click.ps1"):
            source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertNotIn("Software\\CGPlay\\CGPlay", source)


if __name__ == "__main__":
    unittest.main()
