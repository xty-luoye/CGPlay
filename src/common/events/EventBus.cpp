#include "EventBus.h"

namespace cgplay {

EventBus::EventBus(QObject* parent)
    : QObject(parent)
{
}

EventBus::~EventBus() = default;

IEventBus::SubscriptionId EventBus::subscribeRaw(
    const QString& eventType,
    std::function<void(const void*)> handler)
{
    const SubscriptionId id = _nextId++;
    _subscriptions[eventType].append(Subscription{ id, std::move(handler) });
    return id;
}

void EventBus::unsubscribeRaw(const QString& eventType, SubscriptionId subscriptionId)
{
    auto it = _subscriptions.find(eventType);
    if (it == _subscriptions.end()) {
        return;
    }

    auto& handlers = it.value();
    for (auto handlerIt = handlers.begin(); handlerIt != handlers.end(); ++handlerIt) {
        if (handlerIt->id == subscriptionId) {
            handlers.erase(handlerIt);
            break;
        }
    }

    if (handlers.isEmpty()) {
        _subscriptions.erase(it);
    }
}

void EventBus::publishRaw(const QString& eventType, const void* payload)
{
    const auto it = _subscriptions.constFind(eventType);
    if (it == _subscriptions.cend()) {
        return;
    }

    const auto handlers = it.value();
    for (const auto& subscription : handlers) {
        if (subscription.handler) {
            subscription.handler(payload);
        }
    }
}

} // namespace cgplay
