#include "ui/app/CommandDispatcher.h"

#include "plugins/CommandRegistry.h"

#include <utility>

namespace cgplay {

CommandDispatcher::CommandDispatcher(const CommandRegistry* registry) :
    _registry(registry)
{}

void CommandDispatcher::setRegistry(const CommandRegistry* registry)
{
    _registry = registry;
}

bool CommandDispatcher::registerHandler(const QString& commandId, Handler handler)
{
    const QString id = commandId.trimmed();
    if (!_registry || !_registry->contains(id) || !handler || _handlers.contains(id)) return false;
    _handlers.insert(id, std::move(handler));
    return true;
}

void CommandDispatcher::clearHandlers()
{
    _handlers.clear();
}

bool CommandDispatcher::dispatch(const QString& commandId, bool checked) const
{
    if (!_registry) return false;
    const CommandDescriptor* command = _registry->find(commandId);
    if (!command) return false;
    if (command->isEnabled && !command->isEnabled()) return false;
    if (command->trigger) {
        command->trigger(checked);
        return true;
    }
    const auto handler = _handlers.constFind(command->id);
    return handler != _handlers.cend() && (*handler)(checked);
}

} // namespace cgplay
