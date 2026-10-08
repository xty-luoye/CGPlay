#pragma once

#include <QJsonObject>
#include <QString>

namespace cgplay {

struct AgentHostPolicy
{
    bool featureEnabled = false;
    bool readOnly = true;
    bool allowExternalProcess = false;
    bool allowRuntimeMutation = false;
};

struct AgentHostRequest
{
    QString operation;
    QJsonObject arguments;
};

struct AgentHostResult
{
    bool accepted = false;
    QString status;
    QJsonObject details;
};

// Phase 1 deliberately exposes no runtime bridge or command executor.
class AgentHost final
{
public:
    explicit AgentHost(AgentHostPolicy policy = defaultPolicy());

    static AgentHostPolicy defaultPolicy();

    const AgentHostPolicy& policy() const;
    bool isAvailable() const;
    AgentHostResult inspect(const QString& topic = QString()) const;
    AgentHostResult request(const AgentHostRequest& request) const;

private:
    AgentHostPolicy _policy;
};

} // namespace cgplay
