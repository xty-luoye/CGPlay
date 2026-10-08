#pragma once

#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

namespace cgplay {

struct ToolbarItemProfile
{
    bool visible = true;
    int order = 0;

    QJsonObject toJson() const;
};

struct CustomToolbarButtonProfile
{
    QString id;
    QString name;
    QString icon;
    QString group;
    QString toolbar;
    QStringList commands;
    QStringList workspaces;

    QJsonObject toJson() const;
    static CustomToolbarButtonProfile fromJson(const QJsonObject& object,
                                               bool* valid = nullptr,
                                               QString* error = nullptr);
    bool validate(const QSet<QString>& knownCommands = {}, QString* error = nullptr) const;
};

class ToolbarProfile
{
public:
    static constexpr int SchemaVersion = 1;

    static ToolbarProfile defaults();
    static QJsonObject migrate(const QJsonObject& object, bool* valid = nullptr,
                               QString* error = nullptr);
    static ToolbarProfile fromJson(const QJsonObject& object, bool* valid = nullptr,
                                   QString* error = nullptr);

    QJsonObject toJson() const;
    bool validate(const QSet<QString>& knownCommands = {}, QString* error = nullptr) const;

    QMap<QString, ToolbarItemProfile> items;
    QList<CustomToolbarButtonProfile> customButtons;
};

} // namespace cgplay
