#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

namespace cgplay {

struct WorkspacePanelProfile
{
    bool visible = true;
    QString area;
    int size = 0;

    QJsonObject toJson() const;
    static WorkspacePanelProfile fromJson(const QJsonObject& object, bool* valid = nullptr,
                                          QString* error = nullptr);
    bool validate(QString* error = nullptr) const;
};

class WorkspaceProfile
{
public:
    static constexpr int SchemaVersion = 2;

    static WorkspaceProfile defaults();
    static QJsonObject migrate(const QJsonObject& object, bool* valid = nullptr,
                               QString* error = nullptr);
    static WorkspaceProfile fromJson(const QJsonObject& object, bool* valid = nullptr,
                                     QString* error = nullptr);

    QJsonObject toJson() const;
    bool validate(QString* error = nullptr) const;

    QString name = QStringLiteral("默认审片");
    QMap<QString, WorkspacePanelProfile> panels;
    QList<int> splitterSizes;
    bool translationEnabled = false;
    bool secondaryWindow = false;
    QString savedAt;
};

} // namespace cgplay
