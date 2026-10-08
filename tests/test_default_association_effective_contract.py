"""Contracts for reporting and repairing the *effective* Windows association."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class EffectiveDefaultAssociationContracts(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_effective_query_uses_shell_resolution_not_classes_only(self) -> None:
        source = self.read("src/services/platform/WindowsFileAssociations.cpp")
        for token in (
            "effectiveDefaultVideoExtensions",
            "AssocQueryStringW",
            "ASSOCSTR_COMMAND",
            "commandTargetsCurrentApplication",
            "Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Explorer\\\\FileExts",
            "UserChoice",
        ):
            self.assertIn(token, source)

    def test_stale_openwith_candidates_are_pruned_without_touching_userchoice(self) -> None:
        source = self.read("src/services/platform/WindowsFileAssociations.cpp")
        self.assertIn("RegEnumValueW", source)
        self.assertIn("RegDeleteValueW", source)
        self.assertIn("hasExplicitUserChoice", source)
        self.assertIn("if (!hasExplicitUserChoice(normalized)) pruneStaleOpenWithProgIds(normalized);", source)
        self.assertIn("pruneStaleOpenWithProgIdsForSupportedExtensions", source)

    def test_settings_displays_actual_effective_count(self) -> None:
        settings = self.read("src/ui/app/ApplicationSettings.cpp")
        automation = self.read("src/ui/app/MainWindowAutomationSettings.cpp")
        self.assertIn("当前系统实际使用 CGPlay", settings)
        self.assertIn("程序不会伪造 UserChoice", settings)
        self.assertIn("effectiveDefaultVideoExtensions", settings)
        self.assertIn("effectiveStatusPresent", automation)

    def test_release_links_shell_association_library(self) -> None:
        cmake = self.read("src/CMakeLists.txt")
        self.assertIn("target_link_libraries(CGPlay PRIVATE advapi32 shell32 shlwapi)", cmake)

    def test_inno_backend_is_required_to_register_associations(self) -> None:
        installer = self.read("scripts/installer/CGPlay.iss")
        for token in (
            "CGPlay.Video",
            "CGPlayThumbnailProvider.dll",
            "RegisteredApplications",
            "FileAssociations",
            "OpenWithProgids",
        ):
            self.assertIn(token, installer)


if __name__ == "__main__":
    unittest.main()
