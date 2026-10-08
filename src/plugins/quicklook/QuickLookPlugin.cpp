#include "QuickLookPlugin.h"

#include "common/core/ServiceLocator.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"

namespace cgplay {

QString QuickLookPlugin::id() const
{
    return QStringLiteral("quicklook");
}

void QuickLookPlugin::initialize()
{
    if (_initialized) {
        return;
    }
    auto* eventBus = ServiceLocator::getService<IEventBus>();
    if (!eventBus) {
        return;
    }

    _eventBus = eventBus;
    _initialized = true;
    const auto subscriptionId = eventBus->subscribe<MediaOpenedEvent>([](const MediaOpenedEvent&) {
    });
    _mediaOpenedSubscriptions.push_back(subscriptionId);
}

void QuickLookPlugin::shutdown()
{
    if (!_initialized) {
        return;
    }
    _initialized = false;
    if (_eventBus) {
        for (const auto subscriptionId : _mediaOpenedSubscriptions) {
            _eventBus->unsubscribe<MediaOpenedEvent>(subscriptionId);
        }
    }
    _mediaOpenedSubscriptions.clear();
    _eventBus = nullptr;
}

QVector<CommandDescriptor> QuickLookPlugin::commandDescriptors() const
{
    return {};
}

QVector<MenuContribution> QuickLookPlugin::menuContributions() const
{
    return {};
}

QVector<ToolbarContribution> QuickLookPlugin::toolbarContributions() const
{
    return {};
}

QVector<PanelContribution> QuickLookPlugin::panelContributions() const
{
    return {};
}

} // namespace cgplay
