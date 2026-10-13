#include "core/playback/api/Timecode.h"
#include "ui/app/SubtitleSessionController.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTextStream>
#include <limits>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QJsonArray results;
    int failures = 0;
    const auto check = [&](const QString& name, bool passed) {
        results.append(QJsonObject{{"name", name}, {"passed", passed}});
        if (!passed) ++failures;
    };
    struct Case { qint64 frame; double fps; const char* time; };
    const Case cases[] = {
        {0, 24, "00:00:00:00"}, {31, 24, "00:00:01:07"},
        {24, 23.976, "00:00:01:00"}, {24000, 23.976, "00:16:40:00"},
        {30, 29.97, "00:00:01:00"}, {1800, 29.97, "00:01:00:00"},
        {86400, 24, "01:00:00:00"}, {31, 1, "00:00:31:00"},
        {-5, 24, "00:00:00:00"}, {24, 0, "00:00:01:00"},
        {24, -1, "00:00:01:00"}, {24, std::numeric_limits<double>::infinity(), "00:00:01:00"},
        {24, std::numeric_limits<double>::quiet_NaN(), "00:00:01:00"},
        {qint64(100) * 3600 * 24, 24, "100:00:00:00"}, {1, .1, "00:00:01:00"},
    };
    for (const auto& c : cases)
        check(QString("Timecode frame=%1 fps=%2").arg(c.frame).arg(c.fps),
            cgplay::formatFrameTimecode(c.frame, c.fps) == QString::fromLatin1(c.time));

    auto generation = std::make_shared<std::atomic_bool>(false);
    auto refinement = std::make_shared<std::atomic_bool>(false);
    auto enhancement = std::make_shared<std::atomic_bool>(false);
    auto replacement = std::make_shared<std::atomic_bool>(false);
    {
        cgplay::SubtitleSessionController session;
        auto& state = session.state();
        state.subtitleGenerationCancelRequested = generation;
        state.subtitleRefinementCancelRequested = refinement;
        state.highQualityEnhancementCancelRequested = enhancement;
        state.subtitleGenerationBusy = state.subtitleRefinementBusy = state.highQualityEnhancementBusy = true;
        QFutureWatcher<cgplay::SubtitleRefinementResult> watcher;
        QPointer<QFutureWatcher<cgplay::SubtitleRefinementResult>> witness = &watcher;
        state.subtitleRefinementWatcher = &watcher;
        state.generatedSubtitleCues.append(cgplay::GeneratedSubtitleCue{});
        state.generatedSubtitlePath = "old.vtt";
        session.reset();
        check("Reset cancels all captured worker tokens", generation->load() && refinement->load() && enhancement->load());
        check("Reset invalidates stale watcher without deleting QObject", state.subtitleRefinementWatcher.isNull() && witness == &watcher);
        check("Reset clears media-specific data", !state.subtitleGenerationBusy && !state.subtitleRefinementBusy &&
            !state.highQualityEnhancementBusy && state.generatedSubtitleCues.isEmpty() && state.generatedSubtitlePath.isEmpty());
        state.subtitleGenerationCancelRequested = replacement;
        generation->store(true);
        check("Old cancellation cannot cancel new session", !replacement->load());
    }
    check("Owner destruction requests cancellation", replacement->load());
    QTextStream(stdout) << QJsonDocument(QJsonObject{{"backgroundMode", true}, {"visibleAutomationWindows", 0},
        {"failed", failures}, {"results", results}}).toJson(QJsonDocument::Indented);
    return failures == 0 ? 0 : 1;
}
