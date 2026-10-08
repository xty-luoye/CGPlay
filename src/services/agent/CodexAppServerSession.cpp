#include "CodexAppServerSession.h"

#include "common/jobs/JobSystem.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QTimer>

namespace cgplay {
namespace {

constexpr int kMaximumBufferedLineBytes = 4 * 1024 * 1024;

QString processErrorMessage(QProcess::ProcessError error)
{
    switch (error) {
    case QProcess::FailedToStart:
        return QStringLiteral("Codex app-server 启动失败。");
    case QProcess::Crashed:
        return QStringLiteral("Codex app-server 已崩溃。");
    case QProcess::Timedout:
        return QStringLiteral("Codex app-server 进程操作超时。");
    case QProcess::WriteError:
        return QStringLiteral("写入 Codex app-server 失败。");
    case QProcess::ReadError:
        return QStringLiteral("读取 Codex app-server 失败。");
    case QProcess::UnknownError:
        return QStringLiteral("Codex app-server 遇到未知进程错误。");
    }
    return QStringLiteral("Codex app-server 遇到无法识别的进程错误。");
}

} // namespace

CodexAppServerSession::CodexAppServerSession(QObject* parent)
    : QObject(parent)
    , _process(this)
    , _gracefulStopTimer(new QTimer(this))
{
    _process.setProcessChannelMode(QProcess::SeparateChannels);
    _gracefulStopTimer->setSingleShot(true);

    connect(&_process, &QProcess::started, this, &CodexAppServerSession::onProcessStarted);
    connect(&_process,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this,
        &CodexAppServerSession::onProcessFinished);
    connect(&_process,
        &QProcess::errorOccurred,
        this,
        &CodexAppServerSession::onProcessError);
    connect(&_process,
        &QProcess::readyReadStandardOutput,
        this,
        &CodexAppServerSession::onStandardOutputReady);
    connect(&_process,
        &QProcess::readyReadStandardError,
        this,
        &CodexAppServerSession::onStandardErrorReady);
    connect(_gracefulStopTimer,
        &QTimer::timeout,
        this,
        &CodexAppServerSession::onGracefulStopTimedOut);
}

CodexAppServerSession::~CodexAppServerSession()
{
    if (_process.state() != QProcess::NotRunning) {
        _process.kill();
        JobContext shutdownContext(400);
        shutdownContext.waitForProcess(_process, 25, 400, 50);
    }
}

bool CodexAppServerSession::start(const Config& config, QString* error)
{
    if (_process.state() != QProcess::NotRunning) {
        if (error) *error = QStringLiteral("Codex app-server 已在运行或启动中。");
        return false;
    }
    if (!validateConfig(config, error)) return false;

    _standardOutputBuffer.clear();
    _standardErrorBuffer.clear();

    QProcessEnvironment environment = config.environment.isEmpty()
        ? QProcessEnvironment::systemEnvironment()
        : config.environment;
    environment.insert(QStringLiteral("CODEX_HOME"), config.codexHome);
    _process.setProcessEnvironment(environment);
    _config = config;
    _config.environment = QProcessEnvironment();
    environment = QProcessEnvironment();
    _process.setWorkingDirectory(config.workingDirectory.isEmpty()
            ? QFileInfo(config.executablePath).absolutePath()
            : config.workingDirectory);
    _process.setProgram(config.executablePath);
    _process.setArguments({QStringLiteral("app-server"), QStringLiteral("--stdio")});

    setState(State::Starting);
    _process.start();
    return true;
}

bool CodexAppServerSession::sendJsonRpc(const QJsonObject& message, QString* error)
{
    if (_process.state() != QProcess::Running || _state == State::Stopping) {
        if (error) *error = QStringLiteral("Codex app-server 当前不接受请求。");
        return false;
    }

    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (line.isEmpty() || line.contains('\n') || line.contains('\r')) {
        if (error) *error = QStringLiteral("Codex JSON-RPC 消息无法编码为单行 UTF-8 文本。");
        return false;
    }
    line.append('\n');
    if (_process.write(line) != line.size()) {
        if (error) *error = _process.errorString();
        emit sessionError(_process.errorString());
        return false;
    }
    return true;
}

void CodexAppServerSession::stop()
{
    if (_process.state() == QProcess::NotRunning) return;

    setState(State::Stopping);
    _process.closeWriteChannel();
    _gracefulStopTimer->start(qMax(0, _config.gracefulStopTimeoutMs));
}

void CodexAppServerSession::terminate()
{
    if (_process.state() == QProcess::NotRunning) return;
    setState(State::Stopping);
    _gracefulStopTimer->stop();
    _process.terminate();
}

void CodexAppServerSession::kill()
{
    if (_process.state() == QProcess::NotRunning) return;
    setState(State::Stopping);
    _gracefulStopTimer->stop();
    _process.kill();
}

CodexAppServerSession::State CodexAppServerSession::state() const
{
    return _state;
}

bool CodexAppServerSession::isRunning() const
{
    return _process.state() == QProcess::Running;
}

qint64 CodexAppServerSession::processId() const
{
    return _process.processId();
}

CodexAppServerSession::Config CodexAppServerSession::config() const
{
    return _config;
}

void CodexAppServerSession::onProcessStarted()
{
    // The child already inherited its environment; do not retain credentials in QProcess diagnostics state.
    _process.setProcessEnvironment(QProcessEnvironment());
    setState(State::Running);
    emit sessionStarted();
}

void CodexAppServerSession::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    _gracefulStopTimer->stop();
    drainStandardOutput(true);
    drainStandardError(true);
    setState(State::NotRunning);
    emit sessionStopped(exitCode, exitStatus);
}

void CodexAppServerSession::onProcessError(QProcess::ProcessError error)
{
    const QString message = QStringLiteral("%1 %2")
                                .arg(processErrorMessage(error), _process.errorString());
    emit sessionError(message.trimmed());
    if (error == QProcess::FailedToStart) setState(State::NotRunning);
}

void CodexAppServerSession::onStandardOutputReady()
{
    drainStandardOutput();
}

void CodexAppServerSession::onStandardErrorReady()
{
    drainStandardError();
}

void CodexAppServerSession::onGracefulStopTimedOut()
{
    if (_process.state() == QProcess::NotRunning) return;
    emit sessionError(QStringLiteral("关闭标准输入后 Codex app-server 未停止，正在终止进程。"));
    _process.terminate();
}

bool CodexAppServerSession::validateConfig(const Config& config, QString* error)
{
    const QFileInfo executable(config.executablePath);
    if (config.executablePath.trimmed().isEmpty() || !executable.isAbsolute() ||
        !executable.exists() || !executable.isFile()) {
        if (error) *error = QStringLiteral("Codex executablePath 必须是存在的绝对文件路径。");
        return false;
    }

    const QDir codexHome(config.codexHome);
    if (config.codexHome.trimmed().isEmpty() || !codexHome.isAbsolute() || !codexHome.exists()) {
        if (error) *error = QStringLiteral("CODEX_HOME 必须是已显式配置且存在的绝对目录。");
        return false;
    }

    if (!config.workingDirectory.trimmed().isEmpty()) {
        const QDir workingDirectory(config.workingDirectory);
        if (!workingDirectory.isAbsolute() || !workingDirectory.exists()) {
            if (error) *error = QStringLiteral("配置 workingDirectory 时，它必须是存在的绝对目录。");
            return false;
        }
    }

    if (config.gracefulStopTimeoutMs < 0) {
        if (error) *error = QStringLiteral("gracefulStopTimeoutMs 不能为负数。");
        return false;
    }
    return true;
}

void CodexAppServerSession::setState(State state)
{
    if (_state == state) return;
    _state = state;
    emit stateChanged(_state);
}

void CodexAppServerSession::drainStandardOutput(bool flushTail)
{
    _standardOutputBuffer.append(_process.readAllStandardOutput());
    emitLines(&_standardOutputBuffer, false, flushTail);
}

void CodexAppServerSession::drainStandardError(bool flushTail)
{
    _standardErrorBuffer.append(_process.readAllStandardError());
    emitLines(&_standardErrorBuffer, true, flushTail);
}

void CodexAppServerSession::emitLines(QByteArray* buffer, bool standardError, bool flushTail)
{
    while (true) {
        const int newline = buffer->indexOf('\n');
        if (newline < 0) break;

        QByteArray line = buffer->left(newline);
        buffer->remove(0, newline + 1);
        if (line.endsWith('\r')) line.chop(1);
        if (standardError) emit standardErrorLineReceived(line);
        else emit jsonRpcLineReceived(line);
    }

    if (buffer->size() > kMaximumBufferedLineBytes) {
        const QString message = QStringLiteral("Codex app-server 输出的单行超过 %1 字节安全上限。")
                                    .arg(kMaximumBufferedLineBytes);
        buffer->clear();
        emit sessionError(message);
        kill();
        return;
    }

    if (flushTail && !buffer->isEmpty()) {
        const QByteArray tail = *buffer;
        buffer->clear();
        if (standardError) emit standardErrorLineReceived(tail);
        else emit jsonRpcLineReceived(tail);
    }
}

} // namespace cgplay
