#pragma once

#include "common/events/api/IEventBus.h"
#include "plugins/api/ICommandProvider.h"
#include "plugins/api/IMenuProvider.h"
#include "plugins/api/IPanelProvider.h"
#include "plugins/api/IPlugin.h"
#include "plugins/api/IToolbarProvider.h"

#include <QAction>
#include <QPointer>
#include <QObject>
#include <memory>
#include <vector>

namespace cgplay {

class OcioManager;
class OcioPlugin : public QObject,
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
    QAction* _ensureEnableAction(QObject* parent) const;
    QAction* _ensureAlphaAction(QObject* parent) const;
    QAction* _ensureSettingsAction(QObject* parent) const;
    QObject* _resolveViewerObject() const;
    void _bindAlphaActionToViewer() const;
    bool _queryOcioEnabled() const;
    void _disposeActions();

    IEventBus* _eventBus = nullptr;
    QPointer<QObject> _ocioObject;
    mutable QPointer<QObject> _viewerObject;
    std::vector<IEventBus::SubscriptionId> _mediaOpenedSubscriptions;
    std::vector<QMetaObject::Connection> _connections;
    mutable QPointer<QAction> _enableAction;
    mutable QPointer<QAction> _alphaAction;
    mutable QMetaObject::Connection _alphaViewerConnection;
    mutable QPointer<QAction> _settingsAction;
    bool _initialized = false;
};

} // namespace cgplay
