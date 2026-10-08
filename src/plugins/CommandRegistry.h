#pragma once

#include "plugins/api/HostExtensionPoints.h"

#include <QSet>
#include <QStringList>
#include <QVector>

namespace cgplay {

// Canonical command metadata collection. Runtime execution belongs to CommandDispatcher.
class CommandRegistry
{
public:
    bool registerCommand(const CommandDescriptor& descriptor);
    void clear();
    bool contains(const QString& id) const;
    const CommandDescriptor* find(const QString& id) const;
    QVector<CommandDescriptor> commands() const { return _commands; }
    QStringList duplicateIds() const { return _duplicates.values(); }

private:
    QVector<CommandDescriptor> _commands;
    QSet<QString> _ids;
    QSet<QString> _duplicates;
};

} // namespace cgplay
