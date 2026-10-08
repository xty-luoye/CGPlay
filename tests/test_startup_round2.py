"""Contracts for startup sampling deferral and benchmark evidence."""

from pathlib import Path
import json
import unittest


ROOT = Path(__file__).resolve().parents[1]
ARTIFACT = ROOT / "tests" / "artifacts" / "optimization_round2_20260922"


class StartupRound2Tests(unittest.TestCase):
    def test_performance_sampler_does_not_sample_synchronously_on_start(self) -> None:
        source = (ROOT / "src/common/core/SystemPerformanceMonitor.cpp").read_text(encoding="utf-8")
        start = source.index("void SystemPerformanceMonitor::start")
        stop = source.index("void SystemPerformanceMonitor::stop", start)
        body = source[start:stop]
        self.assertNotIn("_sample();\n    }\n    _p->timer->start", body)
        self.assertIn("QTimer::singleShot(250, this", body)
        self.assertIn("_p->timer->isActive()", body)

    def test_startup_benchmark_is_background_and_lazy(self) -> None:
        report = ARTIFACT / "startup" / "startup_summary.json"
        if not report.exists():
            self.skipTest("startup benchmark evidence is generated after build")
        # Windows PowerShell writes UTF-8 JSON with a BOM by default.
        data = json.loads(report.read_text(encoding="utf-8-sig"))
        self.assertTrue(data["allReady"])
        self.assertTrue(data["allBackground"])
        self.assertTrue(data["allZeroVisibleWindows"])
        self.assertTrue(data["allLazySettings"])

    def test_theme_refresh_reuses_one_widget_snapshot(self) -> None:
        source = (ROOT / "src/ui/app/ApplicationRuntime.cpp").read_text(encoding="utf-8")
        self.assertIn("const QList<QWidget*> allWidgets = QApplication::allWidgets();", source)
        refresh = source[source.index("void Application::refreshAppearanceSettings"):]
        self.assertLessEqual(refresh.count("QApplication::allWidgets()"), 1)
        self.assertIn("for (QWidget* widget : allWidgets)", refresh)


if __name__ == "__main__":
    unittest.main()
