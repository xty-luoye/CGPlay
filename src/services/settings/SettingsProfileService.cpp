#include "SettingsProfileService.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QKeySequence>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <functional>

namespace cgplay {
namespace {

void setError(QString* error, const QString& value)
{
    if (error) *error = value;
}

bool writeAtomic(const QString& path, const QJsonObject& object, QString* error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        setError(error, QStringLiteral("Unable to create settings directory"));
        return false;
    }
    if (QFileInfo::exists(path)) {
        const QString backupPath = path + QStringLiteral(".bak");
        QFile::remove(backupPath);
        if (!QFile::copy(path, backupPath)) {
            setError(error, QStringLiteral("Unable to update settings backup"));
            return false;
        }
    }
    const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Indented);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        setError(error, file.errorString());
        return false;
    }
    return true;
}

bool readObject(const QString& path, QJsonObject* object, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, parseError.error != QJsonParseError::NoError
            ? parseError.errorString() : QStringLiteral("JSON root is not an object"));
        return false;
    }
    if (object) *object = document.object();
    return true;
}

bool isFutureVersionError(const QString& error)
{
    return error.startsWith(QStringLiteral("Unsupported "));
}

template<typename Profile>
Profile loadWithRecovery(
    const QString& path,
    const Profile& fallback,
    const std::function<Profile(const QJsonObject&, bool*, QString*)>& parser,
    bool* recovered,
    QString* error)
{
    if (recovered) *recovered = false;
    if (error) error->clear();
    if (!QFileInfo::exists(path)) return fallback;

    QJsonObject object;
    QString primaryError;
    bool primaryParsed = readObject(path, &object, &primaryError);
    bool valid = false;
    Profile result = primaryParsed ? parser(object, &valid, &primaryError) : fallback;
    if (primaryParsed && valid) return result;
    // A newer version is not corruption. Leave it untouched so a newer CGPlay
    // build can still open it.
    if (primaryParsed && isFutureVersionError(primaryError)) {
        setError(error, primaryError);
        return fallback;
    }

    const QString corruptPath = path + QStringLiteral(".corrupt-") +
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddhhmmsszzz"));
    QFile::rename(path, corruptPath);
    const QString backupPath = path + QStringLiteral(".bak");
    QJsonObject backupObject;
    QString backupError;
    if (readObject(backupPath, &backupObject, &backupError)) {
        bool backupValid = false;
        Profile backup = parser(backupObject, &backupValid, &backupError);
        if (backupValid) {
            QString recoveryWriteError;
            QSaveFile recoveredFile(path);
            const QByteArray data = QJsonDocument(backupObject).toJson(QJsonDocument::Indented);
            if (recoveredFile.open(QIODevice::WriteOnly) &&
                recoveredFile.write(data) == data.size() && recoveredFile.commit()) {
                if (recovered) *recovered = true;
                setError(error, QStringLiteral("Current profile was invalid; restored the last valid backup"));
                return backup;
            }
            recoveryWriteError = recoveredFile.errorString();
            setError(error, QStringLiteral("Backup was valid but recovery write failed: %1").arg(recoveryWriteError));
            return backup;
        }
    }
    if (recovered) *recovered = true;
    setError(error, primaryError.isEmpty() ? QStringLiteral("Profile is invalid; using defaults") : primaryError);
    return fallback;
}

bool validateBindingObject(const QJsonObject& bindings, const QSet<QString>& knownCommands,
                           QString* error)
{
    QSet<QString> gestures;
    for (auto it = bindings.constBegin(); it != bindings.constEnd(); ++it) {
        const QString id = it.key().trimmed();
        if (id.isEmpty() || !it.value().isString() ||
            (!knownCommands.isEmpty() && !knownCommands.contains(id))) {
            setError(error, QStringLiteral("Input binding contains an unknown command: %1").arg(id));
            return false;
        }
        const QString gesture = it.value().toString().trimmed().toLower();
        if (gesture.isEmpty() || gestures.contains(gesture)) {
            setError(error, QStringLiteral("Input binding is empty or duplicated: %1").arg(gesture));
            return false;
        }
        gestures.insert(gesture);
    }
    return true;
}

} // namespace

SettingsProfilePackage SettingsProfilePackage::defaults()
{
    SettingsProfilePackage result;
    result.appearance = AppearanceProfile::defaults();
    result.workspace = WorkspaceProfile::defaults();
    result.toolbar = ToolbarProfile::defaults();
    return result;
}

QJsonObject SettingsProfilePackage::migrate(const QJsonObject& object, bool* valid, QString* error)
{
    QJsonObject result = object;
    const int version = result.value(QStringLiteral("version")).toInt(1);
    if (version < 1 || version > SchemaVersion) {
        setError(error, QStringLiteral("Unsupported settings package version: %1").arg(version));
        if (valid) *valid = false;
        return {};
    }
    if (!result.contains(QStringLiteral("workspacePreset"))) {
        const QJsonObject workspace = result.value(QStringLiteral("workspace")).toObject();
        result.insert(QStringLiteral("workspacePreset"),
            workspace.value(QStringLiteral("name")).toString(
                workspace.value(QStringLiteral("preset")).toString(QStringLiteral("默认审片"))));
    }
    if (!result.contains(QStringLiteral("buttonStyles")))
        result.insert(QStringLiteral("buttonStyles"), QJsonObject{});
    if (!result.contains(QStringLiteral("input")))
        result.insert(QStringLiteral("input"), QJsonObject{{QStringLiteral("mouseWheel"), QStringLiteral("zoom")}});
    if (!result.contains(QStringLiteral("mouseBindings")))
        result.insert(QStringLiteral("mouseBindings"), QJsonObject{});
    if (!result.contains(QStringLiteral("gamepadBindings")))
        result.insert(QStringLiteral("gamepadBindings"), QJsonObject{});
    result.insert(QStringLiteral("version"), SchemaVersion);
    if (valid) *valid = true;
    return result;
}

SettingsProfilePackage SettingsProfilePackage::fromJson(const QJsonObject& object,
                                                        const QSet<QString>& knownCommands,
                                                        bool* valid, QString* error)
{
    SettingsProfilePackage result = defaults();
    if (error) error->clear();
    bool ok = false;
    const QJsonObject value = migrate(object, &ok, error);
    if (!ok) {
        if (valid) *valid = false;
        return result;
    }
    bool partOk = false;
    result.appearance = AppearanceProfile::fromPackageJson(
        value.value(QStringLiteral("theme")).toObject(),
        value.value(QStringLiteral("buttonStyles")).toObject(), &partOk, error);
    ok = ok && partOk;
    result.workspace = WorkspaceProfile::fromJson(
        value.value(QStringLiteral("workspace")).toObject(), &partOk, error);
    ok = ok && partOk;
    result.toolbar = ToolbarProfile::fromJson(
        value.value(QStringLiteral("toolbar")).toObject(), &partOk, error);
    ok = ok && partOk;
    result.shortcuts = value.value(QStringLiteral("shortcuts")).toObject();
    result.input = value.value(QStringLiteral("input")).toObject();
    result.mouseBindings = value.value(QStringLiteral("mouseBindings")).toObject();
    result.gamepadBindings = value.value(QStringLiteral("gamepadBindings")).toObject();
    result.workspacePreset = value.value(QStringLiteral("workspacePreset")).toString(
        result.workspace.name).trimmed();
    QString validationError;
    ok = ok && result.validate(knownCommands, &validationError);
    if (!ok && error && error->isEmpty()) *error = validationError;
    if (valid) *valid = ok;
    return result;
}

SettingsProfilePackage SettingsProfilePackage::fromParts(
    const QMap<QString, QJsonObject>& parts, const QSet<QString>& knownCommands,
    bool* valid, QString* error)
{
    QJsonObject merged{{QStringLiteral("version"),
        parts.value(QStringLiteral("metadata.json")).value(QStringLiteral("version")).toInt(SchemaVersion)}};
    merged.insert(QStringLiteral("theme"), parts.value(QStringLiteral("theme.json")));
    merged.insert(QStringLiteral("shortcuts"), parts.value(QStringLiteral("shortcuts.json")));
    merged.insert(QStringLiteral("toolbar"), parts.value(QStringLiteral("toolbar.json")));
    merged.insert(QStringLiteral("buttonStyles"), parts.value(QStringLiteral("button_styles.json")));
    merged.insert(QStringLiteral("input"), parts.value(QStringLiteral("input.json")));
    merged.insert(QStringLiteral("mouseBindings"), parts.value(QStringLiteral("mouse_bindings.json")));
    merged.insert(QStringLiteral("gamepadBindings"), parts.value(QStringLiteral("gamepad_bindings.json")));
    const QJsonObject workspace = parts.value(QStringLiteral("workspace.json"));
    merged.insert(QStringLiteral("workspace"), workspace);
    merged.insert(QStringLiteral("workspacePreset"), workspace.value(QStringLiteral("name")).toString(
        workspace.value(QStringLiteral("preset")).toString(QStringLiteral("默认审片"))));
    return fromJson(merged, knownCommands, valid, error);
}

QJsonObject SettingsProfilePackage::toJson() const
{
    return {{QStringLiteral("version"), SchemaVersion},
        {QStringLiteral("theme"), appearance.toJson()},
        {QStringLiteral("shortcuts"), shortcuts},
        {QStringLiteral("toolbar"), toolbar.toJson()},
        {QStringLiteral("buttonStyles"), appearance.buttonStylesToJson()},
        {QStringLiteral("input"), input},
        {QStringLiteral("mouseBindings"), mouseBindings},
        {QStringLiteral("gamepadBindings"), gamepadBindings},
        {QStringLiteral("workspace"), workspace.toJson()},
        {QStringLiteral("workspacePreset"), workspacePreset}};
}

QMap<QString, QJsonObject> SettingsProfilePackage::toParts(const QString& createdAt) const
{
    return {{QStringLiteral("theme.json"), appearance.toJson()},
        {QStringLiteral("shortcuts.json"), shortcuts},
        {QStringLiteral("toolbar.json"), toolbar.toJson()},
        {QStringLiteral("button_styles.json"), appearance.buttonStylesToJson()},
        {QStringLiteral("input.json"), input},
        {QStringLiteral("mouse_bindings.json"), mouseBindings},
        {QStringLiteral("gamepad_bindings.json"), gamepadBindings},
        {QStringLiteral("workspace.json"), workspace.toJson()},
        {QStringLiteral("metadata.json"), QJsonObject{
            {QStringLiteral("version"), SchemaVersion},
            {QStringLiteral("createdAt"), createdAt.isEmpty()
                ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate) : createdAt}}}};
}

bool SettingsProfilePackage::validate(const QSet<QString>& knownCommands, QString* error) const
{
    if (!appearance.validate(error) || !workspace.validate(error) ||
        !toolbar.validate(knownCommands, error)) return false;
    if (workspacePreset.trimmed().isEmpty()) {
        setError(error, QStringLiteral("Workspace preset is empty"));
        return false;
    }
    QSet<QString> shortcutOwners;
    for (auto it = shortcuts.constBegin(); it != shortcuts.constEnd(); ++it) {
        const QString id = it.key().trimmed();
        if (id.isEmpty() || !it.value().isString() ||
            (!knownCommands.isEmpty() && !knownCommands.contains(id))) {
            setError(error, QStringLiteral("Shortcut contains an unknown command: %1").arg(id));
            return false;
        }
        const QString raw = it.value().toString().trimmed();
        if (raw.isEmpty()) continue;
        const QKeySequence sequence(raw);
        const QString normalized = sequence.toString(QKeySequence::PortableText).toLower();
        if (sequence.isEmpty() || normalized.isEmpty() || shortcutOwners.contains(normalized)) {
            setError(error, QStringLiteral("Shortcut is invalid or duplicated: %1").arg(raw));
            return false;
        }
        shortcutOwners.insert(normalized);
    }
    if (!validateBindingObject(mouseBindings, knownCommands, error) ||
        !validateBindingObject(gamepadBindings, knownCommands, error)) return false;
    const QString wheel = input.value(QStringLiteral("mouseWheel")).toString(QStringLiteral("zoom"));
    if (wheel != QStringLiteral("zoom") && wheel != QStringLiteral("none") && wheel != QStringLiteral("frames") &&
        wheel != QStringLiteral("volume")) {
        setError(error, QStringLiteral("Unknown mouse wheel mode: %1").arg(wheel));
        return false;
    }
    return true;
}

QStringList SettingsProfilePackage::partFileNames()
{
    return {QStringLiteral("theme.json"), QStringLiteral("shortcuts.json"),
        QStringLiteral("toolbar.json"), QStringLiteral("button_styles.json"),
        QStringLiteral("input.json"), QStringLiteral("mouse_bindings.json"),
        QStringLiteral("gamepad_bindings.json"), QStringLiteral("workspace.json"),
        QStringLiteral("metadata.json")};
}

SettingsProfileService::SettingsProfileService(QString rootPath)
    : _rootPath(rootPath.isEmpty()
          ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
          : std::move(rootPath))
{
    if (_rootPath.isEmpty()) _rootPath = QDir::homePath() + QStringLiteral("/.cgplay");
}

QString SettingsProfileService::rootPath() const { return _rootPath; }

QString SettingsProfileService::sanitizeName(const QString& name)
{
    QString safe = name.trimmed();
    safe.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    return safe.isEmpty() ? QStringLiteral("默认审片") : safe;
}

QString SettingsProfileService::appearancePath(const QString& name) const
{
    return QDir(_rootPath).filePath(QStringLiteral("themes/%1.json").arg(sanitizeName(name)));
}

QString SettingsProfileService::workspacePath(const QString& name) const
{
    return QDir(_rootPath).filePath(QStringLiteral("workspaces/%1.json").arg(sanitizeName(name)));
}

QString SettingsProfileService::toolbarPath() const
{
    return QDir(_rootPath).filePath(QStringLiteral("toolbar.json"));
}

bool SettingsProfileService::saveAppearance(const QString& name, const AppearanceProfile& profile,
                                            QString* error) const
{
    if (!profile.validate(error)) return false;
    QJsonObject object = profile.toJson();
    object.insert(QStringLiteral("buttonStyles"), profile.buttonStylesToJson());
    return writeAtomic(appearancePath(name), object, error);
}

AppearanceProfile SettingsProfileService::loadAppearance(const QString& name, bool* recovered,
                                                         QString* error) const
{
    return loadWithRecovery<AppearanceProfile>(appearancePath(name), AppearanceProfile::defaults(),
        [](const QJsonObject& object, bool* valid, QString* parseError) {
            return AppearanceProfile::fromJson(object, valid, parseError);
        }, recovered, error);
}

bool SettingsProfileService::saveWorkspace(const WorkspaceProfile& profile, QString* error) const
{
    if (!profile.validate(error)) return false;
    return writeAtomic(workspacePath(profile.name), profile.toJson(), error);
}

WorkspaceProfile SettingsProfileService::loadWorkspace(const QString& name, bool* recovered,
                                                       QString* error) const
{
    return loadWithRecovery<WorkspaceProfile>(workspacePath(name), WorkspaceProfile::defaults(),
        [](const QJsonObject& object, bool* valid, QString* parseError) {
            return WorkspaceProfile::fromJson(object, valid, parseError);
        }, recovered, error);
}

bool SettingsProfileService::saveToolbar(const ToolbarProfile& profile, QString* error) const
{
    if (!profile.validate({}, error)) return false;
    return writeAtomic(toolbarPath(), profile.toJson(), error);
}

ToolbarProfile SettingsProfileService::loadToolbar(bool* recovered, QString* error) const
{
    return loadWithRecovery<ToolbarProfile>(toolbarPath(), ToolbarProfile::defaults(),
        [](const QJsonObject& object, bool* valid, QString* parseError) {
            return ToolbarProfile::fromJson(object, valid, parseError);
        }, recovered, error);
}

bool SettingsProfileService::savePackage(const QString& path,
                                         const SettingsProfilePackage& package,
                                         const QSet<QString>& knownCommands,
                                         QString* error) const
{
    if (!package.validate(knownCommands, error)) return false;
    return writeAtomic(path, package.toJson(), error);
}

SettingsProfilePackage SettingsProfileService::loadPackage(
    const QString& path, const QSet<QString>& knownCommands,
    bool* recovered, QString* error) const
{
    return loadWithRecovery<SettingsProfilePackage>(path, SettingsProfilePackage::defaults(),
        [&knownCommands](const QJsonObject& object, bool* valid, QString* parseError) {
            return SettingsProfilePackage::fromJson(object, knownCommands, valid, parseError);
        }, recovered, error);
}

} // namespace cgplay
