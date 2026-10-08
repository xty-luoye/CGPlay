#include "JobSystem.h"

#include <QFutureWatcher>
#include <QMetaObject>
#include <QtConcurrent>

#include <algorithm>

namespace cgplay {

JobOutcome JobOutcome::success(const QVariant& value)
{
    return {JobState::Succeeded, value, {}, {}};
}

JobOutcome JobOutcome::failure(const QString& code, const QString& message)
{
    return {JobState::Failed, {}, code, message};
}

JobOutcome JobOutcome::canceled(const QString& message)
{
    return {JobState::Canceled, {}, QStringLiteral("canceled"), message};
}

JobOutcome JobOutcome::timedOut(const QString& message)
{
    return {JobState::TimedOut, {}, QStringLiteral("timeout"), message};
}

JobContext::JobContext(int timeoutMs, ProgressCallback progress)
    : _timeoutMs(std::max(0, timeoutMs))
    , _progress(std::move(progress))
{
    _elapsed.start();
}

void JobContext::cancel()
{
    _cancelRequested.store(true, std::memory_order_release);
}

bool JobContext::isCancellationRequested() const
{
    return _cancelRequested.load(std::memory_order_acquire);
}

bool JobContext::hasTimedOut() const
{
    return _timeoutMs > 0 && _elapsed.elapsed() >= _timeoutMs;
}

bool JobContext::shouldStop() const
{
    return isCancellationRequested() || hasTimedOut();
}

int JobContext::timeoutMs() const
{
    return _timeoutMs;
}

qint64 JobContext::elapsedMs() const
{
    return _elapsed.elapsed();
}

void JobContext::reportProgress(int percent, const QString& message) const
{
    if (_progress) {
        _progress(std::clamp(percent, 0, 100), message);
    }
}

ProcessOutcome JobContext::waitForProcess(
    QProcess& process,
    int pollIntervalMs,
    int operationTimeoutMs,
    int terminationGraceMs) const
{
    ProcessOutcome result;
    const int pollMs = std::clamp(pollIntervalMs, 5, 250);
    const int graceMs = std::clamp(terminationGraceMs, 0, 5000);
    QElapsedTimer operationElapsed;
    operationElapsed.start();
    while (process.state() != QProcess::NotRunning) {
        const bool operationTimedOut =
            operationTimeoutMs > 0 && operationElapsed.elapsed() >= operationTimeoutMs;
        if (shouldStop() || operationTimedOut) {
            process.terminate();
            if (graceMs == 0 || !process.waitForFinished(graceMs)) {
                process.kill();
                if (graceMs > 0) process.waitForFinished(graceMs);
            }
            result.state = isCancellationRequested() ? JobState::Canceled : JobState::TimedOut;
            result.standardOutput = process.readAllStandardOutput();
            result.standardError = process.readAllStandardError();
            return result;
        }
        process.waitForFinished(pollMs);
    }

    result.exitCode = process.exitCode();
    result.standardOutput = process.readAllStandardOutput();
    result.standardError = process.readAllStandardError();
    if (process.error() == QProcess::FailedToStart) {
        result.exitCode = -1;
        if (result.standardError.isEmpty()) {
            result.standardError = process.errorString().toLocal8Bit();
        }
        result.state = JobState::Failed;
        return result;
    }
    result.state = process.exitStatus() == QProcess::NormalExit && result.exitCode == 0
        ? JobState::Succeeded
        : JobState::Failed;
    return result;
}

JobHandle::JobHandle(QString id, QObject* parent)
    : QObject(parent)
    , _id(std::move(id))
{
}

JobHandle::~JobHandle()
{
    cancel();
}

QString JobHandle::id() const { return _id; }
JobState JobHandle::state() const { return _state; }
int JobHandle::progress() const { return _progress; }
QString JobHandle::progressMessage() const { return _progressMessage; }

void JobHandle::cancel()
{
    if (_context) {
        _context->cancel();
    }
}

void JobHandle::setContext(const std::shared_ptr<JobContext>& context)
{
    _context = context;
}

void JobHandle::setRunning()
{
    _state = JobState::Running;
    Q_EMIT stateChanged(_state);
}

void JobHandle::updateProgress(int percent, const QString& message)
{
    _progress = std::clamp(percent, 0, 100);
    _progressMessage = message;
    Q_EMIT progressChanged(_progress, _progressMessage);
}

void JobHandle::complete(const JobOutcome& outcome)
{
    _state = outcome.state;
    if (_state == JobState::Succeeded) {
        _progress = 100;
    }
    Q_EMIT stateChanged(_state);
    Q_EMIT finished(outcome);
    _context.reset();
}

JobHandle* JobRunner::start(
    const QString& id,
    QObject* owner,
    int timeoutMs,
    Worker worker)
{
    auto* handle = new JobHandle(id, owner);
    const QPointer<JobHandle> handleGuard(handle);
    auto context = std::make_shared<JobContext>(
        timeoutMs,
        [handleGuard](int percent, const QString& message) {
            if (!handleGuard) {
                return;
            }
            QMetaObject::invokeMethod(
                handleGuard,
                [handleGuard, percent, message]() {
                    if (handleGuard) {
                        handleGuard->updateProgress(percent, message);
                    }
                },
                Qt::QueuedConnection);
        });
    handle->setContext(context);
    handle->setRunning();

    auto* watcher = new QFutureWatcher<JobOutcome>(handle);
    QObject::connect(watcher, &QFutureWatcher<JobOutcome>::finished, handle,
        [handleGuard, watcher]() {
            const JobOutcome outcome = watcher->result();
            watcher->deleteLater();
            if (handleGuard) {
                handleGuard->complete(outcome);
            }
        });
    watcher->setFuture(QtConcurrent::run([context, worker = std::move(worker)]() mutable {
        if (context->isCancellationRequested()) {
            return JobOutcome::canceled();
        }
        try {
            JobOutcome outcome = worker(*context);
            if (context->isCancellationRequested() && outcome.state != JobState::Canceled) {
                return JobOutcome::canceled();
            }
            if (context->hasTimedOut() && outcome.state != JobState::TimedOut) {
                return JobOutcome::timedOut();
            }
            return outcome;
        } catch (const std::exception& error) {
            return JobOutcome::failure(QStringLiteral("exception"), QString::fromUtf8(error.what()));
        } catch (...) {
            return JobOutcome::failure(QStringLiteral("exception"), QStringLiteral("Unknown job exception"));
        }
    }));
    return handle;
}

} // namespace cgplay
