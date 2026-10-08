#include "plugins/CommandRegistry.h"

namespace cgplay {

void CommandRegistry::clear()
{
    _commands.clear();
    _ids.clear();
    _duplicates.clear();
}

bool CommandRegistry::registerCommand(const CommandDescriptor& descriptor)
{
    const QString id = descriptor.id.trimmed();
    if (id.isEmpty() || _ids.contains(id)) {
        if (!id.isEmpty()) _duplicates.insert(id);
        return false;
    }
    CommandDescriptor normalized = descriptor;
    normalized.id = id;
    _ids.insert(id);
    _commands.push_back(std::move(normalized));
    return true;
}

bool CommandRegistry::contains(const QString& id) const
{
    return _ids.contains(id.trimmed());
}

const CommandDescriptor* CommandRegistry::find(const QString& id) const
{
    const QString normalized = id.trimmed();
    for (const auto& command : _commands) {
        if (command.id == normalized) return &command;
    }
    return nullptr;
}

} // namespace cgplay
