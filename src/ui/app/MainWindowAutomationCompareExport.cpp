#include "Application.h"
#include "ApplicationPrivate.h"
#include "ApplicationRuntimeSupport.h"
#include "CommandDispatcher.h"

#include "component/ComponentRuntimePaths.h"
#include "playback/api/IPlaybackService.h"
#include "ocio/OcioManager.h"
#include "viewer/CompareToolbar.h"

#include <QComboBox>
#include <QColor>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardItemModel>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <memory>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace cgplay {
namespace {

int nativeVisibleWindows(bool ownProcessOnly)
{
    int count = 0;
#ifdef Q_OS_WIN
    struct Context { bool own; int count; } context{ownProcessOnly, 0};
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto* context = reinterpret_cast<Context*>(parameter);
        if (!IsWindowVisible(window)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (!context->own || pid == GetCurrentProcessId()) ++context->count;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    count = context.count;
#else
    Q_UNUSED(ownProcessOnly);
#endif
    return count;
}

bool variedImage(const QImage& original)
{
    if (original.isNull()) return false;
    const QImage image = original.convertToFormat(QImage::Format_RGB32);
    const QRgb first = image.pixel(0, 0);
    const int xStep = std::max(1, image.width() / 80);
    const int yStep = std::max(1, image.height() / 80);
    for (int y = 0; y < image.height(); y += yStep)
        for (int x = 0; x < image.width(); x += xStep)
            if (image.pixel(x, y) != first) return true;
    return false;
}

QJsonObject saveFormEvidence(QDialog* dialog, const QString& directory)
{
    const QString printPath = QDir(directory).filePath(QStringLiteral("form_printwindow.png"));
    const QString finalPath = QDir(directory).filePath(QStringLiteral("form.png"));
    QString method;
    const bool firstSaved = application_runtime::saveWindowEvidence(dialog, printPath, &method);
    const bool firstVaried = firstSaved && variedImage(QImage(printPath));
    bool saved = firstVaried;
    QString fallbackReason;
    if (firstVaried) {
        saved = QImage(printPath).save(finalPath, "PNG");
    } else {
        fallbackReason = firstSaved ? QStringLiteral("PrintWindow returned a uniform image")
                                    : QStringLiteral("PrintWindow capture unavailable");
        const QPixmap grabbed = dialog->grab();
        saved = !grabbed.isNull() && variedImage(grabbed.toImage()) && grabbed.save(finalPath, "PNG");
        method = QStringLiteral("QWidget::grab fallback");
    }
    return {{QStringLiteral("saved"), saved}, {QStringLiteral("path"), finalPath},
        {QStringLiteral("method"), method}, {QStringLiteral("firstAttemptPath"), printPath},
        {QStringLiteral("firstAttemptVaried"), firstVaried}, {QStringLiteral("fallbackReason"), fallbackReason}};
}

struct CompareSmokeState
{
    int phase = 0;
    int maximumOwnVisible = 0;
    int platformVisibleBefore = 0;
    bool completed = false;
    QElapsedTimer elapsed;
    QElapsedTimer phaseElapsed;
    QJsonArray checks;
    QJsonObject evidence;
    ComponentRuntimePaths runtime;
    QPointer<QTimer> timer;
    QPointer<QProcess> probe;
    std::function<void(QJsonObject)> finished;
};

} // namespace

void MainWindow::runCompareExportSmokeChecks(const QString& sourceA, const QString& sourceB,
    const QString& artifactDirectory, std::function<void(QJsonObject)> finished)
{
    auto state = std::make_shared<CompareSmokeState>();
    state->finished = std::move(finished);
    state->elapsed.start();
    state->phaseElapsed.start();
    state->platformVisibleBefore = nativeVisibleWindows(false);
    state->runtime = captureComponentRuntimePaths();
    state->timer = new QTimer(this);
    state->timer->setInterval(25);
    state->evidence.insert(QStringLiteral("sourceA"), sourceA);
    state->evidence.insert(QStringLiteral("sourceB"), sourceB);
    const QString outputPath = QDir(artifactDirectory).filePath(QStringLiteral("native_compare.mp4"));
    const auto add = [state](const QString& name, bool passed, const QJsonObject& details = {}) {
        state->checks.append(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("passed"), passed},
            {QStringLiteral("status"), passed ? QStringLiteral("PASS") : QStringLiteral("FAIL")},
            {QStringLiteral("details"), details}});
    };
    const auto finish = [this, state, add](const QString& failure = QString()) {
        if (state->completed) return;
        state->completed = true;
        if (!failure.isEmpty()) add(QStringLiteral("smoke completed"), false, {{QStringLiteral("error"), failure}});
        if (state->timer) { state->timer->stop(); state->timer->deleteLater(); }
        if (state->probe && state->probe->state() != QProcess::NotRunning) state->probe->kill();
        if (property("cgplay.compareExportRunning").toBool())
            for (auto* progress : findChildren<QProgressDialog*>()) progress->cancel();
        state->maximumOwnVisible = std::max(state->maximumOwnVisible, nativeVisibleWindows(true));
        const bool background = qApp->property("cgplay.automationBackground").toBool();
        add(QStringLiteral("background windows policy"), background && state->maximumOwnVisible == 0,
            {{QStringLiteral("backgroundMode"), background}, {QStringLiteral("visibleAutomationWindowsMax"), state->maximumOwnVisible}});
        int passed = 0, failed = 0;
        for (const auto& value : state->checks)
            value.toObject().value(QStringLiteral("passed")).toBool() ? ++passed : ++failed;
        state->evidence.insert(QStringLiteral("elapsedMs"), state->elapsed.elapsed());
        QJsonObject report{{QStringLiteral("passed"), failed == 0}, {QStringLiteral("backgroundMode"), background},
            {QStringLiteral("platformVisibleTopLevelWindowsBefore"), state->platformVisibleBefore},
            {QStringLiteral("platformVisibleTopLevelWindowsAfter"), nativeVisibleWindows(false)},
            {QStringLiteral("visibleAutomationWindowsMax"), state->maximumOwnVisible},
            {QStringLiteral("checks"), state->checks}, {QStringLiteral("evidence"), state->evidence},
            {QStringLiteral("summary"), QJsonObject{{QStringLiteral("pass"), passed}, {QStringLiteral("fail"), failed}}}};
        if (state->finished) state->finished(report);
    };
    if (!QDir().mkpath(artifactDirectory) || !QFileInfo::exists(sourceA) || !QFileInfo::exists(sourceB)) {
        finish(QStringLiteral("Deterministic fixture paths or artifact directory unavailable"));
        return;
    }
    if (!_p->playbackCtrl || !_p->compareBar || !_p->ocioManager || !_p->commandDispatcher) {
        finish(QStringLiteral("Native player, compare controls, color manager or command dispatcher missing"));
        return;
    }
    setAttribute(Qt::WA_DontShowOnScreen);
    _p->ocioManager->setEnabled(false);
    _p->ocioManager->setExposure(0.F);
    _p->ocioManager->setGamma(1.F);
    openFile(sourceA);

    connect(state->timer, &QTimer::timeout, this,
        [this, state, sourceA, sourceB, outputPath, artifactDirectory, add, finish] {
        if (state->completed) return;
        state->maximumOwnVisible = std::max(state->maximumOwnVisible, nativeVisibleWindows(true));
        if (state->elapsed.elapsed() > 90000) { finish(QStringLiteral("N01 background smoke timed out")); return; }
        auto* playback = _p->playbackCtrl.get();
        if (!playback || !_p->compareBar || !_p->ocioManager) { finish(QStringLiteral("Player UI was destroyed")); return; }
        if (state->phase == 0) {
#if CGPLAY_HAS_TLRENDER
            if (!playback->isValid() || !playback->player() ||
                QFileInfo(playback->currentPath()).canonicalFilePath().compare(
                    QFileInfo(sourceA).canonicalFilePath(), Qt::CaseInsensitive) != 0) return;
#else
            finish(QStringLiteral("tlRender is disabled")); return;
#endif
            if (playback->totalFrames() < 15) { finish(QStringLiteral("Fixture needs at least 15 native timeline frames")); return; }
            add(QStringLiteral("native A source rate"), std::fabs(playback->fps() - 24.0) < .00001,
                {{QStringLiteral("fps"), playback->fps()}, {QStringLiteral("totalFrames"), playback->totalFrames()}});
            playback->pause(); playback->setMute(true);
            playback->clearInOutPoints(); playback->setInPoint(3); playback->setOutPoint(14);
            playback->setCompareFile(sourceB); playback->setCompareTime(0);
            _p->compareBar->setCompareMode(2);
            _p->compareBar->setWipeCenter(.25F, .75F);
            _p->compareBar->setWipeRotation(45.F);
            _p->compareBar->setOverlay(.25F);
            state->phase = 1; state->phaseElapsed.restart(); return;
        }
        if (state->phase == 1) {
            if (!playback->hasCompare() || state->phaseElapsed.elapsed() < 125) return;
            const bool dispatched = _p->commandDispatcher->dispatch(QStringLiteral("export.compareVideo"));
            add(QStringLiteral("registered compare command dispatched"), dispatched);
            if (!dispatched) { finish(QStringLiteral("export.compareVideo rejected deterministic A/B inputs")); return; }
            state->phase = 2; state->phaseElapsed.restart(); return;
        }
        if (state->phase == 2) {
            auto* dialog = findChild<QDialog*>(QStringLiteral("CompareExportDialog"));
            if (!dialog) {
                const QString error = property("cgplay.compareExportError").toString();
                if (!error.isEmpty()) finish(error);
                else if (state->phaseElapsed.elapsed() > 10000) finish(QStringLiteral("Compare export dialog not created"));
                return;
            }
            auto* modes = dialog->findChild<QComboBox*>(QStringLiteral("CompareExportMode"));
            auto* ranges = dialog->findChild<QComboBox*>(QStringLiteral("CompareExportRange"));
            auto* audio = dialog->findChild<QComboBox*>(QStringLiteral("CompareExportAudio"));
            auto* fps = dialog->findChild<QLabel*>(QStringLiteral("CompareExportFps"));
            auto* x = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("CompareExportWipeX"));
            auto* y = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("CompareExportWipeY"));
            auto* angle = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("CompareExportWipeRotation"));
            auto* overlay = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("CompareExportOverlay"));
            auto* output = dialog->findChild<QLineEdit*>(QStringLiteral("CompareExportOutput"));
            auto* codec = dialog->findChild<QComboBox*>(QStringLiteral("CompareExportCodec"));
            auto* start = dialog->findChild<QPushButton*>(QStringLiteral("CompareExportStart"));
            if (!modes || !ranges || !audio || !fps || !x || !y || !angle || !overlay || !output || !codec || !start) {
                finish(QStringLiteral("Compare export form control is missing")); return;
            }
            const int rangeIndex = ranges->findData(QStringLiteral("inout"));
            const int audioIndex = audio->findData(QStringLiteral("b"));
            const bool rangeEnabled = rangeIndex >= 0 && (ranges->model()->flags(ranges->model()->index(rangeIndex, 0)) & Qt::ItemIsEnabled);
            const bool audioEnabled = audioIndex >= 0 && (audio->model()->flags(audio->model()->index(audioIndex, 0)) & Qt::ItemIsEnabled);
            add(QStringLiteral("current compare form values"), modes->currentData().toString() == QStringLiteral("wipe") &&
                std::fabs(x->value() - .25) < .0001 && std::fabs(y->value() - .75) < .0001 &&
                std::fabs(angle->value() - 45.0) < .0001 && std::fabs(overlay->value() - .25) < .0001 &&
                fps->text().contains(QStringLiteral("24")) && fps->text().contains(QStringLiteral("CFR")) && rangeEnabled && audioEnabled,
                {{QStringLiteral("mode"), modes->currentData().toString()}, {QStringLiteral("fpsLabel"), fps->text()},
                 {QStringLiteral("rangeEnabled"), rangeEnabled}, {QStringLiteral("bAudioEnabled"), audioEnabled}});
            if (!rangeEnabled || !audioEnabled) { finish(QStringLiteral("Fixture range or B audio cannot be selected")); return; }
            add(QStringLiteral("default audio uses available A source"), audio->currentData().toString() == QStringLiteral("a"));
            codec->setCurrentIndex(codec->findData(QStringLiteral("prores_hq")));
            const QJsonObject prores{{QStringLiteral("codec"), codec->currentData().toString()},
                {QStringLiteral("path"), output->text()}, {QStringLiteral("suffix"), QFileInfo(output->text()).suffix()}};
            add(QStringLiteral("ProRes selection changes container path to MOV"),
                codec->currentData().toString() == QStringLiteral("prores_hq") &&
                QFileInfo(output->text()).suffix().compare(QStringLiteral("mov"), Qt::CaseInsensitive) == 0, prores);
            state->evidence.insert(QStringLiteral("proresControlSelection"), prores);
            codec->setCurrentIndex(codec->findData(QStringLiteral("h264")));
            add(QStringLiteral("H264 selection restores MP4 path"),
                codec->currentData().toString() == QStringLiteral("h264") &&
                QFileInfo(output->text()).suffix().compare(QStringLiteral("mp4"), Qt::CaseInsensitive) == 0);
            ranges->setCurrentIndex(rangeIndex); audio->setCurrentIndex(audioIndex); output->setText(outputPath);
            state->phase = 3; state->phaseElapsed.restart(); return;
        }
        if (state->phase == 3) {
            if (state->phaseElapsed.elapsed() < 100) return;
            auto* dialog = findChild<QDialog*>(QStringLiteral("CompareExportDialog"));
            auto* start = dialog ? dialog->findChild<QPushButton*>(QStringLiteral("CompareExportStart")) : nullptr;
            if (!dialog || !start) { finish(QStringLiteral("Configured export form disappeared")); return; }
            const QJsonObject screenshot = saveFormEvidence(dialog, artifactDirectory);
            state->evidence.insert(QStringLiteral("formScreenshot"), screenshot);
            add(QStringLiteral("configured form screenshot"), screenshot.value(QStringLiteral("saved")).toBool(), screenshot);
            start->click();
            const QJsonObject job = QJsonObject::fromVariantMap(property("cgplay.compareExportLastJob").toMap());
            state->evidence.insert(QStringLiteral("frozenJob"), job);
            const auto comparison = job.value(QStringLiteral("compare")).toObject();
            const auto range = job.value(QStringLiteral("range")).toObject();
            const auto sources = job.value(QStringLiteral("sources")).toObject();
            const auto colors = sources.value(QStringLiteral("a")).toObject().value(QStringLiteral("color")).toObject();
            const auto center = comparison.value(QStringLiteral("wipeCenter")).toArray();
            const bool frozen = comparison.value(QStringLiteral("mode")).toString() == QStringLiteral("wipe") &&
                comparison.value(QStringLiteral("time")).toString() == QStringLiteral("relative") && center.size() == 2 &&
                center.at(0).toDouble() == .25 && center.at(1).toDouble() == .75 &&
                comparison.value(QStringLiteral("wipeRotation")).toDouble() == 45.0 &&
                comparison.value(QStringLiteral("overlay")).toDouble() == .25 &&
                range.value(QStringLiteral("start")).toInt() == 3 && range.value(QStringLiteral("end")).toInt() == 14 &&
                job.value(QStringLiteral("audio")).toObject().value(QStringLiteral("source")).toString() == QStringLiteral("b") &&
                colors.value(QStringLiteral("exposure")).toDouble() == 0.0;
            add(QStringLiteral("job freezes mode clocks range audio and color"), frozen);
            if (!frozen) { finish(property("cgplay.compareExportError").toString().isEmpty()
                ? QStringLiteral("Export did not publish its immutable job") : property("cgplay.compareExportError").toString()); return; }
            // A/B are the same fixture. A worker that rereads these live controls
            // produces black Difference instead of the captured non-black wipe.
            playback->setCompareTime(1); _p->compareBar->setCompareMode(4); _p->ocioManager->setExposure(1.F);
            state->phase = 4; state->phaseElapsed.restart(); return;
        }
        if (state->phase == 4) {
            const QVariantMap result = property("cgplay.compareExportResult").toMap();
            if (property("cgplay.compareExportRunning").toBool() || !result.contains(QStringLiteral("success"))) return;
            state->evidence.insert(QStringLiteral("exportResult"), QJsonObject::fromVariantMap(result));
            add(QStringLiteral("native async export committed"), result.value(QStringLiteral("success")).toBool() &&
                !result.value(QStringLiteral("canceled")).toBool() && QFileInfo(outputPath).size() > 0,
                QJsonObject::fromVariantMap(result));
            if (!result.value(QStringLiteral("success")).toBool()) { finish(result.value(QStringLiteral("error")).toString()); return; }
            if (state->runtime.pythonExecutable.isEmpty()) { finish(QStringLiteral("Prepared Python probe runtime missing")); return; }
            auto* process = new QProcess(this); state->probe = process;
            process->setProcessEnvironment(state->runtime.pythonEnvironment());
            process->setWorkingDirectory(artifactDirectory);
#ifdef Q_OS_WIN
            process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
            connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) finish(QStringLiteral("Output probe Python failed to start"));
            });
            connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [state, process, artifactDirectory, add, finish](int code, QProcess::ExitStatus status) {
                const QByteArray output = process->readAllStandardOutput();
                const QByteArray errors = process->readAllStandardError();
                QSaveFile transcript(QDir(artifactDirectory).filePath(QStringLiteral("probe.log")));
                if (transcript.open(QIODevice::WriteOnly)) { transcript.write(output + errors); transcript.commit(); }
                const QJsonObject probe = QJsonDocument::fromJson(output.trimmed()).object();
                state->evidence.insert(QStringLiteral("outputProbe"), probe);
                add(QStringLiteral("real PyAV output frames CFR audio tail and frozen pixels"),
                    code == 0 && status == QProcess::NormalExit && probe.value(QStringLiteral("passed")).toBool(), probe);
                process->deleteLater();
                state->phase = 6;
                state->phaseElapsed.restart();
            });
            const QString probeCode = QString::fromUtf8(R"py(
import av,json,sys
import numpy as np
from fractions import Fraction
path=sys.argv[1]
with av.open(path) as c:
    v=c.streams.video[0]
    rate=v.average_rate
    count=0; times=[]; maximum=0.0
    for frame in c.decode(v):
        count+=1
        if count>2000: raise RuntimeError('unexpected unbounded frame count')
        times.append(frame.pts*frame.time_base)
        maximum=max(maximum,float(frame.to_ndarray(format='rgb24').mean()))
with av.open(path) as c:
    audio_count=len(c.streams.audio)
    if audio_count!=1: raise RuntimeError('expected selected B audio')
    a=c.streams.audio[0]
    audio_duration=a.duration*a.time_base
    chunks=[f.to_ndarray() for f in c.decode(a)]
    samples=np.concatenate(chunks,axis=1)
head=float(np.abs(samples[:,:3500]).max())
tail=float(np.abs(samples[:,22000:24000]).max())
cfr=times==[Fraction(i,24) for i in range(12)]
ok=count==12 and rate==Fraction(24) and cfr and maximum>3.0 and audio_count==1 and abs(audio_duration-Fraction(1,2))<=Fraction(1,48000) and head<.003 and tail>.015
print(json.dumps({'passed':ok,'frames':count,'fps':str(rate),'cfr':cfr,'maximumFrameMean':maximum,'audioStreams':audio_count,'audioDuration':str(audio_duration),'audioHeadPeak':head,'audioTailPeak':tail,'decodedSamplesIncludingPadding':samples.shape[1],'pyavVersion':av.__version__}))
raise SystemExit(0 if ok else 1)
)py");
            process->start(state->runtime.pythonExecutable,
                {QStringLiteral("-B"), QStringLiteral("-c"), probeCode, outputPath}, QIODevice::ReadOnly);
            state->phase = 5; return;
        }
        if (state->phase == 6) {
            QImage silent(64, 48, QImage::Format_RGB32);
            silent.fill(QColor(30, 70, 180));
            const QString path = QDir(artifactDirectory).filePath(QStringLiteral("silent_b.png"));
            if (!silent.save(path, "PNG")) { finish(QStringLiteral("Could not create deterministic silent B fixture")); return; }
            _p->ocioManager->setExposure(0.F); _p->ocioManager->setGamma(1.F); _p->ocioManager->setEnabled(false);
            playback->setCompareFile(path); playback->setCompareTime(0); _p->compareBar->setCompareMode(5);
            state->evidence.insert(QStringLiteral("silentBFixture"), path);
            state->phase = 7; state->phaseElapsed.restart(); return;
        }
        if (state->phase == 7) {
            if (state->phaseElapsed.elapsed() < 150 || !playback->hasCompare()) return;
            const bool dispatched = _p->commandDispatcher->dispatch(QStringLiteral("export.compareVideo"));
            add(QStringLiteral("silent B opens real export form"), dispatched);
            if (!dispatched) { finish(property("cgplay.compareExportError").toString()); return; }
            state->phase = 8; state->phaseElapsed.restart(); return;
        }
        if (state->phase == 8) {
            auto* dialog = findChild<QDialog*>(QStringLiteral("CompareExportDialog"));
            auto* audio = dialog ? dialog->findChild<QComboBox*>(QStringLiteral("CompareExportAudio")) : nullptr;
            if (!dialog || !audio) { finish(QStringLiteral("Silent B form controls missing")); return; }
            const int bIndex = audio->findData(QStringLiteral("b"));
            const bool disabled = bIndex >= 0 && !(audio->model()->flags(audio->model()->index(bIndex, 0)) & Qt::ItemIsEnabled);
            const QString reason = bIndex >= 0 ? audio->itemData(bIndex, Qt::ToolTipRole).toString() : QString();
            const bool defaultsA = audio->currentData().toString() == QStringLiteral("a");
            audio->setCurrentIndex(audio->findData(QStringLiteral("none")));
            const bool noneAvailable = audio->currentData().toString() == QStringLiteral("none");
            const QJsonObject details{{QStringLiteral("bDisabled"), disabled}, {QStringLiteral("reason"), reason},
                {QStringLiteral("defaultAudioA"), defaultsA}, {QStringLiteral("noneAvailable"), noneAvailable}};
            add(QStringLiteral("missing B audio disabled with explanation and real default"),
                disabled && !reason.trimmed().isEmpty() && defaultsA && noneAvailable, details);
            state->evidence.insert(QStringLiteral("missingAudioControls"), details);
            dialog->reject(); state->phase = 9; state->phaseElapsed.restart(); return;
        }
        if (state->phase == 9 && !property("cgplay.compareExportBusy").toBool()) {
            add(QStringLiteral("canceled configuration releases reentrancy state"), true);
            finish();
        }
    });
    state->timer->start();
}

int Application::_runCompareExportSmoke()
{
    const QString outputPath = _compareExportSmokeOutput.isEmpty()
        ? QDir::current().filePath(QStringLiteral("tests/artifacts/compare_export_20261025/native/report.json"))
        : QFileInfo(_compareExportSmokeOutput).absoluteFilePath();
    const QString artifactDirectory = QFileInfo(outputPath).absolutePath();
    int exitCode = 1;
    if (!_mainWindow) return exitCode;
    _mainWindow->setAttribute(Qt::WA_DontShowOnScreen);
    _mainWindow->resize(1500, 900);
    _mainWindow->show();
    QTimer::singleShot(0, this, [this, outputPath, artifactDirectory, &exitCode] {
        _mainWindow->runCompareExportSmokeChecks(_compareExportSmokeA, _compareExportSmokeB, artifactDirectory,
            [this, outputPath, &exitCode](QJsonObject report) {
            exitCode = application_runtime::writeJsonObjectFile(outputPath, report)
                ? (report.value(QStringLiteral("passed")).toBool() ? 0 : 5) : 4;
            quit();
        });
    });
    const int applicationCode = exec();
    return applicationCode == 0 ? exitCode : applicationCode;
}

} // namespace cgplay
