"""Hidden Qt routing regressions for viewer replacement and explicit comparison."""

from pathlib import Path
import unittest

from tests.native_review_probe import build_and_run
from tests.test_review_metadata_repairs import QT, function


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tests/artifacts/2026-11-11-playback-interaction/viewer-drop" / QT.name


def callback_body(source, marker, occurrence=0):
    position = -1
    for _ in range(occurrence + 1):
        position = source.index(marker, position + 1)
    start = source.index("{", source.index("[this", position))
    end = start + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start + 1:end - 1]


CODE = r'''
#include <QApplication>
#include <QAction>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QSignalBlocker>
#include <QUrl>
#include <QWidget>
#include <functional>

class CompareBar : public QWidget {
public:
    int mode = 0;
    QString shotA;
    QString shotB;
    std::function<void(int)> modeChanged;
    CompareBar() { setAttribute(Qt::WA_DontShowOnScreen); }
    void setCompareMode(int value) { mode = value; if (modeChanged) modeChanged(value); }
    void setShotALabel(const QString& value) { shotA = value; }
    void setShotBLabel(const QString& value) { shotB = value; }
};

struct Playback {
    QString comparePath;
    int compareRequests = 0;
    int playRequests = 0;
    bool valid = true;
    bool isValid() const { return valid; }
    void play() { ++playRequests; }
    void setCompareFile(const QString& path) { comparePath = path; ++compareRequests; }
};

struct NavigationRail { enum { Playlist, Compare }; };

class Fixture {
public:
    CompareBar compare;
    Playback playback;
    struct Private {
        CompareBar* compareBar;
        Playback* playbackCtrl;
        QString currentPath;
        bool mediaProbeJob = false;
        bool autoPlayPending = false;
        QString pendingComparePath;
    } state{&compare, &playback, QStringLiteral("first.mp4")};
    Private* _p = &state;
    QString openedPath;
    int openRequests = 0;
    QAction menuAction;
    QAction* actCmp = &menuAction;
    Fixture() { compare.modeChanged = [this](int mode) { MODE_CHANGED }; }
    void mediaOpenRequested(const QString&) {}
    void openFile(const QString& path) {
        OPEN_START
        openedPath = path;
        state.currentPath = path;
        state.mediaProbeJob = true;
        ++openRequests;
    }
    void finishOpen(bool autoPlay = true) {
        state.mediaProbeJob = false;
        state.autoPlayPending = autoPlay;
        playback.valid = true;
        playback.comparePath.clear();
        playerReady(true);
    }
    void playerReady(bool player) { PLAYER_READY }
    void playbackFailure(const QString& path) { PLAYBACK_FAILURE }
    void closeMedia() { CLOSE_START }
    void _selectCompareB(const QString& path);
    void rebuiltDrop(const QString& p) { REBUILT_DROP }
    void originalDrop(const QString& p) { ORIGINAL_DROP }
    void rebuiltToggle() { REBUILT_TOGGLE }
    void originalToggle() { ORIGINAL_TOGGLE }
    void selectA(const QString& p) { SELECT_A }
    void selectB(const QString& p) { SELECT_B }
    void dropB(const QString& p) { DROP_B }
    void menuVisible(bool visible) { MENU_VISIBLE }
    void syncMenuChecked() { MENU_CHECKED }
    void navigationPage(int page) { NAVIGATION_PREFIX }
};
COMPARE_HELPER

class DropSurface : public QWidget {
public:
    std::function<void(const QString&)> route;
    DropSurface() { setAcceptDrops(true); setAttribute(Qt::WA_DontShowOnScreen); }
    void droppedFile(const QString& path) { route(path); }
protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
};
DROP_EVENTS

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QJsonObject report;
    const QString second = QDir(QString::fromUtf8(argv[1])).absoluteFilePath("second.mp4");
    const QList<void (Fixture::*)(const QString&)> routes{&Fixture::originalDrop, &Fixture::rebuiltDrop};
    for (int route = 0; route < routes.size(); ++route) {
        Fixture fixture;
        fixture.compare.mode = 5;
        fixture.compare.shotB = "previous comparison";
        fixture.playback.comparePath = "previous.mp4";
        DropSurface surface;
        surface.route = [&](const QString& path) { (fixture.*routes[route])(path); };
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(second)});
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&surface, &enter);
        QApplication::sendEvent(&surface, &drop);
        report[QString("viewerRoute%1ReplacesMedia").arg(route)] = enter.isAccepted() && drop.isAccepted()
            && fixture.openRequests == 1 && fixture.openedPath == second
            && fixture.playback.compareRequests == 0;
        report[QString("viewerRoute%1ReturnsToSingleView").arg(route)] = fixture.compare.mode == 0
            && fixture.compare.shotA == "second" && fixture.compare.shotB == "--";
        fixture.state.currentPath.clear();
        (fixture.*routes[route])(second);
        report[QString("viewerRoute%1AlsoOpensInitially").arg(route)] = fixture.openRequests == 2;
    }

    Fixture manual;
    manual.selectA(second);
    report["selectedAOpensActualMedia"] = manual.openedPath == second && manual.openRequests == 1
        && manual.compare.mode == 0 && manual.compare.shotB == "--" && manual.compare.isVisible();
    manual.finishOpen();
    manual.selectB("first.mp4");
    report["selectedBStartsExplicitComparison"] = manual.playback.comparePath == "first.mp4"
        && manual.compare.mode == 5 && manual.compare.isVisible() && manual.openRequests == 1;
    manual.compare.hide();
    manual.dropB("third.mp4");
    report["comparisonLabelDropRemainsExplicit"] = manual.playback.comparePath == "third.mp4"
        && manual.compare.mode == 5 && manual.compare.isVisible() && manual.openRequests == 1;

    Fixture asynchronous;
    asynchronous.playback.comparePath = "old-b.mp4";
    asynchronous.selectA(second);
    asynchronous.selectB("first-b.mp4");
    report["selectionWaitsForNewA"] = asynchronous.state.pendingComparePath == "first-b.mp4"
        && asynchronous.playback.comparePath == "old-b.mp4" && asynchronous.playback.compareRequests == 0;
    asynchronous.dropB("latest-b.mp4");
    asynchronous.finishOpen();
    report["latestQueuedBReachesNewPlayer"] = asynchronous.playback.comparePath == "latest-b.mp4"
        && asynchronous.playback.compareRequests == 1 && asynchronous.state.pendingComparePath.isEmpty()
        && asynchronous.compare.mode == 5 && asynchronous.playback.playRequests == 1;
    asynchronous.selectA(second);
    asynchronous.selectB("still-b.mp4");
    asynchronous.finishOpen(false);
    report["queuedComparisonAlsoAppliesToStillA"] = asynchronous.playback.comparePath == "still-b.mp4"
        && asynchronous.state.pendingComparePath.isEmpty() && asynchronous.playback.playRequests == 1;
    asynchronous.selectA(second);
    asynchronous.selectB("canceled-b.mp4");
    asynchronous.originalDrop("replacement.mp4");
    asynchronous.finishOpen();
    report["ordinaryDropCancelsQueuedComparison"] = asynchronous.state.pendingComparePath.isEmpty()
        && asynchronous.playback.comparePath.isEmpty() && asynchronous.compare.mode == 0;
    asynchronous.selectA(second);
    asynchronous.selectB("canceled-by-close.mp4");
    asynchronous.originalToggle();
    asynchronous.finishOpen();
    report["closingComparisonCancelsQueuedSelection"] = asynchronous.state.pendingComparePath.isEmpty()
        && asynchronous.playback.comparePath.isEmpty() && asynchronous.compare.mode == 0;
    asynchronous.selectA(second);
    asynchronous.selectB("canceled-by-failure.mp4");
    asynchronous.playbackFailure(second);
    report["failedPlayerDiscardsQueuedSelection"] = asynchronous.state.pendingComparePath.isEmpty()
        && !asynchronous.state.autoPlayPending;
    asynchronous.selectB("canceled-by-media-close.mp4");
    asynchronous.closeMedia();
    report["closingMediaDiscardsQueuedSelection"] = asynchronous.state.pendingComparePath.isEmpty();

    const QList<void (Fixture::*)()> toggles{&Fixture::originalToggle, &Fixture::rebuiltToggle};
    for (int toggle = 0; toggle < toggles.size(); ++toggle) {
        manual.compare.show();
        manual.compare.mode = 5;
        (manual.*toggles[toggle])();
        report[QString("comparisonToggle%1ClosesSplitView").arg(toggle)] = manual.compare.isHidden()
            && manual.compare.mode == 0;
        (manual.*toggles[toggle])();
        report[QString("comparisonToggle%1ReopensSingleView").arg(toggle)] = manual.compare.isVisible()
            && manual.compare.mode == 0;
    }
    manual.compare.mode = 5;
    manual.menuAction.setCheckable(true);
    int menuToggles = 0;
    QObject::connect(&manual.menuAction, &QAction::toggled, [&] { ++menuToggles; });
    manual.syncMenuChecked();
    report["menuReflectsComparisonOpenedFromViewer"] = manual.menuAction.isChecked() && menuToggles == 0;
    manual.menuVisible(false);
    report["menuCloseExitsComparison"] = manual.compare.isHidden() && manual.compare.mode == 0;
    manual.syncMenuChecked();
    report["menuReflectsHiddenToolbarWithoutToggling"] = !manual.menuAction.isChecked() && menuToggles == 0;
    manual.compare.show();
    manual.compare.mode = 5;
    manual.navigationPage(NavigationRail::Playlist);
    report["leavingComparisonPageExitsSplitView"] = manual.compare.isHidden() && manual.compare.mode == 0;
    return finish(report);
}
'''


class ViewerDropPlaybackTests(unittest.TestCase):
    def test_hidden_drop_and_comparison_routes(self):
        source = (ROOT / "src/ui/app/Application.cpp").read_text(encoding="utf-8-sig")
        media = (ROOT / "src/ui/app/ApplicationMedia.cpp").read_text(encoding="utf-8-sig")
        viewer = (ROOT / "src/ui/viewer/ViewerWidget.cpp").read_text(encoding="utf-8-sig")
        open_body = function(media, "void MainWindow::openFile(").split("{", 1)[1]
        close_body = function(media, "void MainWindow::_closeCurrentMedia(").split("{", 1)[1]
        replacements = {
            "OPEN_START": open_body.split("const QFileInfo incomingInfo")[0],
            "CLOSE_START": close_body.split("++_p->mediaProbeGeneration")[0],
            "COMPARE_HELPER": function(media, "void MainWindow::_selectCompareB(").replace("MainWindow::", "Fixture::"),
            "PLAYER_READY": callback_body(source, "&PlaybackServiceSignals::playerReady"),
            "PLAYBACK_FAILURE": callback_body(source, "&PlaybackServiceSignals::fileOpenFailed"),
            "MODE_CHANGED": callback_body(source, "&CompareToolbar::compareModeChanged"),
            "REBUILT_DROP": callback_body(source, "&ViewerWidget::droppedFile", 0),
            "ORIGINAL_DROP": callback_body(source, "&ViewerWidget::droppedFile", 1),
            "REBUILT_TOGGLE": callback_body(source, "&ViewerWidget::compareRequested", 0),
            "ORIGINAL_TOGGLE": callback_body(source, "&ViewerWidget::compareRequested", 1),
            "SELECT_A": callback_body(source, "&PlaylistPanel::shotASelected"),
            "SELECT_B": callback_body(source, "&PlaylistPanel::shotBSelected"),
            "DROP_B": callback_body(source, "&CompareToolbar::shotBFileDropped"),
            "MENU_VISIBLE": callback_body(source, "connect(actCmp, &QAction::toggled"),
            "MENU_CHECKED": callback_body(source, "connect(viewMenu, &QMenu::aboutToShow"),
            "NAVIGATION_PREFIX": callback_body(source, "&NavigationRail::pageChanged").split("switch (page)")[0],
            "DROP_EVENTS": "\n".join(function(viewer, signature).replace("ViewerWidget::", "DropSurface::")
                for signature in ("void ViewerWidget::dragEnterEvent(", "void ViewerWidget::dropEvent(")),
        }
        code = CODE
        for name, body in replacements.items():
            code = code.replace(name, body)
        build_and_run(OUT, code)

    def test_probe_failure_clears_only_current_pending_selection(self):
        media = (ROOT / "src/ui/app/ApplicationMedia.cpp").read_text(encoding="utf-8-sig")
        opened = function(media, "void MainWindow::openFile(")
        finished = opened[opened.index("[this, job, path,"):]
        generation_guard = finished.index("generation != _p->mediaProbeGeneration")
        first_clear = finished.index("_p->pendingComparePath.clear()")
        self.assertLess(generation_guard, first_clear)
        failure = finished[finished.index("if (!outcome.succeeded())"):]
        self.assertLess(failure.index("_p->pendingComparePath.clear()"), failure.index("Q_EMIT mediaOpenFailed"))


if __name__ == "__main__":
    unittest.main()
