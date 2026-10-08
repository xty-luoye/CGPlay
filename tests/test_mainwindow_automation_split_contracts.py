import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APP_DIR = ROOT / "src" / "ui" / "app"


class MainWindowAutomationSplitContracts(unittest.TestCase):
    def test_translation_units_stay_below_owner_limit(self):
        sources = sorted(APP_DIR.glob("MainWindowAutomation*.cpp"))
        self.assertEqual(
            ["MainWindowAutomation.cpp", "MainWindowAutomationFullscreen.cpp", "MainWindowAutomationMedia.cpp", "MainWindowAutomationSettings.cpp", "MainWindowAutomationSubtitles.cpp"],
            [source.name for source in sources],
        )
        for source in sources:
            line_count = len(source.read_text(encoding="utf-8").splitlines())
            self.assertLessEqual(line_count, 3000, f"{source.name}: {line_count} lines")

    def test_each_automation_entry_point_has_one_owner(self):
        sources = {
            source.name: source.read_text(encoding="utf-8")
            for source in APP_DIR.glob("MainWindowAutomation*.cpp")
        }
        expected_owners = {
            "captureRuntimeAnnotationState": "MainWindowAutomation.cpp",
            "runPlayerSmokeChecks": "MainWindowAutomation.cpp",
            "_runFullscreenSmokeChecks": "MainWindowAutomationFullscreen.cpp",
            "_runMediaOpenSmokeCheck": "MainWindowAutomationMedia.cpp",
            "_runSettingsSmokeChecks": "MainWindowAutomationSettings.cpp",
            "runSubtitleGenerationSmokeChecks": "MainWindowAutomationSubtitles.cpp",
            "runSubtitleCacheDisplaySmokeChecks": "MainWindowAutomationSubtitles.cpp",
            "runSubtitleSwitchSequenceSmokeChecks": "MainWindowAutomationSubtitles.cpp",
            "runSubtitleRefinedFallbackSmokeChecks": "MainWindowAutomationSubtitles.cpp",
            "runQwenAsrProviderSmokeChecks": "MainWindowAutomation.cpp",
            "runPhase915ViewerRebuildChecks": "MainWindowAutomation.cpp",
            "runPhase915PluginReloadChecks": "MainWindowAutomation.cpp",
            "runPhase915FallbackToggleChecks": "MainWindowAutomation.cpp",
            "runPhase14PerformanceBaselineChecks": "MainWindowAutomation.cpp",
            "runPhase14StressChecks": "MainWindowAutomation.cpp",
            "prepareCaptureDemoState": "MainWindowAutomation.cpp",
        }
        combined = "\n".join(sources.values())
        for method, owner in expected_owners.items():
            signature = f"MainWindow::{method}("
            self.assertEqual(combined.count(signature), 1, method)
            self.assertEqual(sources[owner].count(signature), 1, method)

    def test_shared_helpers_have_one_definition(self):
        sources = {
            source.name: source.read_text(encoding="utf-8")
            for source in APP_DIR.glob("MainWindowAutomation*.cpp")
        }
        original = sources["MainWindowAutomation.cpp"]
        subtitles = sources["MainWindowAutomationSubtitles.cpp"]
        for helper in (
            "pumpUntil",
            "writeSmokeSubtitleVtt",
            "smokeCueHasDisplayableTranslation",
            "makeSummary",
        ):
            definitions = re.findall(
                rf"^[^\n;]*\b{helper}\([^;\n]*\)\s*\n\{{",
                "\n".join(sources.values()),
                flags=re.MULTILINE,
            )
            self.assertEqual(len(definitions), 1, helper)
            self.assertRegex(original, rf"\b{helper}\([^;\n]*\)\s*\n\{{")
            self.assertIn(f"{helper}(", subtitles)

    def test_cmake_owns_real_translation_units_without_source_includes(self):
        cmake = (ROOT / "src" / "CMakeLists.txt").read_text(encoding="utf-8")
        source_list = re.search(r"set\(CGPLAY_SOURCES\s+(.*?)\n\)", cmake, re.DOTALL)
        self.assertIsNotNone(source_list)
        for source in (
            "ui/app/MainWindowAutomation.cpp",
            "ui/app/MainWindowAutomationFullscreen.cpp",
            "ui/app/MainWindowAutomationMedia.cpp",
            "ui/app/MainWindowAutomationSettings.cpp",
            "ui/app/MainWindowAutomationSubtitles.cpp",
        ):
            self.assertEqual(source_list.group(1).count(source), 1, source)

        for source in APP_DIR.glob("MainWindowAutomation*.cpp"):
            text = source.read_text(encoding="utf-8")
            self.assertNotRegex(text, r'#\s*include\s*[<\"][^>\"]*\.(?:inc|cpp)[>\"]')

    def test_event_pumps_remain_in_automation_translation_units(self):
        original = (APP_DIR / "MainWindowAutomation.cpp").read_text(encoding="utf-8")
        subtitles = (APP_DIR / "MainWindowAutomationSubtitles.cpp").read_text(encoding="utf-8")
        self.assertIn("processEvents(", original)
        self.assertIn("processEvents(", subtitles)

    def test_refined_fixture_tracks_display_and_provenance_contracts(self):
        subtitles = (APP_DIR / "MainWindowAutomationSubtitles.cpp").read_text(encoding="utf-8")
        self.assertIn("SubtitleGenerationService::writeMediaIdentitySidecar(", subtitles)
        self.assertIn(".online.translated.zh.vtt", subtitles)
        for stale_english_fixture in (
            "quick fallback target",
            "fresh refined early",
            "enhanced display early",
        ):
            self.assertNotIn(stale_english_fixture, subtitles)


if __name__ == "__main__":
    unittest.main()
