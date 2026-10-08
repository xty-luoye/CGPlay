#pragma once

#include <QString>

#include <functional>

namespace cgplay {

class IEventBus
{
public:
    using SubscriptionId = unsigned long long;

    virtual ~IEventBus() = default;

    template<typename Event>
    SubscriptionId subscribe(std::function<void(const Event&)> handler)
    {
        return subscribeRaw(
            Event::eventName(),
            [typedHandler = std::move(handler)](const void* payload) {
                typedHandler(*static_cast<const Event*>(payload));
            });
    }

    template<typename Event>
    void unsubscribe(SubscriptionId subscriptionId)
    {
        unsubscribeRaw(Event::eventName(), subscriptionId);
    }

    template<typename Event>
    void publish(const Event& event)
    {
        publishRaw(Event::eventName(), &event);
    }

protected:
    virtual SubscriptionId subscribeRaw(
        const QString& eventType,
        std::function<void(const void*)> handler) = 0;
    virtual void unsubscribeRaw(const QString& eventType, SubscriptionId subscriptionId) = 0;
    virtual void publishRaw(const QString& eventType, const void* payload) = 0;
};

} // namespace cgplay
