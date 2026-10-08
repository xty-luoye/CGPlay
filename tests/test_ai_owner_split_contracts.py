import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AI = ROOT / "src" / "services" / "ai"


class AiOwnerSplitContracts(unittest.TestCase):
    def read(self, name: str) -> str:
        return (AI / name).read_text(encoding="utf-8")

    def test_each_translation_unit_stays_below_owner_limit(self) -> None:
        files = (
            "SubtitleGenerationService.cpp",
            "SubtitleGenerationSupport.cpp",
            "TranslationEnhancementScheduler.cpp",
            "TranslationEnhancementSupport.cpp",
        )
        lines = {name: len(self.read(name).splitlines()) for name in files}
        self.assertTrue(all(count <= 3000 for count in lines.values()), lines)

    def test_support_modules_are_real_named_translation_units(self) -> None:
        subtitle = self.read("SubtitleGenerationSupport.cpp")
        enhancement = self.read("TranslationEnhancementSupport.cpp")
        self.assertIn("namespace cgplay::subtitle_generation_support", subtitle)
        self.assertIn("namespace cgplay::translation_enhancement_support", enhancement)
        self.assertNotIn(".inc", subtitle + enhancement)
        self.assertNotRegex(subtitle + enhancement, re.compile(r"#\s*define\s+CGPLAY_.*IMPLEMENT"))

    def test_moved_responsibilities_have_one_definition(self) -> None:
        files = list(AI.glob("*.cpp"))
        combined = "\n".join(path.read_text(encoding="utf-8") for path in files)
        symbols = (
            "baseOutputPath",
            "analyzeSubtitleSourceQuality",
            "resolveSubtitleSourcePath",
            "selectCurrentFrameFinalDisplay",
            "cacheState",
            "highQualityNeedReason",
        )
        for symbol in symbols:
            definitions = re.findall(
                rf"^[\w:<>,*&\s]+\b{symbol}\s*\([^;{{}}]*\)\s*\{{",
                combined,
                re.MULTILINE,
            )
            self.assertEqual(1, len(definitions), f"{symbol}: {len(definitions)} definitions")

    def test_quick_subtitle_constants_remain_frozen(self) -> None:
        owner = self.read("SubtitleGenerationService.cpp")
        for declaration in (
            "constexpr int kChunkSeconds = 4;",
            "constexpr int kDefaultAsrConcurrency = 8;",
            "constexpr int kDefaultTranslationConcurrency = 8;",
            "constexpr qint64 kForwardPartialCoalesceMs = 650;",
        ):
            self.assertIn(declaration, owner)

    def test_cmake_owns_each_split_unit_once(self) -> None:
        cmake = (ROOT / "src" / "CMakeLists.txt").read_text(encoding="utf-8")
        for name in (
            "services/ai/SubtitleGenerationSupport.cpp",
            "services/ai/SubtitleGenerationSupport.h",
            "services/ai/TranslationEnhancementSupport.cpp",
            "services/ai/TranslationEnhancementSupport.h",
        ):
            self.assertEqual(1, cmake.count(name), name)


if __name__ == "__main__":
    unittest.main()
