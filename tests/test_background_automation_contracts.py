import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class BackgroundAutomationContracts(unittest.TestCase):
    def test_all_builtin_automation_modes_default_to_background(self):
        header = (ROOT / "src/ui/app/Application.h").read_text(encoding="utf-8")
        runtime = (ROOT / "src/ui/app/ApplicationRuntime.cpp").read_text(encoding="utf-8")

        for token in (
            "_captureUiMode",
            "_codexWorkbenchSmokeMode",
            "_playerSmokeMode",
            "_subtitleGenerationSmokeMode",
            "_subtitleCacheDisplaySmokeMode",
            "_subtitleSwitchSequenceSmokeMode",
            "_subtitleRefinedFallbackSmokeMode",
            "_qwenAsrProviderSmokeMode",
            "_recoveryPromptSmokeMode",
            "_dumpRuntimeMode",
            "_benchmarkMode",
            "_componentCheckMode",
            "_phase915ViewerRebuildMode",
            "_phase915PluginReloadMode",
            "_phase915FallbackToggleMode",
            "_phase11PerformanceBaselineMode",
            "_phase14PerformanceBaselineMode",
            "_phase14StressMode",
        ):
            self.assertIn(token, runtime)

        self.assertIn("bool isAutomationMode() const;", header)
        self.assertIn("isAutomationMode() && !_automationVisibleMode", runtime)
        self.assertIn('QStringLiteral("--automation-visible")', runtime)

    def test_top_level_windows_are_suppressed_before_show(self):
        runtime = (ROOT / "src/ui/app/ApplicationRuntime.cpp").read_text(encoding="utf-8")
        self.assertIn("bool Application::notify(QObject* receiver, QEvent* event)", runtime)
        self.assertIn("event->type() == QEvent::Show", runtime)
        self.assertIn("Qt::WA_DontShowOnScreen", runtime)
        self.assertIn("Qt::WA_ShowWithoutActivating", runtime)

    def test_player_smoke_launchers_enforce_background_evidence(self):
        powershell = (ROOT / "tools/run_player_smoke_one_click.ps1").read_text(encoding="utf-8")
        python = (ROOT / "tools/player_function_smoke.py").read_text(encoding="utf-8")
        automation = (ROOT / "src/ui/app/ApplicationRuntimeAutomation.cpp").read_text(encoding="utf-8")

        for source in (powershell, python):
            self.assertIn("--automation-background", source)
            self.assertIn("visiblePlatformWindows", source)
        self.assertIn("CREATE_NO_WINDOW", python)
        self.assertIn('report["automation"]', automation)
        self.assertIn("hiddenTopLevelWindows", automation)


if __name__ == "__main__":
    unittest.main()
