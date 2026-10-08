import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "src" / "ui" / "app"
UNITS = (
    "AIAgentWorkspace.cpp",
    "AIAgentWorkspaceActions.cpp",
    "AIAgentWorkspaceControl.cpp",
    "AIAgentWorkspacePresentation.cpp",
    "AIAgentWorkspaceSettings.cpp",
    "AIAgentWorkspaceSupport.cpp",
)


class AiWorkspaceSplitContracts(unittest.TestCase):
    def read(self, name: str) -> str:
        return (APP / name).read_text(encoding="utf-8")

    def test_each_translation_unit_stays_below_owner_limit(self) -> None:
        lines = {name: len(self.read(name).splitlines()) for name in UNITS}
        self.assertTrue(all(count <= 3000 for count in lines.values()), lines)

    def test_split_uses_real_named_translation_units(self) -> None:
        combined = "\n".join(self.read(name) for name in UNITS)
        self.assertIn("namespace ai_agent_workspace_support", self.read("AIAgentWorkspaceSupport.cpp"))
        self.assertNotRegex(combined, re.compile(r'#\s*include\s*[<\"][^>\"]+\.cpp[>\"]'))
        self.assertNotRegex(combined, re.compile(r'#\s*include\s*[<\"][^>\"]+\.inc[>\"]'))
        self.assertNotRegex(combined, re.compile(r"#\s*define\s+CGPLAY_.*IMPLEMENT"))

    def test_member_definitions_have_single_owners(self) -> None:
        combined = "\n".join(self.read(name) for name in UNITS)
        definitions = re.findall(
            r"\bAIAgentWorkspace::(~?[A-Za-z_][A-Za-z0-9_]*)\s*\(", combined
        )
        duplicates = sorted({name for name in definitions if definitions.count(name) != 1})
        self.assertEqual([], duplicates)
        for required in (
            "_setupUi",
            "_submitCurrentFrameAnalysis",
            "_loadProviderConfig",
            "_saveSubtitlePipelineConfig",
            "_buildRuntimeControlState",
            "_executeAiControlActions",
            "_renderResponseHtml",
            "_submitBatchScan",
            "_applyAllAnnotationSuggestions",
        ):
            self.assertEqual(1, definitions.count(required), required)

        header = self.read("AIAgentWorkspace.h")
        declarations = set(re.findall(
            r"(?:^|\s)(~?AIAgentWorkspace|refreshFromRuntime|runQwenAsrProviderSmoke|"
            r"saveSettingsDialogSmokeScreenshot|eventFilter|changeEvent|"
            r"_[A-Za-z][A-Za-z0-9_]*)\s*\(",
            header,
        ))
        self.assertEqual(declarations, set(definitions))
        self.assertEqual(68, len(definitions))

    def test_settings_and_protocol_constants_have_single_definitions(self) -> None:
        combined = "\n".join(self.read(name) for name in UNITS)
        support_header = self.read("AIAgentWorkspaceSupport.h")
        declarations = re.findall(
            r"^extern const char\* const (k[A-Za-z0-9_]+);",
            support_header,
            re.MULTILINE,
        )
        definitions = re.findall(
            r"^const char\* const (k[A-Za-z0-9_]+)\s*=",
            combined,
            re.MULTILINE,
        )
        self.assertEqual(set(declarations), set(definitions))
        for symbol in declarations:
            self.assertEqual(
                1,
                definitions.count(symbol),
                symbol,
            )

        helper_declarations = re.findall(
            r"^[A-Za-z][\w:<>,*& ]+\s+([a-z][A-Za-z0-9_]*)\s*\([^;]*\);",
            support_header,
            re.MULTILINE,
        )
        for symbol in helper_declarations:
            definitions = re.findall(
                rf"^[\w:<>,*&\s]+\b{symbol}\s*\([^;{{}}]*\)\s*\{{",
                combined,
                re.MULTILINE,
            )
            self.assertEqual(1, len(definitions), symbol)

    def test_settings_screenshot_is_synchronous_without_event_pumping(self) -> None:
        settings = self.read("AIAgentWorkspaceSettings.cpp")
        screenshot = settings.split(
            "bool AIAgentWorkspace::saveSettingsDialogSmokeScreenshot", 1
        )[1].split("void AIAgentWorkspace::_clearStoredProviderCredential", 1)[0]
        self.assertNotIn("processEvents", screenshot)
        self.assertIn("ensurePolished", screenshot)
        self.assertIn("layout->activate()", screenshot)
        self.assertIn("target->render(&capture)", screenshot)

    def test_cmake_owns_each_split_file_once(self) -> None:
        cmake = (ROOT / "src" / "CMakeLists.txt").read_text(encoding="utf-8")
        source_list = re.search(r"set\(CGPLAY_SOURCES\s+(.*?)\n\)", cmake, re.DOTALL)
        self.assertIsNotNone(source_list)
        for name in (*UNITS, "AIAgentWorkspace.h", "AIAgentWorkspaceSupport.h"):
            self.assertEqual(1, source_list.group(1).count(f"ui/app/{name}"), name)


if __name__ == "__main__":
    unittest.main()
