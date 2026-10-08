from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
CODEX_DIR = ROOT / "src/plugins/codex"
CMAKE = ROOT / "src/CMakeLists.txt"


class CodexOwnerSplitContractTests(unittest.TestCase):
    def test_every_codex_production_cpp_stays_below_owner_limit(self):
        oversized = {
            path.name: len(path.read_text(encoding="utf-8").splitlines())
            for path in CODEX_DIR.glob("*.cpp")
            if len(path.read_text(encoding="utf-8").splitlines()) > 3000
        }
        self.assertEqual({}, oversized)

    def test_split_uses_real_compilation_units(self):
        sources = {
            path.name: path.read_text(encoding="utf-8")
            for path in CODEX_DIR.glob("CodexPlugin*.cpp")
        }
        self.assertIn("CodexPluginTools.cpp", sources)
        self.assertIn("CodexPluginWorkspace.cpp", sources)
        for name, source in sources.items():
            self.assertNotRegex(source, re.compile(r'#include\s+["<].*\.cpp[">]'), name)

    def test_cmake_owns_each_split_source_once(self):
        cmake = CMAKE.read_text(encoding="utf-8")
        for source in (
            "CodexPlugin.cpp",
            "CodexPluginSupport.cpp",
            "CodexPluginTools.cpp",
            "CodexPluginWorkspace.cpp",
        ):
            self.assertEqual(1, cmake.count(f"plugins/codex/{source}"), source)

    def test_major_owners_are_separated(self):
        main = (CODEX_DIR / "CodexPlugin.cpp").read_text(encoding="utf-8")
        support = (CODEX_DIR / "CodexPluginSupport.cpp").read_text(encoding="utf-8")
        tools = (CODEX_DIR / "CodexPluginTools.cpp").read_text(encoding="utf-8")
        workspace = (CODEX_DIR / "CodexPluginWorkspace.cpp").read_text(encoding="utf-8")
        self.assertNotIn("CodexPlugin::executeDynamicTool", main)
        self.assertNotIn("CodexPlugin::connectWorkspace", main)
        self.assertIn("JobOutcome runProcessJob", support)
        self.assertIn("CodexPlugin::executeDynamicTool", tools)
        self.assertIn("CodexPlugin::connectWorkspace", workspace)


if __name__ == "__main__":
    unittest.main()
