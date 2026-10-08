import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


def production_sources():
    return sorted(path for path in SRC.rglob("*") if path.suffix in {".cpp", ".h"})


class ArchitectureTenContracts(unittest.TestCase):
    def test_nonempty_production_loc_is_below_verified_baseline(self) -> None:
        nonempty_lines = 0
        for path in production_sources():
            text = path.read_text(encoding="utf-8", errors="replace")
            nonempty_lines += sum(bool(line.strip()) for line in text.splitlines())
        self.assertLess(nonempty_lines, 78_328)

    def test_process_waits_are_owned_by_job_system(self) -> None:
        violations = []
        for path in production_sources():
            if path.as_posix().endswith("src/common/jobs/JobSystem.cpp"):
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            if "waitForFinished(" in text:
                violations.append(path.relative_to(ROOT).as_posix())
        self.assertEqual([], violations)

    def test_non_automation_production_paths_do_not_pump_events(self) -> None:
        automation_files = {
            "src/ui/app/ApplicationRuntimeAutomation.cpp",
            "src/ui/app/MainWindowAutomation.cpp",
            "src/ui/app/MainWindowAutomationSubtitles.cpp",
        }
        violations = []
        for path in production_sources():
            relative = path.relative_to(ROOT).as_posix()
            if relative in automation_files:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            if "processEvents(" in text:
                violations.append(relative)
        self.assertEqual([], violations)

    def test_production_cpp_files_stay_within_owner_limit(self) -> None:
        violations = {}
        for path in sorted(SRC.rglob("*.cpp")):
            lines = len(path.read_text(encoding="utf-8", errors="replace").splitlines())
            if lines > 3000:
                violations[path.relative_to(ROOT).as_posix()] = lines
        self.assertEqual({}, violations)

    def test_legacy_command_paths_remain_removed(self) -> None:
        combined = "\n".join(
            path.read_text(encoding="utf-8", errors="replace")
            for path in production_sources()
        )
        self.assertNotIn("_executeCommandFallback", combined)
        self.assertNotRegex(combined, re.compile(r"#\s*if\s+0\b"))


if __name__ == "__main__":
    unittest.main()
