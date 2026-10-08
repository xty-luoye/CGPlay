#include "ToolbarProfile.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>

namespace cgplay {
namespace {

void setError(QString* error, const QString& value)
{
    if (error) *error = value;
}

bool validId(const QString& value)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_.-]+$"));
    return pattern.match(value).hasMatch();
}

QStringList stringArray(const QJsonValue& value, bool* valid)
{
    QStringList result;
    if (value.isUndefined()) return result;
    if (!value.isArray()) {
        if (valid) *valid = false;
        return result;
    }
    for (const QJsonValue& entry : value.toArray()) {
        if (!entry.isString() || entry.toString().trimmed().isEmpty()) {
            if (valid) *valid = false;
            continue;
        }
        result.append(entry.toString().trimmed());
    }
    return result;
}

} // namespace

QJsonObject ToolbarItemProfile::toJson() const
{
    return {{QStringLiteral("visible"), visible}, {QStringLiteral("order"), order}};
}

QJsonObject CustomToolbarButtonProfile::toJson() const
{
    QJsonObject result{{QStringLiteral("id"), id}, {QStringLiteral("name"), name},
        {QStringLiteral("icon"), icon}, {QStringLiteral("group"), group},
        {QStringLiteral("commands"), QJsonArray::fromStringList(commands)}};
    if (!toolbar.isEmpty()) result.insert(QStringLiteral("toolbar"), toolbar);
    if (!workspaces.isEmpty())
        result.insert(QStringLiteral("workspaces"), QJsonArray::fromStringList(workspaces));
    return result;
}

CustomToolbarButtonProfile CustomToolbarButtonProfile::fromJson(const QJsonObject& object,
                                                               bool* valid, QString* error)
{
    CustomToolbarButtonProfile result;
    bool ok = true;
    result.id = object.value(QStringLiteral("id")).toString().trimmed();
    result.name = object.value(QStringLiteral("name")).toString().trimmed();
    result.icon = object.value(QStringLiteral("icon")).toString().trimmed();
    result.group = object.value(QStringLiteral("group")).toString().trimmed();
    result.toolbar = object.value(QStringLiteral("toolbar")).toString().trimmed();
    result.commands = stringArray(object.value(QStringLiteral("commands")), &ok);
    result.workspaces = stringArray(object.value(QStringLiteral("workspaces")), &ok);
    QString validationError;
    ok = ok && result.validate({}, &validationError);
    if (!ok) setError(error, validationError.isEmpty() ? QStringLiteral("Invalid custom toolbar button") : validationError);
    if (valid) *valid = ok;
    return result;
}

bool CustomToolbarButtonProfile::validate(const QSet<QString>& knownCommands, QString* error) const
{
    if (!validId(id) || name.trimmed().isEmpty() || commands.isEmpty()) {
        setError(error, QStringLiteral("Custom toolbar button requires a valid id, name, and command"));
        return false;
    }
    QSet<QString> commandIds;
    for (const QString& command : commands) {
        const QString normalized = command.trimmed();
        if (!validId(normalized) || commandIds.contains(normalized) ||
            (!knownCommands.isEmpty() && !knownCommands.contains(normalized))) {
            setError(error, QStringLiteral("Invalid or unknown toolbar command: %1").arg(command));
            return false;
        }
        commandIds.insert(normalized);
    }
    QSet<QString> workspaceIds;
    for (const QString& workspace : workspaces) {
        const QString normalized = workspace.trimmed();
        if (normalized.isEmpty() || workspaceIds.contains(normalized)) {
            setError(error, QStringLiteral("Invalid or duplicate toolbar workspace: %1").arg(workspace));
            return false;
        }
        workspaceIds.insert(normalized);
    }
    return true;
}

ToolbarProfile ToolbarProfile::defaults()
{
    return {};
}

QJsonObject ToolbarProfile::migrate(const QJsonObject& object, bool* valid, QString* error)
{
    QJsonObject result = object;
    const int version = result.value(QStringLiteral("version")).toInt(0);
    if (version < 0 || version > SchemaVersion) {
        setError(error, QStringLiteral("Unsupported toolbar profile version: %1").arg(version));
        if (valid) *valid = false;
        return {};
    }
    result.insert(QStringLiteral("version"), SchemaVersion);
    if (valid) *valid = true;
    return result;
}

ToolbarProfile ToolbarProfile::fromJson(const QJsonObject& object, bool* valid, QString* error)
{
    ToolbarProfile result = defaults();
    if (error) error->clear();
    bool ok = false;
    const QJsonObject value = migrate(object, &ok, error);
    if (!ok) {
        if (valid) *valid = false;
        return result;
    }
    QSet<QString> customIds;
    for (auto it = value.constBegin(); it != value.constEnd(); ++it) {
        if (it.key() == QStringLiteral("version")) continue;
        if (it.key() == QStringLiteral("customButtons")) {
            if (!it.value().isArray()) {
                ok = false;
                continue;
            }
            for (const QJsonValue& entry : it.value().toArray()) {
                bool buttonOk = entry.isObject();
                CustomToolbarButtonProfile button;
                if (buttonOk) button = CustomToolbarButtonProfile::fromJson(entry.toObject(), &buttonOk, error);
                if (!buttonOk || customIds.contains(button.id)) ok = false;
                else {
                    customIds.insert(button.id);
                    result.customButtons.append(button);
                }
            }
            continue;
        }
        if (!validId(it.key())) {
            ok = false;
            continue;
        }
        ToolbarItemProfile item;
        // Version 0 accepted a boolean shorthand for toolbar visibility.
        if (it.value().isBool()) {
            item.visible = it.value().toBool();
        } else if (it.value().isObject()) {
            const QJsonObject itemObject = it.value().toObject();
            if (itemObject.value(QStringLiteral("visible")).isBool())
                item.visible = itemObject.value(QStringLiteral("visible")).toBool();
            if (itemObject.value(QStringLiteral("order")).isDouble())
                item.order = itemObject.value(QStringLiteral("order")).toInt();
        } else {
            ok = false;
        }
        result.items.insert(it.key(), item);
    }
    QString validationError;
    ok = ok && result.validate({}, &validationError);
    if (!ok && error && error->isEmpty()) *error = validationError;
    if (valid) *valid = ok;
    return result;
}

QJsonObject ToolbarProfile::toJson() const
{
    QJsonObject result{{QStringLiteral("version"), SchemaVersion}};
    for (auto it = items.constBegin(); it != items.constEnd(); ++it)
        result.insert(it.key(), it.value().toJson());
    QJsonArray buttons;
    for (const CustomToolbarButtonProfile& button : customButtons) buttons.append(button.toJson());
    result.insert(QStringLiteral("customButtons"), buttons);
    return result;
}

bool ToolbarProfile::validate(const QSet<QString>& knownCommands, QString* error) const
{
    for (auto it = items.constBegin(); it != items.constEnd(); ++it) {
        if (!validId(it.key()) || it.value().order < -1000 || it.value().order > 1000) {
            setError(error, QStringLiteral("Invalid toolbar item: %1").arg(it.key()));
            return false;
        }
    }
    QSet<QString> ids;
    for (const CustomToolbarButtonProfile& button : customButtons) {
        if (ids.contains(button.id) || !button.validate(knownCommands, error)) {
            if (error && error->isEmpty()) *error = QStringLiteral("Duplicate toolbar button: %1").arg(button.id);
            return false;
        }
        ids.insert(button.id);
    }
    return true;
}

} // namespace cgplay
