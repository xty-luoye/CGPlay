import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APP_DIR = ROOT / "src" / "ui" / "app"
SOURCE_LIST = ROOT / "src" / "CMakeLists.txt"


class ApplicationSplitContracts(unittest.TestCase):
    def application_sources(self):
        return sorted(APP_DIR.glob("Application*.cpp"))

    def test_every_application_translation_unit_is_below_owner_limit(self):
        oversized = {
            source.name: len(source.read_text(encoding="utf-8").splitlines())
            for source in self.application_sources()
            if len(source.read_text(encoding="utf-8").splitlines()) >= 3000
        }
        self.assertEqual({}, oversized)

    def test_split_uses_real_translation_units(self):
        forbidden = re.compile(r'#\s*include\s*[<"][^>"]+\.(?:cpp|inc)[>"]')
        violations = []
        for source in self.application_sources():
            if forbidden.search(source.read_text(encoding="utf-8")):
                violations.append(source.name)
        self.assertEqual([], violations)

    def test_main_window_methods_have_one_owner(self):
        definition = re.compile(
            r"(?:^|\n)(?:[\w:<>,*&]+\s+)*MainWindow::(~?\w+)\s*\(",
            re.MULTILINE,
        )
        owners = {}
        for source in self.application_sources():
            for method in definition.findall(source.read_text(encoding="utf-8")):
                owners.setdefault(method, []).append(source.name)
        duplicates = {name: paths for name, paths in owners.items() if len(paths) != 1}
        self.assertEqual({}, duplicates)
        self.assertGreaterEqual(len(owners), 55)

    def test_cmake_registers_each_application_owner(self):
        cmake = SOURCE_LIST.read_text(encoding="utf-8")
        expected = {
            "Application.cpp",
            "ApplicationEnhancement.cpp",
            "ApplicationInput.cpp",
            "ApplicationMedia.cpp",
            "ApplicationSettings.cpp",
            "ApplicationSubtitles.cpp",
            "ApplicationUiSupport.cpp",
        }
        missing = [name for name in sorted(expected) if f"ui/app/{name}" not in cmake]
        self.assertEqual([], missing)

    def test_owners_match_stable_responsibilities(self):
        expectations = {
            "Application.cpp": ["_setupUI", "_setupMenuBar", "_executeCommandId"],
            "ApplicationSettings.cpp": ["_installSettingsWidgets"],
            "ApplicationMedia.cpp": ["openFile", "_restoreSplitterLayoutIfNeeded"],
            "ApplicationSubtitles.cpp": ["_loadGeneratedSubtitleTrack", "_startHighQualityPrePlaybackWait"],
            "ApplicationEnhancement.cpp": ["_buildTranslationEnhancementRequest", "_generateSubtitlesForCurrentMedia"],
            "ApplicationInput.cpp": ["_restoreState", "eventFilter", "keyPressEvent"],
        }
        for filename, methods in expectations.items():
            text = (APP_DIR / filename).read_text(encoding="utf-8")
            for method in methods:
                self.assertIn(f"MainWindow::{method}", text, f"{method} moved out of {filename}")

    def test_fullscreen_content_margins_are_removed_and_restored(self):
        media = (APP_DIR / "ApplicationMedia.cpp").read_text(encoding="utf-8")
        self.assertIn("QMargins{} : QMargins{16, 0, 16, 8}", media)
        self.assertIn("setContentHostMargins(_p->horzSplitter, true);", media)
        self.assertIn("setContentHostMargins(_p->horzSplitter, false);", media)
        self.assertIn("findChildren<QDockWidget*>()", media)
        self.assertIn("const QSignalBlocker blocker(dock);", media)
        self.assertIn("restoreDockWidgetsAfterFullscreen(this);", media)
        self.assertIn("horizontal->setHandleWidth(fullscreen ? 0 : 8)", media)
        self.assertIn("vertical->setHandleWidth(fullscreen ? 0 : 1)", media)
        self.assertIn("setFullscreenSplitterHandles(_p->horzSplitter, _p->centerSplitter, true);", media)
        self.assertIn("setFullscreenSplitterHandles(_p->horzSplitter, _p->centerSplitter, false);", media)
        self.assertIn("_p->fullscreenCenterSizes = _p->centerSplitter", media)
        self.assertIn("_p->fullscreenTimelineHeight = _p->timeline->height();", media)
        self.assertIn("_p->fullscreenPlaybackBarHeight = _p->playbackBar->height();", media)
        self.assertIn("_p->timeline->setFixedHeight(visible && _p->fullscreenTimeline", media)
        self.assertIn("_p->playbackBar->setFixedHeight(visible && _p->fullscreenPlaybackBar", media)
        self.assertIn("_p->centerSplitter->setHandleWidth(visible ? 1 : 0);", media)
        self.assertIn("_p->centerSplitter->setSizes(_p->fullscreenCenterSizes);", media)
        self.assertIn("_p->fullscreenMousePollTimer->setInterval(50);", media)
        self.assertIn("const QPoint cursorPos = systemCursorPosition();", media)
        self.assertIn("::GetCursorPos(&point)", media)
        self.assertIn("const bool cursorNearBottom", media)
        self.assertIn("_p->fullscreenMousePollTimer->stop();\n        showFullScreen();", media)
        self.assertIn("QTimer::singleShot(0, this, [this]()", media)
        self.assertIn("if (_p->fullscreenActive && isFullScreen()) {", media)
        self.assertIn("_setFullScreenChromeVisible(false, true);", media)
        self.assertIn("QTimer::singleShot(150, this", media)
        self.assertIn("isFullScreen() && _p->fullscreenChromeVisible", media)
        self.assertIn("_p->timeline->raise();", media)
        self.assertIn("_p->playbackBar->raise();", media)
        self.assertIn("_p->centerSplitter->setCollapsible(1, true);", media)
        self.assertIn("_p->centerSplitter->setSizes({std::max(total, _p->centerSplitter->height()), 0, 0});", media)
        self.assertIn("_p->timeline->hide();", media)
        self.assertIn("_p->playbackBar->hide();", media)
        self.assertIn("_p->viewerShell->raise();", media)
        self.assertIn("_p->viewer->viewport()->update();", media)
        self.assertIn("_showFullScreenChromeTemporarily();", media)
        self.assertIn("_p->fullscreenMousePollTimer->stop();", media)
        self.assertIn("const bool fullscreen = _p->fullscreenActive || isFullScreen();", media)
        self.assertIn("_p->fullscreenActive = false;", media)
        self.assertIn("_p->fullscreenActive = true;", media)

    def test_fullscreen_shortcuts_are_handled_at_application_boundary(self):
        source = (APP_DIR / "ApplicationInput.cpp").read_text(encoding="utf-8")
        self.assertIn("const bool fullscreenActive = _p->fullscreenActive || isFullScreen();", source)
        self.assertIn('descriptor.id != QStringLiteral("view.fullscreen")', source)
        self.assertNotIn("Qt::Key_F11", source.split("bool MainWindow::eventFilter", 1)[1].split("bool MainWindow::nativeEvent", 1)[0])
        self.assertIn("_toggleFullScreen();", source)
        self.assertIn("keyEvent->accept();", source)

    def test_automation_covers_both_fullscreen_exit_paths(self):
        source = (APP_DIR / "MainWindowAutomationFullscreen.cpp").read_text(encoding="utf-8")
        self.assertIn('QStringLiteral("Esc exits fullscreen")', source)
        self.assertIn('QStringLiteral("F11 exits fullscreen")', source)
        self.assertIn('QStringLiteral("Fullscreen rapid transitions")', source)
        self.assertIn('QStringLiteral("Fullscreen first frame and delayed zoom stability")', source)
        self.assertIn('QStringLiteral("Fullscreen explicit zoom after entry")', source)
        self.assertIn('QStringLiteral("Fullscreen entry restores repainting")', source)
        self.assertIn('QStringLiteral("Fullscreen playing transition responsiveness")', source)
        self.assertIn('"p95PaintGapMs"', source)
        self.assertIn('"mediaFrameEvents"', source)

    def test_fullscreen_entry_defers_presentation_until_layout_settles(self):
        media = (APP_DIR / "ApplicationMedia.cpp").read_text(encoding="utf-8")
        toggle = media.split("void MainWindow::_toggleFullScreen()", 1)[1].split(
            "void MainWindow::_showFullScreenChromeTemporarily()", 1)[0]
        entry = toggle.split("_p->fullscreenActive = true;", 1)[1]
        self.assertLess(entry.index("setUpdatesEnabled(false)"), entry.index("showFullScreen()"))
        self.assertNotIn("QTimer::singleShot(150", entry)
        commit = entry.split("// The chrome helper queues one final splitter reconciliation.", 1)[1]
        self.assertIn("QTimer::singleShot(0, this", commit)
        self.assertIn("_p->fullscreenTransitionGeneration != transitionGeneration", commit)
        self.assertLess(commit.index("layout()->activate()"), commit.index("vp->setFrameView(true)"))
        self.assertLess(commit.index("vp->setFrameView(true)"), commit.index("setUpdatesEnabled(true)"))
        self.assertLess(commit.index("setUpdatesEnabled(true)"), commit.index("fullscreenMousePollTimer->start()"))
        show_chrome = media.split("void MainWindow::_showFullScreenChromeTemporarily()", 1)[1].split(
            "void MainWindow::_hideFullScreenChrome()", 1)[0]
        self.assertIn("!isFullScreen() || _p->fullscreenEntryPending", show_chrome)
        exit_entry = toggle.split("_p->fullscreenActive = false;", 1)[0]
        self.assertIn("_p->fullscreenEntryPending = false;", exit_entry)
        self.assertIn("setUpdatesEnabled(true);", exit_entry)

    def test_fullscreen_chrome_does_not_synchronously_repaint_gl_ancestors(self):
        media = (APP_DIR / "ApplicationMedia.cpp").read_text(encoding="utf-8")
        chrome = media.split("void MainWindow::_setFullScreenChromeVisible", 1)[1].split(
            "void MainWindow::_setFullScreenCursorHidden", 1)[0]
        self.assertNotIn("->repaint()", chrome)
        self.assertIn("_p->viewer->viewport()->update()", chrome)


if __name__ == "__main__":
    unittest.main()
