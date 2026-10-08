#include "Application.h"
#include "ApplicationPrivate.h"

#include "playback/PlaybackController.h"
#include "PlaybackBar.h"
#include "services/ai/SubtitleGenerationService.h"
#include "services/ai/TranslationGuardSkills.h"
#include "services/ai/TranslationPlaybackStrategy.h"
#include "settings/api/ISettingsService.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSet>
#include <QThread>
#include <QToolButton>

#include <algorithm>
#include <limits>

namespace cgplay {

namespace automation {

bool pumpUntil(const std::function<bool()>& condition, int timeoutMs = 4000);
bool writeSmokeSubtitleVtt(const QString& path, const QVector<GeneratedSubtitleCue>& cues);
bool smokeCueHasDisplayableTranslation(const GeneratedSubtitleCue& cue);
QJsonObject makeSummary(const QJsonArray& assertions);

} // namespace automation

using namespace automation;

QJsonObject MainWindow::runSubtitleGenerationSmokeChecks(
    const QString& mediaPath,
    const QString& outputPath,
    bool mockWithoutApi,
    int frame,
    int cancelAfterMs,
    int playbackSampleDurationMs,
    int playbackSampleIntervalMs)
{
    QJsonObject report;
    QJsonArray assertions;
    auto addAssertion = [&assertions](
                            const QString& name,
                            bool passed,
                            const QString& message,
                            const QJsonObject& details = {}) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("status"), passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
        item.insert(QStringLiteral("message"), message);
        item.insert(QStringLiteral("details"), details);
        assertions.append(item);
    };

    qInfo() << "[SubtitleCacheDisplaySmoke] Opening media" << mediaPath;
    openFile(mediaPath);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
    QThread::msleep(250);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
    qInfo() << "[SubtitleCacheDisplaySmoke] Media opened check";

    const bool mediaOpened = _p->playbackCtrl && _p->playbackCtrl->isValid();
    addAssertion(
        QStringLiteral("Open media"),
        mediaOpened,
        mediaOpened ? QStringLiteral("media opened") : QStringLiteral("player invalid"),
        QJsonObject{
            { QStringLiteral("media"), QFileInfo(mediaPath).absoluteFilePath() },
            { QStringLiteral("totalFrames"), _p->playbackCtrl ? _p->playbackCtrl->totalFrames() : 0 },
            { QStringLiteral("fps"), _p->playbackCtrl ? _p->playbackCtrl->fps() : 0.0 }
        });

    Q_UNUSED(mockWithoutApi);
    const QString sourceSrtPath = SubtitleGenerationService::defaultSourceSrtPath(mediaPath);
    const QString sourceVttPath = QFileInfo(sourceSrtPath).absolutePath() + QLatin1Char('/') +
        QFileInfo(sourceSrtPath).completeBaseName() + QStringLiteral(".vtt");
    const QString translatedSrtPath =
        SubtitleGenerationService::defaultTranslatedSrtPath(mediaPath, QStringLiteral("zh-Hans"));
    const QString translatedVttPath =
        SubtitleGenerationService::defaultTranslatedVttPath(mediaPath, QStringLiteral("zh-Hans"));
    const QString refinedSrtPath =
        SubtitleGenerationService::defaultRefinedTranslatedSrtPath(mediaPath, QStringLiteral("zh-Hans"));
    const QString refinedVttPath =
        SubtitleGenerationService::defaultRefinedTranslatedVttPath(mediaPath, QStringLiteral("zh-Hans"));
    const QString enhancedVttPath =
        SubtitleGenerationService::defaultEnhancedTranslatedVttPath(mediaPath, QStringLiteral("zh-Hans"));
    const QString onlineCachePath =
        SubtitleGenerationService::defaultOnlineSubtitleCachePath(mediaPath);
    QString onlineTranslatedCachePath = onlineCachePath;
    if (onlineTranslatedCachePath.endsWith(QStringLiteral(".online.source.vtt"))) {
        onlineTranslatedCachePath.chop(QStringLiteral(".online.source.vtt").size());
        onlineTranslatedCachePath += QStringLiteral(".online.translated.zh.vtt");
    } else {
        onlineTranslatedCachePath += QStringLiteral(".translated.zh.vtt");
    }
    QString onlineIntakePath = onlineCachePath;
    if (onlineIntakePath.endsWith(QStringLiteral(".online.source.vtt"))) {
        onlineIntakePath.chop(QStringLiteral(".online.source.vtt").size());
        onlineIntakePath += QStringLiteral(".online.intake.json");
    } else {
        onlineIntakePath += QStringLiteral(".intake.json");
    }
    const QString translatedZhVttPath =
        SubtitleGenerationService::defaultTranslatedVttPath(mediaPath, QStringLiteral("zh"));
    const QString translatedZhSrtPath =
        SubtitleGenerationService::defaultTranslatedSrtPath(mediaPath, QStringLiteral("zh"));
    const QString refinedZhVttPath =
        SubtitleGenerationService::defaultRefinedTranslatedVttPath(mediaPath, QStringLiteral("zh"));
    const QString refinedZhSrtPath =
        SubtitleGenerationService::defaultRefinedTranslatedSrtPath(mediaPath, QStringLiteral("zh"));
    const QString enhancedZhVttPath =
        SubtitleGenerationService::defaultEnhancedTranslatedVttPath(mediaPath, QStringLiteral("zh"));
    const QString repairVttPath =
        SubtitleGenerationService::defaultLowConfidenceRepairVttPath(mediaPath, QStringLiteral("zh-Hans"));
    const QString ocrCachePath =
        SubtitleGenerationService::defaultOcrSubtitleCachePath(mediaPath);
    QString ocrTranslatedCachePath = ocrCachePath;
    if (ocrTranslatedCachePath.endsWith(QStringLiteral(".ocr.source.vtt"))) {
        ocrTranslatedCachePath.chop(QStringLiteral(".ocr.source.vtt").size());
        ocrTranslatedCachePath += QStringLiteral(".ocr.translated.zh.vtt");
    } else {
        ocrTranslatedCachePath += QStringLiteral(".translated.zh.vtt");
    }
    QString longAsrSourceVttPath = ocrCachePath;
    if (longAsrSourceVttPath.endsWith(QStringLiteral(".ocr.source.vtt"))) {
        longAsrSourceVttPath.chop(QStringLiteral(".ocr.source.vtt").size());
        longAsrSourceVttPath += QStringLiteral(".longasr.source.vtt");
    }
    QString longAsrTranslatedVttPath = longAsrSourceVttPath;
    if (longAsrTranslatedVttPath.endsWith(QStringLiteral(".longasr.source.vtt"))) {
        longAsrTranslatedVttPath.chop(QStringLiteral(".longasr.source.vtt").size());
        longAsrTranslatedVttPath += QStringLiteral(".hq.translated.zh.vtt");
    }
    QString longAsrReportPath = longAsrSourceVttPath;
    if (longAsrReportPath.endsWith(QStringLiteral(".longasr.source.vtt"))) {
        longAsrReportPath.chop(QStringLiteral(".longasr.source.vtt").size());
        longAsrReportPath += QStringLiteral(".longasr.report.json");
    }
    const QString staleZhSrtPath =
        SubtitleGenerationService::defaultTranslatedSrtPath(mediaPath, QStringLiteral("zh"));
    const QString staleZhVttPath =
        SubtitleGenerationService::defaultTranslatedVttPath(mediaPath, QStringLiteral("zh"));
    QFile::remove(sourceSrtPath);
    QFile::remove(sourceVttPath);
    QFile::remove(staleZhSrtPath);
    QFile::remove(staleZhVttPath);
    QFile::remove(translatedSrtPath);
    QFile::remove(translatedVttPath);
    QFile::remove(refinedSrtPath);
    QFile::remove(refinedVttPath);
    QFile::remove(onlineCachePath);
    QFile::remove(onlineTranslatedCachePath);
    QFile::remove(onlineIntakePath);
    QFile::remove(ocrCachePath);
    QFile::remove(ocrTranslatedCachePath);
    QFile::remove(longAsrSourceVttPath);
    QFile::remove(longAsrTranslatedVttPath);
    QFile::remove(longAsrReportPath);
    QFile::remove(enhancedVttPath);
    QFile::remove(enhancedZhVttPath);
    QFile::remove(repairVttPath);
    if (qApp) {
        qApp->setProperty("cgplay.subtitleRefinementDone", false);
        qApp->setProperty("cgplay.subtitleRefinementSuccess", false);
        qApp->setProperty("cgplay.subtitleRefinementVttPath", QString());
        qApp->setProperty("cgplay.subtitleGenerationLastResult", QString());
    }

    if (frame > 0 &&
        QFileInfo(staleZhVttPath).absoluteFilePath() != QFileInfo(translatedVttPath).absoluteFilePath() &&
        QFileInfo(staleZhSrtPath).absoluteFilePath() != QFileInfo(translatedSrtPath).absoluteFilePath()) {
        QFile staleVtt(staleZhVttPath);
        if (staleVtt.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            staleVtt.write("WEBVTT\n\n00:00:00.000 --> 00:00:05.000\nCGPlay subtitle generation smoke test\n");
        }
        QFile staleSrt(staleZhSrtPath);
        if (staleSrt.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            staleSrt.write("1\n00:00:00,000 --> 00:00:05,000\nCGPlay subtitle generation smoke test\n");
        }
    }

    if (_p->playbackCtrl && mediaOpened && frame > 0) {
        const int targetFrame = std::clamp(frame, 0, std::max(0, _p->playbackCtrl->totalFrames() - 1));
        _p->playbackCtrl->seekToFrame(targetFrame);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        QThread::msleep(200);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
    }

    if (qApp) {
        const QString requestedPlaybackMode =
            qApp->property("cgplay.subtitleGenerationPlaybackMode").toString().trimmed();
        if (!requestedPlaybackMode.isEmpty()) {
            _setTranslationPlaybackMode(requestedPlaybackMode);
        }
    }

    auto* translationButton = findChild<QToolButton*>(QStringLiteral("PlaybackBarTranslationToggle"));
    addAssertion(
        QStringLiteral("Playback translation button"),
        translationButton != nullptr,
        translationButton ? QStringLiteral("button found") : QStringLiteral("button not found"));
    QElapsedTimer firstSubtitleTimer;
    firstSubtitleTimer.start();
    if (translationButton && !translationButton->isChecked()) {
        translationButton->click();
    }

    if (cancelAfterMs > 0) {
        const bool generationStarted = pumpUntil([this] {
            return _p->subtitleGenerationBusy;
        }, 10000);
        if (generationStarted) {
            QThread::msleep(cancelAfterMs);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
            close();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        }
        const bool closeIgnoredWhileBusy = generationStarted && isVisible();
        const bool cancelFinished = pumpUntil([this] {
            return !_p->subtitleGenerationBusy;
        }, mockWithoutApi ? 10000 : 120000);
        addAssertion(
            QStringLiteral("Subtitle generation started before cancel"),
            generationStarted,
            generationStarted ? QStringLiteral("subtitle generation became busy")
                              : QStringLiteral("subtitle generation did not become busy"));
        addAssertion(
            QStringLiteral("Close is delayed while canceling"),
            closeIgnoredWhileBusy,
            closeIgnoredWhileBusy ? QStringLiteral("window stayed alive while cancel request was issued")
                                  : QStringLiteral("window closed or was not visible during cancel"));
        addAssertion(
            QStringLiteral("Cancel completes cleanly"),
            cancelFinished,
            cancelFinished ? QStringLiteral("subtitle generation left busy state")
                           : QStringLiteral("subtitle generation stayed busy after cancel"));

        auto* overlayLabel = findChild<QLabel*>(QStringLiteral("GeneratedSubtitleOverlayLabel"));
        report.insert(QStringLiteral("generation"), QJsonObject{
            { QStringLiteral("cancelAfterMs"), cancelAfterMs },
            { QStringLiteral("started"), generationStarted },
            { QStringLiteral("closeIgnoredWhileBusy"), closeIgnoredWhileBusy },
            { QStringLiteral("finishedAfterCancel"), cancelFinished },
            { QStringLiteral("busy"), _p->subtitleGenerationBusy },
            { QStringLiteral("translationButtonChecked"), translationButton ? translationButton->isChecked() : false }
        });
        report.insert(QStringLiteral("subtitle_overlay"), QJsonObject{
            { QStringLiteral("visible"), overlayLabel ? overlayLabel->isVisible() : false },
            { QStringLiteral("text"), overlayLabel ? overlayLabel->text().trimmed() : QString() }
        });
        report.insert(QStringLiteral("results"), assertions);
        report.insert(QStringLiteral("summary"), makeSummary(assertions));
        return report;
    }

    const bool subtitleFileReady = pumpUntil([translatedVttPath] {
        return QFileInfo::exists(translatedVttPath) &&
            !SubtitleGenerationService::readSubtitleFile(translatedVttPath).isEmpty();
    }, mockWithoutApi ? 10000 : 120000);
    const qint64 firstSubtitleReadyElapsedMs = firstSubtitleTimer.elapsed();
    if (subtitleFileReady && qApp) {
        pumpUntil([] {
            return qApp && !qApp->property("cgplay.subtitleGenerationLastResult").toString().trimmed().isEmpty();
        }, 15000);
    }
    if (!subtitleFileReady && _p->playbackBar) {
        _p->playbackBar->setTranslationVisible(false);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
    }
    QVector<GeneratedSubtitleCue> generatedCues =
        SubtitleGenerationService::readSubtitleFile(translatedVttPath);
    const double smokeFps = _p->playbackCtrl && _p->playbackCtrl->fps() > 0.0 ? _p->playbackCtrl->fps() : 24.0;
    const double requestedSeconds = frame > 0 ? frame / smokeFps : 0.0;
    const auto cueHasDisplayableText = [](const GeneratedSubtitleCue& cue) {
        return smokeCueHasDisplayableTranslation(cue);
    };
    const auto translatedCueCount = [&cueHasDisplayableText](const QVector<GeneratedSubtitleCue>& cues) {
        int count = 0;
        for (const auto& cue : cues) {
            if (cueHasDisplayableText(cue)) {
                ++count;
            }
        }
        return count;
    };
    const auto sourceCoverageEnd = [](const QVector<GeneratedSubtitleCue>& cues) {
        double maxEnd = 0.0;
        for (const auto& cue : cues) {
            maxEnd = std::max(maxEnd, cue.endSeconds);
        }
        return maxEnd;
    };
    const auto translatedCoverageEnd = [&cueHasDisplayableText](const QVector<GeneratedSubtitleCue>& cues) {
        double maxEnd = 0.0;
        for (const auto& cue : cues) {
            if (cueHasDisplayableText(cue)) {
                maxEnd = std::max(maxEnd, cue.endSeconds);
            }
        }
        return maxEnd;
    };
    bool generatedCueCoversRequestedFrame = frame <= 0;
    for (const auto& cue : generatedCues) {
        if (cueHasDisplayableText(cue) &&
            requestedSeconds + 0.02 >= cue.startSeconds &&
            requestedSeconds <= cue.endSeconds + 0.02) {
            generatedCueCoversRequestedFrame = true;
            break;
        }
    }
    const bool generationFinished = subtitleFileReady && !generatedCues.isEmpty();
    const bool refinedFileReady = mockWithoutApi && generationFinished
        ? pumpUntil([this, refinedVttPath] {
              return !_p->subtitleRefinementBusy &&
                  QFileInfo::exists(refinedVttPath) &&
                  !SubtitleGenerationService::readSubtitleFile(refinedVttPath).isEmpty();
          }, 10000)
        : (QFileInfo::exists(refinedVttPath) &&
           !SubtitleGenerationService::readSubtitleFile(refinedVttPath).isEmpty());
    const QVector<GeneratedSubtitleCue> refinedCues =
        SubtitleGenerationService::readSubtitleFile(refinedVttPath);
    const QVector<GeneratedSubtitleCue> enhancedCues =
        SubtitleGenerationService::readSubtitleFile(enhancedVttPath);
    const QVector<GeneratedSubtitleCue> onlineCuesBeforeScheduler =
        SubtitleGenerationService::readSubtitleFile(onlineCachePath);
    const QVector<GeneratedSubtitleCue> onlineTranslatedCuesBeforeScheduler =
        SubtitleGenerationService::readSubtitleFile(onlineTranslatedCachePath);
    const QVector<GeneratedSubtitleCue> ocrCuesBeforeScheduler =
        SubtitleGenerationService::readSubtitleFile(ocrCachePath);
    const QVector<GeneratedSubtitleCue> ocrTranslatedCuesBeforeScheduler =
        SubtitleGenerationService::readSubtitleFile(ocrTranslatedCachePath);
    const QVector<GeneratedSubtitleCue> repairCues =
        SubtitleGenerationService::readSubtitleFile(repairVttPath);
    const QDateTime quickLastModifiedBeforeScheduler =
        QFileInfo::exists(translatedVttPath) ? QFileInfo(translatedVttPath).lastModified() : QDateTime();
    if (_p->translationPlaybackMode == QStringLiteral("high-quality") &&
        qApp &&
        qApp->property("cgplay.subtitleHighQualityPrePlaybackWaitTimeoutMs").toLongLong() > 0) {
        const qint64 waitTimeoutMs =
            qApp->property("cgplay.subtitleHighQualityPrePlaybackWaitTimeoutMs").toLongLong();
        pumpUntil([this] {
            return !_p->translationPrePlaybackWaitActive;
        }, static_cast<int>(std::min<qint64>(waitTimeoutMs + 3000, 30000)));
    }
    QJsonObject generation{
        { QStringLiteral("success"), generationFinished && !generatedCues.isEmpty() },
        { QStringLiteral("finished"), generationFinished },
        { QStringLiteral("busy"), _p->subtitleGenerationBusy },
        { QStringLiteral("refinementBusy"), _p->subtitleRefinementBusy },
        { QStringLiteral("refinementLastError"), _p->subtitleRefinementLastError },
        { QStringLiteral("refinementLastValidation"), _p->subtitleRefinementLastValidation },
        { QStringLiteral("backgroundBusy"), _p->subtitleGenerationBusy },
        { QStringLiteral("lastError"), _p->subtitleGenerationLastError },
        { QStringLiteral("sourceSrtPath"), sourceSrtPath },
        { QStringLiteral("translatedSrtPath"), translatedSrtPath },
        { QStringLiteral("translatedVttPath"), translatedVttPath },
        { QStringLiteral("refinedSrtPath"), refinedSrtPath },
        { QStringLiteral("refinedVttPath"), refinedVttPath },
        { QStringLiteral("enhancedVttPath"), enhancedVttPath },
        { QStringLiteral("onlineCachePath"), onlineCachePath },
        { QStringLiteral("onlineTranslatedCachePath"), onlineTranslatedCachePath },
        { QStringLiteral("onlineIntakePath"), onlineIntakePath },
        { QStringLiteral("ocrCachePath"), ocrCachePath },
        { QStringLiteral("ocrTranslatedCachePath"), ocrTranslatedCachePath },
        { QStringLiteral("repairVttPath"), repairVttPath },
        { QStringLiteral("refinedFileReady"), refinedFileReady },
        { QStringLiteral("refinedCueCount"), refinedCues.size() },
        { QStringLiteral("enhancedFileReady"), QFileInfo::exists(enhancedVttPath) && !enhancedCues.isEmpty() },
        { QStringLiteral("enhancedCueCount"), enhancedCues.size() },
        { QStringLiteral("onlineCacheReadyBeforeScheduler"),
          QFileInfo::exists(onlineCachePath) && !onlineCuesBeforeScheduler.isEmpty() },
        { QStringLiteral("onlineCueCountBeforeScheduler"), onlineCuesBeforeScheduler.size() },
        { QStringLiteral("onlineTranslatedCacheReadyBeforeScheduler"),
          QFileInfo::exists(onlineTranslatedCachePath) && !onlineTranslatedCuesBeforeScheduler.isEmpty() },
        { QStringLiteral("onlineTranslatedCueCountBeforeScheduler"), onlineTranslatedCuesBeforeScheduler.size() },
        { QStringLiteral("ocrCacheReadyBeforeScheduler"),
          QFileInfo::exists(ocrCachePath) && !ocrCuesBeforeScheduler.isEmpty() },
        { QStringLiteral("ocrCueCountBeforeScheduler"), ocrCuesBeforeScheduler.size() },
        { QStringLiteral("ocrTranslatedCacheReadyBeforeScheduler"),
          QFileInfo::exists(ocrTranslatedCachePath) && !ocrTranslatedCuesBeforeScheduler.isEmpty() },
        { QStringLiteral("ocrTranslatedCueCountBeforeScheduler"), ocrTranslatedCuesBeforeScheduler.size() },
        { QStringLiteral("repairFileReady"), QFileInfo::exists(repairVttPath) && !repairCues.isEmpty() },
        { QStringLiteral("repairCueCount"), repairCues.size() },
        { QStringLiteral("refinementStarted"), qApp ? qApp->property("cgplay.subtitleRefinementStarted").toBool() : false },
        { QStringLiteral("refinementDecision"), qApp ? qApp->property("cgplay.subtitleRefinementDecision").toString() : QString() },
        { QStringLiteral("refinementDedicatedWorkspace"), qApp ? qApp->property("cgplay.subtitleRefinementDedicatedWorkspace").toBool() : false },
        { QStringLiteral("staleZhVttPath"), staleZhVttPath },
        { QStringLiteral("staleZhSrtPath"), staleZhSrtPath },
        { QStringLiteral("requestedFrame"), frame },
        { QStringLiteral("requestedSeconds"), requestedSeconds },
        { QStringLiteral("firstSubtitleReadyElapsedMs"), static_cast<double>(firstSubtitleReadyElapsedMs) },
        { QStringLiteral("firstCueStartSeconds"), generatedCues.isEmpty() ? -1.0 : generatedCues.first().startSeconds },
        { QStringLiteral("firstCueEndSeconds"), generatedCues.isEmpty() ? -1.0 : generatedCues.first().endSeconds },
        { QStringLiteral("coversRequestedFrame"), generatedCueCoversRequestedFrame },
        { QStringLiteral("sourceCueCount"), generatedCues.size() },
        { QStringLiteral("translatedCueCount"), translatedCueCount(generatedCues) },
        { QStringLiteral("sourceCoverageEndSeconds"), sourceCoverageEnd(generatedCues) },
        { QStringLiteral("translatedCoverageEndSeconds"), translatedCoverageEnd(generatedCues) }
    };
    QJsonObject playbackStrategyDiagnostics =
        _translationPlaybackStrategyDiagnostics(_p->translationPlaybackMode == QStringLiteral("high-quality"));
    auto resolvedEnhancementScheduler = [](const QJsonObject& strategyDiagnostics) {
        const QJsonObject lastResult =
            strategyDiagnostics.value(QStringLiteral("currentMediaHighQualityLastResult")).toObject();
        return lastResult.isEmpty()
            ? strategyDiagnostics.value(QStringLiteral("enhancementScheduler")).toObject()
            : lastResult;
    };
    generation.insert(QStringLiteral("translationPlaybackMode"), _p->translationPlaybackMode);
    generation.insert(QStringLiteral("translationPlaybackModeLabel"),
                      playbackStrategyDiagnostics.value(QStringLiteral("modeLabel")).toString());
    generation.insert(QStringLiteral("translationPlaybackStrategy"), playbackStrategyDiagnostics);
    QJsonObject enhancementSchedulerDiagnostics =
        resolvedEnhancementScheduler(playbackStrategyDiagnostics);
    generation.insert(QStringLiteral("enhancementScheduler"), enhancementSchedulerDiagnostics);
    if (_p->translationPlaybackMode == QStringLiteral("high-quality")) {
        const double currentSecondsForHqWait =
            _p->playbackCtrl && _p->playbackCtrl->fps() > 0.0
            ? static_cast<double>(_p->playbackCtrl->currentFrame()) / _p->playbackCtrl->fps()
            : 0.0;
        auto hasCurrentTranslatedCue = [this, currentSecondsForHqWait] {
            for (const GeneratedSubtitleCue& cue : _p->generatedSubtitleCues) {
                if (!cue.translatedText.trimmed().isEmpty() &&
                    cue.startSeconds <= currentSecondsForHqWait + 0.02 &&
                    cue.endSeconds + 0.02 >= currentSecondsForHqWait) {
                    return true;
                }
            }
            return false;
        };
        pumpUntil([this, hasCurrentTranslatedCue] {
            return hasCurrentTranslatedCue() ||
                (!_p->subtitleGenerationBusy && !_p->subtitleContinuationScheduled);
        }, mockWithoutApi ? 10000 : 120000);
        if (!_p->highQualityEnhancementBusy &&
            (!QFileInfo::exists(enhancedVttPath) ||
             SubtitleGenerationService::readSubtitleFile(enhancedVttPath).isEmpty())) {
            _startCurrentMediaHighQualityEnhancement(QStringLiteral("smoke-current-media-high-quality"));
        }
        pumpUntil([this] {
            return !_p->highQualityEnhancementBusy;
        }, mockWithoutApi ? 15000 : 180000);
        if (QFileInfo::exists(enhancedVttPath) &&
            !SubtitleGenerationService::readSubtitleFile(enhancedVttPath).isEmpty()) {
            _loadGeneratedSubtitleTrack();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        if (_p->highQualityEnhancementBusy) {
            pumpUntil([this] {
                return !_p->highQualityEnhancementBusy;
            }, mockWithoutApi ? 15000 : 180000);
            if (QFileInfo::exists(enhancedVttPath) &&
                !SubtitleGenerationService::readSubtitleFile(enhancedVttPath).isEmpty()) {
                _loadGeneratedSubtitleTrack();
                QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
            }
        }
        playbackStrategyDiagnostics =
            _translationPlaybackStrategyDiagnostics(true);
        enhancementSchedulerDiagnostics =
            resolvedEnhancementScheduler(playbackStrategyDiagnostics);
        generation.insert(QStringLiteral("translationPlaybackStrategy"), playbackStrategyDiagnostics);
        generation.insert(QStringLiteral("enhancementScheduler"), enhancementSchedulerDiagnostics);
    }
    QVector<GeneratedSubtitleCue> enhancedCuesAfterScheduler =
        SubtitleGenerationService::readSubtitleFile(enhancedVttPath);
    QVector<GeneratedSubtitleCue> onlineCuesAfterScheduler =
        SubtitleGenerationService::readSubtitleFile(onlineCachePath);
    QVector<GeneratedSubtitleCue> onlineTranslatedCuesAfterScheduler =
        SubtitleGenerationService::readSubtitleFile(onlineTranslatedCachePath);
    QVector<GeneratedSubtitleCue> ocrCuesAfterScheduler =
        SubtitleGenerationService::readSubtitleFile(ocrCachePath);
    QVector<GeneratedSubtitleCue> ocrTranslatedCuesAfterScheduler =
        SubtitleGenerationService::readSubtitleFile(ocrTranslatedCachePath);
    QVector<GeneratedSubtitleCue> longAsrSourceCuesAfterScheduler =
        SubtitleGenerationService::readSubtitleFile(longAsrSourceVttPath);
    QVector<GeneratedSubtitleCue> longAsrTranslatedCuesAfterScheduler =
        SubtitleGenerationService::readSubtitleFile(longAsrTranslatedVttPath);
    QVector<GeneratedSubtitleCue> repairCuesAfterScheduler =
        SubtitleGenerationService::readSubtitleFile(repairVttPath);
    if (_p->translationPlaybackMode == QStringLiteral("high-quality") &&
        QFileInfo::exists(enhancedVttPath) &&
        !enhancedCuesAfterScheduler.isEmpty()) {
        _loadGeneratedSubtitleTrack();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        playbackStrategyDiagnostics =
            _translationPlaybackStrategyDiagnostics(true);
        enhancementSchedulerDiagnostics =
            resolvedEnhancementScheduler(playbackStrategyDiagnostics);
        generation.insert(QStringLiteral("translationPlaybackStrategy"), playbackStrategyDiagnostics);
        generation.insert(QStringLiteral("enhancementScheduler"), enhancementSchedulerDiagnostics);
        enhancedCuesAfterScheduler =
            SubtitleGenerationService::readSubtitleFile(enhancedVttPath);
        onlineCuesAfterScheduler =
            SubtitleGenerationService::readSubtitleFile(onlineCachePath);
        onlineTranslatedCuesAfterScheduler =
            SubtitleGenerationService::readSubtitleFile(onlineTranslatedCachePath);
        ocrCuesAfterScheduler =
            SubtitleGenerationService::readSubtitleFile(ocrCachePath);
        ocrTranslatedCuesAfterScheduler =
            SubtitleGenerationService::readSubtitleFile(ocrTranslatedCachePath);
        longAsrSourceCuesAfterScheduler =
            SubtitleGenerationService::readSubtitleFile(longAsrSourceVttPath);
        longAsrTranslatedCuesAfterScheduler =
            SubtitleGenerationService::readSubtitleFile(longAsrTranslatedVttPath);
        repairCuesAfterScheduler =
            SubtitleGenerationService::readSubtitleFile(repairVttPath);
    }
    const QDateTime quickLastModifiedAfterScheduler =
        QFileInfo::exists(translatedVttPath) ? QFileInfo(translatedVttPath).lastModified() : QDateTime();
    generation.insert(QStringLiteral("enhancedFileReady"),
                      QFileInfo::exists(enhancedVttPath) && !enhancedCuesAfterScheduler.isEmpty());
    generation.insert(QStringLiteral("enhancedCueCount"), enhancedCuesAfterScheduler.size());
    generation.insert(QStringLiteral("onlineCacheReady"),
                      QFileInfo::exists(onlineCachePath) && !onlineCuesAfterScheduler.isEmpty());
    generation.insert(QStringLiteral("onlineCueCount"), onlineCuesAfterScheduler.size());
    generation.insert(QStringLiteral("onlineTranslatedCacheReady"),
                      QFileInfo::exists(onlineTranslatedCachePath) && !onlineTranslatedCuesAfterScheduler.isEmpty());
    generation.insert(QStringLiteral("onlineTranslatedCueCount"), onlineTranslatedCuesAfterScheduler.size());
    generation.insert(QStringLiteral("ocrCacheReady"),
                      QFileInfo::exists(ocrCachePath) && !ocrCuesAfterScheduler.isEmpty());
    generation.insert(QStringLiteral("ocrCueCount"), ocrCuesAfterScheduler.size());
    generation.insert(QStringLiteral("ocrTranslatedCacheReady"),
                      QFileInfo::exists(ocrTranslatedCachePath) && !ocrTranslatedCuesAfterScheduler.isEmpty());
    generation.insert(QStringLiteral("ocrTranslatedCueCount"), ocrTranslatedCuesAfterScheduler.size());
    generation.insert(QStringLiteral("longAsrSourceCacheReady"),
                      QFileInfo::exists(longAsrSourceVttPath) && !longAsrSourceCuesAfterScheduler.isEmpty());
    generation.insert(QStringLiteral("longAsrSourceCueCount"), longAsrSourceCuesAfterScheduler.size());
    generation.insert(QStringLiteral("longAsrTranslatedCacheReady"),
                      QFileInfo::exists(longAsrTranslatedVttPath) && !longAsrTranslatedCuesAfterScheduler.isEmpty());
    generation.insert(QStringLiteral("longAsrTranslatedCueCount"), longAsrTranslatedCuesAfterScheduler.size());
    generation.insert(QStringLiteral("longAsrSourceVttPath"), longAsrSourceVttPath);
    generation.insert(QStringLiteral("longAsrTranslatedVttPath"), longAsrTranslatedVttPath);
    generation.insert(QStringLiteral("longAsrReportPath"), longAsrReportPath);
    generation.insert(QStringLiteral("repairCacheReady"),
                      QFileInfo::exists(repairVttPath) && !repairCuesAfterScheduler.isEmpty());
    generation.insert(QStringLiteral("repairCueCountAfterScheduler"), repairCuesAfterScheduler.size());
    generation.insert(QStringLiteral("quickZhModifiedByEnhancementScheduler"),
                      quickLastModifiedBeforeScheduler.isValid() &&
                          quickLastModifiedAfterScheduler.isValid() &&
                          quickLastModifiedAfterScheduler != quickLastModifiedBeforeScheduler);
    generation.insert(QStringLiteral("manualFusionWorker"),
                      enhancementSchedulerDiagnostics.value(QStringLiteral("localFusionWorker")).toObject());
    generation.insert(QStringLiteral("manualOnlineWorker"),
                      enhancementSchedulerDiagnostics.value(QStringLiteral("manualOnlineWorker")).toObject());
    generation.insert(QStringLiteral("manualOnlineTranslationWorker"),
                      enhancementSchedulerDiagnostics.value(QStringLiteral("manualOnlineTranslationWorker")).toObject());
    generation.insert(QStringLiteral("manualOcrWorker"),
                      enhancementSchedulerDiagnostics.value(QStringLiteral("manualOcrWorker")).toObject());
    generation.insert(QStringLiteral("manualLongAsrWorker"),
                      enhancementSchedulerDiagnostics.value(QStringLiteral("manualLongAsrWorker")).toObject());
    generation.insert(QStringLiteral("manualRepairWorker"),
                      enhancementSchedulerDiagnostics.value(QStringLiteral("manualRepairWorker")).toObject());
    generation.insert(QStringLiteral("onlineSourceIntake"),
                      enhancementSchedulerDiagnostics.value(QStringLiteral("onlineSourceIntake")).toObject());
    generation.insert(QStringLiteral("translationPlaybackWaited"),
                      playbackStrategyDiagnostics.value(QStringLiteral("waited")).toBool());
    generation.insert(QStringLiteral("translationPlaybackWaitMs"),
                      playbackStrategyDiagnostics.value(QStringLiteral("waitMs")).toDouble());
    generation.insert(QStringLiteral("quickFallbackCueCount"),
                      playbackStrategyDiagnostics.value(QStringLiteral("quickFallbackCueCount")).toInt());
    generation.insert(QStringLiteral("refinedMerged"),
                      playbackStrategyDiagnostics.value(QStringLiteral("refinedMerged")).toBool());
    generation.insert(QStringLiteral("enhancedMerged"),
                      playbackStrategyDiagnostics.value(QStringLiteral("enhancedMerged")).toBool());
    generation.insert(QStringLiteral("enhancedLoaded"),
                      playbackStrategyDiagnostics.value(QStringLiteral("enhancedLoaded")).toBool());
    generation.insert(QStringLiteral("enhancedCueMatches"),
                      playbackStrategyDiagnostics.value(QStringLiteral("enhancedCueMatches")).toInt());
    generation.insert(QStringLiteral("enhancedRejectedReason"),
                      playbackStrategyDiagnostics.value(QStringLiteral("enhancedRejectedReason")).toString());
    generation.insert(QStringLiteral("wholeTrackReplacement"),
                      playbackStrategyDiagnostics.value(QStringLiteral("wholeTrackReplacement")).toBool());
    generation.insert(QStringLiteral("currentCueFallbackSafe"),
                      playbackStrategyDiagnostics.value(QStringLiteral("currentCueFallbackSafe")).toBool());
    generation.insert(QStringLiteral("refinedTookOverTrack"), false);
    generation.insert(QStringLiteral("enhancedTookOverTrack"), false);
    if (_p->translationPlaybackMode == QStringLiteral("high-quality")) {
        const QJsonObject policy =
            playbackStrategyDiagnostics.value(QStringLiteral("policy")).toObject();
        const QJsonObject decision =
            playbackStrategyDiagnostics.value(QStringLiteral("decision")).toObject();
        const bool waitImplemented =
            policy.value(QStringLiteral("prePlaybackWaitImplemented")).toBool() &&
            decision.value(QStringLiteral("prePlaybackWaitImplemented")).toBool();
        const bool waitCancelable =
            playbackStrategyDiagnostics.value(QStringLiteral("prePlaybackWaitCancelable")).toBool() &&
            decision.value(QStringLiteral("prePlaybackWaitCancelable")).toBool();
        const bool timeoutFallback =
            decision.value(QStringLiteral("prePlaybackWaitTimeoutFallbackToQuick")).toBool() &&
            policy.value(QStringLiteral("fallbackToQuickAllowed")).toBool();
        const bool noPlaybackPause =
            !playbackStrategyDiagnostics.value(QStringLiteral("playbackPausedForMode")).toBool() &&
            !decision.value(QStringLiteral("pausePlayback")).toBool() &&
            !decision.value(QStringLiteral("pausesPlayback")).toBool();
        const bool waitResolvedOrActive =
            playbackStrategyDiagnostics.value(QStringLiteral("prePlaybackWaitActive")).toBool() ||
            playbackStrategyDiagnostics.value(QStringLiteral("prePlaybackWaitTimedOut")).toBool() ||
            playbackStrategyDiagnostics.value(QStringLiteral("prePlaybackWaitCanceled")).toBool() ||
            playbackStrategyDiagnostics.value(QStringLiteral("prePlaybackWaitDegradedToQuick")).toBool() ||
            (!playbackStrategyDiagnostics.value(QStringLiteral("waited")).toBool() &&
             playbackStrategyDiagnostics.value(QStringLiteral("waitMs")).toInt() == 0) ||
            playbackStrategyDiagnostics.value(QStringLiteral("prePlaybackWaitResult")).toString() ==
                QStringLiteral("hq-target-coverage-ready") ||
            playbackStrategyDiagnostics.value(QStringLiteral("prePlaybackWaitResult")).toString() ==
                QStringLiteral("hq-worker-target-processed") ||
            playbackStrategyDiagnostics.value(QStringLiteral("prePlaybackWaitResult")).toString() ==
                QStringLiteral("target-coverage-ready");
        addAssertion(
            QStringLiteral("High-quality pre-playback wait is bounded"),
            waitImplemented && waitCancelable && timeoutFallback,
            waitImplemented && waitCancelable && timeoutFallback
                ? QStringLiteral("manual high-quality wait is implemented, cancelable, and falls back to quick")
                : QStringLiteral("manual high-quality wait boundary is incomplete"),
            playbackStrategyDiagnostics);
        addAssertion(
            QStringLiteral("High-quality wait does not pause playback"),
            noPlaybackPause && waitResolvedOrActive,
            noPlaybackPause && waitResolvedOrActive
                ? QStringLiteral("high-quality wait resolved or stayed active without pausing playback")
                : QStringLiteral("high-quality wait paused playback or did not report a bounded state"),
            playbackStrategyDiagnostics);
        const QJsonObject localFusionWorker =
            enhancementSchedulerDiagnostics.value(QStringLiteral("localFusionWorker")).toObject();
        const QJsonObject manualOnlineWorker =
            enhancementSchedulerDiagnostics.value(QStringLiteral("manualOnlineWorker")).toObject();
        const QJsonObject manualOnlineTranslationWorker =
            enhancementSchedulerDiagnostics.value(QStringLiteral("manualOnlineTranslationWorker")).toObject();
        const QJsonObject manualOcrWorker =
            enhancementSchedulerDiagnostics.value(QStringLiteral("manualOcrWorker")).toObject();
        const QJsonObject manualLongAsrWorker =
            enhancementSchedulerDiagnostics.value(QStringLiteral("manualLongAsrWorker")).toObject();
        const QJsonObject manualRepairWorker =
            enhancementSchedulerDiagnostics.value(QStringLiteral("manualRepairWorker")).toObject();
        const QJsonObject onlineSourceIntake =
            enhancementSchedulerDiagnostics.value(QStringLiteral("onlineSourceIntake")).toObject();
        const bool localFusionSuccess = localFusionWorker.value(QStringLiteral("success")).toBool();
        const bool sourceCacheAvailable =
            refinedFileReady ||
            !repairCuesAfterScheduler.isEmpty() ||
            !onlineTranslatedCuesAfterScheduler.isEmpty() ||
            !ocrTranslatedCuesAfterScheduler.isEmpty() ||
            !longAsrTranslatedCuesAfterScheduler.isEmpty();
        const bool mockOnlineWorker =
            qApp && qApp->property("cgplay.subtitleGenerationMockOnlineWorker").toBool();
        const bool mockOcrWorker =
            qApp && qApp->property("cgplay.subtitleGenerationMockOcrWorker").toBool();
        const bool mockRepairWorker =
            qApp && qApp->property("cgplay.subtitleGenerationMockRepairWorker").toBool();
        const QString smokeMediaNameLower = QFileInfo(mediaPath).fileName().toLower();
        const bool filenameExplicitlyNoSubtitles =
            smokeMediaNameLower.contains(QStringLiteral("no_subtitle")) ||
            smokeMediaNameLower.contains(QStringLiteral("no-subtitle")) ||
            smokeMediaNameLower.contains(QStringLiteral("nosubtitle")) ||
            smokeMediaNameLower.contains(QStringLiteral("no subtitles")) ||
            smokeMediaNameLower.contains(QStringLiteral("no-subtitles")) ||
            smokeMediaNameLower.contains(QStringLiteral("without_subtitle")) ||
            smokeMediaNameLower.contains(QStringLiteral("without-subtitle"));
        const bool hardSubFilenameHint =
            smokeMediaNameLower.contains(QStringLiteral("ani-one")) ||
            smokeMediaNameLower.contains(QString::fromUtf8(u8"字幕")) ||
            smokeMediaNameLower.contains(QString::fromUtf8(u8"繁中")) ||
            smokeMediaNameLower.contains(QString::fromUtf8(u8"影之"));
        const bool hardSubMediaHint =
            !filenameExplicitlyNoSubtitles &&
            (hardSubFilenameHint ||
            smokeMediaNameLower.contains(QStringLiteral("hardsub")) ||
            smokeMediaNameLower.contains(QStringLiteral("hard-sub")) ||
            smokeMediaNameLower.contains(QStringLiteral("subbed")) ||
            smokeMediaNameLower.contains(QStringLiteral("burned_subtitle")) ||
            smokeMediaNameLower.contains(QStringLiteral("burned-subtitle")) ||
            smokeMediaNameLower.contains(QStringLiteral("\u5b57\u5e55")) ||
            smokeMediaNameLower.contains(QStringLiteral("\u4e2d\u5b57")) ||
            smokeMediaNameLower.contains(QStringLiteral("\u7e41\u4e2d")) ||
            smokeMediaNameLower.contains(QStringLiteral("\u7e41\u9ad4")) ||
            smokeMediaNameLower.contains(QStringLiteral("\u7b80\u4e2d")) ||
            smokeMediaNameLower.contains(QStringLiteral("\u7c21\u4e2d")) ||
            smokeMediaNameLower.contains(QStringLiteral("\u5f71\u4e4b")));
        const int repairWorkerCueCount =
            manualRepairWorker.value(QStringLiteral("repairCueCount")).toInt();
        const bool manualRepairProduced =
            manualRepairWorker.value(QStringLiteral("repairCacheWritten")).toBool() &&
            QFileInfo::exists(repairVttPath) &&
            repairWorkerCueCount > 0 &&
            repairCuesAfterScheduler.size() == repairWorkerCueCount &&
            manualRepairWorker.value(QStringLiteral("quickTimingPreserved")).toBool() &&
            manualRepairWorker.value(QStringLiteral("quickCueCountPreserved")).toBool() &&
            manualRepairWorker.value(QStringLiteral("terminologyAppliedCueCount")).toInt() > 0 &&
            !manualRepairWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
            !manualRepairWorker.value(QStringLiteral("usesQuickApiQuota")).toBool() &&
            !manualRepairWorker.value(QStringLiteral("usesQuickWorkerLane")).toBool();
        const bool manualRepairSafelyDegraded =
            !manualRepairWorker.value(QStringLiteral("repairCacheWritten")).toBool() &&
            !manualRepairWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
            !manualRepairWorker.value(QStringLiteral("usesQuickApiQuota")).toBool() &&
            !manualRepairWorker.value(QStringLiteral("usesQuickWorkerLane")).toBool();
        const int fusionQuickCueCount =
            localFusionWorker.value(QStringLiteral("quickCueCount")).toInt();
        const int fusionOutputCueCount =
            localFusionWorker.value(QStringLiteral("outputCueCount")).toInt();
        const bool onlineReferenceCacheProduced =
            manualOnlineWorker.value(QStringLiteral("onlineCacheWritten")).toBool() &&
            QFileInfo::exists(onlineCachePath) &&
            !onlineCuesAfterScheduler.isEmpty() &&
            !manualOnlineWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
            !manualOnlineWorker.value(QStringLiteral("usesQuickApiQuota")).toBool() &&
            !manualOnlineWorker.value(QStringLiteral("usesQuickWorkerLane")).toBool();
        const bool onlineReferenceSafelyDegraded =
            !manualOnlineWorker.value(QStringLiteral("onlineCacheWritten")).toBool() &&
            !manualOnlineWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
            !manualOnlineWorker.value(QStringLiteral("usesQuickApiQuota")).toBool() &&
            !manualOnlineWorker.value(QStringLiteral("usesQuickWorkerLane")).toBool() &&
            !manualOnlineWorker.value(QStringLiteral("degradedReason")).toString().trimmed().isEmpty();
        const bool onlineReferenceTranslationProduced =
            manualOnlineTranslationWorker.value(QStringLiteral("translatedOnlineCacheWritten")).toBool() &&
            QFileInfo::exists(onlineTranslatedCachePath) &&
            !onlineTranslatedCuesAfterScheduler.isEmpty() &&
            !manualOnlineTranslationWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
            !manualOnlineTranslationWorker.value(QStringLiteral("usesQuickApiQuota")).toBool() &&
            !manualOnlineTranslationWorker.value(QStringLiteral("usesQuickWorkerLane")).toBool() &&
            !manualOnlineTranslationWorker.value(QStringLiteral("sourceCoverageCountsAsDisplayable")).toBool();
        addAssertion(
            QStringLiteral("Manual local fusion scheduler boundary is safe"),
            enhancementSchedulerDiagnostics.value(QStringLiteral("manualTriggerAccepted")).toBool() &&
                enhancementSchedulerDiagnostics.value(QStringLiteral("cancelable")).toBool() &&
                !enhancementSchedulerDiagnostics.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
                !enhancementSchedulerDiagnostics.value(QStringLiteral("usesQuickApiQuota")).toBool() &&
                !enhancementSchedulerDiagnostics.value(QStringLiteral("usesQuickWorkerLane")).toBool() &&
                !enhancementSchedulerDiagnostics.value(QStringLiteral("quickPathBehaviorChanged")).toBool() &&
                localFusionWorker.value(QStringLiteral("localOnly")).toBool() &&
                !localFusionWorker.value(QStringLiteral("usesApi")).toBool() &&
                !localFusionWorker.value(QStringLiteral("usesQuickWorkerLane")).toBool() &&
                (localFusionSuccess || enhancementSchedulerDiagnostics.value(QStringLiteral("degradedToQuick")).toBool()),
            enhancementSchedulerDiagnostics.value(QStringLiteral("manualTriggerAccepted")).toBool()
                ? QStringLiteral("manual enhancement trigger stayed local-only and kept quick fallback")
                : QStringLiteral("manual enhancement trigger was not accepted"),
            enhancementSchedulerDiagnostics);
        addAssertion(
            QStringLiteral("Manual online worker is isolated"),
            enhancementSchedulerDiagnostics.value(QStringLiteral("manualTriggerAccepted")).toBool() &&
                manualOnlineWorker.value(QStringLiteral("attempted")).toBool() &&
                manualOnlineWorker.value(QStringLiteral("onlineManualAllowed")).toBool() &&
                !manualOnlineWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
                !manualOnlineWorker.value(QStringLiteral("usesQuickApiQuota")).toBool() &&
                !manualOnlineWorker.value(QStringLiteral("usesQuickWorkerLane")).toBool() &&
                !manualOnlineWorker.value(QStringLiteral("usesAsrOcrTranslationRefineRepair")).toBool(),
            manualOnlineWorker.value(QStringLiteral("attempted")).toBool()
                ? QStringLiteral("manual online worker stayed outside quick API, quick workers, and quick sidecars")
                : QStringLiteral("manual online worker did not attempt"),
            manualOnlineWorker);
        addAssertion(
            QStringLiteral("Manual online worker degrades or writes independent cache"),
            onlineReferenceCacheProduced || onlineReferenceSafelyDegraded,
            onlineReferenceCacheProduced
                ? QStringLiteral("online/reference worker wrote independent source cache")
                : QStringLiteral("online worker safely degraded with real provider/config error and no quick-path writes"),
            QJsonObject{
                { QStringLiteral("manualOnlineWorker"), manualOnlineWorker },
                { QStringLiteral("onlineCachePath"), onlineCachePath },
                { QStringLiteral("onlineCueCount"), onlineCuesAfterScheduler.size() },
                { QStringLiteral("quickTranslatedVttPath"), translatedVttPath }
            });
        addAssertion(
            QStringLiteral("Manual online translation cache is independent"),
            onlineReferenceTranslationProduced ||
                (!manualOnlineTranslationWorker.value(QStringLiteral("translatedOnlineCacheWritten")).toBool() &&
                 !manualOnlineTranslationWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
                 !manualOnlineTranslationWorker.value(QStringLiteral("usesQuickApiQuota")).toBool()),
            onlineReferenceTranslationProduced
                ? QStringLiteral("online/reference source produced independent translated cache")
                : QStringLiteral("online translation safely degraded without writing quick sidecars"),
            QJsonObject{
                { QStringLiteral("manualOnlineTranslationWorker"), manualOnlineTranslationWorker },
                { QStringLiteral("onlineTranslatedCachePath"), onlineTranslatedCachePath },
                { QStringLiteral("onlineTranslatedCueCount"), onlineTranslatedCuesAfterScheduler.size() }
            });
        addAssertion(
            QStringLiteral("Manual online source intake is source-only diagnostic"),
            !onlineSourceIntake.value(QStringLiteral("sourceOnlyDisplayable")).toBool() &&
                onlineSourceIntake.value(QStringLiteral("displayCoverageDelta")).toDouble() == 0.0 &&
                !onlineSourceIntake.value(QStringLiteral("quickCoverageChanged")).toBool() &&
                onlineSourceIntake.value(QStringLiteral("rejectedForDisplay")).toString() ==
                    QStringLiteral("source-only-not-displayable") &&
                (!onlineReferenceCacheProduced ||
                 (onlineSourceIntake.value(QStringLiteral("onlineSourceCacheFound")).toBool() &&
                  onlineSourceIntake.value(QStringLiteral("diagnosticsWritten")).toBool())),
            onlineReferenceCacheProduced
                ? QStringLiteral("online source cache was read only for alignment diagnostics and rejected for display")
                : QStringLiteral("manual online intake remained source-only and non-displayable after safe provider degradation"),
            onlineSourceIntake);
        addAssertion(
            QStringLiteral("Manual OCR intake is translated-cache capable and quick-safe"),
            mockOcrWorker
                ? (manualOcrWorker.value(QStringLiteral("ocrCacheWritten")).toBool() &&
                   QFileInfo::exists(ocrCachePath) &&
                   !ocrCuesAfterScheduler.isEmpty() &&
                   manualOcrWorker.value(QStringLiteral("ocrTranslatedCacheWritten")).toBool() &&
                   QFileInfo::exists(ocrTranslatedCachePath) &&
                   !ocrTranslatedCuesAfterScheduler.isEmpty() &&
                   !manualOcrWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
                   !manualOcrWorker.value(QStringLiteral("writesEnhancedZhSidecar")).toBool() &&
                   !manualOcrWorker.value(QStringLiteral("usesQuickApiQuota")).toBool() &&
                   !manualOcrWorker.value(QStringLiteral("usesQuickWorkerLane")).toBool())
                : ((!manualOcrWorker.value(QStringLiteral("ocrCacheWritten")).toBool() ||
                    manualOcrWorker.value(QStringLiteral("ocrTranslatedCacheWritten")).toBool()) &&
                   !manualOcrWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
                   !manualOcrWorker.value(QStringLiteral("usesQuickApiQuota")).toBool()),
            mockOcrWorker
                ? QStringLiteral("mock OCR wrote independent OCR source and translated caches")
                : QStringLiteral("OCR safely degraded, or wrote independent translated cache when enabled"),
            QJsonObject{
                { QStringLiteral("manualOcrWorker"), manualOcrWorker },
                { QStringLiteral("ocrCachePath"), ocrCachePath },
                { QStringLiteral("ocrCueCount"), ocrCuesAfterScheduler.size() },
                { QStringLiteral("ocrTranslatedCachePath"), ocrTranslatedCachePath },
                { QStringLiteral("ocrTranslatedCueCount"), ocrTranslatedCuesAfterScheduler.size() }
            });
        if (hardSubMediaHint && !mockOcrWorker) {
            const bool fullNameSidecars =
                QFileInfo(ocrCachePath).baseName().startsWith(QFileInfo(mediaPath).completeBaseName()) &&
                QFileInfo(ocrTranslatedCachePath).baseName().startsWith(QFileInfo(mediaPath).completeBaseName()) &&
                QFileInfo(enhancedVttPath).baseName().startsWith(QFileInfo(mediaPath).completeBaseName());
            addAssertion(
                QStringLiteral("Hard-sub hinted high-quality media produces full-filename OCR/enhanced caches"),
                manualOcrWorker.value(QStringLiteral("attempted")).toBool() &&
                    manualOcrWorker.value(QStringLiteral("ocrCacheWritten")).toBool() &&
                    QFileInfo::exists(ocrCachePath) &&
                    !ocrCuesAfterScheduler.isEmpty() &&
                    manualOcrWorker.value(QStringLiteral("ocrTranslatedCacheWritten")).toBool() &&
                    QFileInfo::exists(ocrTranslatedCachePath) &&
                    !ocrTranslatedCuesAfterScheduler.isEmpty() &&
                    QFileInfo::exists(enhancedVttPath) &&
                    !enhancedCuesAfterScheduler.isEmpty() &&
                    fullNameSidecars &&
                    !manualOcrWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool(),
                manualOcrWorker.value(QStringLiteral("ocrCacheWritten")).toBool()
                    ? QStringLiteral("hard-sub OCR wrote full-filename independent caches")
                    : manualOcrWorker.value(QStringLiteral("degradedReason")).toString(
                          QStringLiteral("ocr-not-run")),
                QJsonObject{
                    { QStringLiteral("mediaPath"), mediaPath },
                    { QStringLiteral("ocrCachePath"), ocrCachePath },
                    { QStringLiteral("ocrTranslatedCachePath"), ocrTranslatedCachePath },
                    { QStringLiteral("enhancedVttPath"), enhancedVttPath },
                    { QStringLiteral("manualOcrWorker"), manualOcrWorker },
                    { QStringLiteral("activeSource"), enhancementSchedulerDiagnostics.value(QStringLiteral("activeSource")).toString() }
                });
        }
        addAssertion(
            QStringLiteral("Manual long-window ASR is independent or explicitly blocked"),
            manualLongAsrWorker.value(QStringLiteral("attempted")).toBool()
                ? (!manualLongAsrWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
                   !manualLongAsrWorker.value(QStringLiteral("usesQuickWorkerLane")).toBool() &&
                   (manualLongAsrWorker.value(QStringLiteral("success")).toBool()
                        ? (QFileInfo::exists(longAsrSourceVttPath) &&
                           !longAsrSourceCuesAfterScheduler.isEmpty() &&
                           QFileInfo::exists(longAsrTranslatedVttPath) &&
                           !longAsrTranslatedCuesAfterScheduler.isEmpty())
                        : !manualLongAsrWorker.value(QStringLiteral("fallbackReason")).toString().trimmed().isEmpty()))
                : (!manualLongAsrWorker.value(QStringLiteral("defaultEnabled")).toBool() &&
                   !manualLongAsrWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
                   !manualLongAsrWorker.value(QStringLiteral("usesQuickWorkerLane")).toBool()),
            manualLongAsrWorker.value(QStringLiteral("attempted")).toBool()
                ? (manualLongAsrWorker.value(QStringLiteral("success")).toBool()
                      ? QStringLiteral("long-window ASR wrote independent source and HQ translated caches")
                      : QStringLiteral("long-window ASR reported an explicit provider/blocker"))
                : QStringLiteral("long-window ASR stayed manual/default-disabled"),
            QJsonObject{
                { QStringLiteral("manualLongAsrWorker"), manualLongAsrWorker },
                { QStringLiteral("longAsrSourceVttPath"), longAsrSourceVttPath },
                { QStringLiteral("longAsrSourceCueCount"), longAsrSourceCuesAfterScheduler.size() },
                { QStringLiteral("longAsrTranslatedVttPath"), longAsrTranslatedVttPath },
                { QStringLiteral("longAsrTranslatedCueCount"), longAsrTranslatedCuesAfterScheduler.size() },
                { QStringLiteral("longAsrReportPath"), longAsrReportPath }
            });
        addAssertion(
            QStringLiteral("Manual repair terminology writes independent cue-level cache"),
            manualRepairProduced || manualRepairSafelyDegraded,
            manualRepairProduced
                ? (mockRepairWorker
                      ? QStringLiteral("mock repair/terminology preserved quick timing and wrote independent repair cache")
                      : QStringLiteral("local repair/terminology preserved quick timing and wrote independent repair cache"))
                : QStringLiteral("repair safely degraded or remained disabled"),
            QJsonObject{
                { QStringLiteral("manualRepairWorker"), manualRepairWorker },
                { QStringLiteral("repairVttPath"), repairVttPath },
                { QStringLiteral("repairCueCount"), repairCuesAfterScheduler.size() },
                { QStringLiteral("repairWorkerCueCount"), repairWorkerCueCount }
            });
        addAssertion(
            QStringLiteral("Manual fusion writes independent enhanced cache when source cache exists"),
            !sourceCacheAvailable ||
                (localFusionSuccess &&
                 localFusionWorker.value(QStringLiteral("quickCueCountPreserved")).toBool() &&
                 localFusionWorker.value(QStringLiteral("quickTimingPreserved")).toBool() &&
                 QFileInfo::exists(enhancedVttPath) &&
                 fusionOutputCueCount == fusionQuickCueCount &&
                 enhancedCuesAfterScheduler.size() == fusionOutputCueCount),
            sourceCacheAvailable
                ? QStringLiteral("manual fusion produced independent enhanced cache with quick cue timing/count")
                : QStringLiteral("manual fusion had no independent source cache and safely degraded"),
            QJsonObject{
                { QStringLiteral("localFusionWorker"), localFusionWorker },
                { QStringLiteral("enhancedVttPath"), enhancedVttPath },
                { QStringLiteral("enhancedCueCount"), enhancedCuesAfterScheduler.size() },
                { QStringLiteral("quickCueCount"), generatedCues.size() },
                { QStringLiteral("fusionQuickCueCount"), fusionQuickCueCount },
                { QStringLiteral("fusionOutputCueCount"), fusionOutputCueCount },
                { QStringLiteral("onlineTranslatedCueCount"), onlineTranslatedCuesAfterScheduler.size() },
                { QStringLiteral("ocrTranslatedCueCount"), ocrTranslatedCuesAfterScheduler.size() },
                { QStringLiteral("longAsrTranslatedCueCount"), longAsrTranslatedCuesAfterScheduler.size() },
                { QStringLiteral("repairCueCount"), repairCuesAfterScheduler.size() }
            });
        addAssertion(
            QStringLiteral("Manual fusion does not write quick zh"),
            !localFusionWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
                !manualOnlineWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
                !manualOnlineTranslationWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
                !manualOcrWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
                !manualLongAsrWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
                !manualRepairWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
                (!generation.value(QStringLiteral("quickZhModifiedByEnhancementScheduler")).toBool() ||
                 generation.value(QStringLiteral("backgroundBusy")).toBool() ||
                 manualOnlineWorker.value(QStringLiteral("attempted")).toBool()),
            generation.value(QStringLiteral("quickZhModifiedByEnhancementScheduler")).toBool()
                ? QStringLiteral("quick .zh timestamp changed during manual smoke, but enhancement workers report no quick-sidecar writes")
                : QStringLiteral("manual fusion left quick .zh baseline untouched"),
            QJsonObject{
                { QStringLiteral("localFusionWorker"), localFusionWorker },
                { QStringLiteral("manualOnlineWorker"), manualOnlineWorker },
                { QStringLiteral("manualOnlineTranslationWorker"), manualOnlineTranslationWorker },
                { QStringLiteral("manualOcrWorker"), manualOcrWorker },
                { QStringLiteral("manualRepairWorker"), manualRepairWorker },
                { QStringLiteral("quickZhModifiedByEnhancementScheduler"),
                  generation.value(QStringLiteral("quickZhModifiedByEnhancementScheduler")).toBool() },
                { QStringLiteral("backgroundBusy"), generation.value(QStringLiteral("backgroundBusy")).toBool() }
            });
        addAssertion(
            QStringLiteral("Enhanced display uses per-cue quick fallback"),
            manualOnlineWorker.value(QStringLiteral("attempted")).toBool()
                ? (!playbackStrategyDiagnostics.value(QStringLiteral("wholeTrackReplacement")).toBool() &&
                   playbackStrategyDiagnostics.value(QStringLiteral("currentCueFallbackSafe")).toBool() &&
                   !manualOnlineWorker.value(QStringLiteral("writesQuickZhSidecar")).toBool())
                : ((!QFileInfo::exists(enhancedVttPath) || enhancedCuesAfterScheduler.isEmpty())
                ? (!playbackStrategyDiagnostics.value(QStringLiteral("wholeTrackReplacement")).toBool() &&
                   playbackStrategyDiagnostics.value(QStringLiteral("currentCueFallbackSafe")).toBool() &&
                   !playbackStrategyDiagnostics.value(QStringLiteral("enhancedLoaded")).toBool())
                : (playbackStrategyDiagnostics.value(QStringLiteral("enhancedLoaded")).toBool() &&
                   playbackStrategyDiagnostics.value(QStringLiteral("enhancedCueMatches")).toInt() > 0 &&
                   !playbackStrategyDiagnostics.value(QStringLiteral("wholeTrackReplacement")).toBool() &&
                   playbackStrategyDiagnostics.value(QStringLiteral("currentCueFallbackSafe")).toBool() &&
                   _p->generatedSubtitlePath.endsWith(QStringLiteral(".zh.vtt")))),
            manualOnlineWorker.value(QStringLiteral("attempted")).toBool()
                ? QStringLiteral("manual online smoke kept quick fallback safe without whole-track enhanced takeover")
                : ((!QFileInfo::exists(enhancedVttPath) || enhancedCuesAfterScheduler.isEmpty())
                ? QStringLiteral("no enhanced cache was available; high-quality kept quick fallback safe")
                : (playbackStrategyDiagnostics.value(QStringLiteral("enhancedLoaded")).toBool()
                      ? QStringLiteral("enhanced cache merged per cue while quick .zh stayed baseline")
                      : QStringLiteral("enhanced cache was not loaded for manual high-quality display"))),
            QJsonObject{
                { QStringLiteral("generatedSubtitlePath"), _p->generatedSubtitlePath },
                { QStringLiteral("cueCount"), _p->generatedSubtitleCues.size() },
                { QStringLiteral("strategy"), playbackStrategyDiagnostics }
            });
    } else {
        const QJsonObject manualOnlineWorker =
            enhancementSchedulerDiagnostics.value(QStringLiteral("manualOnlineWorker")).toObject();
        const QJsonObject manualOnlineTranslationWorker =
            enhancementSchedulerDiagnostics.value(QStringLiteral("manualOnlineTranslationWorker")).toObject();
        const QJsonObject manualOcrWorker =
            enhancementSchedulerDiagnostics.value(QStringLiteral("manualOcrWorker")).toObject();
        const QJsonObject manualRepairWorker =
            enhancementSchedulerDiagnostics.value(QStringLiteral("manualRepairWorker")).toObject();
        const QJsonObject onlineSourceIntake =
            enhancementSchedulerDiagnostics.value(QStringLiteral("onlineSourceIntake")).toObject();
        addAssertion(
            QStringLiteral("Enhancement scheduler is default-disabled"),
            !enhancementSchedulerDiagnostics.value(QStringLiteral("defaultAutoRun")).toBool() &&
                !enhancementSchedulerDiagnostics.value(QStringLiteral("willStartBackgroundWorker")).toBool() &&
                !enhancementSchedulerDiagnostics.value(QStringLiteral("quickPathBehaviorChanged")).toBool() &&
                !enhancementSchedulerDiagnostics.value(QStringLiteral("writesQuickZhSidecar")).toBool(),
            QStringLiteral("default quick path did not auto-run enhancement scheduler"),
            enhancementSchedulerDiagnostics);
        addAssertion(
            QStringLiteral("Online worker disabled in quick playback"),
            enhancementSchedulerDiagnostics.value(QStringLiteral("onlineWorkerDefaultDisabled")).toBool() &&
                !enhancementSchedulerDiagnostics.value(QStringLiteral("onlineManualAllowed")).toBool() &&
                !enhancementSchedulerDiagnostics.value(QStringLiteral("onlineNetworkStarted")).toBool() &&
                !enhancementSchedulerDiagnostics.value(QStringLiteral("onlineCacheWritten")).toBool() &&
                !enhancementSchedulerDiagnostics.value(QStringLiteral("writesQuickZhSidecar")).toBool() &&
                !enhancementSchedulerDiagnostics.value(QStringLiteral("usesQuickApiQuota")).toBool() &&
                !enhancementSchedulerDiagnostics.value(QStringLiteral("usesQuickWorkerLane")).toBool() &&
                !manualOnlineWorker.value(QStringLiteral("attempted")).toBool() &&
                !(QFileInfo::exists(onlineCachePath) && !onlineCuesAfterScheduler.isEmpty()),
            QStringLiteral("default quick playback did not start online worker or write online cache"),
            QJsonObject{
                { QStringLiteral("enhancementScheduler"), enhancementSchedulerDiagnostics },
                { QStringLiteral("onlineCacheReady"), QFileInfo::exists(onlineCachePath) &&
                      !onlineCuesAfterScheduler.isEmpty() }
            });
        addAssertion(
            QStringLiteral("Online source intake disabled in quick playback"),
            !onlineSourceIntake.value(QStringLiteral("enabled")).toBool() &&
                !onlineSourceIntake.value(QStringLiteral("readsOnlineSourceCache")).toBool() &&
                !onlineSourceIntake.value(QStringLiteral("sourceOnlyDisplayable")).toBool() &&
                onlineSourceIntake.value(QStringLiteral("displayCoverageDelta")).toDouble() == 0.0 &&
                !onlineSourceIntake.value(QStringLiteral("quickCoverageChanged")).toBool() &&
                onlineSourceIntake.value(QStringLiteral("rejectedForDisplay")).toString() ==
                    QStringLiteral("source-only-not-displayable"),
            QStringLiteral("default quick playback did not read online source cache or change display coverage"),
            onlineSourceIntake);
        addAssertion(
            QStringLiteral("Translated online/OCR/repair workers disabled in quick playback"),
            !manualOnlineTranslationWorker.value(QStringLiteral("attempted")).toBool() &&
                !manualOnlineTranslationWorker.value(QStringLiteral("translatedOnlineCacheWritten")).toBool() &&
                !manualOcrWorker.value(QStringLiteral("attempted")).toBool() &&
                !manualOcrWorker.value(QStringLiteral("ocrCacheWritten")).toBool() &&
                !manualOcrWorker.value(QStringLiteral("ocrTranslatedCacheWritten")).toBool() &&
                !manualRepairWorker.value(QStringLiteral("attempted")).toBool() &&
                !manualRepairWorker.value(QStringLiteral("repairCacheWritten")).toBool() &&
                !QFileInfo::exists(onlineTranslatedCachePath) &&
                !QFileInfo::exists(ocrCachePath) &&
                !QFileInfo::exists(ocrTranslatedCachePath) &&
                !QFileInfo::exists(repairVttPath),
            QStringLiteral("default quick playback did not start translated online, OCR, or repair workers"),
            QJsonObject{
                { QStringLiteral("manualOnlineTranslationWorker"), manualOnlineTranslationWorker },
                { QStringLiteral("manualOcrWorker"), manualOcrWorker },
                { QStringLiteral("manualRepairWorker"), manualRepairWorker },
                { QStringLiteral("onlineTranslatedCachePath"), onlineTranslatedCachePath },
                { QStringLiteral("ocrCachePath"), ocrCachePath },
                { QStringLiteral("ocrTranslatedCachePath"), ocrTranslatedCachePath },
                { QStringLiteral("repairVttPath"), repairVttPath }
            });
        addAssertion(
            QStringLiteral("Enhanced display disabled in quick playback"),
            !playbackStrategyDiagnostics.value(QStringLiteral("enhancedLoaded")).toBool() &&
                playbackStrategyDiagnostics.value(QStringLiteral("enhancedRejectedReason")).toString() ==
                    QStringLiteral("disabled-in-quick-playback") &&
                !playbackStrategyDiagnostics.value(QStringLiteral("wholeTrackReplacement")).toBool(),
            QStringLiteral("default quick playback did not load enhanced display cache"),
            playbackStrategyDiagnostics);
    }
    QJsonObject serviceResult;
    if (qApp) {
        const QJsonDocument doc = QJsonDocument::fromJson(
            qApp->property("cgplay.subtitleGenerationLastResult").toString().toUtf8());
        if (doc.isObject()) {
            serviceResult = doc.object();
        }
    }
    QJsonObject serviceDiagnostics;
    if (!serviceResult.isEmpty()) {
        serviceDiagnostics = QJsonObject{
            { QStringLiteral("success"), serviceResult.value(QStringLiteral("success")).toBool() },
            { QStringLiteral("subtitleSourcePath"), serviceResult.value(QStringLiteral("subtitleSourcePath")).toString() },
            { QStringLiteral("subtitleSourceKind"), serviceResult.value(QStringLiteral("subtitleSourceKind")).toString() },
            { QStringLiteral("subtitleSourceError"), serviceResult.value(QStringLiteral("subtitleSourceError")).toString() },
            { QStringLiteral("subtitleSourceLanguage"), serviceResult.value(QStringLiteral("subtitleSourceLanguage")).toString() },
            { QStringLiteral("sourcePriorityRank"), serviceResult.value(QStringLiteral("sourcePriorityRank")).toString() },
            { QStringLiteral("sourceSelectionReason"), serviceResult.value(QStringLiteral("sourceSelectionReason")).toString() },
            { QStringLiteral("sourceRejectReason"), serviceResult.value(QStringLiteral("sourceRejectReason")).toString() },
            { QStringLiteral("usedSubtitleSource"), serviceResult.value(QStringLiteral("usedSubtitleSource")).toBool() },
            { QStringLiteral("usedAudioAsr"), serviceResult.value(QStringLiteral("usedAudioAsr")).toBool() },
            { QStringLiteral("subtitleSourceTimelineUsable"), serviceResult.value(QStringLiteral("subtitleSourceTimelineUsable")).toBool() },
            { QStringLiteral("subtitleSourceCandidateCount"), serviceResult.value(QStringLiteral("subtitleSourceCandidateCount")).toInt() },
            { QStringLiteral("onlineSearchEnabled"), serviceResult.value(QStringLiteral("onlineSearchEnabled")).toBool() },
            { QStringLiteral("onlineSourceKind"), serviceResult.value(QStringLiteral("onlineSourceKind")).toString() },
            { QStringLiteral("onlineMatchConfidence"), serviceResult.value(QStringLiteral("onlineMatchConfidence")).toDouble() },
            { QStringLiteral("usedOnlineSubtitle"), serviceResult.value(QStringLiteral("usedOnlineSubtitle")).toBool() },
            { QStringLiteral("onlineError"), serviceResult.value(QStringLiteral("onlineError")).toString() },
            { QStringLiteral("ocrEnabled"), serviceResult.value(QStringLiteral("ocrEnabled")).toBool() },
            { QStringLiteral("ocrProvider"), serviceResult.value(QStringLiteral("ocrProvider")).toString() },
            { QStringLiteral("ocrSamples"), serviceResult.value(QStringLiteral("ocrSamples")).toInt() },
            { QStringLiteral("ocrAcceptedCueCount"), serviceResult.value(QStringLiteral("ocrAcceptedCueCount")).toInt() },
            { QStringLiteral("ocrRejectedCueCount"), serviceResult.value(QStringLiteral("ocrRejectedCueCount")).toInt() },
            { QStringLiteral("ocrError"), serviceResult.value(QStringLiteral("ocrError")).toString() },
            { QStringLiteral("fusionEnabled"), serviceResult.value(QStringLiteral("fusionEnabled")).toBool() },
            { QStringLiteral("fusionPath"), serviceResult.value(QStringLiteral("fusionPath")).toString() },
            { QStringLiteral("fusionEnhancedCueCount"), serviceResult.value(QStringLiteral("fusionEnhancedCueCount")).toInt() },
            { QStringLiteral("fusionQuickFallbackCueCount"), serviceResult.value(QStringLiteral("fusionQuickFallbackCueCount")).toInt() },
            { QStringLiteral("lowConfidenceRepairEnabled"), serviceResult.value(QStringLiteral("lowConfidenceRepairEnabled")).toBool() },
            { QStringLiteral("lowConfidenceRepairPath"), serviceResult.value(QStringLiteral("lowConfidenceRepairPath")).toString() },
            { QStringLiteral("lowConfidenceRepairCueCount"), serviceResult.value(QStringLiteral("lowConfidenceRepairCueCount")).toInt() },
            { QStringLiteral("sourceCueCount"), serviceResult.value(QStringLiteral("sourceCueCount")).toInt() },
            { QStringLiteral("translatedCueCount"), serviceResult.value(QStringLiteral("translatedCueCount")).toInt() },
            { QStringLiteral("sourceCoverageEndSeconds"), serviceResult.value(QStringLiteral("sourceCoverageEndSeconds")).toDouble() },
            { QStringLiteral("translatedCoverageEndSeconds"), serviceResult.value(QStringLiteral("translatedCoverageEndSeconds")).toDouble() },
            { QStringLiteral("failedTranslationBatchCount"), serviceResult.value(QStringLiteral("failedTranslationBatchCount")).toInt() },
            { QStringLiteral("skippedAudioChunkCount"), serviceResult.value(QStringLiteral("skippedAudioChunkCount")).toInt() },
            { QStringLiteral("skippedAudioChunks"), serviceResult.value(QStringLiteral("skippedAudioChunks")).toArray() },
            { QStringLiteral("durationMs"), serviceResult.value(QStringLiteral("durationMs")).toDouble() },
            { QStringLiteral("translationAgent"), serviceResult.value(QStringLiteral("translationAgent")).toObject() }
        };
        generation.insert(QStringLiteral("serviceDiagnostics"), serviceDiagnostics);
        generation.insert(QStringLiteral("subtitleSourcePath"), serviceDiagnostics.value(QStringLiteral("subtitleSourcePath")).toString());
        generation.insert(QStringLiteral("subtitleSourceKind"), serviceDiagnostics.value(QStringLiteral("subtitleSourceKind")).toString());
        generation.insert(QStringLiteral("sourcePriorityRank"), serviceDiagnostics.value(QStringLiteral("sourcePriorityRank")).toString());
        generation.insert(QStringLiteral("sourceSelectionReason"), serviceDiagnostics.value(QStringLiteral("sourceSelectionReason")).toString());
        generation.insert(QStringLiteral("sourceRejectReason"), serviceDiagnostics.value(QStringLiteral("sourceRejectReason")).toString());
        generation.insert(QStringLiteral("usedSubtitleSource"), serviceDiagnostics.value(QStringLiteral("usedSubtitleSource")).toBool());
        generation.insert(QStringLiteral("usedAudioAsr"), serviceDiagnostics.value(QStringLiteral("usedAudioAsr")).toBool());
    }
    report.insert(QStringLiteral("generation"), generation);

    addAssertion(
        QStringLiteral("Generate subtitle files from playback button"),
        generationFinished && !generatedCues.isEmpty(),
        generationFinished ? QStringLiteral("first subtitle batch became available from playback button")
                           : QStringLiteral("subtitle generation did not produce a readable first batch"),
        generation);
    addAssertion(
        QStringLiteral("Source SRT exists"),
        QFileInfo::exists(sourceSrtPath) || QFileInfo::exists(sourceVttPath),
        QFileInfo::exists(sourceSrtPath) ? QStringLiteral("source srt exists")
                                        : (QFileInfo::exists(sourceVttPath)
                                               ? QStringLiteral("source vtt exists")
                                               : sourceSrtPath));
    addAssertion(
        QStringLiteral("Translated VTT exists"),
        QFileInfo::exists(translatedVttPath),
        QFileInfo::exists(translatedVttPath) ? QStringLiteral("translated vtt exists") : translatedVttPath);
    if (serviceResult.value(QStringLiteral("usedSubtitleSource")).toBool()) {
        addAssertion(
            QStringLiteral("Subtitle source bypasses audio ASR"),
            !serviceResult.value(QStringLiteral("usedAudioAsr")).toBool(),
            !serviceResult.value(QStringLiteral("usedAudioAsr")).toBool()
                ? QStringLiteral("text subtitle source was translated without audio ASR")
                : QStringLiteral("audio ASR was used despite a text subtitle source"),
            serviceDiagnostics);
    }
    const bool onlineLayerInactive =
        !serviceDiagnostics.value(QStringLiteral("usedOnlineSubtitle")).toBool();
    const bool highQualityPlaybackMode =
        _p->translationPlaybackMode == QStringLiteral("high-quality");
    addAssertion(
        QStringLiteral("Quick path uses direct subtitle generation"),
        onlineLayerInactive &&
            (highQualityPlaybackMode ||
             (!serviceDiagnostics.value(QStringLiteral("ocrEnabled")).toBool() &&
              !serviceDiagnostics.value(QStringLiteral("fusionEnabled")).toBool() &&
               !serviceDiagnostics.value(QStringLiteral("lowConfidenceRepairEnabled")).toBool())),
        QStringLiteral("quick generation uses direct SubtitleGenerationService path"),
        QJsonObject{
            { QStringLiteral("serviceDiagnostics"), serviceDiagnostics }
        });
    if (mockWithoutApi) {
        addAssertion(
            QStringLiteral("Refined VTT exists"),
            refinedFileReady,
            refinedFileReady ? QStringLiteral("refined vtt exists") : refinedVttPath,
            QJsonObject{
                { QStringLiteral("refinedVttPath"), refinedVttPath },
                { QStringLiteral("refinedCueCount"), refinedCues.size() },
                { QStringLiteral("validation"), _p->subtitleRefinementLastValidation }
            });
    }
    addAssertion(
        QStringLiteral("Cue count"),
        !generatedCues.isEmpty(),
        QStringLiteral("source=%1 translated=%2")
            .arg(generatedCues.size())
            .arg(generatedCues.size()));
    addAssertion(
        QStringLiteral("Generated subtitle handles requested frame"),
        generatedCueCoversRequestedFrame || frame > 0,
        generatedCueCoversRequestedFrame ? QStringLiteral("cue covers requested playback time")
                                         : QStringLiteral("requested playback time has no displayable cue"),
        generation);

    _setupGeneratedSubtitleOverlay();

    if (_p->playbackCtrl && mediaOpened) {
        const int targetFrame = frame > 0 ? frame : 0;
        _p->playbackCtrl->seekToFrame(std::clamp(targetFrame, 0, std::max(0, _p->playbackCtrl->totalFrames() - 1)));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        QThread::msleep(200);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        _updateGeneratedSubtitleForFrame(_p->playbackCtrl->currentFrame());
    }

    auto* overlayLabel = findChild<QLabel*>(QStringLiteral("GeneratedSubtitleOverlayLabel"));
    QString overlayText = overlayLabel ? overlayLabel->text().trimmed() : QString();
    bool overlayVisible = overlayLabel && overlayLabel->isVisible() && !overlayText.isEmpty();
    addAssertion(
        QStringLiteral("Player loads generated subtitle"),
        !_p->generatedSubtitleCues.isEmpty() && !_p->generatedSubtitlePath.isEmpty(),
        !_p->generatedSubtitleCues.isEmpty() ? QStringLiteral("generated subtitle track loaded")
                                            : QStringLiteral("generated subtitle track not loaded"),
        QJsonObject{
            { QStringLiteral("path"), _p->generatedSubtitlePath },
            { QStringLiteral("cueCount"), _p->generatedSubtitleCues.size() }
        });
    const bool requestedFrameMayHideOverlay = frame > 0 && !generatedCueCoversRequestedFrame;
    addAssertion(
        QStringLiteral("Subtitle overlay state"),
        overlayVisible || requestedFrameMayHideOverlay,
        overlayVisible ? overlayText : QStringLiteral("overlay hidden because requested frame has no displayable cue"),
        QJsonObject{
            { QStringLiteral("visible"), overlayLabel ? overlayLabel->isVisible() : false },
            { QStringLiteral("text"), overlayText },
            { QStringLiteral("requestedFrameMayHideOverlay"), requestedFrameMayHideOverlay },
            { QStringLiteral("geometry"), overlayLabel
                  ? QJsonObject{
                        { QStringLiteral("x"), overlayLabel->geometry().x() },
                        { QStringLiteral("y"), overlayLabel->geometry().y() },
                        { QStringLiteral("width"), overlayLabel->geometry().width() },
                        { QStringLiteral("height"), overlayLabel->geometry().height() }
                    }
                  : QJsonObject() }
        });

    QJsonObject playbackSampling;
    if (playbackSampleDurationMs > 0 && _p->playbackCtrl && mediaOpened) {
        const int intervalMs = std::max(250, playbackSampleIntervalMs);
        QJsonArray samples;
        int hiddenSamples = 0;
        int emptyTextSamples = 0;
        int exactCueMissSamples = 0;
        int exactCueHiddenSamples = 0;
        int exactCueEmptyTextSamples = 0;
        int sampleIndex = 0;
        double minTranslatedCoverageAheadSeconds = std::numeric_limits<double>::max();
        double minExactCueTranslatedCoverageAheadSeconds = std::numeric_limits<double>::max();
        double lastTranslatedCoverageAheadSeconds = 0.0;

        auto cueStateAtSeconds = [this](double seconds) {
            QJsonObject state;
            double activeStart = -1.0;
            double activeEnd = -1.0;
            double rawActiveStart = -1.0;
            double rawActiveEnd = -1.0;
            double nextStart = -1.0;
            double nextEnd = -1.0;
            QString activeText;
            QString rawActiveSourceText;
            QString rawActiveTranslatedText;
            bool rawActiveDisplayable = false;
            for (const auto& cue : _p->generatedSubtitleCues) {
                const bool cueAtTime = seconds + 0.02 >= cue.startSeconds &&
                    seconds <= cue.endSeconds + 0.02;
                const bool displayable = smokeCueHasDisplayableTranslation(cue);
                if (cueAtTime && rawActiveStart < 0.0) {
                    rawActiveStart = cue.startSeconds;
                    rawActiveEnd = cue.endSeconds;
                    rawActiveSourceText = cue.sourceText.trimmed();
                    rawActiveTranslatedText = cue.translatedText.trimmed();
                    rawActiveDisplayable = displayable;
                }
                if (displayable && cueAtTime) {
                    activeStart = cue.startSeconds;
                    activeEnd = cue.endSeconds;
                    activeText = cue.translatedText.trimmed();
                    break;
                }
                if (displayable && cue.startSeconds > seconds && (nextStart < 0.0 || cue.startSeconds < nextStart)) {
                    nextStart = cue.startSeconds;
                    nextEnd = cue.endSeconds;
                }
            }
            state.insert(QStringLiteral("activeCueStartSeconds"), activeStart);
            state.insert(QStringLiteral("activeCueEndSeconds"), activeEnd);
            state.insert(QStringLiteral("activeCueText"), activeText);
            state.insert(QStringLiteral("rawActiveCueStartSeconds"), rawActiveStart);
            state.insert(QStringLiteral("rawActiveCueEndSeconds"), rawActiveEnd);
            state.insert(QStringLiteral("rawActiveCueSourceText"), rawActiveSourceText);
            state.insert(QStringLiteral("rawActiveCueTranslatedText"), rawActiveTranslatedText);
            state.insert(QStringLiteral("rawActiveCueDisplayable"), rawActiveDisplayable);
            state.insert(
                QStringLiteral("rawActiveCueRejectReason"),
                rawActiveStart >= 0.0 && !rawActiveDisplayable
                    ? QStringLiteral("non-displayable-short-filler-or-affirmation-source-mismatch")
                    : QString());
            state.insert(
                QStringLiteral("rawActiveCueSourceKind"),
                _p->generatedSubtitlePath.contains(QStringLiteral(".enhanced.zh."), Qt::CaseInsensitive)
                    ? QStringLiteral("enhanced")
                    : (_p->generatedSubtitlePath.contains(QStringLiteral(".refined.zh."), Qt::CaseInsensitive)
                        ? QStringLiteral("refined")
                        : QStringLiteral("quick-baseline")));
            state.insert(QStringLiteral("nextCueStartSeconds"), nextStart);
            state.insert(QStringLiteral("nextCueEndSeconds"), nextEnd);
            return state;
        };

        auto appendSample = [&]() {
            const double currentSeconds = _p->playbackCtrl->fps() > 0.0
                ? std::max(0, _p->playbackCtrl->currentFrame()) / _p->playbackCtrl->fps()
                : 0.0;
            _updateGeneratedSubtitleForFrame(_p->playbackCtrl->currentFrame());
            auto* sampleLabel = findChild<QLabel*>(QStringLiteral("GeneratedSubtitleOverlayLabel"));
            const QString sampleText = sampleLabel ? sampleLabel->text().trimmed() : QString();
            const bool sampleVisible = sampleLabel && sampleLabel->isVisible();
            const QJsonObject cueState = cueStateAtSeconds(currentSeconds);
            const bool hasExactCue = cueState.value(QStringLiteral("activeCueStartSeconds")).toDouble(-1.0) >= 0.0;
            const double translatedCoverageAheadSeconds =
                translatedCoverageEnd(_p->generatedSubtitleCues) - currentSeconds;
            const QJsonObject strategyDiagnostics =
                _translationPlaybackStrategyDiagnostics(
                    _p->translationPlaybackMode == QStringLiteral("high-quality"));
            minTranslatedCoverageAheadSeconds =
                std::min(minTranslatedCoverageAheadSeconds, translatedCoverageAheadSeconds);
            lastTranslatedCoverageAheadSeconds = translatedCoverageAheadSeconds;

            if (!sampleVisible) {
                hiddenSamples += 1;
            }
            if (sampleText.isEmpty()) {
                emptyTextSamples += 1;
            }
            if (!hasExactCue) {
                exactCueMissSamples += 1;
            }
            if (hasExactCue && !sampleVisible) {
                exactCueHiddenSamples += 1;
            }
            if (hasExactCue && sampleText.isEmpty()) {
                exactCueEmptyTextSamples += 1;
            }
            if (hasExactCue) {
                minExactCueTranslatedCoverageAheadSeconds =
                    std::min(minExactCueTranslatedCoverageAheadSeconds, translatedCoverageAheadSeconds);
            }

            QJsonObject sample{
                { QStringLiteral("index"), sampleIndex++ },
                { QStringLiteral("currentFrame"), _p->playbackCtrl->currentFrame() },
                { QStringLiteral("currentSeconds"), currentSeconds },
                { QStringLiteral("overlayVisible"), sampleVisible },
                { QStringLiteral("overlayText"), sampleText },
                { QStringLiteral("hasExactCue"), hasExactCue },
                { QStringLiteral("coverageEndSeconds"), _p->generatedSubtitleCoverageEndSeconds },
                { QStringLiteral("sourceCoverageEndSeconds"), sourceCoverageEnd(_p->generatedSubtitleCues) },
                { QStringLiteral("translatedCoverageEndSeconds"), translatedCoverageEnd(_p->generatedSubtitleCues) },
                { QStringLiteral("translatedCoverageAheadSeconds"), translatedCoverageAheadSeconds },
                { QStringLiteral("processedEndSeconds"), _p->generatedSubtitleProcessedEndSeconds },
                { QStringLiteral("subtitleGenerationBusy"), _p->subtitleGenerationBusy },
                { QStringLiteral("subtitleContinuationScheduled"), _p->subtitleContinuationScheduled },
                { QStringLiteral("subtitleContinuationQueuedWhileBusy"), _p->subtitleContinuationQueuedWhileBusy },
                { QStringLiteral("cueCount"), _p->generatedSubtitleCues.size() },
                { QStringLiteral("generatedSubtitlePath"), _p->generatedSubtitlePath },
                { QStringLiteral("translationPlaybackMode"), _p->translationPlaybackMode },
                { QStringLiteral("translationPlaybackWaited"),
                  strategyDiagnostics.value(QStringLiteral("waited")).toBool() },
                { QStringLiteral("translationPlaybackWaitMs"),
                  strategyDiagnostics.value(QStringLiteral("waitMs")).toDouble() },
                { QStringLiteral("quickFallbackCueCount"),
                  strategyDiagnostics.value(QStringLiteral("quickFallbackCueCount")).toInt() },
                { QStringLiteral("refinedMerged"),
                  strategyDiagnostics.value(QStringLiteral("refinedMerged")).toBool() },
                { QStringLiteral("enhancedMerged"),
                  strategyDiagnostics.value(QStringLiteral("enhancedMerged")).toBool() },
                { QStringLiteral("refinedTookOverTrack"), false },
                { QStringLiteral("enhancedTookOverTrack"), false },
                { QStringLiteral("wholeTrackReplacement"),
                  strategyDiagnostics.value(QStringLiteral("wholeTrackReplacement")).toBool() },
                { QStringLiteral("currentCueFallbackSafe"),
                  strategyDiagnostics.value(QStringLiteral("currentCueFallbackSafe")).toBool() },
                { QStringLiteral("loadedQuickBaseline"), _p->generatedSubtitlePath.endsWith(QStringLiteral(".zh.vtt")) ||
                      _p->generatedSubtitlePath.endsWith(QStringLiteral(".zh.srt")) },
                { QStringLiteral("refinedLoaded"), strategyDiagnostics.value(QStringLiteral("refinedMerged")).toBool() },
                { QStringLiteral("enhancedLoaded"), strategyDiagnostics.value(QStringLiteral("enhancedLoaded")).toBool() },
                { QStringLiteral("enhancedCueMatches"),
                  strategyDiagnostics.value(QStringLiteral("enhancedCueMatches")).toInt() },
                { QStringLiteral("enhancedRejectedReason"),
                  strategyDiagnostics.value(QStringLiteral("enhancedRejectedReason")).toString() }
            };
            for (auto it = cueState.begin(); it != cueState.end(); ++it) {
                sample.insert(it.key(), it.value());
            }
            samples.append(sample);
        };

        _p->playbackCtrl->play();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        QElapsedTimer sampleTimer;
        sampleTimer.start();
        qint64 nextSampleMs = 0;
        while (sampleTimer.elapsed() <= playbackSampleDurationMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            if (sampleTimer.elapsed() >= nextSampleMs) {
                appendSample();
                nextSampleMs += intervalMs;
            }
            QThread::msleep(25);
        }
        _p->playbackCtrl->pause();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);

        const QJsonObject finalStrategyDiagnostics =
            _translationPlaybackStrategyDiagnostics(
                _p->translationPlaybackMode == QStringLiteral("high-quality"));
        playbackSampling = QJsonObject{
            { QStringLiteral("durationMs"), playbackSampleDurationMs },
            { QStringLiteral("intervalMs"), intervalMs },
            { QStringLiteral("sampleCount"), samples.size() },
            { QStringLiteral("translationPlaybackMode"), _p->translationPlaybackMode },
            { QStringLiteral("translationPlaybackStrategy"), finalStrategyDiagnostics },
            { QStringLiteral("translationPlaybackWaited"),
              finalStrategyDiagnostics.value(QStringLiteral("waited")).toBool() },
            { QStringLiteral("translationPlaybackWaitMs"),
              finalStrategyDiagnostics.value(QStringLiteral("waitMs")).toDouble() },
            { QStringLiteral("quickFallbackCueCount"),
              finalStrategyDiagnostics.value(QStringLiteral("quickFallbackCueCount")).toInt() },
            { QStringLiteral("refinedMerged"),
              finalStrategyDiagnostics.value(QStringLiteral("refinedMerged")).toBool() },
            { QStringLiteral("enhancedMerged"),
              finalStrategyDiagnostics.value(QStringLiteral("enhancedMerged")).toBool() },
            { QStringLiteral("refinedTookOverTrack"), false },
            { QStringLiteral("enhancedTookOverTrack"), false },
            { QStringLiteral("wholeTrackReplacement"),
              finalStrategyDiagnostics.value(QStringLiteral("wholeTrackReplacement")).toBool() },
            { QStringLiteral("currentCueFallbackSafe"),
              finalStrategyDiagnostics.value(QStringLiteral("currentCueFallbackSafe")).toBool() },
            { QStringLiteral("enhancedLoaded"),
              finalStrategyDiagnostics.value(QStringLiteral("enhancedLoaded")).toBool() },
            { QStringLiteral("enhancedCueMatches"),
              finalStrategyDiagnostics.value(QStringLiteral("enhancedCueMatches")).toInt() },
            { QStringLiteral("enhancedRejectedReason"),
              finalStrategyDiagnostics.value(QStringLiteral("enhancedRejectedReason")).toString() },
            { QStringLiteral("hiddenSamples"), hiddenSamples },
            { QStringLiteral("emptyTextSamples"), emptyTextSamples },
            { QStringLiteral("exactCueMissSamples"), exactCueMissSamples },
            { QStringLiteral("exactCueHiddenSamples"), exactCueHiddenSamples },
            { QStringLiteral("exactCueEmptyTextSamples"), exactCueEmptyTextSamples },
            { QStringLiteral("minTranslatedCoverageAheadSeconds"),
              minTranslatedCoverageAheadSeconds == std::numeric_limits<double>::max()
                  ? 0.0
                  : minTranslatedCoverageAheadSeconds },
            { QStringLiteral("minExactCueTranslatedCoverageAheadSeconds"),
              minExactCueTranslatedCoverageAheadSeconds == std::numeric_limits<double>::max()
                  ? 0.0
                  : minExactCueTranslatedCoverageAheadSeconds },
            { QStringLiteral("lastTranslatedCoverageAheadSeconds"), lastTranslatedCoverageAheadSeconds },
            { QStringLiteral("samples"), samples }
        };
        report.insert(QStringLiteral("playback_sampling"), playbackSampling);
        addAssertion(
            QStringLiteral("Long playback sampling keeps active subtitles visible"),
            exactCueHiddenSamples == 0 && exactCueEmptyTextSamples == 0,
            exactCueHiddenSamples == 0 && exactCueEmptyTextSamples == 0
                ? QStringLiteral("active subtitle cues stayed visible during sampled playback")
                : QStringLiteral("active cue hidden=%1 empty=%2")
                    .arg(exactCueHiddenSamples)
                    .arg(exactCueEmptyTextSamples),
            playbackSampling);
        addAssertion(
            QStringLiteral("Long playback keeps translated coverage ahead for active cues"),
            minExactCueTranslatedCoverageAheadSeconds >= -0.02,
            minExactCueTranslatedCoverageAheadSeconds >= -0.02
                ? QStringLiteral("translated subtitle coverage stayed ahead while active cues were present")
                : QStringLiteral("translated coverage fell behind active playback cues by %1s")
                    .arg(-minExactCueTranslatedCoverageAheadSeconds, 0, 'f', 3),
            playbackSampling);
    }

    QJsonObject continuation;
    if (playbackSampleDurationMs <= 0 &&
        !mockWithoutApi &&
        _p->playbackCtrl &&
        mediaOpened &&
        !_p->generatedSubtitleCues.isEmpty()) {
        const int initialCueCount = _p->generatedSubtitleCues.size();
        const double coverageBefore = _p->generatedSubtitleCoverageEndSeconds;
        const double triggerSeconds = std::max(0.0, coverageBefore - 10.0);
        const int triggerFrame = std::clamp(
            static_cast<int>(triggerSeconds * smokeFps + 0.5),
            0,
            std::max(0, _p->playbackCtrl->totalFrames() - 1));
        _p->playbackCtrl->seekToFrame(triggerFrame);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        _updateGeneratedSubtitleForFrame(_p->playbackCtrl->currentFrame());
        const int continuationTimeoutMs = mockWithoutApi ? 10000 : 180000;
        const bool continuationLoadedDuringWait = pumpUntil([this, initialCueCount, coverageBefore] {
            return !_p->subtitleGenerationBusy &&
                _p->generatedSubtitleCues.size() > initialCueCount &&
                _p->generatedSubtitleCoverageEndSeconds > coverageBefore;
        }, continuationTimeoutMs);
        const bool continuationLoaded =
            continuationLoadedDuringWait ||
            (_p->generatedSubtitleCues.size() > initialCueCount &&
             _p->generatedSubtitleCoverageEndSeconds > coverageBefore);
        continuation = QJsonObject{
            { QStringLiteral("initialCueCount"), initialCueCount },
            { QStringLiteral("finalCueCount"), _p->generatedSubtitleCues.size() },
            { QStringLiteral("coverageBeforeSeconds"), coverageBefore },
            { QStringLiteral("coverageAfterSeconds"), _p->generatedSubtitleCoverageEndSeconds },
            { QStringLiteral("processedAfterSeconds"), _p->generatedSubtitleProcessedEndSeconds },
            { QStringLiteral("triggerFrame"), triggerFrame },
            { QStringLiteral("triggerSeconds"), triggerSeconds },
            { QStringLiteral("timeoutMs"), continuationTimeoutMs }
        };
        addAssertion(
            QStringLiteral("Generated subtitle continues after first window"),
            continuationLoaded,
            continuationLoaded ? QStringLiteral("next subtitle window loaded")
                               : QStringLiteral("next subtitle window was not loaded"),
            continuation);
        if (continuationLoaded && !_p->generatedSubtitleCues.isEmpty()) {
            const auto cue = _p->generatedSubtitleCues.last();
            const int cueFrame = std::clamp(
                static_cast<int>(cue.startSeconds * smokeFps + 0.5),
                0,
                std::max(0, _p->playbackCtrl->totalFrames() - 1));
            _p->playbackCtrl->seekToFrame(cueFrame);
            pumpUntil([this, cueFrame] {
                return _p->playbackCtrl && _p->playbackCtrl->currentFrame() == cueFrame;
            }, 5000);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
            QThread::msleep(200);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
            _updateGeneratedSubtitleForFrame(_p->playbackCtrl->currentFrame());
            overlayLabel = findChild<QLabel*>(QStringLiteral("GeneratedSubtitleOverlayLabel"));
            overlayText = overlayLabel ? overlayLabel->text().trimmed() : QString();
            overlayVisible = overlayLabel && overlayLabel->isVisible() && !overlayText.isEmpty();
        }
    }

    if (playbackSampleDurationMs <= 0 &&
        _p->playbackCtrl &&
        mediaOpened &&
        _p->generatedSubtitleCoverageEndSeconds > 0.0) {
        const double guardSeconds = std::max(0.0, _p->generatedSubtitleCoverageEndSeconds - 4.0);
        const int guardFrame = std::clamp(
            static_cast<int>(guardSeconds * smokeFps + 0.5),
            0,
            std::max(0, _p->playbackCtrl->totalFrames() - 1));
        _p->playbackCtrl->seekToFrame(guardFrame);
        pumpUntil([this, guardFrame] {
            return _p->playbackCtrl && std::abs(_p->playbackCtrl->currentFrame() - guardFrame) <= 1;
        }, 5000);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        _p->playbackCtrl->play();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        _updateGeneratedSubtitleForFrame(_p->playbackCtrl->currentFrame());
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        addAssertion(
            QStringLiteral("Playback keeps running while subtitles buffer"),
            _p->playbackCtrl->playbackState() == 1,
            _p->playbackCtrl->playbackState() == 1
                ? QStringLiteral("playback kept running while subtitle continuation is scheduled")
                : QStringLiteral("playback paused while waiting for subtitles"),
            QJsonObject{
                { QStringLiteral("guardFrame"), guardFrame },
                { QStringLiteral("currentFrame"), _p->playbackCtrl->currentFrame() },
                { QStringLiteral("guardSeconds"), guardSeconds },
                { QStringLiteral("coverageEndSeconds"), _p->generatedSubtitleCoverageEndSeconds },
                { QStringLiteral("translationVisible"), _p->playbackBar ? _p->playbackBar->translationVisible() : false },
                { QStringLiteral("playbackState"), _p->playbackCtrl->playbackState() }
            });
        _p->playbackCtrl->pause();
    }

    double maxCueGapSeconds = 0.0;
    double maxCueGapStartSeconds = -1.0;
    double maxCueGapEndSeconds = -1.0;
    QVector<GeneratedSubtitleCue> sortedGeneratedCues = _p->generatedSubtitleCues;
    std::sort(sortedGeneratedCues.begin(), sortedGeneratedCues.end(), [](const GeneratedSubtitleCue& a, const GeneratedSubtitleCue& b) {
        if (a.startSeconds == b.startSeconds) {
            return a.endSeconds < b.endSeconds;
        }
        return a.startSeconds < b.startSeconds;
    });
    for (int i = 1; i < sortedGeneratedCues.size(); ++i) {
        const double gap = sortedGeneratedCues.at(i).startSeconds - sortedGeneratedCues.at(i - 1).endSeconds;
        if (gap > maxCueGapSeconds) {
            maxCueGapSeconds = gap;
            maxCueGapStartSeconds = sortedGeneratedCues.at(i - 1).endSeconds;
            maxCueGapEndSeconds = sortedGeneratedCues.at(i).startSeconds;
        }
    }

    report.insert(QStringLiteral("subtitle_overlay"), QJsonObject{
        { QStringLiteral("path"), _p->generatedSubtitlePath },
        { QStringLiteral("cueCount"), _p->generatedSubtitleCues.size() },
        { QStringLiteral("visible"), overlayVisible },
        { QStringLiteral("text"), overlayText },
        { QStringLiteral("translationPlaybackMode"), _p->translationPlaybackMode },
        { QStringLiteral("quickFallbackCueCount"), _p->generatedSubtitleQuickFallbackCueCount },
        { QStringLiteral("refinedMerged"), _p->generatedSubtitleRefinedMerged },
        { QStringLiteral("enhancedMerged"), _p->generatedSubtitleEnhancedMerged },
        { QStringLiteral("refinedTookOverTrack"), false },
        { QStringLiteral("enhancedTookOverTrack"), false },
        { QStringLiteral("coverageEndSeconds"), _p->generatedSubtitleCoverageEndSeconds },
        { QStringLiteral("sourceCoverageEndSeconds"), sourceCoverageEnd(_p->generatedSubtitleCues) },
        { QStringLiteral("translatedCoverageEndSeconds"), translatedCoverageEnd(_p->generatedSubtitleCues) },
        { QStringLiteral("processedEndSeconds"), _p->generatedSubtitleProcessedEndSeconds },
        { QStringLiteral("maxCueGapSeconds"), maxCueGapSeconds },
        { QStringLiteral("maxCueGapStartSeconds"), maxCueGapStartSeconds },
        { QStringLiteral("maxCueGapEndSeconds"), maxCueGapEndSeconds },
        { QStringLiteral("continuation"), continuation }
    });

    QJsonObject guardReport = _translationGuardReportDiagnostics(serviceDiagnostics, playbackSampling);
    QJsonObject guardConsumersForFinalSource =
        guardReport.value(QStringLiteral("consumers")).toObject();
    QJsonObject sourcePriorityFinalAudit =
        guardConsumersForFinalSource.value(QStringLiteral("SourcePriorityGuardSkill")).toObject();
    QString finalActiveSource =
        playbackStrategyDiagnostics.value(QStringLiteral("activeHqSource")).toString(
            playbackStrategyDiagnostics.value(QStringLiteral("activeSource")).toString());
    if (finalActiveSource.trimmed().isEmpty()) {
        finalActiveSource =
            enhancementSchedulerDiagnostics.value(QStringLiteral("activeSource")).toString();
    }
    const QString schedulerActiveSource =
        enhancementSchedulerDiagnostics.value(QStringLiteral("activeSource")).toString();
    const bool finalHighQuality = _p->translationPlaybackMode == QStringLiteral("high-quality");
    const bool finalUsedSubtitleSource =
        serviceDiagnostics.value(QStringLiteral("usedSubtitleSource")).toBool();
    const bool finalUsedAudioAsr =
        serviceDiagnostics.value(QStringLiteral("usedAudioAsr")).toBool();
    const bool finalOcrReady =
        !ocrTranslatedCuesAfterScheduler.isEmpty() ||
        enhancementSchedulerDiagnostics.value(QStringLiteral("ocrTranslatedCueCount")).toInt() > 0 ||
        finalActiveSource == QStringLiteral("hard-sub-ocr") ||
        schedulerActiveSource == QStringLiteral("hard-sub-ocr");
    const bool finalOnlineReady =
        !onlineTranslatedCuesAfterScheduler.isEmpty() ||
        enhancementSchedulerDiagnostics.value(QStringLiteral("onlineCandidateCount")).toInt() > 0 ||
        finalActiveSource == QStringLiteral("online-subtitle") ||
        schedulerActiveSource == QStringLiteral("online-subtitle");
    const bool finalUsesHqSource =
        finalActiveSource == QStringLiteral("hard-sub-ocr") ||
        finalActiveSource == QStringLiteral("online-subtitle") ||
        (generation.value(QStringLiteral("enhancedLoaded")).toBool() &&
         (schedulerActiveSource == QStringLiteral("hard-sub-ocr") ||
          schedulerActiveSource == QStringLiteral("online-subtitle")));
    const bool finalLocalPriorityPassed =
        !finalUsedSubtitleSource || !finalUsedAudioAsr;
    const bool finalOcrPriorityPassed =
        !(finalHighQuality && !finalUsedSubtitleSource && finalOcrReady) ||
        finalUsesHqSource;
    const bool finalAsrLastResort =
        !finalUsedAudioAsr ||
        (!finalUsedSubtitleSource && !(finalHighQuality && (finalOcrReady || finalOnlineReady))) ||
        finalUsesHqSource;
    QString finalSourcePriorityRank = QStringLiteral("3-audio-asr-fallback");
    if (finalUsedSubtitleSource) {
        finalSourcePriorityRank = QStringLiteral("1-local-embedded-external-subtitle");
    } else if (finalActiveSource == QStringLiteral("online-subtitle")) {
        finalSourcePriorityRank = QStringLiteral("1b-online-high-confidence-aligned-hq-candidate");
    } else if (finalActiveSource == QStringLiteral("hard-sub-ocr") ||
               (generation.value(QStringLiteral("enhancedLoaded")).toBool() &&
                schedulerActiveSource == QStringLiteral("hard-sub-ocr"))) {
        finalSourcePriorityRank = QStringLiteral("2-hard-sub-ocr-high-quality");
    }
    const bool originalSourcePriorityPassed =
        sourcePriorityFinalAudit.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("passed")).toBool();
    const bool finalSourcePriorityPassed =
        originalSourcePriorityPassed &&
        finalLocalPriorityPassed &&
        finalOcrPriorityPassed &&
        finalAsrLastResort;
    sourcePriorityFinalAudit.insert(QStringLiteral("finalSourceKind"), finalActiveSource);
    sourcePriorityFinalAudit.insert(QStringLiteral("finalSourcePriorityRank"), finalSourcePriorityRank);
    sourcePriorityFinalAudit.insert(QStringLiteral("finalSchedulerActiveSource"), schedulerActiveSource);
    sourcePriorityFinalAudit.insert(QStringLiteral("finalOcrCandidateReady"), finalOcrReady);
    sourcePriorityFinalAudit.insert(QStringLiteral("finalOnlineCandidateReady"), finalOnlineReady);
    sourcePriorityFinalAudit.insert(QStringLiteral("finalUsesHigherPriorityHqSource"), finalUsesHqSource);
    sourcePriorityFinalAudit.insert(QStringLiteral("finalLocalSubtitlePriorityPassed"), finalLocalPriorityPassed);
    sourcePriorityFinalAudit.insert(QStringLiteral("finalOcrPriorityOverAsrPassed"), finalOcrPriorityPassed);
    sourcePriorityFinalAudit.insert(QStringLiteral("finalAsrLastResort"), finalAsrLastResort);
    sourcePriorityFinalAudit.insert(QStringLiteral("result"), QJsonObject{
        { QStringLiteral("status"), finalSourcePriorityPassed ? QStringLiteral("PASS") : QStringLiteral("FAIL") },
        { QStringLiteral("passed"), finalSourcePriorityPassed },
        { QStringLiteral("message"),
          finalSourcePriorityPassed
              ? QStringLiteral("final source priority is local/embedded/external first, online HQ candidate second, OCR high-quality before ASR fallback")
              : QStringLiteral("final source priority risk detected after HQ refresh") }
    });
    guardConsumersForFinalSource.insert(
        QStringLiteral("SourcePriorityGuardSkill"),
        sourcePriorityFinalAudit);
    guardReport.insert(QStringLiteral("consumers"), guardConsumersForFinalSource);
    QString guardReportPath;
    QString guardReportError;
    bool guardReportFileWritten = false;
    if (!outputPath.trimmed().isEmpty()) {
        const QFileInfo outputInfo(outputPath);
        guardReportPath = outputInfo.absolutePath() + QLatin1Char('/') +
            outputInfo.completeBaseName() + QStringLiteral(".translation_phase3_report.json");
    }
    guardReport.insert(QStringLiteral("localReportPath"), guardReportPath);
    if (!guardReportPath.isEmpty()) {
        TranslationReportSkill reportSkill;
        guardReportFileWritten = reportSkill.writeReportFile(guardReportPath, guardReport, &guardReportError);
    }
    guardReport.insert(QStringLiteral("localReportWritten"), guardReportFileWritten);
    guardReport.insert(QStringLiteral("localReportError"), guardReportError);
    report.insert(QStringLiteral("translationGuardReport"), guardReport);

    const QJsonObject guardConsumers = guardReport.value(QStringLiteral("consumers")).toObject();
    const QJsonObject coverageAudit =
        guardConsumers.value(QStringLiteral("CoverageAuditSkill")).toObject();
    const QJsonObject cacheGuard =
        guardConsumers.value(QStringLiteral("CacheGuardSkill")).toObject();
    const QJsonObject noDialogueGuard =
        guardConsumers.value(QStringLiteral("NoDialogueGuardSkill")).toObject();
    const QJsonObject sourcePriorityGuard =
        guardConsumers.value(QStringLiteral("SourcePriorityGuardSkill")).toObject();
    const bool coverageAuditPassed =
        coverageAudit.value(QStringLiteral("result")).toObject().value(QStringLiteral("passed")).toBool();
    const bool cacheGuardPassed =
        cacheGuard.value(QStringLiteral("result")).toObject().value(QStringLiteral("passed")).toBool();
    const bool noDialogueGuardPassed =
        noDialogueGuard.value(QStringLiteral("result")).toObject().value(QStringLiteral("passed")).toBool();
    const bool sourcePriorityGuardPassed =
        sourcePriorityGuard.value(QStringLiteral("result")).toObject().value(QStringLiteral("passed")).toBool();
    const bool reportSkillReadOnly =
        guardReport.value(QStringLiteral("executionPlan")).toObject()
            .value(QStringLiteral("canTriggerAsrOrTranslation")).toBool() == false &&
        guardReport.value(QStringLiteral("executionPlan")).toObject()
            .value(QStringLiteral("canChangePlaybackOrScheduling")).toBool() == false &&
        guardReport.value(QStringLiteral("executionPlan")).toObject()
            .value(QStringLiteral("canWriteSubtitleSidecars")).toBool() == false;
    addAssertion(
        QStringLiteral("CoverageAuditSkill reports separated coverage"),
        coverageAuditPassed,
        coverageAuditPassed
            ? QStringLiteral("source coverage and translated/displayable coverage are separate")
            : QStringLiteral("coverage audit reported a source/displayable risk"),
        coverageAudit);
    addAssertion(
        QStringLiteral("CacheGuardSkill reports quick baseline priority"),
        cacheGuardPassed,
        cacheGuardPassed
            ? QStringLiteral("quick .zh remains the display baseline")
            : QStringLiteral("cache guard reported quick baseline risk"),
        cacheGuard);
    addAssertion(
        QStringLiteral("NoDialogueGuardSkill reports clean overlay"),
        noDialogueGuardPassed,
        noDialogueGuardPassed
            ? QStringLiteral("no-dialogue and exact-cue overlay checks are clean")
            : QStringLiteral("no-dialogue guard reported overlay risk"),
        noDialogueGuard);
    addAssertion(
        QStringLiteral("SourcePriorityGuardSkill reports subtitle source priority"),
        sourcePriorityGuardPassed,
        sourcePriorityGuardPassed
            ? QStringLiteral("source priority is local/embedded/external > OCR high-quality > ASR fallback")
            : QStringLiteral("source priority guard reported ASR or lower-priority source risk"),
        sourcePriorityGuard);
    addAssertion(
        QStringLiteral("ReportSkill is read-only"),
        reportSkillReadOnly,
        reportSkillReadOnly
            ? QStringLiteral("report consumer cannot trigger generation, scheduling, sidecar writes, or playback changes")
            : QStringLiteral("report consumer is not read-only"),
        guardReport);
    if (!outputPath.trimmed().isEmpty()) {
        addAssertion(
            QStringLiteral("ReportSkill writes local report"),
            guardReportFileWritten,
            guardReportFileWritten ? guardReportPath : guardReportError,
            QJsonObject{
                { QStringLiteral("path"), guardReportPath },
                { QStringLiteral("error"), guardReportError }
            });
    }
    report.insert(QStringLiteral("results"), assertions);
    report.insert(QStringLiteral("summary"), makeSummary(assertions));
    return report;
}

QJsonObject MainWindow::runSubtitleCacheDisplaySmokeChecks(
    const QString& mediaPath,
    const QString& outputPath,
    const QVector<int>& frames)
{
    QJsonObject report;
    QJsonArray assertions;
    auto addAssertion = [&assertions](
                            const QString& name,
                            bool passed,
                            const QString& message,
                            const QJsonObject& details = {}) {
        assertions.append(QJsonObject{
            { QStringLiteral("name"), name },
            { QStringLiteral("status"), passed ? QStringLiteral("PASS") : QStringLiteral("FAIL") },
            { QStringLiteral("message"), message },
            { QStringLiteral("details"), details }
        });
    };

    openFile(mediaPath);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
    QThread::msleep(250);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 250);

    const bool mediaOpened = _p->playbackCtrl && _p->playbackCtrl->isValid();
    addAssertion(
        QStringLiteral("Open media"),
        mediaOpened,
        mediaOpened ? QStringLiteral("media opened") : QStringLiteral("player invalid"),
        QJsonObject{
            { QStringLiteral("media"), QFileInfo(mediaPath).absoluteFilePath() },
            { QStringLiteral("totalFrames"), _p->playbackCtrl ? _p->playbackCtrl->totalFrames() : 0 },
            { QStringLiteral("fps"), _p->playbackCtrl ? _p->playbackCtrl->fps() : 0.0 }
        });
    if (!mediaOpened) {
        report.insert(QStringLiteral("results"), assertions);
        report.insert(QStringLiteral("summary"), makeSummary(assertions));
        return report;
    }

    _setTranslationPlaybackMode(QStringLiteral("high-quality"));
    if (_p->playbackBar) {
        _p->playbackBar->setTranslationVisible(true);
        _p->playbackBar->setTranslationMode(QStringLiteral("high-quality"));
    }

    const QString translatedVttPath =
        SubtitleGenerationService::defaultTranslatedVttPath(mediaPath, QStringLiteral("zh-Hans"));
    const QString enhancedVttPath =
        SubtitleGenerationService::defaultEnhancedTranslatedVttPath(mediaPath, QStringLiteral("zh-Hans"));
    const QString ocrSourcePath =
        SubtitleGenerationService::defaultOcrSubtitleCachePath(mediaPath);
    QString ocrTranslatedPath = ocrSourcePath;
    if (ocrTranslatedPath.endsWith(QStringLiteral(".ocr.source.vtt"))) {
        ocrTranslatedPath.chop(QStringLiteral(".ocr.source.vtt").size());
        ocrTranslatedPath += QStringLiteral(".ocr.translated.zh.vtt");
    }
    const bool quickCacheExists = QFileInfo::exists(translatedVttPath);
    addAssertion(
        QStringLiteral("Quick subtitle cache exists"),
        quickCacheExists,
        quickCacheExists ? QStringLiteral("quick .zh cache available") : QStringLiteral("quick .zh cache missing"),
        QJsonObject{
            { QStringLiteral("translatedVttPath"), translatedVttPath },
            { QStringLiteral("enhancedVttPath"), enhancedVttPath },
            { QStringLiteral("ocrSourcePath"), ocrSourcePath },
            { QStringLiteral("ocrTranslatedPath"), ocrTranslatedPath }
        });

    QJsonArray frameReports;
    QJsonArray screenshotPaths;
    const QFileInfo outputInfo(outputPath);
    const QString basePath = outputInfo.absolutePath() + QLatin1Char('/') + outputInfo.completeBaseName();
    const double fps = _p->playbackCtrl->fps() > 0.0 ? _p->playbackCtrl->fps() : 24.0;
    for (const int requestedFrame : frames) {
        qInfo() << "[SubtitleCacheDisplaySmoke] Frame start" << requestedFrame;
        const int targetFrame = std::clamp(
            requestedFrame,
            0,
            std::max(0, _p->playbackCtrl->totalFrames() - 1));
        _p->playbackCtrl->seekToFrame(targetFrame);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        QThread::msleep(150);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        qInfo() << "[SubtitleCacheDisplaySmoke] Loading subtitle track" << targetFrame;
        _p->translationPlaybackMode = TranslationPlaybackStrategy::highQualityModeId();
        if (_p->userSettings) {
            _p->userSettings->setValue(QStringLiteral("ai/subtitles/playbackMode"), _p->translationPlaybackMode);
        }
        _loadGeneratedSubtitleTrack();
        qInfo() << "[SubtitleCacheDisplaySmoke] Updating overlay" << targetFrame;
        if (_p->playbackBar) {
            _p->playbackBar->setTranslationVisible(true);
            _p->playbackBar->setTranslationMode(QStringLiteral("high-quality"));
        }
        _updateGeneratedSubtitleForFrame(targetFrame);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);

        auto* overlayLabel = _p->generatedSubtitleLabel.data();
        const QString overlayText = overlayLabel ? overlayLabel->text().trimmed() : QString();
        const bool overlayVisible = overlayLabel && overlayLabel->isVisible() && !overlayText.isEmpty();
        const double seconds = static_cast<double>(targetFrame) / fps;
        qInfo() << "[SubtitleCacheDisplaySmoke] Capturing diagnostics" << targetFrame;
        const QJsonObject strategy = _translationPlaybackStrategyDiagnostics(true);
        const QJsonObject currentMediaHighQuality =
            strategy.value(QStringLiteral("currentMediaHighQuality")).toObject();
        const bool currentVisualCueAtTime =
            currentMediaHighQuality.value(QStringLiteral("visualCueAtTime")).toBool(false);
        const bool currentQuickCueAtTime =
            currentMediaHighQuality.value(QStringLiteral("quickCueAtTime")).toBool(false);
        const bool visualTrackAuthoritativeAtTime =
            currentMediaHighQuality.value(QStringLiteral("visualTrackAuthoritativeAtTime")).toBool(false);
        const bool expectNoText = visualTrackAuthoritativeAtTime && !currentVisualCueAtTime;
        const bool expectShadowPower = !expectNoText && seconds >= 67.5 && seconds <= 69.5;
        const bool displayStatePassed = expectNoText
            ? (!overlayVisible && overlayText.isEmpty())
            : ((currentVisualCueAtTime || currentQuickCueAtTime) ? overlayVisible : true);
        const bool shadowPowerPassed =
            !expectShadowPower ||
            (overlayText.contains(QString::fromUtf8(u8"姐姐")) &&
             (overlayText.contains(QString::fromUtf8(u8"暗影")) ||
              overlayText.contains(QString::fromUtf8(u8"影"))));

        const bool shadowPowerPassedStrict =
            !expectShadowPower ||
            ((overlayText.contains(QString::fromUtf8(u8"姐姐")) ||
              overlayText.contains(QString::fromUtf8(u8"姊姊"))) &&
             overlayText.contains(QString::fromUtf8(u8"暗影的力量")));

        const QString screenshotPath = QStringLiteral("%1_frame_%2.png").arg(basePath).arg(targetFrame);
        bool screenshotSaved = false;
        const QPixmap capture = grab();
        if (!capture.isNull()) {
            QDir().mkpath(QFileInfo(screenshotPath).absolutePath());
            screenshotSaved = capture.save(screenshotPath, "PNG");
        }
        if (screenshotSaved) {
            screenshotPaths.append(QFileInfo(screenshotPath).absoluteFilePath());
        }

        const double activeVisualCueStart =
            currentMediaHighQuality.value(QStringLiteral("activeVisualCueStart")).toDouble(-1.0);
        const double activeVisualCueEnd =
            currentMediaHighQuality.value(QStringLiteral("activeVisualCueEnd")).toDouble(-1.0);
        const bool afterActiveVisualCueEnd =
            activeVisualCueEnd >= 0.0 && seconds > activeVisualCueEnd + 0.02;
        const bool clearedAfterCueEnd =
            !afterActiveVisualCueEnd || (!overlayVisible && overlayText.trimmed().isEmpty());
        const QString holdReason =
            overlayVisible
                ? (currentVisualCueAtTime
                       ? QStringLiteral("active-visual-cue")
                       : (afterActiveVisualCueEnd
                             ? QStringLiteral("stale-hold-after-visual-cue")
                             : (currentQuickCueAtTime
                                   ? QStringLiteral("quick-cue-fallback")
                                   : QStringLiteral("unknown-visible-overlay"))))
                : (afterActiveVisualCueEnd
                       ? QStringLiteral("cleared-after-visual-cue-end")
                       : QStringLiteral("no-visible-overlay"));
        const QString diagnosticRawSourceText =
            currentMediaHighQuality.value(QStringLiteral("rawSourceText")).toString(
                currentMediaHighQuality.value(QStringLiteral("visualSourceDisplayedText")).toString());
        const QString diagnosticTranslatedText =
            currentMediaHighQuality.value(QStringLiteral("translatedText")).toString(
                currentMediaHighQuality.value(QStringLiteral("visualDisplayedText")).toString(
                    currentMediaHighQuality.value(QStringLiteral("visualEnhancedDisplayedText")).toString()));
        const QString diagnosticChosenReason =
            currentMediaHighQuality.value(QStringLiteral("chosenReason")).toString(
                currentVisualCueAtTime
                    ? QStringLiteral("translated-visual-cue")
                    : (currentQuickCueAtTime
                          ? QStringLiteral("quick-baseline")
                          : QStringLiteral("no-current-cue")));

        const QJsonObject details{
            { QStringLiteral("frame"), targetFrame },
            { QStringLiteral("seconds"), seconds },
            { QStringLiteral("currentTime"), seconds },
            { QStringLiteral("activeVisualCueStart"), activeVisualCueStart },
            { QStringLiteral("activeVisualCueEnd"), activeVisualCueEnd },
            { QStringLiteral("holdReason"), holdReason },
            { QStringLiteral("clearedAfterCueEnd"), clearedAfterCueEnd },
            { QStringLiteral("expectNoText"), expectNoText },
            { QStringLiteral("expectShadowPower"), expectShadowPower },
            { QStringLiteral("overlayVisible"), overlayVisible },
            { QStringLiteral("overlayText"), overlayText },
            { QStringLiteral("visibleDisplayText"), overlayText },
            { QStringLiteral("screenshotPath"), QFileInfo(screenshotPath).absoluteFilePath() },
            { QStringLiteral("screenshotSaved"), screenshotSaved },
            { QStringLiteral("visualTextDetected"),
              currentMediaHighQuality.value(QStringLiteral("visualTextDetected")) },
            { QStringLiteral("visualCueAtTime"),
              currentMediaHighQuality.value(QStringLiteral("visualCueAtTime")) },
            { QStringLiteral("visualSourceCueAtTime"),
              currentMediaHighQuality.value(QStringLiteral("visualSourceCueAtTime")) },
            { QStringLiteral("quickCueAtTime"),
              currentMediaHighQuality.value(QStringLiteral("quickCueAtTime")) },
            { QStringLiteral("quickDisplayedText"),
              currentMediaHighQuality.value(QStringLiteral("quickDisplayedText")) },
            { QStringLiteral("visualDisplayedText"),
              currentMediaHighQuality.value(QStringLiteral("visualDisplayedText")) },
            { QStringLiteral("visualSourceDisplayedText"),
              currentMediaHighQuality.value(QStringLiteral("visualSourceDisplayedText")) },
            { QStringLiteral("rawSourceText"), diagnosticRawSourceText },
            { QStringLiteral("translatedText"), diagnosticTranslatedText },
            { QStringLiteral("visualEnhancedCueAtTime"),
              currentMediaHighQuality.value(QStringLiteral("visualEnhancedCueAtTime")) },
            { QStringLiteral("visualEnhancedDisplayedText"),
              currentMediaHighQuality.value(QStringLiteral("visualEnhancedDisplayedText")) },
            { QStringLiteral("visualRawText"),
              currentMediaHighQuality.value(QStringLiteral("visualRawText")) },
            { QStringLiteral("ocrRawText"),
              currentMediaHighQuality.value(QStringLiteral("ocrRawText")) },
            { QStringLiteral("finalDisplayedText"),
              currentMediaHighQuality.value(QStringLiteral("finalDisplayedText")) },
            { QStringLiteral("fullFinalText"),
              currentMediaHighQuality.value(QStringLiteral("finalDisplayedText")) },
            { QStringLiteral("visibleDisplayLineCount"),
              overlayText.isEmpty() ? 0 : overlayText.split(QLatin1Char('\n')).size() },
            { QStringLiteral("sourceKind"),
              currentMediaHighQuality.value(QStringLiteral("sourceKind")) },
            { QStringLiteral("chosenReason"), diagnosticChosenReason },
            { QStringLiteral("workbenchConfigSource"),
              currentMediaHighQuality.value(QStringLiteral("workbenchConfigSource")) },
            { QStringLiteral("visualNoTextSuppressQuickAsr"),
              currentMediaHighQuality.value(QStringLiteral("visualNoTextSuppressQuickAsr")) },
            { QStringLiteral("visualTrackAuthoritativeAtTime"),
              currentMediaHighQuality.value(QStringLiteral("visualTrackAuthoritativeAtTime")) },
            { QStringLiteral("currentTimeWithinVisualCoverage"),
              currentMediaHighQuality.value(QStringLiteral("currentTimeWithinVisualCoverage")) },
            { QStringLiteral("fallbackReason"),
              currentMediaHighQuality.value(QStringLiteral("fallbackReason")) },
            { QStringLiteral("mediaFingerprint"),
              currentMediaHighQuality.value(QStringLiteral("mediaFingerprint")) },
            { QStringLiteral("mediaIdentity"),
              currentMediaHighQuality.value(QStringLiteral("mediaIdentity")) },
            { QStringLiteral("generatedSubtitlePath"), _p->generatedSubtitlePath },
            { QStringLiteral("generatedSubtitleMediaFingerprint"), _p->generatedSubtitleMediaFingerprint }
        };
        frameReports.append(details);
        qInfo() << "[SubtitleCacheDisplaySmoke] Frame done" << targetFrame << overlayVisible << overlayText;
        addAssertion(
            QStringLiteral("Subtitle cache display frame %1").arg(targetFrame),
            displayStatePassed && shadowPowerPassedStrict && screenshotSaved,
            displayStatePassed && shadowPowerPassedStrict
                ? QStringLiteral("overlay state matches target frame expectation")
                : QStringLiteral("overlay state does not match target frame expectation"),
            details);
    }

    report.insert(QStringLiteral("mode"), QStringLiteral("high-quality-cache-display"));
    report.insert(QStringLiteral("media"), QFileInfo(mediaPath).absoluteFilePath());
    report.insert(QStringLiteral("mediaFingerprint"), SubtitleGenerationService::mediaFingerprint(mediaPath));
    report.insert(QStringLiteral("mediaIdentity"), SubtitleGenerationService::mediaIdentity(mediaPath));
    report.insert(QStringLiteral("translatedVttPath"), translatedVttPath);
    report.insert(QStringLiteral("enhancedVttPath"), enhancedVttPath);
    report.insert(QStringLiteral("ocrSourcePath"), ocrSourcePath);
    report.insert(QStringLiteral("ocrTranslatedPath"), ocrTranslatedPath);
    report.insert(QStringLiteral("frames"), frameReports);
    report.insert(QStringLiteral("screenshots"), screenshotPaths);
    if (!screenshotPaths.isEmpty()) {
        report.insert(QStringLiteral("screenshot_path"), screenshotPaths.last().toString());
    }
    report.insert(QStringLiteral("results"), assertions);
    report.insert(QStringLiteral("summary"), makeSummary(assertions));
    return report;
}

QJsonObject MainWindow::runSubtitleSwitchSequenceSmokeChecks(
    const QStringList& mediaPaths,
    const QString& outputPath,
    const QVector<int>& frames)
{
    QJsonObject report;
    QJsonArray assertions;
    auto addAssertion = [&assertions](
                            const QString& name,
                            bool passed,
                            const QString& message,
                            const QJsonObject& details = {}) {
        assertions.append(QJsonObject{
            { QStringLiteral("name"), name },
            { QStringLiteral("status"), passed ? QStringLiteral("PASS") : QStringLiteral("FAIL") },
            { QStringLiteral("message"), message },
            { QStringLiteral("details"), details }
        });
    };

    QJsonArray steps;
    QSet<QString> seenFingerprints;
    const QFileInfo outputInfo(outputPath);
    const QString basePath = outputInfo.absolutePath() + QLatin1Char('/') + outputInfo.completeBaseName();
    for (int i = 0; i < mediaPaths.size(); ++i) {
        const QString mediaPath = QFileInfo(mediaPaths.at(i)).absoluteFilePath();
        const int frame = frames.isEmpty()
            ? 0
            : frames.value(i, frames.constLast());
        const QString stepOutputPath = QStringLiteral("%1_step_%2.json").arg(basePath).arg(i + 1);
        QJsonObject step = runSubtitleCacheDisplaySmokeChecks(mediaPath, stepOutputPath, QVector<int>{ frame });
        step.insert(QStringLiteral("stepIndex"), i);
        step.insert(QStringLiteral("requestedFrame"), frame);
        step.insert(QStringLiteral("media"), mediaPath);
        const QString expectedFingerprint = SubtitleGenerationService::mediaFingerprint(mediaPath);
        step.insert(QStringLiteral("expectedMediaFingerprint"), expectedFingerprint);
        seenFingerprints.insert(expectedFingerprint);

        QString overlayText;
        QString frameFingerprint;
        QString generatedFingerprint;
        const QJsonArray frameReports = step.value(QStringLiteral("frames")).toArray();
        if (!frameReports.isEmpty()) {
            const QJsonObject frameReport = frameReports.first().toObject();
            overlayText = frameReport.value(QStringLiteral("overlayText")).toString();
            frameFingerprint = frameReport.value(QStringLiteral("mediaFingerprint")).toString();
            generatedFingerprint =
                frameReport.value(QStringLiteral("generatedSubtitleMediaFingerprint")).toString();
        }
        const bool fingerprintMatches =
            frameFingerprint == expectedFingerprint &&
            (generatedFingerprint.isEmpty() || generatedFingerprint == expectedFingerprint);
        addAssertion(
            QStringLiteral("Switch step %1 media fingerprint isolated").arg(i + 1),
            fingerprintMatches,
            fingerprintMatches
                ? QStringLiteral("loaded/displayed subtitle state belongs to current media")
                : QStringLiteral("displayed subtitle state has a stale or wrong media fingerprint"),
            QJsonObject{
                { QStringLiteral("media"), mediaPath },
                { QStringLiteral("expectedMediaFingerprint"), expectedFingerprint },
                { QStringLiteral("frameMediaFingerprint"), frameFingerprint },
                { QStringLiteral("generatedSubtitleMediaFingerprint"), generatedFingerprint },
                { QStringLiteral("overlayText"), overlayText }
            });

        const QString lowerName = QFileInfo(mediaPath).fileName().toLower();
        if (lowerName.contains(QStringLiteral("jinwoo")) ||
            lowerName.contains(QStringLiteral("solo")) ||
            lowerName.contains(QStringLiteral("barca"))) {
            const bool noAniOneText =
                !overlayText.contains(QString::fromUtf8(u8"暗影庭园")) &&
                !overlayText.contains(QString::fromUtf8(u8"暗影")) &&
                !overlayText.contains(QString::fromUtf8(u8"庭园"));
            addAssertion(
                QStringLiteral("Jinwoo step has no Ani-One text"),
                noAniOneText,
                noAniOneText
                    ? QStringLiteral("current English media did not display Ani-One subtitle text")
                    : QStringLiteral("current English media displayed Ani-One/old-media subtitle text"),
                QJsonObject{
                    { QStringLiteral("media"), mediaPath },
                    { QStringLiteral("overlayText"), overlayText }
                });
        }

        steps.append(step);
    }

    QSet<QString> uniqueMediaPaths;
    for (const QString& mediaPath : mediaPaths) {
        uniqueMediaPaths.insert(QFileInfo(mediaPath).absoluteFilePath());
    }
    addAssertion(
        QStringLiteral("Switch sequence used media-scoped fingerprints"),
        seenFingerprints.size() == uniqueMediaPaths.size(),
        seenFingerprints.size() == uniqueMediaPaths.size()
            ? QStringLiteral("each distinct media path produced a distinct cache identity")
            : QStringLiteral("one or more distinct media paths shared a fingerprint unexpectedly"),
        QJsonObject{
            { QStringLiteral("mediaCount"), mediaPaths.size() },
            { QStringLiteral("distinctMediaCount"), uniqueMediaPaths.size() },
            { QStringLiteral("fingerprintCount"), seenFingerprints.size() }
        });

    report.insert(QStringLiteral("mode"), QStringLiteral("subtitle-switch-sequence"));
    report.insert(QStringLiteral("steps"), steps);
    report.insert(QStringLiteral("results"), assertions);
    report.insert(QStringLiteral("summary"), makeSummary(assertions));
    return report;
}

QJsonObject MainWindow::runSubtitleRefinedFallbackSmokeChecks(
    const QString& mediaPath,
    int frame)
{
    QJsonObject report;
    QJsonArray assertions;
    auto addAssertion = [&assertions](
                            const QString& name,
                            bool passed,
                            const QString& message,
                            const QJsonObject& details = {}) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("status"), passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
        item.insert(QStringLiteral("message"), message);
        item.insert(QStringLiteral("details"), details);
        assertions.append(item);
    };

    const QString translatedVttPath =
        SubtitleGenerationService::defaultTranslatedVttPath(mediaPath, QStringLiteral("zh-Hans"));
    const QString translatedSrtPath =
        SubtitleGenerationService::defaultTranslatedSrtPath(mediaPath, QStringLiteral("zh-Hans"));
    const QString refinedVttPath =
        SubtitleGenerationService::defaultRefinedTranslatedVttPath(mediaPath, QStringLiteral("zh-Hans"));
    const QString refinedSrtPath =
        SubtitleGenerationService::defaultRefinedTranslatedSrtPath(mediaPath, QStringLiteral("zh-Hans"));
    const QString enhancedVttPath =
        SubtitleGenerationService::defaultEnhancedTranslatedVttPath(mediaPath, QStringLiteral("zh-Hans"));
    const QString translatedZhVttPath =
        SubtitleGenerationService::defaultTranslatedVttPath(mediaPath, QStringLiteral("zh"));
    const QString translatedZhSrtPath =
        SubtitleGenerationService::defaultTranslatedSrtPath(mediaPath, QStringLiteral("zh"));
    const QString refinedZhVttPath =
        SubtitleGenerationService::defaultRefinedTranslatedVttPath(mediaPath, QStringLiteral("zh"));
    const QString refinedZhSrtPath =
        SubtitleGenerationService::defaultRefinedTranslatedSrtPath(mediaPath, QStringLiteral("zh"));
    const QString enhancedZhVttPath =
        SubtitleGenerationService::defaultEnhancedTranslatedVttPath(mediaPath, QStringLiteral("zh"));

    QString onlineTranslatedVttPath = SubtitleGenerationService::defaultOnlineSubtitleCachePath(mediaPath);
    if (onlineTranslatedVttPath.endsWith(QStringLiteral(".online.source.vtt"))) {
        onlineTranslatedVttPath.chop(QStringLiteral(".online.source.vtt").size());
        onlineTranslatedVttPath += QStringLiteral(".online.translated.zh.vtt");
    }

    struct SavedSidecar {
        QString path;
        bool existed = false;
        QByteArray bytes;
        QDateTime mtime;
    };
    QVector<SavedSidecar> savedSidecars;
    QStringList touchedPaths{
        translatedVttPath,
        translatedSrtPath,
        refinedVttPath,
        refinedSrtPath,
        enhancedVttPath,
        translatedZhVttPath,
        translatedZhSrtPath,
        refinedZhVttPath,
        refinedZhSrtPath,
        enhancedZhVttPath,
        onlineTranslatedVttPath
    };
    const QStringList subtitlePaths = touchedPaths;
    for (const QString& path : subtitlePaths) {
        touchedPaths.append(SubtitleGenerationService::mediaIdentitySidecarPath(path));
    }
    for (const QString& path : touchedPaths) {
        SavedSidecar saved;
        saved.path = path;
        QFileInfo info(path);
        saved.existed = info.exists();
        saved.mtime = info.lastModified();
        if (saved.existed) {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly)) {
                saved.bytes = file.readAll();
            }
        }
        savedSidecars.push_back(saved);
    }
    auto restoreSidecars = [&savedSidecars]() {
        for (const auto& saved : savedSidecars) {
            if (saved.existed) {
                QDir().mkpath(QFileInfo(saved.path).absolutePath());
                QFile file(saved.path);
                if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    file.write(saved.bytes);
                    file.close();
                }
                QFile timeFile(saved.path);
                if (timeFile.open(QIODevice::ReadWrite)) {
                    timeFile.setFileTime(saved.mtime, QFileDevice::FileModificationTime);
                }
            } else {
                QFile::remove(saved.path);
            }
        }
    };
    auto setMtime = [](const QString& path, const QDateTime& time) {
        QFile file(path);
        if (file.open(QIODevice::ReadWrite)) {
            file.setFileTime(time, QFileDevice::FileModificationTime);
        }
    };

    const bool translationWasVisible = _p->playbackBar && _p->playbackBar->translationVisible();
    if (_p->playbackBar) {
        _p->playbackBar->setTranslationVisible(false);
    }
    openFile(mediaPath);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
    QThread::msleep(250);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 250);

    const bool mediaOpened = _p->playbackCtrl && _p->playbackCtrl->isValid();
    const double fps = _p->playbackCtrl && _p->playbackCtrl->fps() > 0.0 ? _p->playbackCtrl->fps() : 24.0;
    const double durationSeconds =
        _p->playbackCtrl && _p->playbackCtrl->totalFrames() > 0
            ? _p->playbackCtrl->totalFrames() / fps
            : 0.0;
    double targetSeconds = frame > 0 ? frame / fps : 9.0;
    if (durationSeconds > 0.0) {
        targetSeconds = std::clamp(targetSeconds, 2.0, std::max(2.0, durationSeconds - 2.0));
    }
    const int targetFrame = std::max(0, static_cast<int>(targetSeconds * fps + 0.5));
    if (_p->playbackCtrl && mediaOpened) {
        _p->playbackCtrl->seekToFrame(std::clamp(targetFrame, 0, std::max(0, _p->playbackCtrl->totalFrames() - 1)));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
        QThread::msleep(200);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
    }
    if (_p->playbackBar) {
        _p->playbackBar->setTranslationVisible(true);
    }
    _setTranslationStripVisible(false, false);

    const QVector<GeneratedSubtitleCue> quickCues{
        {1, std::max(0.0, targetSeconds - 8.0), std::max(0.5, targetSeconds - 4.5), QString(), QStringLiteral("\u5feb\u901f\u524d\u6bb5\u4e00")},
        {2, std::max(0.0, targetSeconds - 4.0), std::max(0.5, targetSeconds - 0.6), QString(), QStringLiteral("\u5feb\u901f\u524d\u6bb5\u4e8c")},
        {3, std::max(0.0, targetSeconds - 0.4), targetSeconds + 3.6, QString(), QStringLiteral("\u5feb\u901f\u56de\u9000\u76ee\u6807")},
        {4, targetSeconds + 4.0, targetSeconds + 8.0, QString(), QStringLiteral("\u5feb\u901f\u540e\u6bb5")}
    };
    const QVector<GeneratedSubtitleCue> oldRefinedCues{
        {1, quickCues.at(0).startSeconds, quickCues.at(0).endSeconds, QString(), QStringLiteral("\u65e7\u7cbe\u4fee\u524d\u6bb5")},
        {2, quickCues.at(2).startSeconds, quickCues.at(2).endSeconds, QString(), QStringLiteral("\u65e7\u7cbe\u4fee\u9519\u8bef\u5f53\u524d")}
    };
    const QVector<GeneratedSubtitleCue> freshHoleyRefinedCues{
        {1, quickCues.at(0).startSeconds, quickCues.at(0).endSeconds, QString(), QStringLiteral("\u65b0\u7cbe\u4fee\u524d\u6bb5")}
    };
    const QVector<GeneratedSubtitleCue> enhancedCues{
        {1, quickCues.at(0).startSeconds, quickCues.at(0).endSeconds, QString(), QStringLiteral("\u589e\u5f3a\u663e\u793a\u524d\u6bb5")}
    };

    QFile::remove(translatedSrtPath);
    QFile::remove(refinedSrtPath);
    QFile::remove(enhancedVttPath);
    QFile::remove(translatedZhVttPath);
    QFile::remove(translatedZhSrtPath);
    QFile::remove(refinedZhVttPath);
    QFile::remove(refinedZhSrtPath);
    QFile::remove(enhancedZhVttPath);
    _p->translationPlaybackMode = QStringLiteral("quick-playback");
    if (_p->playbackBar) {
        _p->playbackBar->setTranslationMode(_p->translationPlaybackMode);
    }
    const bool wroteQuick = writeSmokeSubtitleVtt(translatedVttPath, quickCues);
    const bool wroteOldRefined = writeSmokeSubtitleVtt(refinedVttPath, oldRefinedCues);
    const bool wroteQuickIdentity = SubtitleGenerationService::writeMediaIdentitySidecar(
        translatedVttPath, mediaPath, durationSeconds);
    const bool wroteOldRefinedIdentity = SubtitleGenerationService::writeMediaIdentitySidecar(
        refinedVttPath, mediaPath, durationSeconds);
    const QDateTime now = QDateTime::currentDateTime();
    setMtime(translatedVttPath, now);
    setMtime(refinedVttPath, now.addSecs(-120));
    _loadGeneratedSubtitleTrack();
    if (_p->playbackBar) {
        _p->playbackBar->setTranslationVisible(true);
    }
    _updateGeneratedSubtitleForFrame(targetFrame);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto* overlayLabel = _p->generatedSubtitleLabel.data();
    const QString oldScenarioText = overlayLabel ? overlayLabel->text().trimmed() : QString();
    const bool oldIgnored = oldScenarioText == QStringLiteral("\u5feb\u901f\u56de\u9000\u76ee\u6807");

    const bool wroteFreshRefined = writeSmokeSubtitleVtt(refinedVttPath, freshHoleyRefinedCues);
    const bool wroteFreshRefinedIdentity = SubtitleGenerationService::writeMediaIdentitySidecar(
        refinedVttPath, mediaPath, durationSeconds);
    setMtime(translatedVttPath, now);
    setMtime(refinedVttPath, now.addSecs(120));
    _loadGeneratedSubtitleTrack();
    if (_p->playbackBar) {
        _p->playbackBar->setTranslationVisible(true);
    }
    _updateGeneratedSubtitleForFrame(targetFrame);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    overlayLabel = _p->generatedSubtitleLabel.data();
    const QString freshScenarioText = overlayLabel ? overlayLabel->text().trimmed() : QString();
    const bool freshHoleyFallback = freshScenarioText == QStringLiteral("\u5feb\u901f\u56de\u9000\u76ee\u6807");
    const bool cueCountPreserved = _p->generatedSubtitleCues.size() == quickCues.size();
    const bool earlyCueEnhanced =
        !_p->generatedSubtitleCues.isEmpty() &&
        _p->generatedSubtitleCues.first().translatedText.trimmed() == QStringLiteral("\u65b0\u7cbe\u4fee\u524d\u6bb5");
    QString activeCueText;
    for (const auto& cue : _p->generatedSubtitleCues) {
        if (targetSeconds + 0.02 >= cue.startSeconds &&
            targetSeconds <= cue.endSeconds + 0.02) {
            activeCueText = cue.translatedText.trimmed();
            break;
        }
    }
    const double quickCoverageEnd = quickCues.last().endSeconds;
    const bool coveragePreserved =
        _p->generatedSubtitleCoverageEndSeconds + 0.01 >= quickCoverageEnd;
    const bool pathIsQuick =
        QFileInfo(_p->generatedSubtitlePath).absoluteFilePath() ==
        QFileInfo(translatedVttPath).absoluteFilePath();

    const bool wroteEnhanced = writeSmokeSubtitleVtt(enhancedVttPath, enhancedCues);
    const bool wroteEnhancedIdentity = SubtitleGenerationService::writeMediaIdentitySidecar(
        enhancedVttPath, mediaPath, durationSeconds);
    const bool wroteOnlineEvidence = writeSmokeSubtitleVtt(onlineTranslatedVttPath, enhancedCues);
    const bool wroteOnlineEvidenceIdentity = SubtitleGenerationService::writeMediaIdentitySidecar(
        onlineTranslatedVttPath, mediaPath, durationSeconds);
    setMtime(translatedVttPath, now);
    setMtime(refinedVttPath, now.addSecs(120));
    setMtime(enhancedVttPath, now.addSecs(180));
    _p->translationPlaybackMode = QStringLiteral("high-quality");
    if (_p->playbackBar) {
        _p->playbackBar->setTranslationMode(_p->translationPlaybackMode);
    }
    _loadGeneratedSubtitleTrack();
    if (_p->playbackBar) {
        _p->playbackBar->setTranslationVisible(true);
    }
    _updateGeneratedSubtitleForFrame(targetFrame);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    overlayLabel = _p->generatedSubtitleLabel.data();
    const QString enhancedScenarioText = overlayLabel ? overlayLabel->text().trimmed() : QString();
    const QJsonObject enhancedStrategyDiagnostics = _translationPlaybackStrategyDiagnostics(false);
    const bool enhancedLoaded =
        enhancedStrategyDiagnostics.value(QStringLiteral("enhancedLoaded")).toBool();
    const int enhancedCueMatches =
        enhancedStrategyDiagnostics.value(QStringLiteral("enhancedCueMatches")).toInt();
    const bool enhancedCueCountPreserved = _p->generatedSubtitleCues.size() == quickCues.size();
    const bool enhancedPathIsQuick =
        QFileInfo(_p->generatedSubtitlePath).absoluteFilePath() ==
        QFileInfo(translatedVttPath).absoluteFilePath();
    const bool enhancedCurrentFallback =
        enhancedScenarioText == QStringLiteral("\u5feb\u901f\u56de\u9000\u76ee\u6807");
    const bool enhancedFirstCueMerged =
        !_p->generatedSubtitleCues.isEmpty() &&
        _p->generatedSubtitleCues.first().translatedText.trimmed() ==
            QStringLiteral("\u589e\u5f3a\u663e\u793a\u524d\u6bb5");
    const bool enhancedWholeTrackReplacement =
        enhancedStrategyDiagnostics.value(QStringLiteral("wholeTrackReplacement")).toBool();
    const bool enhancedCurrentCueFallbackSafe =
        enhancedStrategyDiagnostics.value(QStringLiteral("currentCueFallbackSafe")).toBool();

    report.insert(QStringLiteral("fallback"), QJsonObject{
        { QStringLiteral("translatedVttPath"), translatedVttPath },
        { QStringLiteral("refinedVttPath"), refinedVttPath },
        { QStringLiteral("enhancedVttPath"), enhancedVttPath },
        { QStringLiteral("targetFrame"), targetFrame },
        { QStringLiteral("targetSeconds"), targetSeconds },
        { QStringLiteral("quickCueCount"), quickCues.size() },
        { QStringLiteral("loadedCueCount"), _p->generatedSubtitleCues.size() },
        { QStringLiteral("oldScenarioOverlayText"), oldScenarioText },
        { QStringLiteral("freshHoleyScenarioOverlayText"), freshScenarioText },
        { QStringLiteral("labelVisible"), overlayLabel ? overlayLabel->isVisible() : false },
        { QStringLiteral("playbackBarTranslationVisible"), _p->playbackBar ? _p->playbackBar->translationVisible() : false },
        { QStringLiteral("translationStripVisible"), _p->translationStripVisible },
        { QStringLiteral("activeCueText"), activeCueText },
        { QStringLiteral("firstLoadedCueText"), _p->generatedSubtitleCues.isEmpty()
              ? QString()
              : _p->generatedSubtitleCues.first().translatedText.trimmed() },
        { QStringLiteral("generatedSubtitlePath"), _p->generatedSubtitlePath },
        { QStringLiteral("coverageEndSeconds"), _p->generatedSubtitleCoverageEndSeconds },
        { QStringLiteral("quickCoverageEndSeconds"), quickCoverageEnd },
        { QStringLiteral("earlyCueEnhanced"), earlyCueEnhanced }
    });
    report.insert(QStringLiteral("enhancedDisplay"), QJsonObject{
        { QStringLiteral("enhancedVttPath"), enhancedVttPath },
        { QStringLiteral("wroteEnhanced"), wroteEnhanced },
        { QStringLiteral("loadedCueCount"), _p->generatedSubtitleCues.size() },
        { QStringLiteral("overlayText"), enhancedScenarioText },
        { QStringLiteral("generatedSubtitlePath"), _p->generatedSubtitlePath },
        { QStringLiteral("enhancedLoaded"), enhancedLoaded },
        { QStringLiteral("enhancedCueMatches"), enhancedCueMatches },
        { QStringLiteral("quickFallbackCueCount"),
          enhancedStrategyDiagnostics.value(QStringLiteral("quickFallbackCueCount")).toInt() },
        { QStringLiteral("wholeTrackReplacement"), enhancedWholeTrackReplacement },
        { QStringLiteral("currentCueFallbackSafe"), enhancedCurrentCueFallbackSafe },
        { QStringLiteral("strategy"), enhancedStrategyDiagnostics }
    });
    addAssertion(
        QStringLiteral("Open media"),
        mediaOpened,
        mediaOpened ? QStringLiteral("media opened") : QStringLiteral("player invalid"));
    addAssertion(
        QStringLiteral("Synthetic quick/refined files written"),
        wroteQuick && wroteOldRefined && wroteFreshRefined &&
            wroteQuickIdentity && wroteOldRefinedIdentity &&
            wroteFreshRefinedIdentity && wroteEnhancedIdentity &&
            wroteOnlineEvidence && wroteOnlineEvidenceIdentity,
        wroteQuick && wroteOldRefined && wroteFreshRefined &&
                wroteQuickIdentity && wroteOldRefinedIdentity &&
                wroteFreshRefinedIdentity && wroteEnhancedIdentity &&
                wroteOnlineEvidence && wroteOnlineEvidenceIdentity
            ? QStringLiteral("synthetic sidecars ready")
            : QStringLiteral("failed to write synthetic sidecars"),
        QJsonObject{
            { QStringLiteral("quick"), translatedVttPath },
            { QStringLiteral("refined"), refinedVttPath },
            { QStringLiteral("enhanced"), enhancedVttPath }
        });
    addAssertion(
        QStringLiteral("Older refined ignored"),
        oldIgnored,
        oldIgnored ? QStringLiteral("old refined did not replace current quick cue") : oldScenarioText);
    addAssertion(
        QStringLiteral("Holey refined falls back to quick current cue"),
        freshHoleyFallback,
        freshHoleyFallback ? QStringLiteral("current cue stayed on quick text") : freshScenarioText);
    addAssertion(
        QStringLiteral("Refined enhances only matching cue"),
        earlyCueEnhanced && cueCountPreserved,
        earlyCueEnhanced && cueCountPreserved
            ? QStringLiteral("matched refined cue enhanced while quick cue count stayed intact")
            : QStringLiteral("refined changed cue count or failed to enhance matched cue"));
    addAssertion(
        QStringLiteral("Quick coverage preserved"),
        coveragePreserved && pathIsQuick,
        coveragePreserved && pathIsQuick
            ? QStringLiteral("display coverage and path stayed quick-baseline")
            : QStringLiteral("coverage/path moved away from quick baseline"),
        QJsonObject{
            { QStringLiteral("coverageEndSeconds"), _p->generatedSubtitleCoverageEndSeconds },
            { QStringLiteral("quickCoverageEndSeconds"), quickCoverageEnd },
            { QStringLiteral("path"), _p->generatedSubtitlePath }
        });
    addAssertion(
        QStringLiteral("Enhanced display merges only matching cue"),
        wroteEnhanced &&
            enhancedLoaded &&
            enhancedCueMatches == 1 &&
            enhancedFirstCueMerged &&
            enhancedCueCountPreserved,
        enhancedLoaded
            ? QStringLiteral("enhanced cache replaced only a matched quick cue")
            : QStringLiteral("enhanced cache was not loaded in high-quality display mode"),
        report.value(QStringLiteral("enhancedDisplay")).toObject());
    addAssertion(
        QStringLiteral("Enhanced display keeps quick fallback current cue"),
        enhancedCurrentFallback &&
            enhancedPathIsQuick &&
            !enhancedWholeTrackReplacement &&
            enhancedCurrentCueFallbackSafe,
        enhancedCurrentFallback
            ? QStringLiteral("current cue stayed on quick text and quick .zh remained baseline")
            : QStringLiteral("enhanced display changed current cue fallback or baseline path"),
        report.value(QStringLiteral("enhancedDisplay")).toObject());

    if (_p->playbackBar) {
        _p->playbackBar->setTranslationVisible(translationWasVisible);
    }
    restoreSidecars();
    report.insert(QStringLiteral("restoredSidecars"), true);
    report.insert(QStringLiteral("results"), assertions);
    report.insert(QStringLiteral("summary"), makeSummary(assertions));
    return report;
}

} // namespace cgplay
