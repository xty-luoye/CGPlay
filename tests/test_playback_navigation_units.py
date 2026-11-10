"""Hidden native Qt regressions for navigation units and unclipped progress labels."""
import re
import unittest

from tests.native_review_probe import build_and_run
from tests.test_review_metadata_repairs import ROOT, TL


OUT = ROOT / "tests/artifacts/2026-11-11-playback-interaction/timeline_units"

HEADERS = r'''
#include <QApplication>
#include <QAction>
#include <QFontDatabase>
#include <QMenu>
#include <QToolButton>
#include <limits>
#include "ui/app/PlaybackBar.h"
#include "features/timeline/TimelineWidget.cpp"
using namespace cgplay;
namespace cgplay {
QVector<ThumbnailFrame> ThumbnailService::buildTimelineThumbnails(
 const QString&, int, double, int, const OcioManager::PreviewTransformSettings&, JobContext*) { return {}; }
bool MediaProbe::isStillImagePath(const QString&) { return false; }
QVector<SequenceFrame> MediaProbe::collectSequenceFiles(const QString&) { return {}; }
bool ReviewExport::isStillImage(const QString&) { return false; }
}
'''

MAIN = r'''
int main(int argc, char** argv) {
 QApplication app(argc, argv);
 QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf");
 QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
 QJsonObject report;
 auto playback = std::make_shared<FakePlayback>();
 PlaybackBar bar(playback);
 TimelineWidget timeline(playback);
 bar.setAttribute(Qt::WA_DontShowOnScreen);
 timeline.setAttribute(Qt::WA_DontShowOnScreen);
 bar.resize(840, 54);
 timeline.resize(840, timeline.height());
 QObject::connect(&bar, &PlaybackBar::displaySecondsChanged, &timeline, &TimelineWidget::setDisplaySeconds);
 QObject::connect(&timeline, &TimelineWidget::displaySecondsChanged, &bar, &PlaybackBar::setDisplaySeconds);
 timeline.setMediaPath("fixture.mp4");
 playback->position = 37;
 playback->proxy.currentFrameChanged(37, playback->length);
 auto* time = bar.findChild<QToolButton*>("PlaybackBarTimeDisplay");
 auto* progress = timeline.findChild<QToolButton*>("TimelineProgressDisplay");
 report["initialTimecodeAndFrameCount"] = time && progress && time->text() == "00:00:01:13" &&
     progress->text() == QStringLiteral("38 / 2460 帧") && !bar.displaySeconds() && !timeline.displaySeconds();
 time->click();
 report["timeButtonSynchronizesSeconds"] = bar.displaySeconds() && timeline.displaySeconds() &&
     time->text() == QStringLiteral("1.542 秒") && progress->text() == QStringLiteral("1.542 / 102.500 秒");
 progress->click();
 report["progressButtonSynchronizesFrames"] = !bar.displaySeconds() && !timeline.displaySeconds() &&
     progress->text() == QStringLiteral("38 / 2460 帧");
 report["defaultNavigationFiveSeconds"] = bar.navigationInSeconds() && bar.navigationStepAmount() == 5 &&
     bar.navigationStepFrames() == 120;
 bool menuWorks = true;
 for (const bool seconds : {true, false}) {
     for (const int amount : {1, 5, 10}) {
         const QString name = QStringLiteral("PlaybackBarNavigation%1%2")
             .arg(seconds ? QStringLiteral("Seconds") : QStringLiteral("Frames")).arg(amount);
         auto* action = bar.findChild<QAction*>(name);
         if (!action) { menuWorks = false; continue; }
         action->trigger();
         const int expected = seconds ? amount * 24 : amount;
         menuWorks = menuWorks && action->isChecked() && bar.navigationStepFrames() == expected;
         bar.findChild<QToolButton*>("PlaybackBarNextFrame")->click();
         menuWorks = menuWorks && playback->lastDelta == expected;
         bar.findChild<QToolButton*>("PlaybackBarPreviousFrame")->click();
         menuWorks = menuWorks && playback->lastDelta == -expected;
     }
 }
 report["everyNavigationMenuOptionAndTransportButton"] = menuWorks;
 playback->rate = 23.976;
 bar.setNavigationStep(true, 5);
 report["fractionalFpsUsesMediaRate"] = bar.navigationStepFrames() == 120 &&
     formatFrameSeconds(24000, 23.976) == "1001.001";
 playback->rate = std::numeric_limits<double>::quiet_NaN();
 report["invalidFpsAndNegativeFrameFallback"] = bar.navigationStepFrames() == 120 &&
     formatFrameSeconds(-1, playback->rate) == "0.000";
 playback->rate = 25;
 bool bounded = true;
 QFont tickFont("Segoe UI"); tickFont.setPixelSize(10); tickFont.setWeight(QFont::DemiBold);
 const QFontMetrics metrics(tickFont);
 TimelineBar ticks;
 ticks.setAttribute(Qt::WA_DontShowOnScreen);
 ticks.totalFrames = 2460;
 for (const int width : {320, 840, 1884}) {
     ticks.resize(width, ticks.height());
     for (const bool seconds : {false, true}) {
         ticks.displaySeconds = seconds;
         const QString start = seconds ? QStringLiteral("0.000 秒") : formatFrameTimecode(0, 25);
         const QString end = seconds ? QStringLiteral("98.360 秒") : formatFrameTimecode(2459, 25);
         const QRect first = ticks.tickLabelRect(ticks.frameToX(0), start, metrics);
         const QRect last = ticks.tickLabelRect(ticks.frameToX(2459), end, metrics);
         bounded = bounded && ticks.rect().contains(first) && ticks.rect().contains(last) &&
             first.width() >= metrics.horizontalAdvance(start) && last.width() >= metrics.horizontalAdvance(end) &&
             ticks.frameToX(2459) == width - 1 && ticks.xToFrame(width - 1) == 2459;
     }
 }
 report["bothEndpointLabelsFitAtNarrowAndWideWidths"] = bounded;
 playback->position = 1720;
 playback->proxy.currentFrameChanged(1720, playback->length);
 timeline.setDisplaySeconds(true);
 timeline.resize(520, timeline.height());
 timeline.show();
 app.processEvents();
 report["progressFitsAllocatedHeight"] = progress->height() >= progress->sizeHint().height() &&
     timeline.rect().contains(QRect(progress->mapTo(&timeline, QPoint()), progress->size()));
 report["hiddenTimelineScreenshot"] = timeline.grab().save(QDir(QString::fromUtf8(argv[1])).filePath("timeline_seconds.png"));
 timeline.setMediaPath(QString());
 report["closedMediaClearsProgress"] = progress->text() == "0 / 0";
 return finish(report);
}
'''


class PlaybackNavigationUnits(unittest.TestCase):
    def test_hidden_native_controls_and_label_bounds(self):
        interface = (ROOT / "src/core/playback/api/IPlaybackService.h").read_text(encoding="utf-8-sig")
        overrides = []
        bodies = {
            "signalProxy": "return &proxy;",
            "fps": "return rate;",
            "currentFrame": "return position;",
            "totalFrames": "return length;",
            "speedMultiplier": "return 1.;",
            "seekRelative": "lastDelta = deltaFrames;",
            "getVolume": "return 1.f;",
        }
        for signature in re.findall(r"virtual\s+([^;{}]+?)\s*=\s*0\s*;", interface):
            match = re.fullmatch(r"(.+?)\s+(\w+)\(([^)]*)\)(\s+const)?", signature.strip())
            self.assertIsNotNone(match, signature)
            result_type, name, parameters, const = match.groups()
            body = bodies.get(name, "" if result_type.strip() == "void" else "return {};")
            overrides.append(f"{result_type} {name}({parameters}){const or ''} override {{ {body} }}")
        fake = "struct FakePlayback final : IPlaybackService { mutable PlaybackServiceSignals proxy; " \
            "double rate = 24.; int position = 0, length = 2460, lastDelta = 0;\n" + \
            "\n".join(overrides) + "\n};\n"
        dependency_root = ROOT.parent / "CGPlay-deps"
        include_roots = [
            dependency_root / "engine_migration_1_0_8/tlRender-0.27.1-cgplay-prefix/include",
            dependency_root / "engine_migration_1_0_8/ftk-0.19.0-prefix/include",
            dependency_root / "CGV_install/include",
            TL / "include",
        ]
        extra_cmake = "target_include_directories(probe PRIVATE " + " ".join(
            f'"{path.as_posix()}"' for path in include_roots) + ")\n"
        build_and_run(OUT, HEADERS + fake + MAIN, sources=[
            "src/ui/app/PlaybackBar.cpp", "src/ui/app/PlaybackBar.h",
            "src/features/timeline/TimelineWidget.h",
            "src/common/jobs/JobSystem.cpp", "src/common/jobs/JobSystem.h",
            "src/core/playback/api/PlaybackServiceSignals.h",
        ], extra_cmake=extra_cmake)


if __name__ == "__main__":
    unittest.main()
