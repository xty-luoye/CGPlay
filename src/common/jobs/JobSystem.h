#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QVariant>

#include <atomic>
#include <functional>
#include <memory>

namespace cgplay {

enum class JobState
{
    Pending,
    Running,
    Succeeded,
    Failed,
    Canceled,
    TimedOut
};

struct JobOutcome
{
    JobState state = JobState::Failed;
    QVariant value;
    QString errorCode;
    QString errorMessage;

    bool succeeded() const { return state == JobState::Succeeded; }

    static JobOutcome success(const QVariant& value = {});
    static JobOutcome failure(const QString& code, const QString& message);
    static JobOutcome canceled(const QString& message = {});
    static JobOutcome timedOut(const QString& message = {});
};

struct ProcessOutcome
{
    JobState state = JobState::Failed;
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;

    bool succeeded() const
    {
        return state == JobState::Succeeded && exitCode == 0;
    }
};

class JobContext
{
public:
    using ProgressCallback = std::function<void(int, const QString&)>;

    explicit JobContext(int timeoutMs = 0, ProgressCallback progress = {});

    void cancel();
    bool isCancellationRequested() const;
    bool hasTimedOut() const;
    bool shouldStop() const;
    int timeoutMs() const;
    qint64 elapsedMs() const;
    void reportProgress(int percent, const QString& message = {}) const;

    ProcessOutcome waitForProcess(
        QProcess& process,
        int pollIntervalMs = 25,
        int operationTimeoutMs = 0,
        int terminationGraceMs = 750) const;

private:
    int _timeoutMs = 0;
    QElapsedTimer _elapsed;
    std::atomic_bool _cancelRequested{false};
    ProgressCallback _progress;
};

class JobHandle : public QObject
{
    Q_OBJECT
public:
    QString id() const;
    JobState state() const;
    int progress() const;
    QString progressMessage() const;

public Q_SLOTS:
    void cancel();

Q_SIGNALS:
    void stateChanged(cgplay::JobState state);
    void progressChanged(int percent, const QString& message);
    void finished(const cgplay::JobOutcome& outcome);

private:
    friend class JobRunner;
    explicit JobHandle(QString id, QObject* parent = nullptr);
    ~JobHandle() override;
    void setContext(const std::shared_ptr<JobContext>& context);
    void setRunning();
    void updateProgress(int percent, const QString& message);
    void complete(const JobOutcome& outcome);

    QString _id;
    JobState _state = JobState::Pending;
    int _progress = 0;
    QString _progressMessage;
    std::shared_ptr<JobContext> _context;
};

class JobRunner
{
public:
    using Worker = std::function<JobOutcome(JobContext&)>;

    static JobHandle* start(
        const QString& id,
        QObject* owner,
        int timeoutMs,
        Worker worker);
};

} // namespace cgplay

Q_DECLARE_METATYPE(cgplay::JobState)
Q_DECLARE_METATYPE(cgplay::JobOutcome)
