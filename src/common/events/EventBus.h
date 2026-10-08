#pragma once

#include "events/api/IEventBus.h"

#include <QHash>
#include <QObject>
#include <QString>

#include <functional>

namespace cgplay {

class EventBus : public QObject, public IEventBus
{
    Q_OBJECT
public:
    explicit EventBus(QObject* parent = nullptr);
    ~EventBus() override;

protected:
    SubscriptionId subscribeRaw(
        const QString& eventType,
        std::function<void(const void*)> handler) override;
    void unsubscribeRaw(const QString& eventType, SubscriptionId subscriptionId) override;
    void publishRaw(const QString& eventType, const void* payload) override;

private:
    struct Subscription
    {
        SubscriptionId id = 0;
        std::function<void(const void*)> handler;
    };

    QHash<QString, QList<Subscription>> _subscriptions;
    SubscriptionId _nextId = 1;
};

} // namespace cgplay
