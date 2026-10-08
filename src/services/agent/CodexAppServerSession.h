#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>

class QTimer;

namespace cgplay {

class CodexAppServerSession final : public QObject
{
    Q_OBJECT

public:
    struct Config
    {
        QString executablePath;
        QString codexHome;
        QString workingDirectory;
        int gracefulStopTimeoutMs = 3000;
        QProcessEnvironment environment;
    };

    enum class State
    {
        NotRunning,
        Starting,
        Running,
        Stopping
    };
    Q_ENUM(State)

    explicit CodexAppServerSession(QObject* parent = nullptr);
    ~CodexAppServerSession() override;

    bool start(const Config& config, QString* error = nullptr);
    bool sendJsonRpc(const QJsonObject& message, QString* error = nullptr);

    void stop();
    void terminate();
    void kill();

    State state() const;
    bool isRunning() const;
    qint64 processId() const;
    Config config() const;

signals:
    void stateChanged(cgplay::CodexAppServerSession::State state);
    void sessionStarted();
    void sessionStopped(int exitCode, QProcess::ExitStatus exitStatus);
    void jsonRpcLineReceived(const QByteArray& utf8Line);
    void standardErrorLineReceived(const QByteArray& utf8Line);
    void sessionError(const QString& message);

private slots:
    void onProcessStarted();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void onStandardOutputReady();
    void onStandardErrorReady();
    void onGracefulStopTimedOut();

private:
    static bool validateConfig(const Config& config, QString* error);
    void setState(State state);
    void drainStandardOutput(bool flushTail = false);
    void drainStandardError(bool flushTail = false);
    void emitLines(QByteArray* buffer, bool standardError, bool flushTail);

    QProcess _process;
    QTimer* _gracefulStopTimer = nullptr;
    QByteArray _standardOutputBuffer;
    QByteArray _standardErrorBuffer;
    Config _config;
    State _state = State::NotRunning;
};

} // namespace cgplay
