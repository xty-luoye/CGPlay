import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APP_DIR = ROOT / "src" / "ui" / "app"


class ApplicationRuntimeSplitContracts(unittest.TestCase):
    def test_runtime_translation_units_stay_below_owner_limit(self):
        sources = sorted(APP_DIR.glob("ApplicationRuntime*.cpp"))
        self.assertGreaterEqual(len(sources), 3)
        for source in sources:
            line_count = len(source.read_text(encoding="utf-8").splitlines())
            self.assertLessEqual(line_count, 3000, f"{source.name}: {line_count} lines")

    def test_automation_entry_points_have_one_owner(self):
        automation = (APP_DIR / "ApplicationRuntimeAutomation.cpp").read_text(encoding="utf-8")
        all_sources = "\n".join(
            source.read_text(encoding="utf-8") for source in APP_DIR.glob("*.cpp")
        )
        methods = (
            "_runComponentCheck",
            "_captureRuntimeDump",
            "_writeRuntimeDump",
            "_runRuntimeDump",
            "_runPlaybackBenchmark",
            "_runUiCapture",
            "_runCodexWorkbenchSmoke",
            "_runRecoveryPromptSmoke",
            "_runQwenAsrProviderSmoke",
            "_runPlayerSmokeTest",
            "_runSubtitleGenerationSmoke",
            "_runSubtitleCacheDisplaySmoke",
            "_runSubtitleSwitchSequenceSmoke",
            "_runSubtitleRefinedFallbackSmoke",
            "_runPhase915ViewerRebuild",
            "_runPhase915PluginReload",
            "_runPhase915FallbackToggle",
            "_runPhase11PerformanceBaseline",
            "_runPhase14PerformanceBaseline",
            "_runPhase14Stress",
        )
        for method in methods:
            signature = f"Application::{method}("
            self.assertEqual(automation.count(signature), 1, method)
            self.assertEqual(all_sources.count(signature), 1, method)

    def test_runtime_support_has_one_implementation_per_helper(self):
        support = (APP_DIR / "ApplicationRuntimeSupport.cpp").read_text(encoding="utf-8")
        non_support = "\n".join(
            source.read_text(encoding="utf-8")
            for source in sorted(APP_DIR.glob("ApplicationRuntime*.cpp"))
            if source.name != "ApplicationRuntimeSupport.cpp"
        )
        helpers = (
            "defaultPhase915MediaPath",
            "defaultPhase14MediaPath",
            "deriveRuntimeDumpPath",
            "baselineScenario",
            "writeJsonObjectFile",
            "reportHasFailures",
            "deriveSiblingArtifactPath",
            "saveWindowEvidence",
        )
        for helper in helpers:
            support_definitions = re.findall(
                rf"^[^\n;]*\b{re.escape(helper)}\([^;\n]*\)\s*\n\{{",
                support,
                flags=re.MULTILINE,
            )
            non_support_definitions = re.findall(
                rf"^[^\n;]*\b{re.escape(helper)}\([^;\n]*\)\s*\n\{{",
                non_support,
                flags=re.MULTILINE,
            )
            self.assertEqual(len(support_definitions), 1, helper)
            self.assertEqual(non_support_definitions, [], helper)

    def test_cmake_owns_real_translation_units_without_inc_inclusion(self):
        cmake = (ROOT / "src" / "CMakeLists.txt").read_text(encoding="utf-8")
        source_list = re.search(r"set\(CGPLAY_SOURCES\s+(.*?)\n\)", cmake, re.DOTALL)
        self.assertIsNotNone(source_list)
        for source in (
            "ui/app/ApplicationRuntime.cpp",
            "ui/app/ApplicationRuntimeAutomation.cpp",
            "ui/app/ApplicationRuntimeSupport.cpp",
        ):
            self.assertEqual(source_list.group(1).count(source), 1, source)

        for source in APP_DIR.glob("ApplicationRuntime*.cpp"):
            text = source.read_text(encoding="utf-8")
            self.assertNotRegex(text, r'#\s*include\s*[<\"][^>\"]*\.(?:inc|cpp)[>\"]')

    def test_runtime_report_writes_are_atomic_and_reject_short_writes(self):
        support = (APP_DIR / "ApplicationRuntimeSupport.cpp").read_text(encoding="utf-8")
        self.assertIn("QSaveFile out(outputPath)", support)
        self.assertRegex(support, r"out\.write\(payload\)\s*!=\s*payload\.size\(\)")
        self.assertIn("out.cancelWriting()", support)
        self.assertIn("return out.commit()", support)


if __name__ == "__main__":
    unittest.main()
