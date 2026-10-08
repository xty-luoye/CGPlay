#pragma once

#include <QCoreApplication>
#include <QString>
#include <QVariant>

#include <memory>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <utility>

namespace cgplay {

class ServiceLocator
{
public:
    template<typename T>
    static void registerService(std::shared_ptr<T> service, const QString& slot = {})
    {
        _registry()[_makeKey<T>(slot)] = std::move(service);
    }

    template<typename T>
    static void registerService(T* service, const QString& slot = {})
    {
        _registry()[_makeKey<T>(slot)] = std::shared_ptr<T>(service, [](T*) {});
    }

    template<typename T>
    static T* getService(const QString& slot = {})
    {
        const auto& registry = _registry();
        const auto it = registry.find(_makeKey<T>(slot));
        if (it == registry.end()) {
            return nullptr;
        }
        return std::static_pointer_cast<T>(it->second).get();
    }

    template<typename T>
    static std::shared_ptr<T> getSharedService(const QString& slot = {})
    {
        const auto& registry = _registry();
        const auto it = registry.find(_makeKey<T>(slot));
        if (it == registry.end()) {
            return {};
        }
        return std::static_pointer_cast<T>(it->second);
    }

    template<typename T>
    static bool hasService(const QString& slot = {})
    {
        const auto& registry = _registry();
        return registry.find(_makeKey<T>(slot)) != registry.end();
    }

private:
    static std::unordered_map<std::string, std::shared_ptr<void>>& _registry()
    {
        using Registry = std::unordered_map<std::string, std::shared_ptr<void>>;
        static Registry fallbackRegistry;
        static constexpr auto kRegistryProperty = "_cgplay_service_locator_registry";

        auto* app = QCoreApplication::instance();
        if (!app) {
            return fallbackRegistry;
        }

        const QVariant existing = app->property(kRegistryProperty);
        if (existing.isValid()) {
            auto* registry = reinterpret_cast<Registry*>(existing.value<quintptr>());
            if (registry) {
                return *registry;
            }
        }

        auto* registry = new Registry();
        app->setProperty(
            kRegistryProperty,
            QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(registry)));
        return *registry;
    }

    template<typename T>
    static std::string _makeKey(const QString& slot)
    {
        std::string key = typeid(T).name();
        key.push_back('|');
        key += slot.toStdString();
        return key;
    }
};

} // namespace cgplay
