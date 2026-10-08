import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class ViewerZoomInputContracts(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text(encoding="utf-8")

    def test_manual_zoom_disables_auto_fit(self) -> None:
        viewer = self.read("src/ui/viewer/ViewerWidget.cpp")
        for method in ("zoom1to1", "zoomIn", "zoomOut", "setZoom"):
            body = viewer.split(f"void ViewerWidget::{method}", 1)[1].split("\n}", 1)[0]
            self.assertIn("_p->viewport->setFrameView(false);", body, method)
        self.assertIn("bool ViewerWidget::_applyWheelZoom", viewer)
        wheel = viewer.split("bool ViewerWidget::_applyWheelZoom", 1)[1].split("\n}", 1)[0]
        self.assertIn("_p->viewport->setFrameView(false);", wheel)
        self.assertIn("std::pow(1.1, steps)", wheel)

    def test_zoom_commands_and_physical_plus_are_registered(self) -> None:
        app = self.read("src/ui/app/Application.cpp")
        input_code = self.read("src/ui/app/ApplicationInput.cpp")
        for command in ("view.zoomIn", "view.zoomOut"):
            self.assertIn(f'addBuiltIn({{QStringLiteral("{command}")', app)
            self.assertIn(f'QStringLiteral("{command}")', app)
        self.assertIn("event->key() == Qt::Key_Plus", input_code)
        self.assertIn("shortcutModifiers &= ~Qt::ShiftModifier", input_code)

    def test_wheel_zoom_is_default_and_scoped_to_viewer(self) -> None:
        input_code = self.read("src/ui/app/ApplicationInput.cpp")
        settings = self.read("src/ui/app/ApplicationSettings.cpp")
        runtime = self.read("src/ui/app/ApplicationRuntimeAutomation.cpp")
        profiles = self.read("src/services/settings/SettingsProfileService.cpp")
        self.assertIn('QStringLiteral("input/mouseWheel"), QStringLiteral("zoom")', input_code)
        self.assertIn("if (eventBelongsToViewer && event->type() == QEvent::Wheel", input_code)
        self.assertIn('QStringLiteral("滚轮放大/缩小"), QStringLiteral("zoom")', settings)
        self.assertGreaterEqual(settings.count('QStringLiteral("input/mouseWheel"), QStringLiteral("zoom")'), 2)
        self.assertIn('QStringLiteral("input/mouseWheel"), QStringLiteral("zoom")', runtime)
        self.assertIn("eventBelongsToSettings && event->type() == QEvent::Wheel", input_code)
        self.assertIn("if (delta == 0) delta = wheel->pixelDelta().y();", input_code)
        self.assertIn("if (wheel->inverted()) delta = -delta;", input_code)
        for mode in ("none", "frames", "volume"):
            self.assertIn(f'binding == QStringLiteral("{mode}")', input_code)
        self.assertGreaterEqual(profiles.count('QStringLiteral("mouseWheel"), QStringLiteral("zoom")'), 1)
        self.assertIn('wheel != QStringLiteral("zoom")', profiles)

    def test_player_smoke_uses_real_key_and_wheel_events(self) -> None:
        smoke = self.read("src/ui/app/MainWindowAutomation.cpp")
        self.assertIn('runKey(QStringLiteral("+"), Qt::Key_Plus, Qt::ShiftModifier)', smoke)
        self.assertIn('runKey(QStringLiteral("-"), Qt::Key_Minus)', smoke)
        self.assertIn("QWheelEvent wheelIn", smoke)
        self.assertIn("QWheelEvent wheelOut", smoke)
        self.assertIn("QApplication::sendEvent(viewport, &wheelIn)", smoke)
        self.assertIn("QApplication::sendEvent(viewport, &wheelOut)", smoke)
        for name in ("Viewer pixel wheel frames", "Viewer pixel wheel volume", "Viewer wheel none"):
            self.assertIn(f'QStringLiteral("{name}")', smoke)
        self.assertIn('afterWheelIn.value("frame").toInt() == beforeWheelIn.value("frame").toInt()', smoke)


if __name__ == "__main__":
    unittest.main()
