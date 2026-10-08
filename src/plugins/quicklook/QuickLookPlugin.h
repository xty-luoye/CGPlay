#pragma once

#include "common/events/api/IEventBus.h"
#include "plugins/api/ICommandProvider.h"
#include "plugins/api/IMenuProvider.h"
#include "plugins/api/IPanelProvider.h"
#include "plugins/api/IPlugin.h"
#include "plugins/api/IToolbarProvider.h"

#include <QObject>
#include <vector>

namespace cgplay {

class QuickLookPlugin : public QObject,
                        public IPlugin,
                        public ICommandProvider,
                        public IMenuProvider,
                        public IToolbarProvider,
                        public IPanelProvider
{
    Q_OBJECT
    Q_INTERFACES(cgplay::IPlugin cgplay::ICommandProvider cgplay::IMenuProvider cgplay::IToolbarProvider cgplay::IPanelProvider)
#ifdef CGPLAY_BUILD_QT_PLUGIN
    Q_PLUGIN_METADATA(IID CGPLAY_IPLUGIN_IID)
#endif
public:
    QString id() const override;
    void initialize() override;
    void shutdown() override;
    QVector<CommandDescriptor> commandDescriptors() const override;
    QVector<MenuContribution> menuContributions() const override;
    QVector<ToolbarContribution> toolbarContributions() const override;
    QVector<PanelContribution> panelContributions() const override;

private:
    IEventBus* _eventBus = nullptr;
    std::vector<IEventBus::SubscriptionId> _mediaOpenedSubscriptions;
    bool _initialized = false;
};

} // namespace cgplay
