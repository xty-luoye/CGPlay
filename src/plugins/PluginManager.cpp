#include "PluginManager.h"

#include "common/core/OverlayRuntimeDebug.h"

#include <QDir>
#include <QFileInfoList>
#include <QDebug>
#include <QHash>
#include <QLibrary>
#include <QPluginLoader>

#include <algorithm>

namespace cgplay {

namespace {

void logPluginManagerEvent(
    const QString& action,
    const QString& providerType,
    const QString& pluginId,
    int providerCount)
{
    runtimeCapabilityLog(
        QStringLiteral("PluginManager"),
        QStringLiteral("provider.%1").arg(action),
        QJsonObject{
            { QStringLiteral("pluginName"), pluginId },
            { QStringLiteral("providerType"), providerType },
            { QStringLiteral("pluginId"), pluginId },
            { QStringLiteral("providerCount"), providerCount }
        });
}

template<typename ProviderEntry>
QVector<ProviderCountSummary> buildProviderSummaries(const std::vector<ProviderEntry>& providers)
{
    QHash<QString, int> counts;
    for (const auto& entry : providers) {
        if (!entry.provider || entry.pluginId.isEmpty()) {
            continue;
        }
        counts[entry.pluginId] += 1;
    }

    QVector<ProviderCountSummary> summaries;
    summaries.reserve(counts.size());
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        summaries.push_back({ it.key(), it.value() });
    }
    std::sort(summaries.begin(), summaries.end(), [](const ProviderCountSummary& lhs, const ProviderCountSummary& rhs) {
        return lhs.plugin < rhs.plugin;
    });
    return summaries;
}

} // namespace

PluginManager::PluginManager(QObject* parent)
    : QObject(parent)
{
}

PluginManager::~PluginManager()
{
    shutdownPlugins();
}

bool PluginManager::registerCommandProvider(const QString& pluginId, ICommandProvider* provider)
{
    if (!_hasPluginId(pluginId) || !provider || _hasCommandProvider(pluginId, provider)) {
        return false;
    }
    _commandProviders.push_back({ pluginId, provider });
    logPluginManagerEvent(QStringLiteral("register"), QStringLiteral("command"), pluginId, static_cast<int>(_commandProviders.size()));
    return true;
}

bool PluginManager::registerMenuProvider(const QString& pluginId, IMenuProvider* provider)
{
    if (!_hasPluginId(pluginId) || !provider || _hasMenuProvider(pluginId, provider)) {
        return false;
    }
    _menuProviders.push_back({ pluginId, provider });
    logPluginManagerEvent(QStringLiteral("register"), QStringLiteral("menu"), pluginId, static_cast<int>(_menuProviders.size()));
    return true;
}

bool PluginManager::registerToolbarProvider(const QString& pluginId, IToolbarProvider* provider)
{
    if (!_hasPluginId(pluginId) || !provider || _hasToolbarProvider(pluginId, provider)) {
        return false;
    }
    _toolbarProviders.push_back({ pluginId, provider });
    logPluginManagerEvent(QStringLiteral("register"), QStringLiteral("toolbar"), pluginId, static_cast<int>(_toolbarProviders.size()));
    return true;
}

bool PluginManager::registerPanelProvider(const QString& pluginId, IPanelProvider* provider)
{
    if (!_hasPluginId(pluginId) || !provider || _hasPanelProvider(pluginId, provider)) {
        return false;
    }
    _panelProviders.push_back({ pluginId, provider });
    logPluginManagerEvent(QStringLiteral("register"), QStringLiteral("panel"), pluginId, static_cast<int>(_panelProviders.size()));
    return true;
}

bool PluginManager::registerPlugin(std::unique_ptr<IPlugin> plugin)
{
    if (!plugin) {
        return false;
    }

    const QString pluginId = plugin->id();
    if (_findEntry(pluginId)) {
        return false;
    }

    Entry entry;
    entry.id = pluginId;
    entry.plugin = plugin.get();
    entry.object = dynamic_cast<QObject*>(plugin.get());
    entry.ownedPlugin = std::move(plugin);
    _plugins.push_back(std::move(entry));
    _addPluginId(pluginId);
    Entry& registeredEntry = _plugins.back();
    _autoRegisterProviders(registeredEntry);
    if (_initialized && registeredEntry.plugin) {
        registeredEntry.plugin->initialize();
        registeredEntry.initialized = true;
    }
    runtimeCapabilityLog(
        QStringLiteral("PluginManager"),
        QStringLiteral("plugin.register"),
        QJsonObject{
            { QStringLiteral("pluginName"), pluginId },
            { QStringLiteral("pluginId"), pluginId },
            { QStringLiteral("dynamic"), false }
        });
    return true;
}

QStringList PluginManager::loadPluginsFromDirectory(const QString& directoryPath)
{
    QStringList loadedPluginIds;
    const QDir pluginDir(directoryPath);
    if (!pluginDir.exists()) {
        return loadedPluginIds;
    }

    const QFileInfoList entries = pluginDir.entryInfoList(QDir::Files);
    for (const QFileInfo& fileInfo : entries) {
        if (!QLibrary::isLibrary(fileInfo.absoluteFilePath())) {
            continue;
        }

        auto loader = std::make_unique<QPluginLoader>(fileInfo.absoluteFilePath());
        QObject* instance = loader->instance();
        if (!instance) {
            qWarning() << "[Plugins] Failed to load" << fileInfo.fileName() << ":" << loader->errorString();
            continue;
        }

        auto* plugin = qobject_cast<IPlugin*>(instance);
        if (!plugin) {
            qWarning() << "[Plugins] Not an IPlugin:" << fileInfo.fileName();
            loader->unload();
            continue;
        }

        const QString pluginId = plugin->id();
        if (_findEntry(pluginId)) {
            qInfo() << "[Plugins] Skip duplicate plugin id" << pluginId << "from" << fileInfo.fileName();
            loader->unload();
            continue;
        }

        Entry entry;
        entry.id = pluginId;
        entry.plugin = plugin;
        entry.object = instance;
        entry.loader = std::move(loader);
        entry.dynamic = true;
        _plugins.push_back(std::move(entry));
        _addPluginId(pluginId);
        Entry& registeredEntry = _plugins.back();
        _autoRegisterProviders(registeredEntry);
        if (_initialized && registeredEntry.plugin) {
            registeredEntry.plugin->initialize();
            registeredEntry.initialized = true;
        }
        loadedPluginIds.append(pluginId);
        qInfo() << "[Plugins] Loaded" << pluginId << "from" << fileInfo.fileName();
        runtimeCapabilityLog(
            QStringLiteral("PluginManager"),
            QStringLiteral("plugin.register"),
            QJsonObject{
                { QStringLiteral("pluginName"), pluginId },
                { QStringLiteral("pluginId"), pluginId },
                { QStringLiteral("dynamic"), true },
                { QStringLiteral("source"), fileInfo.absoluteFilePath() }
            });
    }

    return loadedPluginIds;
}

void PluginManager::initializePlugins()
{
    if (_initialized) {
        return;
    }
    _commandProviders.clear();
    _menuProviders.clear();
    _toolbarProviders.clear();
    _panelProviders.clear();
    for (const auto& entry : _plugins) {
        if (!entry.active) {
            continue;
        }
        _autoRegisterProviders(entry);
    }
    for (auto& entry : _plugins) {
        if (entry.active && !entry.initialized && entry.plugin) {
            entry.plugin->initialize();
            entry.initialized = true;
        }
    }
    _initialized = true;
}

void PluginManager::shutdownPlugins()
{
    if (!_initialized) {
        return;
    }
    for (auto it = _plugins.rbegin(); it != _plugins.rend(); ++it) {
        if (it->initialized && it->plugin) {
            it->plugin->shutdown();
            it->initialized = false;
        }
    }
    _initialized = false;
    _commandProviders.clear();
    _menuProviders.clear();
    _toolbarProviders.clear();
    _panelProviders.clear();
    runtimeCapabilityLog(
        QStringLiteral("PluginManager"),
        QStringLiteral("providers.clear"),
        QJsonObject{
            { QStringLiteral("reason"), QStringLiteral("shutdown") }
        });
}

bool PluginManager::deactivatePlugin(const QString& pluginId)
{
    Entry* entry = _findEntry(pluginId);
    if (!entry || !entry->active) {
        _lastLifecycleError = !entry
            ? QStringLiteral("Plugin '%1' is not registered.").arg(pluginId)
            : QStringLiteral("Plugin '%1' is already deactivated.").arg(pluginId);
        return false;
    }

    if (entry->initialized && entry->plugin) {
        entry->plugin->shutdown();
        entry->initialized = false;
    }
    _removeProvidersForPlugin(pluginId);
    _pluginIds.remove(pluginId);
    entry->active = false;
    _lastLifecycleError.clear();
    runtimeCapabilityLog(
        QStringLiteral("PluginManager"),
        QStringLiteral("plugin.deactivate"),
        QJsonObject{
            { QStringLiteral("pluginName"), pluginId },
            { QStringLiteral("pluginId"), pluginId },
            { QStringLiteral("binaryLoaded"), entry->dynamic }
        });
    return true;
}

bool PluginManager::reactivatePlugin(const QString& pluginId)
{
    Entry* entry = _findEntry(pluginId);
    if (!entry || entry->active) {
        _lastLifecycleError = !entry
            ? QStringLiteral("Plugin '%1' is not registered.").arg(pluginId)
            : QStringLiteral("Plugin '%1' is already active.").arg(pluginId);
        return false;
    }

    entry->active = true;
    _addPluginId(pluginId);
    _autoRegisterProviders(*entry);
    if (_initialized && entry->plugin) {
        entry->plugin->initialize();
        entry->initialized = true;
    }
    _lastLifecycleError.clear();
    runtimeCapabilityLog(
        QStringLiteral("PluginManager"),
        QStringLiteral("plugin.reactivate"),
        QJsonObject{
            { QStringLiteral("pluginName"), pluginId },
            { QStringLiteral("pluginId"), pluginId }
        });
    return true;
}

bool PluginManager::unloadPluginBinary(const QString& pluginId)
{
    const Entry* entry = _findEntry(pluginId);
    if (!entry) {
        _lastLifecycleError = QStringLiteral("Plugin '%1' is not registered.").arg(pluginId);
    } else if (!entry->dynamic) {
        _lastLifecycleError = QStringLiteral(
            "Plugin '%1' is built in and has no dynamic binary to unload.").arg(pluginId);
    } else if (entry->active || entry->initialized) {
        _lastLifecycleError = QStringLiteral(
            "Plugin '%1' must be deactivated before its binary can be unloaded.").arg(pluginId);
    } else {
        _lastLifecycleError = QStringLiteral(
            "Plugin '%1' does not advertise unload safety; its binary remains loaded until process exit.")
                                  .arg(pluginId);
    }

    runtimeCapabilityLog(
        QStringLiteral("PluginManager"),
        QStringLiteral("plugin.binaryUnloadRefused"),
        QJsonObject{
            { QStringLiteral("pluginName"), pluginId },
            { QStringLiteral("pluginId"), pluginId },
            { QStringLiteral("reason"), _lastLifecycleError }
        });
    return false;
}

bool PluginManager::canUnloadPluginBinary(const QString& pluginId) const
{
    Q_UNUSED(pluginId);
    // IPlugin intentionally has no ABI-level unload-safety contract. Host-created
    // actions, panels, and callbacks may still reference code in the library.
    return false;
}

PluginManager::LifecycleState PluginManager::pluginLifecycleState(const QString& pluginId) const
{
    const Entry* entry = _findEntry(pluginId);
    if (!entry) {
        return LifecycleState::Missing;
    }
    if (!entry->active) {
        return LifecycleState::Deactivated;
    }
    return entry->initialized ? LifecycleState::Initialized : LifecycleState::Registered;
}

bool PluginManager::isPluginActive(const QString& pluginId) const
{
    const Entry* entry = _findEntry(pluginId);
    return entry && entry->active;
}

bool PluginManager::isPluginInitialized(const QString& pluginId) const
{
    const Entry* entry = _findEntry(pluginId);
    return entry && entry->initialized;
}

QString PluginManager::lastLifecycleError() const
{
    return _lastLifecycleError;
}

bool PluginManager::unloadPlugin(const QString& pluginId)
{
    return deactivatePlugin(pluginId);
}

bool PluginManager::reloadPlugin(const QString& pluginId)
{
    return reactivatePlugin(pluginId);
}

bool PluginManager::hasPlugin(const QString& pluginId) const
{
    return _pluginIds.contains(pluginId);
}

QObject* PluginManager::pluginObject(const QString& pluginId) const
{
    const Entry* entry = _findEntry(pluginId);
    if (!entry || !entry->active) {
        return nullptr;
    }
    return entry->object;
}

QStringList PluginManager::loadedPluginIds() const
{
    return _pluginIds.values();
}

QStringList PluginManager::loadedDynamicPluginIds() const
{
    QStringList ids;
    for (const auto& entry : _plugins) {
        if (entry.active && entry.dynamic) {
            ids.append(entry.id);
        }
    }
    return ids;
}

QVector<CommandDescriptor> PluginManager::commandDescriptors() const
{
    QVector<CommandDescriptor> descriptors;
    for (const auto& entry : _commandProviders) {
        if (entry.provider) {
            const auto items = entry.provider->commandDescriptors();
            for (const auto& item : items) {
                descriptors.append(item);
            }
        }
    }
    return descriptors;
}

QVector<MenuContribution> PluginManager::menuContributions() const
{
    QVector<MenuContribution> contributions;
    for (const auto& entry : _menuProviders) {
        if (entry.provider) {
            const auto items = entry.provider->menuContributions();
            for (const auto& item : items) {
                contributions.append(item);
            }
        }
    }
    return contributions;
}

QVector<ToolbarContribution> PluginManager::toolbarContributions() const
{
    QVector<ToolbarContribution> contributions;
    for (const auto& entry : _toolbarProviders) {
        if (entry.provider) {
            const auto items = entry.provider->toolbarContributions();
            for (const auto& item : items) {
                contributions.append(item);
            }
        }
    }
    return contributions;
}

QVector<PanelContribution> PluginManager::panelContributions() const
{
    QVector<PanelContribution> contributions;
    for (const auto& entry : _panelProviders) {
        if (entry.provider) {
            const auto items = entry.provider->panelContributions();
            for (const auto& item : items) {
                contributions.append(item);
            }
        }
    }
    return contributions;
}

QVector<ProviderCountSummary> PluginManager::menuProviderSummaries() const
{
    return buildProviderSummaries(_menuProviders);
}

QVector<ProviderCountSummary> PluginManager::toolbarProviderSummaries() const
{
    return buildProviderSummaries(_toolbarProviders);
}

QVector<ProviderCountSummary> PluginManager::panelProviderSummaries() const
{
    return buildProviderSummaries(_panelProviders);
}

bool PluginManager::_hasPluginId(const QString& pluginId) const
{
    return _pluginIds.contains(pluginId);
}

void PluginManager::_addPluginId(const QString& pluginId)
{
    _pluginIds.insert(pluginId);
}

void PluginManager::_autoRegisterProviders(const Entry& entry)
{
    if (!entry.active || !entry.object) {
        return;
    }

    if (auto* provider = qobject_cast<ICommandProvider*>(entry.object)) {
        registerCommandProvider(entry.id, provider);
    }
    if (auto* provider = qobject_cast<IMenuProvider*>(entry.object)) {
        registerMenuProvider(entry.id, provider);
    }
    if (auto* provider = qobject_cast<IToolbarProvider*>(entry.object)) {
        registerToolbarProvider(entry.id, provider);
    }
    if (auto* provider = qobject_cast<IPanelProvider*>(entry.object)) {
        registerPanelProvider(entry.id, provider);
    }
}

PluginManager::Entry* PluginManager::_findEntry(const QString& pluginId)
{
    for (auto& entry : _plugins) {
        if (entry.id == pluginId) {
            return &entry;
        }
    }
    return nullptr;
}

const PluginManager::Entry* PluginManager::_findEntry(const QString& pluginId) const
{
    for (const auto& entry : _plugins) {
        if (entry.id == pluginId) {
            return &entry;
        }
    }
    return nullptr;
}

void PluginManager::_removeProvidersForPlugin(const QString& pluginId)
{
    const auto removeByPluginId = [&pluginId](auto& providers) {
        providers.erase(
            std::remove_if(
                providers.begin(),
                providers.end(),
                [&pluginId](const auto& entry) {
                    return entry.pluginId == pluginId;
                }),
            providers.end());
    };

    const int beforeCommands = static_cast<int>(_commandProviders.size());
    const int beforeMenus = static_cast<int>(_menuProviders.size());
    const int beforeToolbars = static_cast<int>(_toolbarProviders.size());
    const int beforePanels = static_cast<int>(_panelProviders.size());
    removeByPluginId(_commandProviders);
    removeByPluginId(_menuProviders);
    removeByPluginId(_toolbarProviders);
    removeByPluginId(_panelProviders);
    runtimeCapabilityLog(
        QStringLiteral("PluginManager"),
        QStringLiteral("providers.remove"),
        QJsonObject{
            { QStringLiteral("pluginName"), pluginId },
            { QStringLiteral("pluginId"), pluginId },
            { QStringLiteral("commandDelta"), beforeCommands - static_cast<int>(_commandProviders.size()) },
            { QStringLiteral("menuDelta"), beforeMenus - static_cast<int>(_menuProviders.size()) },
            { QStringLiteral("toolbarDelta"), beforeToolbars - static_cast<int>(_toolbarProviders.size()) },
            { QStringLiteral("panelDelta"), beforePanels - static_cast<int>(_panelProviders.size()) }
        });
}

bool PluginManager::_hasCommandProvider(const QString& pluginId, ICommandProvider* provider) const
{
    for (const auto& entry : _commandProviders) {
        if (entry.pluginId == pluginId && entry.provider == provider) {
            return true;
        }
    }
    return false;
}

bool PluginManager::_hasMenuProvider(const QString& pluginId, IMenuProvider* provider) const
{
    for (const auto& entry : _menuProviders) {
        if (entry.pluginId == pluginId && entry.provider == provider) {
            return true;
        }
    }
    return false;
}

bool PluginManager::_hasToolbarProvider(const QString& pluginId, IToolbarProvider* provider) const
{
    for (const auto& entry : _toolbarProviders) {
        if (entry.pluginId == pluginId && entry.provider == provider) {
            return true;
        }
    }
    return false;
}

bool PluginManager::_hasPanelProvider(const QString& pluginId, IPanelProvider* provider) const
{
    for (const auto& entry : _panelProviders) {
        if (entry.pluginId == pluginId && entry.provider == provider) {
            return true;
        }
    }
    return false;
}

} // namespace cgplay
