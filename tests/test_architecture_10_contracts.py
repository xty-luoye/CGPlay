import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


def production_sources():
    return sorted(path for path in SRC.rglob("*") if path.suffix in {".cpp", ".h"})


class ArchitectureTenContracts(unittest.TestCase):
    def test_playback_core_has_no_ui_dependency(self) -> None:
        forbidden = re.compile(r'#\s*include\s*[<"](?:ui/|viewer/|app/)')
        for path in (SRC / "core/playback").rglob("*"):
            if path.suffix in {".cpp", ".h"}:
                self.assertNotRegex(path.read_text(encoding="utf-8"), forbidden, str(path))

    def test_timecode_has_one_shared_contract(self) -> None:
        for path in ("ui/app/PlaybackBar.cpp", "ui/viewer/ViewerWidget.cpp", "features/timeline/TimelineWidget.cpp"):
            text = (SRC / path).read_text(encoding="utf-8")
            self.assertIn('playback/api/Timecode.h', text, path)
            self.assertIn('formatFrameTimecode(', text, path)
            self.assertNotIn('QStringLiteral("%1:%2:%3:%4")', text, path)

    def test_main_window_delegates_lifecycle_state(self) -> None:
        state = (SRC / "ui/app/ApplicationPrivate.h").read_text(encoding="utf-8")
        self.assertIn('std::unique_ptr<FullscreenController> fullscreen', state)
        self.assertIn('SubtitleSessionController subtitleSession', state)
        for duplicate in ("fullscreenTransitionGeneration", "fullscreenChromeTimer", "subtitleGenerationWatcher", "subtitleRefinementBusy"):
            self.assertNotIn(duplicate, state)
        media = (SRC / "ui/app/ApplicationMedia.cpp").read_text(encoding="utf-8")
        self.assertIn('_p->fullscreen->toggle()', media)
        self.assertIn('_p->subtitleSession.reset()', media)

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
