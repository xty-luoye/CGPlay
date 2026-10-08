#pragma once

#include <QHash>
#include <QString>

#include <functional>

namespace cgplay {

class CommandRegistry;

class CommandDispatcher
{
public:
    using Handler = std::function<bool(bool checked)>;

    explicit CommandDispatcher(const CommandRegistry* registry = nullptr);

    void setRegistry(const CommandRegistry* registry);
    bool registerHandler(const QString& commandId, Handler handler);
    void clearHandlers();
    bool dispatch(const QString& commandId, bool checked = false) const;

private:
    const CommandRegistry* _registry = nullptr;
    QHash<QString, Handler> _handlers;
};

} // namespace cgplay
