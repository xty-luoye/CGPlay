#include "AgentHost.h"

namespace cgplay {

AgentHost::AgentHost(AgentHostPolicy policy)
    : _policy(policy)
{
}

AgentHostPolicy AgentHost::defaultPolicy()
{
    AgentHostPolicy policy;
    policy.featureEnabled = true;
    return policy;
}

const AgentHostPolicy& AgentHost::policy() const
{
    return _policy;
}

bool AgentHost::isAvailable() const
{
    return _policy.featureEnabled;
}

AgentHostResult AgentHost::inspect(const QString& topic) const
{
    if (!_policy.featureEnabled) {
        return {false, QStringLiteral("Agent host feature is disabled."), {}};
    }

    return {
        true,
        QStringLiteral("Agent host is read-only and has no runtime bridges."),
        {
            {QStringLiteral("topic"), topic.trimmed()},
            {QStringLiteral("readOnly"), _policy.readOnly},
            {QStringLiteral("externalProcessAllowed"), _policy.allowExternalProcess},
            {QStringLiteral("runtimeMutationAllowed"), _policy.allowRuntimeMutation},
            {QStringLiteral("registeredBridgeCount"), 0},
        },
    };
}

AgentHostResult AgentHost::request(const AgentHostRequest& request) const
{
    QJsonObject details{
        {QStringLiteral("operation"), request.operation.trimmed()},
        {QStringLiteral("readOnly"), _policy.readOnly},
        {QStringLiteral("registeredBridgeCount"), 0},
    };
    if (!_policy.featureEnabled) {
        return {false, QStringLiteral("Agent host feature is disabled."), details};
    }

    return {
        false,
        QStringLiteral("Agent host command requests are not available in Phase 1."),
        details,
    };
}

} // namespace cgplay
