#include "Application.h"
#include "ApplicationPrivate.h"
#include "ApplicationRuntimeSupport.h"
#include "recording/RecordingPanel.h"
#include "recording/RecordingService.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace cgplay {
namespace {
int recordingVisibleWindows(bool own)
{
    int count=0;
#ifdef Q_OS_WIN
    struct Context { bool own; int count=0; } context{own};
    EnumWindows([](HWND window,LPARAM pointer)->BOOL {
        auto* context=reinterpret_cast<Context*>(pointer); DWORD pid=0;
        GetWindowThreadProcessId(window,&pid);
        if (IsWindowVisible(window) && (!context->own || pid==GetCurrentProcessId())) ++context->count;
        return TRUE;
    },reinterpret_cast<LPARAM>(&context)); count=context.count;
#endif
    return count;
}
struct RecordingSmokeState {
    int phase=0,visibleMax=0; bool done=false;
    QTimer* timer=nullptr; QElapsedTimer elapsed,phaseElapsed;
    qint64 pausedDuration=0,pauseAckDuration=0,pauseLastDuration=0;
    int pausedEvents=0,pauseAckEvent=0;
    QElapsedTimer pauseStable,pauseDrain;
    QJsonArray checks; QJsonObject evidence;
    QJsonArray timeline;
    QPointer<RecordingPanel> panel; QPointer<RecordingService> service; QPointer<QProcess> probe;
};
} // namespace

void MainWindow::runRecordingSmokeChecks(const QString& directory,std::function<void(QJsonObject)> finished)
{
    QDir().mkpath(directory);
    auto state=std::make_shared<RecordingSmokeState>(); state->elapsed.start(); state->phaseElapsed.start();
    state->timer=new QTimer(this); state->timer->setInterval(30);
    const int platformBefore=recordingVisibleWindows(false);
    const auto add=[state](const QString& name,bool passed,const QJsonObject& details=QJsonObject()) {
        state->checks.append(QJsonObject{{"name",name},{"passed",passed},{"details",details}});
    };
    const auto finish=[this,state,finished,add,platformBefore](const QString& error=QString()) {
        if (state->done) return; state->done=true; state->timer->stop(); state->timer->deleteLater();
        if (state->probe && state->probe->state()!=QProcess::NotRunning) state->probe->kill();
        if (state->service && state->service->isBusy()) state->service->requestStopForClose();
        if (!error.isEmpty()) add(QStringLiteral("recording smoke completed"),false,{{"error",error},{"status",_p->recordingService ? _p->recordingService->status() : QJsonObject()}});
        int failures=0; for (const auto& value : state->checks) if (!value.toObject().value("passed").toBool()) ++failures;
        state->visibleMax=qMax(state->visibleMax,recordingVisibleWindows(true));
        if (state->visibleMax) ++failures;
        state->evidence.insert("elapsedMs",state->elapsed.elapsed());
        state->evidence.insert("timeline",state->timeline);
        finished({{"passed",failures==0},{"backgroundMode",true},{"visibleAutomationWindowsMax",state->visibleMax},
            {"platformVisibleTopLevelWindowsBefore",platformBefore},{"platformVisibleTopLevelWindowsAfter",recordingVisibleWindows(false)},
            {"summary",QJsonObject{{"pass",state->checks.size()-failures},{"fail",failures}}},
            {"checks",state->checks},{"evidence",state->evidence},
            {"environmentItems",QJsonArray{QStringLiteral("Actual screen/window/player WGC capture, DPI/fullscreen resize, HDR and real WASAPI permissions are not exercised by this synthetic smoke.")}}});
    };
    add(QStringLiteral("recording panel and service lazy at startup"),!_p->recordingDock && !_p->recordingService);
    const bool dispatched=_executeCommandId(QStringLiteral("recording.open"));
    add(QStringLiteral("registered recording command dispatched"),dispatched);
    if (!dispatched) { finish(QStringLiteral("Could not open recording panel")); return; }
    state->panel=_p->recordingPanel; state->service=_p->recordingService;
    const auto trace=[state](const QString& action,const QJsonObject& details=QJsonObject()) {
        if (state->timeline.size()>=1024) return;
        state->timeline.append(QJsonObject{{"wallMs",state->elapsed.elapsed()},{"phase",state->phase},{"action",action},
            {"status",state->service ? state->service->status() : QJsonObject()},{"details",details}});
    };
    connect(state->service,&RecordingService::stateChanged,this,[state,trace](const QJsonObject& value) {
        if (value.value("state").toString()==QStringLiteral("Paused")) ++state->pausedEvents;
        trace(QStringLiteral("stateChanged"),value);
    });
    // complete() emits finished before queueing closeReady. Re-enter from that
    // exact gap while the old recording is idle but the window is still closing.
    connect(state->service,&RecordingService::finished,this,[this,state,directory,add](const QJsonObject& result) {
        if (state->done || state->phase!=10 || !_p->recordingClosePending || !state->panel || !state->service) return;
        const auto before=state->service->status(); const auto oldPlan=property("cgplay.recordingLastPlan");
        auto plan=state->panel->currentSettings();
        const QString startPath=QDir(directory).filePath(QStringLiteral("close_reentry_start.mp4"));
        const QString recoverPath=QDir(directory).filePath(QStringLiteral("close_reentry_recover.mp4"));
        plan.insert(QStringLiteral("output"),startPath); state->panel->startRequested(plan);
        const QString startError=property("cgplay.recordingStartError").toString();
        const auto afterStart=state->service->status();
        state->panel->recoverRequested(result.value("sessionDescriptorPath").toString(),recoverPath);
        const QString recoverError=property("cgplay.recordingStartError").toString();
        const auto afterRecover=state->service->status();
        const bool rejected=!state->service->isBusy() && before.value("sessionId")==afterStart.value("sessionId") &&
            before.value("sessionId")==afterRecover.value("sessionId") && before.value("outputPath")==afterRecover.value("outputPath") &&
            property("cgplay.recordingLastPlan")==oldPlan && startError.contains(QStringLiteral("窗口正在")) &&
            recoverError.contains(QStringLiteral("窗口正在")) && !QFileInfo::exists(startPath) && !QFileInfo::exists(recoverPath);
        add(QStringLiteral("pending close rejects same-window start and recovery reentry"),rejected,
            {{"before",before},{"afterStart",afterStart},{"afterRecovery",afterRecover},{"startError",startError},{"recoveryError",recoverError}});
        state->evidence.insert("closePendingReentryChecked",true);
    });
    add(QStringLiteral("registered recording command and coverage"),_automationCoverage().value("complete").toBool());
    connect(state->timer,&QTimer::timeout,this,[this,state,directory,add,finish,trace] {
        if (state->done) return;
        state->visibleMax=qMax(state->visibleMax,recordingVisibleWindows(true));
        if (state->elapsed.elapsed()>150000) { finish(QStringLiteral("Synthetic UI recording timeout")); return; }
        if (!state->panel || !state->service) { finish(QStringLiteral("Recording UI/service disappeared")); return; }
        auto* panel=state->panel.data(); auto* service=state->service.data();
        const auto button=[panel](const char* name){return panel->findChild<QPushButton*>(QString::fromLatin1(name));};
        const auto combo=[panel](const char* name){return panel->findChild<QComboBox*>(QString::fromLatin1(name));};
        const auto checkbox=[panel](const char* name){return panel->findChild<QCheckBox*>(QString::fromLatin1(name));};
        auto* output=panel->findChild<QLineEdit*>(QStringLiteral("RecordingOutput"));
        const auto status=service->status(); const QString actual=status.value("state").toString();
        if (state->phase==2 || state->phase==3 || state->phase==31) trace(QStringLiteral("pause-ui-sample"),{{"pauseButtonEnabled",button("RecordingPause")->isEnabled()}});
        if (state->phase==0) {
            if (_p->recordingCatalog.isEmpty()) {
                const QString message=panel->findChild<QLabel*>(QStringLiteral("RecordingError"))->text();
                if (!message.isEmpty() && state->phaseElapsed.elapsed()>10000) finish(message);
                return;
            }
            const auto catalog=_p->recordingCatalog;
            if (!catalog.value("error").toString().isEmpty()) { finish(catalog.value("error").toString()); return; }
            if (catalog.value("monitors").toArray().isEmpty() || catalog.value("windows").toArray().isEmpty() ||
                catalog.value("systemAudio").toArray().isEmpty() || catalog.value("microphones").toArray().isEmpty()) { finish(QStringLiteral("Synthetic OBS catalog is incomplete")); return; }
            add(QStringLiteral("actual synthetic OBS properties enumerated without capture"),!service->isBusy() && !catalog.value("runtimeIdentity").toString().isEmpty(),{{"catalog",catalog}});
            add(QStringLiteral("audio disabled by default and explicit output required"),!checkbox("RecordingSystemEnabled")->isChecked() && !checkbox("RecordingMicEnabled")->isChecked() && !button("RecordingStart")->isEnabled());
            const QString outputPath=QDir(directory).filePath(QStringLiteral("recording_ui.mp4"));
            panel->setProperty("cgplay.recordingInjectedOutput",outputPath);button("RecordingBrowse")->click();
            combo("RecordingScope")->setCurrentIndex(combo("RecordingScope")->findData(QStringLiteral("window")));
            combo("RecordingSource")->setCurrentIndex(0);
            add(QStringLiteral("window source uses exact catalog plan"),panel->currentSettings().value("windowToken")==catalog.value("windows").toArray().at(0).toObject().value("plan").toObject().value("windowToken"));
            combo("RecordingScope")->setCurrentIndex(combo("RecordingScope")->findData(QStringLiteral("monitor")));combo("RecordingSource")->setCurrentIndex(0);
            combo("RecordingResolution")->setCurrentIndex(0);combo("RecordingFps")->setCurrentIndex(1);
            add(QStringLiteral("1080p60 control values"),panel->currentSettings().value("width").toInt()==1920 && panel->currentSettings().value("height").toInt()==1080 && panel->currentSettings().value("fps").toInt()==60);
            combo("RecordingResolution")->setCurrentIndex(1);combo("RecordingFps")->setCurrentIndex(0);
            checkbox("RecordingSystemEnabled")->click();checkbox("RecordingMicEnabled")->click();
            combo("RecordingSystemDevice")->setCurrentIndex(0);combo("RecordingMicDevice")->setCurrentIndex(0);
            panel->findChild<QSlider*>(QStringLiteral("RecordingSystemGain"))->setValue(50);
            panel->findChild<QSlider*>(QStringLiteral("RecordingMicGain"))->setValue(80);
            add(QStringLiteral("720p30 independent device and gain selection"),panel->currentSettings().value("width").toInt()==1280 && panel->currentSettings().value("fps").toInt()==30 && panel->currentSettings().value("systemDeviceId").toString()==combo("RecordingSystemDevice")->currentData().toString() && panel->currentSettings().value("micGain").toDouble()==.8);
            const QString image=QDir(directory).filePath(QStringLiteral("recording_panel.png"));
            add(QStringLiteral("recording panel widget evidence"),panel->grab().save(image,"PNG"),{{"path",image},{"method","QWidget::grab; no desktop capture"}});
            trace(QStringLiteral("start-request-before"),panel->currentSettings());
            button("RecordingStart")->click();button("RecordingStart")->click();trace(QStringLiteral("start-request-after"));
            add(QStringLiteral("start is synthetic and controls prevent reentry"),property("cgplay.recordingLastPlan").toMap().value("source").toString()==QStringLiteral("synthetic") && !button("RecordingStart")->isEnabled() && !combo("RecordingScope")->isEnabled());
            state->phase=1;state->phaseElapsed.restart();return;
        }
        if ((actual==QStringLiteral("Failed") || actual==QStringLiteral("Recoverable")) && state->phase<6) { finish(status.value("error").toString()); return; }
        if (state->phase==1) {
            if (actual!=QStringLiteral("Recording") || status.value("durationNs").toVariant().toLongLong()<600000000) return;
            add(QStringLiteral("real synthetic recording started"),true,{{"status",status}});
            trace(QStringLiteral("pause-request-before"));button("RecordingPause")->click();trace(QStringLiteral("pause-request-after"));state->phase=2;state->phaseElapsed.restart();return;
        }
        if (state->phase==2) {
            if (actual!=QStringLiteral("Paused")) return;
            state->pauseAckDuration=state->pauseLastDuration=status.value("durationNs").toVariant().toLongLong();
            state->pauseAckEvent=state->pausedEvents;state->pauseStable.start();state->pauseDrain.start();
            trace(QStringLiteral("paused-observed"));
            state->phase=3;state->phaseElapsed.restart();return;
        }
        if (state->phase==3) {
            if (actual!=QStringLiteral("Paused")) { finish(QStringLiteral("Paused state ended before resume request"));return; }
            const qint64 current=status.value("durationNs").toVariant().toLongLong();
            if (current!=state->pauseLastDuration) { state->pauseLastDuration=current;state->pauseStable.restart(); }
            if (state->pauseDrain.elapsed()>2500) { finish(QStringLiteral("Paused encoder did not settle within bounded drain"));return; }
            if (state->pauseStable.elapsed()<500 || state->pausedEvents<state->pauseAckEvent+2) return;
            // OBS33 get_closest_v_ts schedules a source timestamp boundary.
            // durationNs counts packets already sent by send_interleaved, so
            // encoder/interleave backlog is not bounded to two output frames.
            // Require bounded settling, live progress samples, then the strict
            // frozen-clock check below; never accept a continuing recording.
            const qint64 inflight=current-state->pauseAckDuration;
            add(QStringLiteral("pause settles encoded output within bounded drain"),inflight>=0,
                {{"durationAtAck",state->pauseAckDuration},{"settledDuration",current},{"inflightNs",inflight},{"settleMs",state->pauseDrain.elapsed()},{"deadlineMs",2500},{"stableMs",state->pauseStable.elapsed()},{"progressSamples",state->pausedEvents-state->pauseAckEvent}});
            state->pausedDuration=current;trace(QStringLiteral("pause-drained"));
            trace(QStringLiteral("gain-request-before"),{{"systemGain",.25}});panel->findChild<QSlider*>(QStringLiteral("RecordingSystemGain"))->setValue(25);trace(QStringLiteral("gain-request-after"));
            trace(QStringLiteral("mic-mute-request-before"),{{"micMute",true}});checkbox("RecordingMicMute")->click();trace(QStringLiteral("mic-mute-request-after"));
            state->phase=31;state->phaseElapsed.restart();return;
        }
        if (state->phase==31) {
            if (state->phaseElapsed.elapsed()<300) return;
            add(QStringLiteral("pause freezes recorded clock and resume is available"),actual==QStringLiteral("Paused") && status.value("durationNs").toVariant().toLongLong()==state->pausedDuration && button("RecordingPause")->isEnabled(),
                {{"durationAtAck",state->pauseAckDuration},{"settledDuration",state->pausedDuration},{"durationAfterWait",status.value("durationNs")},{"waitMs",state->phaseElapsed.elapsed()},{"state",actual},{"pauseButtonEnabled",button("RecordingPause")->isEnabled()}});
            trace(QStringLiteral("resume-request-before"));button("RecordingPause")->click();trace(QStringLiteral("resume-request-after"));state->phase=4;state->phaseElapsed.restart();return;
        }
        if (state->phase==4) {
            if (actual!=QStringLiteral("Recording") || status.value("durationNs").toVariant().toLongLong()<state->pausedDuration+600000000) return;
            add(QStringLiteral("resume advances recorded clock"),true);trace(QStringLiteral("stop-request-before"));button("RecordingStop")->click();button("RecordingStop")->click();trace(QStringLiteral("stop-request-after"));
            add(QStringLiteral("stop and finalizing prevent new start"),!button("RecordingStart")->isEnabled());
            state->phase=5;state->phaseElapsed.restart();return;
        }
        if (state->phase==5) {
            if (service->isBusy()) return;
            if (actual!=QStringLiteral("Completed")) { finish(status.value("error").toString());return; }
            state->evidence.insert("recordingStatus",status);
            const QString path=status.value("outputPath").toString();
            add(QStringLiteral("normal stop publishes new MP4 and preserves recovery descriptor"),QFileInfo(path).isFile() && QFileInfo(status.value("sessionDescriptorPath").toString()).isFile());
            auto* process=new QProcess(this);state->probe=process;process->setProcessEnvironment(_p->recordingRuntime->pythonEnvironment);
            connect(process,&QProcess::errorOccurred,this,[state,process,finish](QProcess::ProcessError error){
                if (error==QProcess::FailedToStart) { state->probe.clear();process->deleteLater();finish(QStringLiteral("Recording probe failed to start")); }
            });
#ifdef Q_OS_WIN
            process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args){args->flags|=CREATE_NO_WINDOW;});
#endif
            connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,state,process,add,finish](int code,QProcess::ExitStatus exit){
                const auto report=QJsonDocument::fromJson(process->readAllStandardOutput().trimmed()).object();
                const QString error=QString::fromUtf8(process->readAllStandardError());process->deleteLater();state->probe.clear();
                add(QStringLiteral("real MP4 frames CFR and audio gain mute"),code==0 && exit==QProcess::NormalExit && report.value("passed").toBool(),{{"probe",report},{"error",error}});
                add(QStringLiteral("full stable-region synthetic audio continuity"),report.value("audioContinuityPassed").toBool() &&
                    QFileInfo(report.value("audioMeasurementFile").toString()).isFile(),
                    {{"measurement",report.value("audioMeasurement")},{"path",report.value("audioMeasurementFile")}});
                state->evidence.insert("mediaProbe",report);
                if (code || !report.value("passed").toBool()) { finish(QStringLiteral("Recorded media probe failed"));return; }
                state->phase=7;state->phaseElapsed.restart();
            });
            const QString code=QStringLiteral(R"py(
import av,json,sys,numpy as np
from fractions import Fraction
from fractions import Fraction as F
from pathlib import Path
import math
def runs(mask):
    edges = np.diff(np.r_[False, mask, False].astype(np.int8))
    return list(zip(np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)))


def tone_rms_bound(amplitude, frequency, seconds, minimum):
    omega_window = 2 * math.pi * frequency * seconds
    term = abs(math.sin(omega_window)) / (2 * omega_window)
    return amplitude * math.sqrt(.5 - term if minimum else .5 + term)


def synthetic_audio_measurement(path, source_path=None, pause_ns=None):
    path = str(path)
    with av.open(str(source_path or path)) as container:
        source_tick = F(container.streams.audio[0].time_base)
    with av.open(path) as container:
        video = list(container.decode(video=0))
        origin = video[0].pts * video[0].time_base
        video_end = video[-1].pts * video[-1].time_base + F(1, 30)
    with av.open(path) as container:
        stream = container.streams.audio[0]
        rate = stream.codec_context.sample_rate
        frames = list(container.decode(stream))
    block = max(frame.samples for frame in frames)
    sample_tick = max(1, math.ceil(source_tick * rate))
    placed, issues, jitter = [], [], []
    expected = None
    for frame in frames:
        raw = round((frame.pts * frame.time_base - origin) * rate)
        samples = frame.to_ndarray().mean(axis=0)
        if expected is None:
            start = raw
        elif abs(raw - expected) <= sample_tick+1:
            # Preserve every decoded sample: do not insert/delete +/-16 samples
            # for each 1024-sample AAC block quantized to a Matroska 1ms tick.
            start = expected
            jitter.append(raw - expected)
        else:
            start = raw
            issues.append(dict(expectedSample=expected, rawSample=raw, delta=raw-expected))
        placed.append((start, samples))
        expected = start + len(samples)
    audio_first = min(start for start, samples in placed)
    audio_end = max(start + len(samples) for start, samples in placed)
    count = max(audio_end, round((video_end-origin)*rate))
    pcm = np.zeros(count, dtype=np.float32)
    coverage = np.zeros(count, dtype=np.int16)
    for start, samples in placed:
        left, right = max(0, start), min(count, start+len(samples))
        if right > left:
            pcm[left:right] = samples[left-start:right-start]
            coverage[left:right] += 1
    # Explicit AAC priming/final-block boundary exclusions, never arbitrary
    # interior windows. The pause index comes from the observed drained counter.
    stable_first = max(0, audio_first) + block
    stable_last = min(audio_end, round((video_end-origin)*rate)) - block
    boundary = None if pause_ns is None else round(F(pause_ns, 1000000000)*rate)
    segments = [("before", stable_first, stable_last)] if boundary is None else [
        ("before", stable_first, min(stable_last, boundary-block)),
        ("after", max(stable_first, boundary+block), stable_last)]
    window = sample_tick
    seconds = window/rate
    # Known fixture amplitudes: .1 peak * UI gains .5/.8 before, .25/muted after.
    before_floor = max(0., tone_rms_bound(.08, 880, seconds, True)-tone_rms_bound(.05, 440, seconds, False))
    after_floor = tone_rms_bound(.025, 440, seconds, True)
    threshold = .25 * min(before_floor, after_floor)
    if threshold <= 0:
        raise ValueError("Cannot derive a positive known-tone RMS bound")
    energy = np.sqrt(np.convolve(pcm.astype(np.float64)**2, np.ones(window)/window, mode="valid"))
    reports = []
    for label, first, last in segments:
        if last-first < 4*round(.02*rate):
            raise ValueError("Stable fixture interval is too short")
        low = []
        for begin, end in runs(energy[first:last-window+1] < threshold):
            if end-begin+window-1 > sample_tick:
                low.append(dict(startSample=int(first+begin), endSample=int(first+end+window-1),
                                durationSeconds=(end-begin+window-1)/rate,
                                minimumRms=float(energy[first+begin:first+end].min())))
        holes = [(int(first+a),int(first+b)) for a,b in runs(coverage[first:last] == 0) if b-a>sample_tick]
        overlaps = [(int(first+a),int(first+b)) for a,b in runs(coverage[first:last] > 1) if b-a>sample_tick]
        local_issues = [item for item in issues if first <= min(item["expectedSample"],item["rawSample"]) < last]
        size = round(.02*rate)
        t = np.arange(size)/rate
        basis = np.stack([np.sin(2*np.pi*440*t),np.cos(2*np.pi*440*t),
                          np.sin(2*np.pi*880*t),np.cos(2*np.pi*880*t),np.ones(size)],axis=1)
        amplitudes = []
        for begin in range(first,last-size+1,round(.005*rate)):
            values = np.linalg.lstsq(basis,pcm[begin:begin+size],rcond=None)[0]
            amplitudes.append(dict(startSample=begin, endSample=begin+size,
                                   amplitude440=float(np.hypot(*values[:2])),amplitude880=float(np.hypot(*values[2:4]))))
        expected440,expected880=(.05,.08) if label=="before" else (.025,0.)
        tone_dropouts=[item for item in amplitudes if item["amplitude440"]<.5*expected440 or
                       (expected880>0 and item["amplitude880"]<.5*expected880)]
        reports.append(dict(segment=label,firstSample=first,lastSample=last,
                            lowEnergyRuns=low,timestampHoles=holes,timestampOverlaps=overlaps,
                            timestampIssues=local_issues,allToneWindows=amplitudes,
                            toneDropouts=tone_dropouts,toneDropoutFraction=.5,
                            median440=float(np.median([x["amplitude440"] for x in amplitudes])),
                            median880=float(np.median([x["amplitude880"] for x in amplitudes])),
                            passed=not low and not holes and not overlaps and not local_issues and not tone_dropouts))
    return dict(audioContinuityPassed=all(x["passed"] for x in reports),rate=rate,sourceTimeBase=str(source_tick),
                sourceTickSamples=sample_tick,aacBlockSamples=block,observedPauseSample=boundary,
                stableRegions=reports,rmsWindowSamples=window,derivedRmsThreshold=threshold,
                thresholdBasis=dict(beforeTheoreticalFloor=before_floor,afterTheoreticalFloor=after_floor,safetyFraction=.25),
                timestampJitterMaxSamples=max(map(abs,jitter),default=0),
                timestampRoundingSlackSamples=1,
                boundaryExclusions=dict(startSamples=block,endSamples=block,pauseEachSideSamples=block),
                audioVideoTailDifferenceSeconds=float(F(audio_end,rate)-(video_end-origin)))

with av.open(sys.argv[1]) as c:
 s=c.streams.video[0];rate=s.average_rate;base_rate=s.base_rate;guessed_rate=s.guessed_rate;nominal=base_rate or guessed_rate;size=(s.width,s.height);video_tb=str(s.time_base);stream_duration=None if s.duration is None else s.duration*s.time_base;frames=list(c.decode(s));times=[f.pts*f.time_base for f in frames]
with av.open(sys.argv[2]) as c:source_tick=c.streams.video[0].time_base
with av.open(sys.argv[1]) as c:
 s=c.streams.audio[0];sample_rate=s.codec_context.sample_rate;af=list(c.decode(s));chunks=[f.to_ndarray() for f in af];sound=np.concatenate(chunks,axis=1).mean(axis=0);audio_frames=[dict(pts=str(f.pts*f.time_base),samples=f.samples) for f in af[:16]]
with av.open(sys.argv[1]) as c:
 packets=[]
 for p in c.demux(c.streams.audio[0]):
  if p.pts is not None:packets.append(dict(pts=p.pts,dts=p.dts,duration=p.duration,timeBase=str(p.time_base)))
  if len(packets)>=16:break
duration=float(times[-1]+Fraction(1,30))
measurement=synthetic_audio_measurement(sys.argv[1],sys.argv[2],int(sys.argv[3]))
measurement_path=Path(sys.argv[1]).with_suffix('.audio_measurement.json')
measurement_path.write_text(json.dumps(measurement,indent=2),encoding='utf-8')
before,after=measurement['stableRegions']
head440=before['median440'];head880=before['median880'];tail440=after['median440'];tail880=after['median880']
continuity=measurement['audioContinuityPassed']
measurement_summary={key:value for key,value in measurement.items() if key!='stableRegions'}
measurement_summary['stableRegions']=[{key:value for key,value in item.items() if key!='allToneWindows'} for item in measurement['stableRegions']]
grid_error=max(abs(t-times[0]-Fraction(i,30)) for i,t in enumerate(times));tail_error=abs(times[-1]-times[0]+Fraction(1,30)-Fraction(len(frames),30));container_error=None if stream_duration is None else abs(stream_duration-Fraction(len(frames),30))
cfr=nominal==Fraction(30) and times[0]>=0 and all(b>a for a,b in zip(times,times[1:])) and grid_error<=source_tick and tail_error<=source_tick and container_error is not None and container_error<=source_tick
ok=len(frames)>=24 and size==(1280,720) and cfr and continuity and sample_rate==48000 and head440>.005 and head880>.005 and .25*head440<tail440<.8*head440 and tail880<.15*head880
print(json.dumps(dict(passed=ok,frames=len(frames),fps=str(nominal),averageFps=str(rate),baseRate=str(base_rate),guessedRate=str(guessed_rate),width=size[0],height=size[1],cfr=cfr,duration=duration,sampleRate=sample_rate,head440=head440,head880=head880,tail440=tail440,tail880=tail880,videoTimeBase=video_tb,sourceTimeBase=str(source_tick),firstVideoPts=list(map(str,times[:16])),videoOriginGridMaxError=float(grid_error),tailGridError=float(tail_error),containerDurationError=None if container_error is None else float(container_error),audioFirstFrames=audio_frames,audioFirstPackets=packets,audioContinuityPassed=continuity,audioMeasurementFile=str(measurement_path),audioMeasurement=measurement_summary,toneStatistic='median of every 20ms fit at 5ms hop throughout each stable segment')))
raise SystemExit(0 if ok else 1)
)py");
            process->start(_p->recordingRuntime->pythonExecutable,{QStringLiteral("-B"),QStringLiteral("-c"),code,path,status.value("mkvPath").toString(),QString::number(state->pausedDuration)},QIODevice::ReadOnly);
            state->phase=6;state->phaseElapsed.restart();return;
        }
        if (state->phase==7) {
            auto recoveryDisplay=state->evidence.value("recordingStatus").toObject();
            const QString descriptor=recoveryDisplay.value("sessionDescriptorPath").toString();
            const QString mkv=recoveryDisplay.value("mkvPath").toString();
            // UI-only failure-state fixture uses this run's actual validated
            // descriptor/MKV. The following recovery still runs the real service.
            recoveryDisplay.insert("state",QStringLiteral("Recoverable"));
            panel->setServiceState(recoveryDisplay);
            auto* label=panel->findChild<QLabel*>(QStringLiteral("RecordingStatus"));
            add(QStringLiteral("recovery paths are discoverable and copyable"),label->text().contains(descriptor) && label->text().contains(mkv) &&
                label->textFormat()==Qt::PlainText && label->textInteractionFlags().testFlag(Qt::TextSelectableByMouse) &&
                label->textInteractionFlags().testFlag(Qt::TextSelectableByKeyboard) && button("RecordingRecover")->toolTip().contains(descriptor) && !service->isBusy(),
                {{"displayFixtureOnly",true},{"sessionDescriptorPath",descriptor},{"mkvPath",mkv},{"displayText",label->text()}});
            output->setText(QDir(directory).filePath(QStringLiteral("recovered_ui.mp4")));
            panel->setProperty("cgplay.recordingInjectedRecovery",descriptor);button("RecordingRecover")->click();
            add(QStringLiteral("recovery picker defaults to displayed descriptor"),panel->property("cgplay.recordingRecoveryDialogInitial").toString()==descriptor);
            state->phase=8;state->phaseElapsed.restart();return;
        }
        if (state->phase==8) {
            if (service->isBusy()) return;
            if (actual!=QStringLiteral("Completed")) { finish(status.value("error").toString());return; }
            add(QStringLiteral("real recovery button publishes second new MP4"),QFileInfo(QDir(directory).filePath(QStringLiteral("recovered_ui.mp4"))).isFile() && QFileInfo(QDir(directory).filePath(QStringLiteral("recording_ui.mp4"))).isFile(),{{"status",status}});
            checkbox("RecordingMicMute")->setChecked(false);output->setText(QDir(directory).filePath(QStringLiteral("close_ui.mp4")));
            button("RecordingStart")->click();state->phase=9;state->phaseElapsed.restart();return;
        }
        if (state->phase==9) {
            if (actual!=QStringLiteral("Recording") || status.value("durationNs").toVariant().toLongLong()<300000000) return;
            const bool accepted=close();
            add(QStringLiteral("window close defers teardown while recording"),!accepted && isVisible() && _p->recordingClosePending);
            state->phase=10;state->phaseElapsed.restart();return;
        }
        if (state->phase==10 && !service->isBusy() && !isVisible()) {
            add(QStringLiteral("pending-close completion hook was exercised"),state->evidence.value("closePendingReentryChecked").toBool());
            add(QStringLiteral("close completes after recorder finalization"),actual==QStringLiteral("Completed") && QFileInfo(QDir(directory).filePath(QStringLiteral("close_ui.mp4"))).isFile(),{{"status",status}});
            finish();
        }
    });
    state->timer->start();
}

int Application::_runRecordingSmoke()
{
    const QString output=_recordingSmokeOutput.isEmpty()
        ? QDir::current().filePath(QStringLiteral("tests/artifacts/migration_1_0_8_20261025/recording_ui_validation/report.json"))
        : QFileInfo(_recordingSmokeOutput).absoluteFilePath();
    if (!_backgroundAutomationMode || !_mainWindow) {
        application_runtime::writeJsonObjectFile(output,{{"passed",false},{"error","Recording smoke requires background automation"}});return 2;
    }
    setProperty("cgplay.recordingSmoke",true);setProperty("cgplay.recordingSmokeRuntime",_recordingSmokeRuntime);
    const bool oldQuit=quitOnLastWindowClosed();setQuitOnLastWindowClosed(false);
    _mainWindow->setAttribute(Qt::WA_DontShowOnScreen);_mainWindow->resize(1500,950);_mainWindow->show();
    int code=1;
    QTimer::singleShot(0,this,[this,output,oldQuit,&code]{
        _mainWindow->runRecordingSmokeChecks(QFileInfo(output).absolutePath(),[this,output,oldQuit,&code](QJsonObject report){
            code=application_runtime::writeJsonObjectFile(output,report) ? (report.value("passed").toBool()?0:5) : 4;
            setQuitOnLastWindowClosed(oldQuit);quit();
        });
    });
    const int eventCode=exec();return eventCode==0?code:eventCode;
}

} // namespace cgplay
