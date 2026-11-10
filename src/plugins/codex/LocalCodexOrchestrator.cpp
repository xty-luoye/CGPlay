#include "LocalCodexOrchestrator.h"

#include "common/jobs/JobSystem.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace cgplay {
namespace {

ProcessOutcome waitForGitProcess(QProcess& process, int timeoutMs)
{
    JobContext context(timeoutMs);
    return context.waitForProcess(process, 25, timeoutMs);
}

QString gitProcessError(
    const QProcess& process,
    const ProcessOutcome& outcome,
    const QString& timeoutMessage)
{
    if (outcome.state == JobState::TimedOut) return timeoutMessage;
    const QString standardError = QString::fromLocal8Bit(outcome.standardError).trimmed();
    if (!standardError.isEmpty()) return standardError;
    const QString processMessage = process.errorString().trimmed();
    if (!processMessage.isEmpty() && processMessage != QStringLiteral("Unknown error")) {
        return processMessage;
    }
    return QStringLiteral("Git process failed with exit code %1.").arg(outcome.exitCode);
}

} // namespace

LocalCodexOrchestrator::LocalCodexOrchestrator(QObject* parent) : QObject(parent) {}

QString LocalCodexOrchestrator::storePath() const
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(root);
    return QDir(root).filePath(QStringLiteral("cgplay_orchestrator_tasks.json"));
}

QJsonArray LocalCodexOrchestrator::readTasks() const
{
    QFile file(storePath());
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isArray() ? document.array() : QJsonArray{};
}

bool LocalCodexOrchestrator::writeTasks(const QJsonArray& tasks) const
{
    QSaveFile file(storePath());
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(tasks).toJson(QJsonDocument::Indented)) >= 0 && file.commit();
}

QJsonArray LocalCodexOrchestrator::tasks() const { return readTasks(); }

void LocalCodexOrchestrator::updateTask(const QString& taskId, const QString& status, const QString& detail)
{
    QJsonArray values = readTasks();
    for (int i = 0; i < values.size(); ++i) {
        QJsonObject task = values.at(i).toObject();
        if (task.value(QStringLiteral("id")).toString() != taskId) continue;
        task.insert(QStringLiteral("status"), status);
        task.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        if (!detail.isEmpty()) task.insert(QStringLiteral("detail"), detail);
        values.replace(i, task);
        writeTasks(values);
        return;
    }
}

QString LocalCodexOrchestrator::approvalPolicy(const QString& mode)
{
    if (mode == QStringLiteral("full")) return QStringLiteral("never");
    return QStringLiteral("on-request");
}

bool LocalCodexOrchestrator::createTask(const QString& objective, int count, const CodexAppServerSession::Config& baseConfig,
    const QString& model, const QString& effort, const QString& approvalMode, QString* error)
{
    if (objective.trimmed().isEmpty() || count < 1 || baseConfig.workingDirectory.isEmpty()) {
        if (error) *error = QStringLiteral("Objective, agent count, and project directory are required.");
        return false;
    }
    const QString taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject task{{QStringLiteral("protocol"), QStringLiteral("cgplay.orchestrator.v1")}, {QStringLiteral("id"), taskId},
        {QStringLiteral("objective"), objective}, {QStringLiteral("status"), QStringLiteral("running")},
        {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {QStringLiteral("projectRoot"), baseConfig.workingDirectory},
        {QStringLiteral("agentCount"), count}, {QStringLiteral("agents"), QJsonArray{}}};
    QJsonArray tasks = readTasks(); tasks.push_back(task);
    if (!writeTasks(tasks)) { if (error) *error = QStringLiteral("Cannot persist local orchestrator task."); return false; }
    for (int i = 0; i < count; ++i) startAgent(taskId, i + 1, objective, baseConfig, model, effort, approvalMode);
    emit event(tr("Local orchestrator task started: %1 (%2 agents)").arg(taskId.left(8)).arg(count));
    return true;
}

void LocalCodexOrchestrator::startAgent(const QString& taskId, int index, const QString& objective, CodexAppServerSession::Config config,
    const QString& model, const QString& effort, const QString& approvalMode)
{
    const QString agentId = QStringLiteral("%1-a%2").arg(taskId.left(8)).arg(index);
    const QStringList roles{QStringLiteral("analyze and plan"), QStringLiteral("implement focused changes"), QStringLiteral("review changes and tests"), QStringLiteral("validate integration and summarize")};
    const QString role = roles.at((index - 1) % roles.size());
    const QString base = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("orchestrator/%1").arg(taskId));
    const QString worktree = QDir(base).filePath(QStringLiteral("worktree-a%1").arg(index));
    const QString home = QDir(base).filePath(QStringLiteral("codex-home-a%1").arg(index));
    const QString branch = QStringLiteral("cgplay/orchestrator/%1-a%2").arg(taskId.left(12)).arg(index);
    QDir().mkpath(base); QDir().mkpath(home);
    QProcess git;
    git.setWorkingDirectory(config.workingDirectory);
    git.start(QStringLiteral("git"), {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-b"), branch, worktree, QStringLiteral("HEAD")});
    const ProcessOutcome worktreeResult = waitForGitProcess(git, 30000);
    if (!worktreeResult.succeeded()) {
        updateTask(taskId, QStringLiteral("failed"),
            gitProcessError(git, worktreeResult, QStringLiteral("Agent worktree creation timed out.")));
        emit event(tr("Agent worktree creation failed: %1").arg(agentId)); return;
    }
    const QString sourceConfig = QDir(config.codexHome).filePath(QStringLiteral("config.toml"));
    QFile::copy(sourceConfig, QDir(home).filePath(QStringLiteral("config.toml")));
    auto agent = QSharedPointer<Agent>::create();
    agent->id = agentId; agent->taskId = taskId; agent->worktree = worktree; agent->branch = branch; agent->home = home;
    QProcess revision;
    revision.setWorkingDirectory(worktree);
    revision.start(QStringLiteral("git"), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")});
    const ProcessOutcome revisionResult = waitForGitProcess(revision, 10000);
    if (revisionResult.succeeded())
        agent->baseCommit = QString::fromLocal8Bit(revisionResult.standardOutput).trimmed();
    agent->session = std::make_unique<CodexAppServerSession>();
    config.workingDirectory = worktree; config.codexHome = home;
    Agent* raw = agent.get();
    connect(raw->session.get(), &CodexAppServerSession::sessionStarted, this, [this, raw, objective, model, effort, approvalMode] {
        raw->session->sendJsonRpc(QJsonObject{{QStringLiteral("id"), raw->nextRequestId++}, {QStringLiteral("method"), QStringLiteral("initialize")},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("clientInfo"), QJsonObject{{QStringLiteral("name"), QStringLiteral("cgplay.orchestrator.v1")}, {QStringLiteral("version"), QStringLiteral("1")}}}, {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("experimentalApi"), true}}}}}});
        emit event(tr("Agent app-server started: %1").arg(raw->id));
    });
    connect(raw->session.get(), &CodexAppServerSession::jsonRpcLineReceived, this, [this, raw, objective, model, effort, approvalMode](const QByteArray& line) { handleLine(raw, line, objective, model, effort, approvalMode); });
    connect(raw->session.get(), &CodexAppServerSession::standardErrorLineReceived, this, [this, raw](const QByteArray& line) { emit event(QStringLiteral("[%1] %2").arg(raw->id, QString::fromUtf8(line).left(1000))); });
    connect(raw->session.get(), &CodexAppServerSession::sessionError, this, [this, raw](const QString& message) { finishAgent(raw, QStringLiteral("failed"), message); });
    connect(raw->session.get(), &CodexAppServerSession::sessionStopped, this, [this, raw](int, QProcess::ExitStatus) { if (_agents.contains(raw->id) && !raw->finishing) finishAgent(raw, QStringLiteral("stopped")); });
    QString startError;
    if (!raw->session->start(config, &startError)) { updateTask(taskId, QStringLiteral("failed"), startError); emit event(startError); return; }
    _agents.insert(agentId, agent);
}

void LocalCodexOrchestrator::handleLine(Agent* agent, const QByteArray& line, const QString& objective, const QString& model, const QString& effort, const QString& approvalMode)
{
    QJsonParseError error; const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return;
    const QJsonObject message = document.object();
    if (message.contains(QStringLiteral("method")) && message.value(QStringLiteral("method")).toString() == QStringLiteral("turn/completed")) { finishAgent(agent, QStringLiteral("completed")); return; }
    const QJsonObject result = message.value(QStringLiteral("result")).toObject();
    if (!agent->initialized && !result.isEmpty()) {
        agent->initialized = true;
        agent->session->sendJsonRpc(QJsonObject{{QStringLiteral("method"), QStringLiteral("initialized")}});
        agent->session->sendJsonRpc(QJsonObject{{QStringLiteral("id"), agent->nextRequestId++}, {QStringLiteral("method"), QStringLiteral("thread/start")}, {QStringLiteral("params"), QJsonObject{{QStringLiteral("cwd"), agent->worktree}, {QStringLiteral("model"), model}, {QStringLiteral("approvalPolicy"), approvalPolicy(approvalMode)}}}});
        return;
    }
    const QString threadId = result.value(QStringLiteral("thread")).toObject().value(QStringLiteral("id")).toString();
    if (!threadId.isEmpty() && agent->threadId.isEmpty()) {
        agent->threadId = threadId;
        const QStringList roles{QStringLiteral("analyze and plan"), QStringLiteral("implement focused changes"), QStringLiteral("review changes and tests"), QStringLiteral("validate integration and summarize")};
        const QString role = roles.at(qAbs(qHash(agent->id)) % roles.size());
        QJsonObject params{{QStringLiteral("threadId"), threadId}, {QStringLiteral("model"), model}, {QStringLiteral("approvalPolicy"), approvalPolicy(approvalMode)},
            {QStringLiteral("input"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), objective + QStringLiteral("\nAssigned role: ") + role + QStringLiteral(". Work only in your assigned Git worktree. Before completion, summarize changed files, risks, and handoff notes for the local orchestrator.")}}}}};
        if (!effort.isEmpty()) params.insert(QStringLiteral("effort"), effort);
        agent->session->sendJsonRpc(QJsonObject{{QStringLiteral("id"), agent->nextRequestId++}, {QStringLiteral("method"), QStringLiteral("turn/start")}, {QStringLiteral("params"), params}});
    }
}

void LocalCodexOrchestrator::finishAgent(Agent* agent, const QString& status, const QString& detail)
{
    if (!agent || agent->finishing || !_agents.contains(agent->id)) return;
    agent->finishing = true;
    const QString base = agent->baseCommit.isEmpty() ? QStringLiteral("HEAD") : agent->baseCommit;
    QProcess diff; diff.setWorkingDirectory(agent->worktree); diff.start(QStringLiteral("git"), {QStringLiteral("diff"), QStringLiteral("--stat"), base, QStringLiteral("HEAD")});
    const ProcessOutcome diffResult = waitForGitProcess(diff, 10000);
    const QString result = QString::fromLocal8Bit(diffResult.standardOutput).trimmed();
    QProcess names; names.setWorkingDirectory(agent->worktree); names.start(QStringLiteral("git"), {QStringLiteral("diff"), QStringLiteral("--name-only"), base, QStringLiteral("HEAD")});
    const ProcessOutcome namesResult = waitForGitProcess(names, 10000);
    QStringList changedFiles = QString::fromLocal8Bit(namesResult.standardOutput).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QProcess statusProcess; statusProcess.setWorkingDirectory(agent->worktree);
    statusProcess.start(QStringLiteral("git"), {QStringLiteral("status"), QStringLiteral("--porcelain"), QStringLiteral("--untracked-files=all")});
    const ProcessOutcome statusResult = waitForGitProcess(statusProcess, 10000);
    for (const QString& line : QString::fromLocal8Bit(statusResult.standardOutput).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (line.size() > 3) changedFiles.push_back(line.mid(3).trimmed());
    }
    changedFiles.removeDuplicates();
    changedFiles.sort();
    QJsonArray tasks = readTasks();
    QString completedSummary;
    for (int i = 0; i < tasks.size(); ++i) { QJsonObject task = tasks.at(i).toObject(); if (task.value(QStringLiteral("id")).toString() != agent->taskId) continue; QJsonArray agents = task.value(QStringLiteral("agents")).toArray(); QJsonArray changed; for (const QString& file : changedFiles) changed.push_back(file); agents.push_back(QJsonObject{{QStringLiteral("id"), agent->id}, {QStringLiteral("status"), status}, {QStringLiteral("worktree"), agent->worktree}, {QStringLiteral("branch"), agent->branch}, {QStringLiteral("diffStat"), result}, {QStringLiteral("changedFiles"), changed}, {QStringLiteral("detail"), detail}}); task.insert(QStringLiteral("agents"), agents); QJsonArray conflicts; QHash<QString, QStringList> owners; for (const QJsonValue& agentValue : agents) { const QJsonObject candidate = agentValue.toObject(); for (const QJsonValue& fileValue : candidate.value(QStringLiteral("changedFiles")).toArray()) owners[fileValue.toString()].push_back(candidate.value(QStringLiteral("id")).toString()); } for (auto it = owners.cbegin(); it != owners.cend(); ++it) if (it.value().size() > 1) conflicts.push_back(QJsonObject{{QStringLiteral("path"), it.key()}, {QStringLiteral("agents"), QJsonArray::fromStringList(it.value())}}); task.insert(QStringLiteral("conflicts"), conflicts); const int expected = task.value(QStringLiteral("agentCount")).toInt(); if (agents.size() >= expected) { bool failed = false; QStringList summaries; for (const QJsonValue& value : agents) { const QJsonObject resultObject = value.toObject(); if (resultObject.value(QStringLiteral("status")).toString() != QStringLiteral("completed")) failed = true; summaries << QStringLiteral("%1 [%2]\n%3").arg(resultObject.value(QStringLiteral("id")).toString(), resultObject.value(QStringLiteral("status")).toString(), resultObject.value(QStringLiteral("diffStat")).toString()); } task.insert(QStringLiteral("status"), failed ? QStringLiteral("failed") : QStringLiteral("completed")); completedSummary = QStringLiteral("[cgplay.orchestrator.v1 local result]\nTask: %1\nStatus: %2\nConflicts: %3\n%4").arg(task.value(QStringLiteral("objective")).toString(), task.value(QStringLiteral("status")).toString()).arg(conflicts.size()).arg(summaries.join(QStringLiteral("\n\n"))); } tasks.replace(i, task); break; }
    writeTasks(tasks); emit event(tr("Agent finished: %1\n%2").arg(agent->id, result)); if (!completedSummary.isEmpty()) emit taskCompleted(completedSummary);
    // The session may currently be dispatching the completion line. Keep it
    // alive until that signal delivery and its buffer loop have returned.
    auto* session = agent->session.release();
    if (session) {
        disconnect(session, nullptr, this, nullptr);
        session->stop();
        session->deleteLater();
    }
    _agents.remove(agent->id);
}

bool LocalCodexOrchestrator::cancelTask(const QString& taskId, QString* error)
{
    bool found = false; for (auto it = _agents.begin(); it != _agents.end(); ++it) if (it.value()->taskId == taskId) { it.value()->session->kill(); found = true; }
    updateTask(taskId, QStringLiteral("cancelled")); if (!found && !readTasks().isEmpty()) found = true;
    if (!found && error) *error = QStringLiteral("Task not found."); return found;
}

bool LocalCodexOrchestrator::retryTask(const QString& taskId, QString* error)
{
    Q_UNUSED(taskId); if (error) *error = QStringLiteral("Retry creates fresh agents from the task UI; persisted task lacks transient credential config after restart."); return false;
}

bool LocalCodexOrchestrator::mergeTask(const QString& taskId, QString* error)
{
    const QJsonArray values = readTasks(); QJsonObject task;
    for (const QJsonValue& value : values) if (value.toObject().value(QStringLiteral("id")).toString() == taskId) { task = value.toObject(); break; }
    if (task.isEmpty()) { if (error) *error = QStringLiteral("Task not found."); return false; }
    if (!task.value(QStringLiteral("conflicts")).toArray().isEmpty()) { if (error) *error = QStringLiteral("Agent file conflicts detected. Review and resolve them before merging."); return false; }
    const QString root = task.value(QStringLiteral("projectRoot")).toString();
    if (!QDir(root).exists()) { if (error) *error = QStringLiteral("Task project root is unavailable."); return false; }
    for (const QJsonValue& value : task.value(QStringLiteral("agents")).toArray()) {
        const QJsonObject agent = value.toObject();
        const QString worktree = agent.value(QStringLiteral("worktree")).toString();
        const QString branch = agent.value(QStringLiteral("branch")).toString();
        if (!QDir(worktree).exists()) { if (error) *error = QStringLiteral("Agent worktree is unavailable: %1").arg(worktree); return false; }
        // A worktree can contain both committed agent work and additional
        // staged/unstaged/untracked files.  Materialize the latter into the
        // same branch before merging; applying only `git diff HEAD` silently
        // drops committed changes and cannot represent untracked files.
        QProcess status; status.setWorkingDirectory(worktree);
        status.start(QStringLiteral("git"), {QStringLiteral("status"), QStringLiteral("--porcelain"), QStringLiteral("--untracked-files=all")});
        const ProcessOutcome statusResult = waitForGitProcess(status, 15000);
        if (!statusResult.succeeded()) {
            if (error) *error = gitProcessError(status, statusResult, QStringLiteral("Agent status check timed out."));
            return false;
        }
        if (!QString::fromLocal8Bit(statusResult.standardOutput).trimmed().isEmpty()) {
            QProcess add; add.setWorkingDirectory(worktree); add.start(QStringLiteral("git"), {QStringLiteral("add"), QStringLiteral("-A")});
            const ProcessOutcome addResult = waitForGitProcess(add, 15000);
            if (!addResult.succeeded()) {
                if (error) *error = gitProcessError(add, addResult, QStringLiteral("Agent changes could not be staged."));
                return false;
            }
            QProcess commit; commit.setWorkingDirectory(worktree);
            commit.start(QStringLiteral("git"), {QStringLiteral("-c"), QStringLiteral("user.name=CGPlay"),
                QStringLiteral("-c"), QStringLiteral("user.email=cgplay@localhost"),
                QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("cgplay: checkpoint agent work before merge")});
            const ProcessOutcome commitResult = waitForGitProcess(commit, 30000);
            if (!commitResult.succeeded()) {
                if (error) *error = gitProcessError(commit, commitResult, QStringLiteral("Agent changes could not be committed."));
                return false;
            }
        }
        if (!branch.isEmpty()) {
            QProcess git; git.setWorkingDirectory(root); git.start(QStringLiteral("git"), {QStringLiteral("merge"), QStringLiteral("--no-ff"), branch});
            const ProcessOutcome mergeResult = waitForGitProcess(git, 30000);
            if (!mergeResult.succeeded()) { if (error) *error = gitProcessError(git, mergeResult, QStringLiteral("Agent branch merge timed out.")); return false; }
        }
    }
    updateTask(taskId, QStringLiteral("merged")); return true;
}

} // namespace cgplay
