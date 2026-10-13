#pragma once

#include "services/ai/SubtitleGenerationService.h"
#include <QFutureWatcher>
#include <QJsonObject>
#include <QPointer>
#include <atomic>
#include <memory>

namespace cgplay {

// Owns media-local subtitle state. Widgets and user preferences belong to
// the window; worker requests own their captured data. Reset never waits on
// the GUI thread, and late callbacks must check media/watcher identity.
class SubtitleSessionController
{
public:
    struct State {
        bool subtitleGenerationBusy = false;
        std::shared_ptr<std::atomic_bool> subtitleGenerationCancelRequested;
        QPointer<QFutureWatcher<SubtitleGenerationResult>> subtitleGenerationWatcher;
        bool subtitleRefinementBusy = false;
        std::shared_ptr<std::atomic_bool> subtitleRefinementCancelRequested;
        QPointer<QFutureWatcher<SubtitleRefinementResult>> subtitleRefinementWatcher;
        QString subtitleRefinementLastError;
        QJsonObject subtitleRefinementLastValidation;
        double subtitleRefinementLastCoverageEndSeconds = 0.0;
        bool subtitleContinuationAfterRefinementPending = false;
        double subtitleContinuationAfterRefinementStartSeconds = -1.0;
        QVector<GeneratedSubtitleCue> generatedSubtitleCues;
        QString generatedSubtitlePath;
        QString generatedSubtitleMediaFingerprint;
        QString subtitleGenerationLastError;
        QString subtitleGenerationActiveTranslatedVttPath;
        bool translationPlaybackModeWaited = false;
        qint64 translationPlaybackModeWaitMs = 0;
        bool translationPrePlaybackWaitActive = false;
        bool translationPrePlaybackWaitCanceled = false;
        bool translationPrePlaybackWaitTimedOut = false;
        bool translationPrePlaybackWaitDegradedToQuick = false;
        bool translationPrePlaybackWaitPlaybackAlreadyRunning = false;
        QString translationPrePlaybackWaitResult;
        double translationPrePlaybackWaitTargetSeconds = 600.0;
        double translationPrePlaybackWaitCoverageAheadSeconds = 0.0;
        double translationPrePlaybackWaitMaxProcessedSeconds = 0.0;
        bool translationPrePlaybackWaitEnhancementStarted = false;
        qint64 translationPrePlaybackWaitStartMs = 0;
        qint64 translationPrePlaybackWaitTimeoutMs = 600000;
        int generatedSubtitleQuickFallbackCueCount = 0;
        int generatedSubtitleRefinedMatchedCueCount = 0;
        int generatedSubtitleEnhancedMatchedCueCount = 0;
        bool generatedSubtitleEnhancedLoaded = false;
        QString generatedSubtitleEnhancedRejectedReason;
        bool generatedSubtitleCurrentCueFallbackSafe = true;
        bool generatedSubtitleRefinedMerged = false;
        bool generatedSubtitleEnhancedMerged = false;
        bool highQualityEnhancementBusy = false;
        QString highQualityEnhancementMediaPath;
        QString highQualityEnhancementLastReason;
        QJsonObject highQualityEnhancementLastDiagnostics;
        QPointer<QFutureWatcher<QJsonObject>> highQualityEnhancementWatcher;
        std::shared_ptr<std::atomic_bool> highQualityEnhancementCancelRequested;
        qint64 highQualityVisualPrefetchLastCheckMs = 0;
        double generatedSubtitleCoverageEndSeconds = 0.0;
        double generatedSubtitleProcessedEndSeconds = 0.0;
        QString generatedSubtitleLastText;
        double generatedSubtitleLastCueStartSeconds = -1.0;
        double generatedSubtitleLastCueEndSeconds = -1.0;
        double generatedSubtitleLastShownAtSeconds = -1.0;
        bool subtitleContinuationScheduled = false;
        double subtitleContinuationStartSeconds = -1.0;
        bool subtitleContinuationQueuedWhileBusy = false;
        double subtitleContinuationQueuedStartSeconds = -1.0;
        int subtitleContinuationRetryCount = 0;
    };

    ~SubtitleSessionController() { cancelTasks(); }
    State& state() { return _state; }
    const State& state() const { return _state; }

    void cancelTasks()
    {
        for (const auto& token : {_state.subtitleGenerationCancelRequested,
                                  _state.subtitleRefinementCancelRequested,
                                  _state.highQualityEnhancementCancelRequested})
            if (token) token->store(true);
    }

    void reset()
    {
        cancelTasks();
        // Watchers remain QObject-owned until their finished callbacks dispose
        // them. Clearing the current watcher identity invalidates old results.
        _state = State{};
    }

private:
    State _state;
};

} // namespace cgplay
