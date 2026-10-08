#pragma once

#include "plugins/api/HostExtensionPoints.h"
#include "plugins/api/ICommandProvider.h"
#include "plugins/api/IMenuProvider.h"
#include "plugins/api/IPanelProvider.h"
#include "plugins/api/IPlugin.h"
#include "plugins/api/IToolbarProvider.h"

#include <QObject>
#include <QStringList>
#include <QSet>
#include <QVector>

#include <memory>
#include <vector>

class QPluginLoader;

namespace cgplay {

class PluginManager : public QObject
{
    Q_OBJECT
public:
    enum class LifecycleState
    {
        Missing,
        Registered,
        Initialized,
        Deactivated
    };
    Q_ENUM(LifecycleState)

    explicit PluginManager(QObject* parent = nullptr);
    ~PluginManager() override;

    bool registerPlugin(std::unique_ptr<IPlugin> plugin);
    bool registerCommandProvider(const QString& pluginId, ICommandProvider* provider);
    bool registerMenuProvider(const QString& pluginId, IMenuProvider* provider);
    bool registerToolbarProvider(const QString& pluginId, IToolbarProvider* provider);
    bool registerPanelProvider(const QString& pluginId, IPanelProvider* provider);
    QStringList loadPluginsFromDirectory(const QString& directoryPath);
    void initializePlugins();
    void shutdownPlugins();
    bool deactivatePlugin(const QString& pluginId);
    bool reactivatePlugin(const QString& pluginId);
    bool unloadPluginBinary(const QString& pluginId);
    bool canUnloadPluginBinary(const QString& pluginId) const;
    LifecycleState pluginLifecycleState(const QString& pluginId) const;
    bool isPluginActive(const QString& pluginId) const;
    bool isPluginInitialized(const QString& pluginId) const;
    QString lastLifecycleError() const;

    // Compatibility aliases. Runtime "unload" has always meant deactivation.
    bool unloadPlugin(const QString& pluginId);
    bool reloadPlugin(const QString& pluginId);
    bool hasPlugin(const QString& pluginId) const;
    QObject* pluginObject(const QString& pluginId) const;
    QStringList loadedPluginIds() const;
    QStringList loadedDynamicPluginIds() const;
    QVector<CommandDescriptor> commandDescriptors() const;
    QVector<MenuContribution> menuContributions() const;
    QVector<ToolbarContribution> toolbarContributions() const;
    QVector<PanelContribution> panelContributions() const;
    QVector<ProviderCountSummary> menuProviderSummaries() const;
    QVector<ProviderCountSummary> toolbarProviderSummaries() const;
    QVector<ProviderCountSummary> panelProviderSummaries() const;

private:
    template<typename Provider>
    struct ProviderEntry
    {
        QString pluginId;
        Provider* provider = nullptr;
    };

    struct Entry
    {
        QString id;
        std::unique_ptr<IPlugin> ownedPlugin;
        std::unique_ptr<QPluginLoader> loader;
        QObject* object = nullptr;
        IPlugin* plugin = nullptr;
        bool dynamic = false;
        bool active = true;
        bool initialized = false;
    };

    bool _hasPluginId(const QString& pluginId) const;
    void _addPluginId(const QString& pluginId);
    void _autoRegisterProviders(const Entry& entry);
    Entry* _findEntry(const QString& pluginId);
    const Entry* _findEntry(const QString& pluginId) const;
    void _removeProvidersForPlugin(const QString& pluginId);
    bool _hasCommandProvider(const QString& pluginId, ICommandProvider* provider) const;
    bool _hasMenuProvider(const QString& pluginId, IMenuProvider* provider) const;
    bool _hasToolbarProvider(const QString& pluginId, IToolbarProvider* provider) const;
    bool _hasPanelProvider(const QString& pluginId, IPanelProvider* provider) const;

    std::vector<Entry> _plugins;
    std::vector<ProviderEntry<ICommandProvider>> _commandProviders;
    std::vector<ProviderEntry<IMenuProvider>> _menuProviders;
    std::vector<ProviderEntry<IToolbarProvider>> _toolbarProviders;
    std::vector<ProviderEntry<IPanelProvider>> _panelProviders;
    QSet<QString> _pluginIds;
    bool _initialized = false;
    QString _lastLifecycleError;
};

} // namespace cgplay
