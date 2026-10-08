#pragma once

#include "services/agent/CodexAppServerSession.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSharedPointer>
#include <QString>

#include <memory>

namespace cgplay {

class LocalCodexOrchestrator final : public QObject
{
    Q_OBJECT
public:
    explicit LocalCodexOrchestrator(QObject* parent = nullptr);
    bool createTask(const QString& objective, int count, const CodexAppServerSession::Config& baseConfig,
                    const QString& model, const QString& effort, const QString& approvalMode, QString* error);
    QJsonArray tasks() const;
    bool cancelTask(const QString& taskId, QString* error);
    bool retryTask(const QString& taskId, QString* error);
    bool mergeTask(const QString& taskId, QString* error);

signals:
    void event(const QString& text);
    void taskCompleted(const QString& summary);

private:
    struct Agent {
        QString id;
        QString taskId;
        QString worktree;
        QString branch;
        QString home;
        QString threadId;
        QString turnId;
        qint64 nextRequestId = 1;
        bool initialized = false;
        bool finishing = false;
        std::unique_ptr<CodexAppServerSession> session;
    };
    QString storePath() const;
    QJsonArray readTasks() const;
    bool writeTasks(const QJsonArray& tasks) const;
    void updateTask(const QString& taskId, const QString& status, const QString& detail = {});
    void startAgent(const QString& taskId, int index, const QString& objective, CodexAppServerSession::Config config,
                    const QString& model, const QString& effort, const QString& approvalMode);
    void handleLine(Agent* agent, const QByteArray& line, const QString& objective, const QString& model,
                    const QString& effort, const QString& approvalMode);
    void finishAgent(Agent* agent, const QString& status, const QString& detail = {});
    static QString approvalPolicy(const QString& mode);
    QHash<QString, QSharedPointer<Agent>> _agents;
};

} // namespace cgplay
