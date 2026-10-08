"""Source boundaries complement the real background settings interaction smoke."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SETTINGS = (ROOT / "src/ui/app/ApplicationSettings.cpp").read_text(encoding="utf-8")
SMOKE = (ROOT / "src/ui/app/MainWindowAutomationSettings.cpp").read_text(encoding="utf-8")
NAVIGATION = SETTINGS.split("class SettingsNavigation final", 1)[1].split("} // namespace", 1)[0]


class SettingsNavigationContracts(unittest.TestCase):
    def test_existing_pages_are_retained(self):
        for title in ("常规", "外观与布局", "工作区布局", "快捷键", "工具栏", "鼠标与手柄",
                      "命令管理", "播放", "性能", "字幕", "AI"):
            self.assertRegex(SETTINGS, rf'tabs->addTab\([^;]*QStringLiteral\("{title}"\)\)')
        self.assertEqual(SETTINGS.count("tabs->addTab("), 11)
        self.assertIn("_pages->tabBar()->hide()", NAVIGATION)

    def test_navigation_and_result_targets_have_stable_ids(self):
        for name in ("SettingsPages", "SettingsCategoryNavigation", "SettingsPageTitle",
                     "SettingsPageDescription", "SettingsSearchEdit", "SettingsSearchResults",
                     "SettingsSearchEmptyState"):
            self.assertIn(f'QStringLiteral("{name}")', NAVIGATION)
        self.assertIn("Qt::UserRole + 2", NAVIGATION)
        self.assertIn("QPointer<QWidget> target", NAVIGATION)
        self.assertIn("scroll->ensureWidgetVisible", NAVIGATION)
        self.assertIn("entry.target->setFocus", NAVIGATION)

    def test_search_uses_static_metadata_not_edit_values(self):
        index_build = NAVIGATION.split("for (int page = 0;", 1)[1].split("connect(_search", 1)[0]
        self.assertIn("QFormLayout::LabelRole", index_build)
        self.assertIn("group->title()", index_build)
        self.assertIn('property("cgplay.settingsSearchText")', index_build)
        for forbidden in ("findChildren<QLineEdit", "findChildren<QText", "findChildren<QLabel",
                          "currentText()", "toPlainText()", "toHtml()", "allWidgets()"):
            self.assertNotIn(forbidden, index_build)
        result_update = NAVIGATION.split("void updateResults()", 1)[1].split("void navigate(", 1)[0]
        self.assertNotIn("findChildren", result_update)
        self.assertIn("Qt::CaseInsensitive", result_update)
        self.assertIn("_index", result_update)

    def test_keyboard_is_dialog_scoped_and_respects_recorders(self):
        self.assertIn("widget->window() != _dialog", NAVIGATION)
        self.assertIn("qobject_cast<QKeySequenceEdit*>(parent)", NAVIGATION)
        self.assertIn("QEvent::ShortcutOverride", NAVIGATION)
        self.assertIn("push->setAutoDefault(false)", NAVIGATION)
        self.assertIn("push->setDefault(false)", NAVIGATION)
        self.assertIn("Qt::Key_Escape", NAVIGATION)

    def test_import_injection_is_background_and_namespace_gated(self):
        callback = SETTINGS.split("connect(importProfile, &QPushButton::clicked", 1)[1]
        self.assertIn('property("cgplay.automationBackground").toBool()', callback)
        self.assertIn('startsWith(QStringLiteral("CGPlayAutomation_"))', callback)
        self.assertIn('property("cgplay.automationSettingsImportPath")', callback)
        self.assertIn("QFileDialog::getOpenFileName", callback)

    def test_real_smoke_covers_new_interactions(self):
        for evidence in (
            "Settings navigation controls present",
            "Settings all pages and captures",
            "Settings existing action inventory preserved",
            "Settings category keyboard navigation",
            "Settings search matching and target navigation",
            "Settings search privacy clearing and stable reuse",
            "Settings search keyboard and empty Enter safety",
            "Settings all mapped player command hints consistent",
            "Settings shortcut remap clear restore and presentation",
            "Settings imported shortcut refreshes all presentations",
            "Settings shortcut recording keeps Ctrl F",
            "Settings narrow and standard layout captures",
        ):
            self.assertIn(evidence, SMOKE)
        self.assertIn("settingsClickItem(navigation, index)", SMOKE)
        self.assertIn("saveShortcuts->click()", SMOKE)
        self.assertIn("restoreShortcuts->click()", SMOKE)
        self.assertIn("importProfile->click()", SMOKE)
        self.assertIn("dialog->grab().save", SMOKE)
        self.assertIn("targetInViewport && targetFocused", SMOKE)
        self.assertIn("unexpectedSearchClicks == 0", SMOKE)
        self.assertIn("dialog->size() == size", SMOKE)
        self.assertIn("fieldRect.right() > viewportRect.right()", SMOKE)
        self.assertIn("emptyShortcutHintsLocalized()", SMOKE)

    def test_smoke_is_background_and_restores_isolated_values(self):
        self.assertIn('startsWith(QStringLiteral("CGPlayAutomation_"))', SMOKE)
        self.assertIn("Qt::WA_DontShowOnScreen", SMOKE)
        self.assertIn("visibleWindows == 0 && nativeVisibleWindows == 0", SMOKE)
        self.assertIn("savedImportSettings", SMOKE)
        self.assertIn("restoredShortcutSettings == savedShortcutSettings", SMOKE)
        self.assertNotIn("activateWindow", SMOKE)
        self.assertNotIn("->raise()", SMOKE)


if __name__ == "__main__":
    unittest.main()
