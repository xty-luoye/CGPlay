#include "WorkspaceProfile.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>

namespace cgplay {
namespace {

void setError(QString* error, const QString& value)
{
    if (error) *error = value;
}

WorkspacePanelProfile panel(bool visible, const QString& area, int size)
{
    WorkspacePanelProfile result;
    result.visible = visible;
    result.area = area;
    result.size = size;
    return result;
}

} // namespace

QJsonObject WorkspacePanelProfile::toJson() const
{
    return {{QStringLiteral("visible"), visible}, {QStringLiteral("area"), area},
            {QStringLiteral("size"), size}};
}

WorkspacePanelProfile WorkspacePanelProfile::fromJson(const QJsonObject& object, bool* valid,
                                                      QString* error)
{
    WorkspacePanelProfile result;
    if (object.value(QStringLiteral("visible")).isBool())
        result.visible = object.value(QStringLiteral("visible")).toBool();
    if (object.value(QStringLiteral("area")).isString())
        result.area = object.value(QStringLiteral("area")).toString().trimmed();
    if (object.value(QStringLiteral("size")).isDouble())
        result.size = object.value(QStringLiteral("size")).toInt();
    const bool ok = result.validate(error);
    if (valid) *valid = ok;
    return result;
}

bool WorkspacePanelProfile::validate(QString* error) const
{
    static const QSet<QString> areas = {QString(), QStringLiteral("left"), QStringLiteral("right"),
        QStringLiteral("top"), QStringLiteral("bottom"), QStringLiteral("floating")};
    if (!areas.contains(area) || size < 0 || size > 16384) {
        setError(error, QStringLiteral("Workspace panel area or size is invalid"));
        return false;
    }
    return true;
}

WorkspaceProfile WorkspaceProfile::defaults()
{
    WorkspaceProfile result;
    result.panels.insert(QStringLiteral("playlist"), panel(true, QStringLiteral("left"), 280));
    result.panels.insert(QStringLiteral("review"), panel(true, QStringLiteral("right"), 320));
    result.panels.insert(QStringLiteral("codex"), panel(true, QStringLiteral("right"), 460));
    return result;
}

QJsonObject WorkspaceProfile::migrate(const QJsonObject& object, bool* valid, QString* error)
{
    QJsonObject result = object;
    const int version = result.value(QStringLiteral("version")).toInt(1);
    if (version < 1 || version > SchemaVersion) {
        setError(error, QStringLiteral("Unsupported workspace profile version: %1").arg(version));
        if (valid) *valid = false;
        return {};
    }
    if (!result.contains(QStringLiteral("name")) && result.contains(QStringLiteral("preset")))
        result.insert(QStringLiteral("name"), result.value(QStringLiteral("preset")));
    if (version == 1) {
        if (!result.contains(QStringLiteral("translationEnabled")))
            result.insert(QStringLiteral("translationEnabled"), false);
        if (!result.contains(QStringLiteral("secondaryWindow")))
            result.insert(QStringLiteral("secondaryWindow"), false);
    }
    result.insert(QStringLiteral("version"), SchemaVersion);
    if (valid) *valid = true;
    return result;
}

WorkspaceProfile WorkspaceProfile::fromJson(const QJsonObject& object, bool* valid, QString* error)
{
    WorkspaceProfile result = defaults();
    if (error) error->clear();
    bool ok = false;
    const QJsonObject value = migrate(object, &ok, error);
    if (!ok) {
        if (valid) *valid = false;
        return result;
    }
    if (value.value(QStringLiteral("name")).isString())
        result.name = value.value(QStringLiteral("name")).toString().trimmed();
    if (value.value(QStringLiteral("panels")).isObject()) {
        result.panels.clear();
        const QJsonObject panelObject = value.value(QStringLiteral("panels")).toObject();
        for (auto it = panelObject.constBegin(); it != panelObject.constEnd(); ++it) {
            bool panelOk = it.value().isObject();
            WorkspacePanelProfile item;
            if (panelOk) item = WorkspacePanelProfile::fromJson(it.value().toObject(), &panelOk, error);
            if (!panelOk) ok = false;
            result.panels.insert(it.key(), item);
        }
    }
    if (value.value(QStringLiteral("splitterSizes")).isArray()) {
        result.splitterSizes.clear();
        for (const QJsonValue& size : value.value(QStringLiteral("splitterSizes")).toArray()) {
            if (!size.isDouble()) ok = false;
            else result.splitterSizes.append(size.toInt());
        }
    }
    if (value.value(QStringLiteral("translationEnabled")).isBool())
        result.translationEnabled = value.value(QStringLiteral("translationEnabled")).toBool();
    if (value.value(QStringLiteral("secondaryWindow")).isBool())
        result.secondaryWindow = value.value(QStringLiteral("secondaryWindow")).toBool();
    if (value.value(QStringLiteral("savedAt")).isString())
        result.savedAt = value.value(QStringLiteral("savedAt")).toString();

    QString validationError;
    ok = ok && result.validate(&validationError);
    if (!ok && error && error->isEmpty()) *error = validationError;
    if (valid) *valid = ok;
    return result;
}

QJsonObject WorkspaceProfile::toJson() const
{
    QJsonObject panelObject;
    for (auto it = panels.constBegin(); it != panels.constEnd(); ++it)
        panelObject.insert(it.key(), it.value().toJson());
    QJsonArray sizes;
    for (int size : splitterSizes) sizes.append(size);
    QJsonObject result{{QStringLiteral("version"), SchemaVersion},
        {QStringLiteral("name"), name}, {QStringLiteral("panels"), panelObject},
        {QStringLiteral("splitterSizes"), sizes},
        {QStringLiteral("translationEnabled"), translationEnabled},
        {QStringLiteral("secondaryWindow"), secondaryWindow}};
    if (!savedAt.isEmpty()) result.insert(QStringLiteral("savedAt"), savedAt);
    return result;
}

bool WorkspaceProfile::validate(QString* error) const
{
    if (name.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Workspace name is empty"));
        return false;
    }
    static const QRegularExpression idPattern(QStringLiteral("^[A-Za-z0-9_.-]+$"));
    for (auto it = panels.constBegin(); it != panels.constEnd(); ++it) {
        if (!idPattern.match(it.key()).hasMatch() || !it.value().validate(error)) {
            if (error && error->isEmpty()) *error = QStringLiteral("Invalid workspace panel: %1").arg(it.key());
            return false;
        }
    }
    for (int size : splitterSizes) {
        if (size < 0 || size > 16384) {
            setError(error, QStringLiteral("Workspace splitter size is invalid"));
            return false;
        }
    }
    return true;
}

} // namespace cgplay
