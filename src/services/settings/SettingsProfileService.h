#pragma once

#include "common/settings/profiles/AppearanceProfile.h"
#include "common/settings/profiles/ToolbarProfile.h"
#include "common/settings/profiles/WorkspaceProfile.h"

#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace cgplay {

// Owns the stable top-level fields of profile.json and the corresponding ZIP
// part names. Input profiles remain owned by InputBindingStore; they are kept
// as JSON here so this package adapter does not duplicate its behavior model.
struct SettingsProfilePackage
{
    static constexpr int SchemaVersion = 3;

    static SettingsProfilePackage defaults();
    static QJsonObject migrate(const QJsonObject& object, bool* valid = nullptr,
                               QString* error = nullptr);
    static SettingsProfilePackage fromJson(const QJsonObject& object,
                                           const QSet<QString>& knownCommands = {},
                                           bool* valid = nullptr,
                                           QString* error = nullptr);
    static SettingsProfilePackage fromParts(const QMap<QString, QJsonObject>& parts,
                                            const QSet<QString>& knownCommands = {},
                                            bool* valid = nullptr,
                                            QString* error = nullptr);

    QJsonObject toJson() const;
    QMap<QString, QJsonObject> toParts(const QString& createdAt = {}) const;
    bool validate(const QSet<QString>& knownCommands = {}, QString* error = nullptr) const;
    static QStringList partFileNames();

    AppearanceProfile appearance;
    WorkspaceProfile workspace;
    ToolbarProfile toolbar;
    QJsonObject shortcuts;
    QJsonObject input;
    QJsonObject mouseBindings;
    QJsonObject gamepadBindings;
    QString workspacePreset = QStringLiteral("默认审片");
};

// Atomic JSON persistence with one last-known-good .bak and automatic recovery
// for malformed or invalid current files. ZIP extraction stays outside this
// service so callers can keep the existing responsive process implementation.
class SettingsProfileService
{
public:
    explicit SettingsProfileService(QString rootPath = {});

    QString rootPath() const;
    QString appearancePath(const QString& name) const;
    QString workspacePath(const QString& name) const;
    QString toolbarPath() const;

    bool saveAppearance(const QString& name, const AppearanceProfile& profile,
                        QString* error = nullptr) const;
    AppearanceProfile loadAppearance(const QString& name, bool* recovered = nullptr,
                                     QString* error = nullptr) const;

    bool saveWorkspace(const WorkspaceProfile& profile, QString* error = nullptr) const;
    WorkspaceProfile loadWorkspace(const QString& name, bool* recovered = nullptr,
                                   QString* error = nullptr) const;

    bool saveToolbar(const ToolbarProfile& profile, QString* error = nullptr) const;
    ToolbarProfile loadToolbar(bool* recovered = nullptr, QString* error = nullptr) const;

    bool savePackage(const QString& path, const SettingsProfilePackage& package,
                     const QSet<QString>& knownCommands = {}, QString* error = nullptr) const;
    SettingsProfilePackage loadPackage(const QString& path,
                                       const QSet<QString>& knownCommands = {},
                                       bool* recovered = nullptr,
                                       QString* error = nullptr) const;

    static QString sanitizeName(const QString& name);

private:
    QString _rootPath;
};

} // namespace cgplay
