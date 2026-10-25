#include "RecordingService.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace cgplay {
namespace {
QVector<QPointer<RecordingService>> services;
struct ActorJob {
#ifdef Q_OS_WIN
    HANDLE handle = nullptr;
    ~ActorJob() { close(); }
    void close() { if (handle) { CloseHandle(handle); handle = nullptr; } }
    bool attach(qint64 pid) {
        handle = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        bool ok = handle && child && SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) && AssignProcessToJobObject(handle, child);
        if (child) CloseHandle(child);
        return ok;
    }
#else
    void close() {}
    bool attach(qint64) { return true; }
#endif
};
void ownActor(QProcess* child, QObject* receiver) {
    auto job = std::make_shared<ActorJob>();
    QObject::connect(child, &QProcess::started, receiver, [child, job] {
        if (!job->attach(child->processId())) { child->setProperty("cgplay.recordingJobError", true); child->kill(); }
    });
    QObject::connect(child, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), receiver, [job](int, QProcess::ExitStatus) { job->close(); });
}
bool testGate() {
    auto* app = QCoreApplication::instance();
    return app && app->property("cgplay.automationBackground").toBool() && app->property("cgplay.recordingSmoke").toBool();
}
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool runtimeValid(const RecordingRuntimeSnapshot& runtime, QString* error) {
    const QString expected = QDir(runtime.recorderRoot).absoluteFilePath(QStringLiteral("CGPlayRecorder.exe"));
    if (runtime.recorderRoot.isEmpty() || QDir::cleanPath(runtime.helperExecutable).compare(QDir::cleanPath(expected), Qt::CaseInsensitive) ||
        !QFileInfo(runtime.helperExecutable).isFile() || !QFileInfo(runtime.pythonExecutable).isFile() ||
        !QFileInfo(runtime.finalizerScript).isFile() || !QFileInfo(runtime.ffmpegExecutable).isFile() || !QFileInfo(runtime.ffprobeExecutable).isFile()) {
        if (error) *error = QStringLiteral("录屏组件或保存工具不完整，请安装录屏运行组件。");
        return false;
    }
    return true;
}
void appendBounded(QByteArray& target, const QByteArray& data, int limit = 65536) {
    target += data;
    if (target.size() > limit) target = target.right(limit);
}
}

struct RecordingService::Private {
    RecordingService* q;
    RecordingRuntimeSnapshot runtime;
    QProcess* process = nullptr;
    QProcess* discovery = nullptr;
    QTimer timer;
    QElapsedTimer phaseClock, heartbeat, stopClock;
    QString role, state = QStringLiteral("Idle"), id, finalPath, mkvPath, descriptor, error, nativeState;
    QString requestPath;
    QJsonObject plan, result;
    QStringList protectedMedia;
    QByteArray stdoutBuffer, stderrBuffer;
    qint64 sequence = 0, lastNativeSeq = 0, duration = 0, bytes = 0;
    bool busy = false, paused = false, closePending = false, closeSignalScheduled = false, shutdown = false, synthetic = false, stopRequested = false;

    explicit Private(RecordingService* owner) : q(owner), timer(owner) {
        timer.setInterval(100);
        QObject::connect(&timer, &QTimer::timeout, q, [this] {
            if (!process) return;
            const int stopBudget = testGate() ? QCoreApplication::instance()->property("cgplay.recordingHelperStopBudgetMs").toInt() : 0;
            if (role == QStringLiteral("helper") && stopRequested && stopClock.isValid() && stopClock.elapsed() > (stopBudget > 0 ? stopBudget : 35000)) { error = QStringLiteral("录屏停止超时，已保留可恢复录像。"); process->kill(); }
            else if (role == QStringLiteral("helper") && heartbeat.isValid() && heartbeat.elapsed() > 10000 && !stopRequested) requestStop(QStringLiteral("录屏进程失去响应，正在保留录像。"));
            else if (role != QStringLiteral("helper") && phaseClock.elapsed() > 600000) { error = QStringLiteral("录像处理超时，原始录像已保留。"); process->kill(); }
        });
    }
    QJsonObject status() const {
        return {{"schema", 1}, {"state", state}, {"busy", busy}, {"paused", paused},
                {"canPause", role == QStringLiteral("helper") && !stopRequested && (state == QStringLiteral("Recording") || state == QStringLiteral("Paused"))},
                {"sessionId", id}, {"durationNs", duration}, {"bytes", bytes}, {"error", error},
                {"outputPath", finalPath}, {"mkvPath", mkvPath}, {"sessionDescriptorPath", descriptor}, {"closePending", closePending}};
    }
    void update(const QString& next = {}) {
        if (!next.isEmpty()) state = next;
        Q_EMIT q->stateChanged(status());
    }
    void closeIfReady() {
        if (!busy && closePending && !closeSignalScheduled) {
            closeSignalScheduled = true;
            QTimer::singleShot(0, q, [this] { closeSignalScheduled = false; if (!busy && closePending) { closePending = false; Q_EMIT q->closeReady(); } });
        }
    }
    void complete(const QString& next, const QString& failure = {}) {
        timer.stop(); busy = false; paused = false; stopRequested = false;
        if (!failure.isEmpty()) error = failure;
        update(next);
        Q_EMIT q->finished(status());
        closeIfReady();
    }
    QJsonArray inputs() const { QJsonArray values; for (const auto& path : protectedMedia) values.append(path); return values; }
    bool send(const QString& command, const QJsonObject& values = {}) {
        if (!process || role != QStringLiteral("helper") || process->state() != QProcess::Running) return false;
        QJsonObject object{{"protocol", 1}, {"session", id}, {"seq", ++sequence}, {"command", command}};
        for (auto it = values.begin(); it != values.end(); ++it) object.insert(it.key(), it.value());
        const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
        if (line.size() > 65536 || process->bytesToWrite() + line.size() > 65536 || process->write(line) != line.size()) {
            error = QStringLiteral("录屏控制管道拥塞，原始录像将保留。"); process->kill(); return false;
        }
        return true;
    }
    void requestStop(const QString& reason = {}) {
        if (!busy || stopRequested) return;
        if (role == QStringLiteral("finalize") || role == QStringLiteral("recover")) return;
        if (!reason.isEmpty()) error = reason;
        stopRequested = true; stopClock.start();
        if (role == QStringLiteral("helper")) { send(QStringLiteral("stop")); update(QStringLiteral("Stopping")); }
        else {
            if (!requestPath.isEmpty()) { QFile cancel(QFileInfo(requestPath).absolutePath() + QLatin1Char('/') + QFileInfo(requestPath).completeBaseName() + QStringLiteral(".cancel")); if (cancel.open(QIODevice::WriteOnly)) cancel.write("cancel"); }
            if (process) process->kill();
        }
    }
    bool writeRequest(QJsonObject object) {
        QString base;
        if (testGate()) base = QCoreApplication::instance()->property("cgplay.recordingArtifactRoot").toString();
        if (base.isEmpty() && testGate()) base = QFileInfo(finalPath).absolutePath();
        if (base.isEmpty()) base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        QTemporaryDir directory(QDir(base).filePath(QStringLiteral("cgplay-recording-control-XXXXXX")));
        if (!directory.isValid()) { error = QStringLiteral("无法建立录像控制目录。"); return false; }
        directory.setAutoRemove(false);
        requestPath = QDir(directory.path()).filePath(QStringLiteral("request.json"));
        QSaveFile file(requestPath); file.setDirectWriteFallback(false);
        const auto data = QJsonDocument(object).toJson(QJsonDocument::Compact);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) { error = QStringLiteral("无法保存录像处理请求。"); return false; }
        return true;
    }
    void launchPython(const QString& operation) {
        QJsonObject request{{"schema", 1}, {"operation", operation}, {"sessionId", id}, {"sessionDirectory", QFileInfo(mkvPath).absolutePath()},
                            {"sessionDescriptorPath", descriptor}, {"finalOutputPath", finalPath}, {"protectedMediaPaths", inputs()},
                            {"helperPlan", plan}, {"ffmpeg", runtime.ffmpegExecutable}, {"ffprobe", runtime.ffprobeExecutable}, {"helperExited", true}};
        if (!writeRequest(request)) { complete(mkvPath.isEmpty() ? QStringLiteral("Failed") : QStringLiteral("Recoverable"), error); return; }
        launch(operation, runtime.pythonExecutable, {QStringLiteral("-B"), runtime.finalizerScript, QStringLiteral("--request"), requestPath});
    }
    void launch(const QString& nextRole, const QString& executable, const QStringList& arguments) {
        if (nextRole == QStringLiteral("finalize") || nextRole == QStringLiteral("recover")) { stopRequested = false; stopClock.invalidate(); }
        role = nextRole; result = {}; stdoutBuffer.clear(); stderrBuffer.clear(); phaseClock.start(); heartbeat.start();
        auto* child = new QProcess(q); process = child;
        ownActor(child, q);
        child->setProcessChannelMode(QProcess::SeparateChannels);
        QProcessEnvironment environment = runtime.pythonEnvironment.isEmpty() ? QProcessEnvironment::systemEnvironment() : runtime.pythonEnvironment;
        if (role != QStringLiteral("helper")) {
            environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
            if (!runtime.pythonHome.isEmpty()) environment.insert(QStringLiteral("PYTHONHOME"), runtime.pythonHome);
            if (!runtime.toolsRoot.isEmpty()) environment.insert(QStringLiteral("PYTHONPATH"), runtime.toolsRoot);
        }
        child->setProcessEnvironment(environment);
#ifdef Q_OS_WIN
        child->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= 0x08000000; });
#endif
        QObject::connect(child, &QProcess::readyReadStandardError, q, [this, child] { if (process == child) appendBounded(stderrBuffer, child->readAllStandardError()); });
        QObject::connect(child, &QProcess::readyReadStandardOutput, q, [this, child] { if (process == child) consume(child->readAllStandardOutput()); });
        QObject::connect(child, &QProcess::errorOccurred, q, [this, child](QProcess::ProcessError code) {
            if (process == child && code == QProcess::FailedToStart) { process = nullptr; child->deleteLater(); complete(QStringLiteral("Failed"), QStringLiteral("无法启动录屏运行组件。")); }
        });
        QObject::connect(child, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), q, [this, child](int code, QProcess::ExitStatus exitStatus) {
            if (process != child) { child->deleteLater(); return; }
            consume(child->readAllStandardOutput()); appendBounded(stderrBuffer, child->readAllStandardError());
            const QString completedRole = role;
            process = nullptr; child->deleteLater();
            if (shutdown) { complete(QStringLiteral("Recoverable"), QStringLiteral("程序退出，录像已保留以供恢复。")); return; }
            if (completedRole == QStringLiteral("helper")) {
                if (!stopRequested && code == 0 && nativeState != QStringLiteral("Completed")) error = QStringLiteral("录屏进程意外结束。");
                if (code == 0 && exitStatus == QProcess::NormalExit && nativeState == QStringLiteral("Completed")) { error.clear(); update(QStringLiteral("Finalizing")); launchPython(QStringLiteral("finalize")); }
                else complete((bytes > 0 || duration > 0) ? QStringLiteral("Recoverable") : QStringLiteral("Failed"), error.isEmpty() ? QStringLiteral("录屏中断，已保留 MKV，请恢复到新的 MP4 文件。") : error);
            } else if (code == 0 && exitStatus == QProcess::NormalExit && result.value("ok").isBool() && result.value("ok").toBool()) {
                if (completedRole == QStringLiteral("prepare")) {
                    descriptor = result.value("sessionDescriptorPath").toString(); mkvPath = result.value("mkvPath").toString();
                    if (stopRequested) { complete(QStringLiteral("Failed"), QStringLiteral("录屏已取消。")); return; }
                    plan.insert(QStringLiteral("output"), mkvPath); nativeState.clear(); lastNativeSeq = 0; sequence = 0;
                    launch(QStringLiteral("helper"), runtime.helperExecutable, synthetic ? QStringList{QStringLiteral("--synthetic")} : QStringList{});
                } else {
                    finalPath = result.value("outputPath").toString(finalPath); mkvPath = result.value("mkvPath").toString(mkvPath);
                    descriptor = result.value("sessionDescriptorPath").toString(descriptor); id = result.value("sessionId").toString(id);
                    if (result.contains("durationSeconds")) duration = static_cast<qint64>(result.value("durationSeconds").toDouble() * 1000000000.0);
                    if (result.contains("outputBytes")) bytes = result.value("outputBytes").toVariant().toLongLong();
                    error.clear(); complete(QStringLiteral("Completed"));
                }
            } else {
                if (!result.value("mkvPath").toString().isEmpty()) mkvPath = result.value("mkvPath").toString();
                complete((bytes > 0 || duration > 0 || result.value("mkvRetained").toBool()) ? QStringLiteral("Recoverable") : QStringLiteral("Failed"), !result.value("error").toString().isEmpty() ? result.value("error").toString() : error.isEmpty() ? QStringLiteral("录像处理失败，原始录像已保留。") : error);
            }
        });
        child->start(executable, arguments, QIODevice::ReadWrite); timer.start();
    }
    void consume(const QByteArray& bytesRead) {
        stdoutBuffer += bytesRead;
        if (stdoutBuffer.size() > 65536 && !stdoutBuffer.contains('\n')) { error = QStringLiteral("录屏协议消息超出限制。"); if (process) process->kill(); stdoutBuffer.clear(); return; }
        for (;;) {
            const auto end = stdoutBuffer.indexOf('\n'); if (end < 0) break;
            const auto line = stdoutBuffer.left(end); stdoutBuffer.remove(0, end + 1);
            if (line.size() > 65536) { error = QStringLiteral("录屏协议消息超出限制。"); if (process) process->kill(); return; }
            QJsonParseError parse; const auto document = QJsonDocument::fromJson(line, &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) { error = QStringLiteral("录屏协议数据无效。"); if (process) process->kill(); return; }
            const auto object = document.object();
            if (role == QStringLiteral("helper")) {
                const auto event = object.value("event").toString(); const auto messageSeq = object.value("seq").toVariant().toLongLong();
                if (object.value("protocol").toInt() != 1 || messageSeq <= lastNativeSeq) continue;
                if (event != QStringLiteral("ready") && object.value("session").toString() != id) continue;
                lastNativeSeq = messageSeq; heartbeat.restart();
                if (event == QStringLiteral("ready")) { if (!stopRequested) send(QStringLiteral("start"), {{QStringLiteral("plan"), plan}}); else process->kill(); continue; }
                const QString failure = object.value("error").toString(); if (!failure.isEmpty()) error = failure;
                if (event == QStringLiteral("commandError")) { update(); continue; }
                if (event == QStringLiteral("progress") || event == QStringLiteral("state") || event == QStringLiteral("finalized")) {
                    duration = qMax(duration, object.value("durationNs").toVariant().toLongLong());
                    bytes = qMax(bytes, object.value("bytes").toVariant().toLongLong());
                }
                nativeState = object.value("state").toString(nativeState); paused = nativeState == QStringLiteral("Paused");
                update(nativeState == QStringLiteral("Completed") ? QStringLiteral("Finalizing") : nativeState);
                if (event == QStringLiteral("finalized") && process) process->closeWriteChannel();
            } else {
                if (object.value("schema").toInt() != 1) { error = QStringLiteral("录像保存协议版本无效。"); if (process) process->kill(); return; }
                if (object.value("event").toString() == QStringLiteral("result")) result = object;
                else update(role == QStringLiteral("prepare") ? QStringLiteral("Preparing") : QStringLiteral("Finalizing"));
            }
        }
    }
};

RecordingService::RecordingService(QObject* parent) : QObject(parent), _p(std::make_unique<Private>(this)) { services.push_back(this); }
RecordingService::~RecordingService() {
    _p->shutdown = true; _p->timer.stop();
    for (auto* child : findChildren<QProcess*>(QString{}, Qt::FindDirectChildrenOnly)) if (child) {
        QObject::disconnect(child, nullptr, this, nullptr); child->setParent(nullptr); child->kill();
        QObject::connect(child, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), child, &QObject::deleteLater);
        if (child->state() == QProcess::NotRunning) child->deleteLater();
    }
    services.removeAll(this);
}
bool RecordingService::isBusy() const { return _p->busy; }
QJsonObject RecordingService::status() const { return _p->status(); }
bool RecordingService::hasActiveSessions() {
    for (const auto& service : std::as_const(services)) if (service && service->isBusy()) return true;
    return false;
}
void RecordingService::refreshCatalog(const RecordingRuntimeSnapshot& runtime, bool synthetic) {
    QString failure;
    if (_p->busy || (synthetic && !testGate()) || !runtimeValid(runtime, &failure)) { Q_EMIT catalogReady({{"schema", 1}, {"error", failure.isEmpty() ? QStringLiteral("当前不能刷新录屏设备。") : failure}}); return; }
    if (_p->discovery) _p->discovery->kill();
    auto* child = new QProcess(this); _p->discovery = child;
    ownActor(child, this);
    auto output = std::make_shared<QByteArray>(); auto errors = std::make_shared<QByteArray>();
#ifdef Q_OS_WIN
    child->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= 0x08000000; });
#endif
    QObject::connect(child, &QProcess::readyReadStandardOutput, this, [this, child, output] { output->append(child->readAllStandardOutput()); if (output->size() > 65536) child->kill(); });
    QObject::connect(child, &QProcess::readyReadStandardError, this, [child, errors] { appendBounded(*errors, child->readAllStandardError()); });
    QObject::connect(child, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, child, output, errors](int code, QProcess::ExitStatus exit) {
        child->deleteLater(); if (_p->discovery != child) return; _p->discovery = nullptr;
        output->append(child->readAllStandardOutput()); const auto value = QJsonDocument::fromJson(*output).object();
        if (code == 0 && exit == QProcess::NormalExit && value.value("ok").toBool() && output->size() <= 65536) Q_EMIT catalogReady(value);
        else Q_EMIT catalogReady({{"schema", 1}, {"error", QStringLiteral("录屏设备枚举失败或运行组件不兼容。")}});
        _p->closeIfReady();
    });
    QObject::connect(child, &QProcess::errorOccurred, this, [this, child](QProcess::ProcessError code) { if (code == QProcess::FailedToStart && _p->discovery == child) { _p->discovery = nullptr; child->deleteLater(); Q_EMIT catalogReady({{"schema", 1}, {"error", QStringLiteral("无法启动录屏组件。")}}); _p->closeIfReady(); } });
    QTimer::singleShot(20000, child, [guarded = QPointer<QProcess>(child)] { if (guarded && guarded->state() != QProcess::NotRunning) guarded->kill(); });
    child->start(runtime.helperExecutable, synthetic ? QStringList{QStringLiteral("--enumerate"), QStringLiteral("--synthetic")} : QStringList{QStringLiteral("--enumerate")});
}
bool RecordingService::start(const RecordingStartRequest& request, QString* failure) {
    if (_p->busy || _p->closePending || _p->shutdown || (request.synthetic && !testGate()) || !runtimeValid(request.runtime, failure)) { if (failure && failure->isEmpty()) *failure = QStringLiteral("当前不能开始新的录屏。"); return false; }
    if (!QDir::isAbsolutePath(request.finalOutputPath) || QFileInfo(request.finalOutputPath).suffix().compare(QStringLiteral("mp4"), Qt::CaseInsensitive)) { if (failure) *failure = QStringLiteral("请选择新的 MP4 文件路径。"); return false; }
    _p->runtime = request.runtime; _p->id = newId(); _p->plan = request.helperPlan; _p->plan.remove(QStringLiteral("output"));
    _p->finalPath = request.finalOutputPath; _p->protectedMedia = request.protectedMediaPaths; _p->synthetic = request.synthetic;
    _p->mkvPath = QDir(QFileInfo(_p->finalPath).absolutePath()).filePath(QStringLiteral(".cgplay-recording-%1/capture.recording.%1.mkv").arg(_p->id));
    _p->descriptor = QDir(QFileInfo(_p->mkvPath).absolutePath()).filePath(QStringLiteral("session.json"));
    _p->error.clear(); _p->stopRequested = false; _p->duration = _p->bytes = 0; _p->busy = true;
    _p->update(QStringLiteral("Preparing")); _p->launchPython(QStringLiteral("prepare")); return true;
}
bool RecordingService::recover(const RecordingRecoveryRequest& request, QString* failure) {
    if (_p->busy || _p->closePending || _p->shutdown || !runtimeValid(request.runtime, failure)) { if (failure && failure->isEmpty()) *failure = QStringLiteral("当前不能恢复录像。"); return false; }
    _p->runtime = request.runtime; _p->descriptor = request.sessionDescriptorPath; _p->finalPath = request.finalOutputPath;
    _p->mkvPath.clear(); _p->protectedMedia = request.protectedMediaPaths; _p->id = newId(); _p->plan = {};
    _p->error.clear(); _p->duration = _p->bytes = 0; _p->stopRequested = false; _p->busy = true; _p->update(QStringLiteral("Finalizing")); _p->launchPython(QStringLiteral("recover")); return true;
}
void RecordingService::setPaused(bool value) { if (_p->busy && !_p->stopRequested) _p->send(value ? QStringLiteral("pause") : QStringLiteral("resume")); }
void RecordingService::setAudio(const QJsonObject& values) {
    QJsonObject selected; for (const auto& key : {"systemGain", "micGain", "systemMute", "micMute"}) if (values.contains(key)) selected.insert(key, values.value(key));
    _p->send(QStringLiteral("setAudio"), selected);
}
void RecordingService::setPlayerCrop(const QJsonObject& geometry) {
    QJsonObject selected; for (const auto& key : {"geometryEpoch", "cropLeft", "cropTop", "cropWidth", "cropHeight", "clientWidth", "clientHeight"}) if (geometry.contains(key)) selected.insert(key, geometry.value(key));
    _p->send(QStringLiteral("crop"), selected);
}
void RecordingService::stop() { _p->requestStop(); }
void RecordingService::requestStopForClose() {
    _p->closePending = true;
    if (_p->discovery) _p->discovery->kill();
    if (_p->busy) {
        if (_p->role != QStringLiteral("finalize") && _p->role != QStringLiteral("recover")) _p->requestStop();
    } else _p->closeIfReady();
}
void RecordingService::shutdownAndDrainAll(int timeoutMs) {
    for (const auto& service : std::as_const(services)) if (service) {
        auto& p = *service->_p; p.shutdown = true;
        if (p.discovery) { p.discovery->kill(); p.discovery->waitForFinished(2000); }
        p.requestStop();
        QPointer<QProcess> child = p.process;
        if (child && (p.role == QStringLiteral("finalize") || p.role == QStringLiteral("recover"))) child->kill();
        if (child && !child->waitForFinished(timeoutMs)) { child->kill(); child->waitForFinished(3000); }
    }
}
} // namespace cgplay
