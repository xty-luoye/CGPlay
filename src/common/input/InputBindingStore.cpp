#include "InputBindingStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace cgplay {

namespace {

QString deviceKey(InputDevice device)
{
    switch (device) {
    case InputDevice::Keyboard: return QStringLiteral("keyboard");
    case InputDevice::Mouse: return QStringLiteral("mouse");
    case InputDevice::Gamepad: return QStringLiteral("gamepad");
    }
    return QStringLiteral("keyboard");
}

} // namespace

InputProfile InputProfile::defaults()
{
    return {};
}

QJsonObject InputProfile::migrate(const QJsonObject& object, bool* valid, QString* error)
{
    QJsonObject result = object;
    const int version = result.value(QStringLiteral("version")).toInt(0);
    const bool ok = version >= 0 && version <= SchemaVersion;
    if (ok) {
        result.insert(QStringLiteral("version"), SchemaVersion);
    } else if (error) {
        *error = QStringLiteral("不支持的输入配置版本: %1").arg(version);
    }
    if (valid) *valid = ok;
    return result;
}

InputProfile InputProfile::fromJson(const QJsonObject& object, bool* valid, QString* error)
{
    InputProfile result = defaults();
    if (error) error->clear();
    bool ok = false;
    const QJsonObject migrated = migrate(object, &ok, error);

    for (const InputDevice device : {InputDevice::Keyboard, InputDevice::Mouse, InputDevice::Gamepad}) {
        const QJsonValue deviceValue = migrated.value(deviceKey(device));
        if (deviceValue.isUndefined()) continue;
        if (!deviceValue.isObject()) {
            ok = false;
            continue;
        }
        const QJsonObject bindings = deviceValue.toObject();
        for (auto it = bindings.constBegin(); it != bindings.constEnd(); ++it) {
            if (it.key().trimmed().isEmpty() || !it.value().isString() ||
                it.value().toString().trimmed().isEmpty()) {
                ok = false;
                continue;
            }
            result.map(device).insert(it.key().trimmed(), it.value().toString().trimmed());
        }
    }

    QString validationError;
    if (!result.validate(&validationError)) {
        ok = false;
        if (error) *error = validationError;
    } else if (!ok && error && error->isEmpty()) {
        *error = QStringLiteral("输入配置格式或版本无效");
    }
    if (valid) *valid = ok;
    return result;
}

QJsonObject InputProfile::toJson() const
{
    QJsonObject object{{QStringLiteral("version"), SchemaVersion}};
    for (const InputDevice device : {InputDevice::Keyboard, InputDevice::Mouse, InputDevice::Gamepad}) {
        QJsonObject bindings;
        for (auto it = map(device).constBegin(); it != map(device).constEnd(); ++it)
            bindings.insert(it.key(), it.value());
        object.insert(deviceKey(device), bindings);
    }
    return object;
}

bool InputProfile::validate(QString* error) const
{
    for (const InputDevice device : {InputDevice::Keyboard, InputDevice::Mouse, InputDevice::Gamepad}) {
        const QStringList duplicateBindings = conflicts(device);
        if (!duplicateBindings.isEmpty()) {
            if (error) {
                *error = QStringLiteral("%1 输入绑定冲突: %2")
                    .arg(deviceKey(device), duplicateBindings.join(QStringLiteral(", ")));
            }
            return false;
        }
    }
    return true;
}

QHash<QString, QString>& InputProfile::map(InputDevice device)
{
    switch (device) {
    case InputDevice::Keyboard: return _keyboard;
    case InputDevice::Mouse: return _mouse;
    case InputDevice::Gamepad: return _gamepad;
    }
    return _keyboard;
}

const QHash<QString, QString>& InputProfile::map(InputDevice device) const
{
    return const_cast<InputProfile*>(this)->map(device);
}

QString InputProfile::binding(InputDevice device, const QString& commandId) const
{
    return map(device).value(commandId);
}

QString InputProfile::commandFor(InputDevice device, const QString& gesture) const
{
    const QString normalized = gesture.trimmed();
    if (normalized.isEmpty()) return {};
    for (auto it = map(device).constBegin(); it != map(device).constEnd(); ++it)
        if (it.value().compare(normalized, Qt::CaseInsensitive) == 0) return it.key();
    return {};
}

bool InputProfile::setBinding(InputDevice device, const QString& commandId,
                              const QString& gesture, QString* conflict)
{
    const QString id = commandId.trimmed();
    const QString value = gesture.trimmed();
    if (id.isEmpty()) {
        if (!value.isEmpty()) {
            const QString old = commandFor(device, value);
            if (!old.isEmpty()) map(device).remove(old);
        }
        return true;
    }
    if (value.isEmpty()) {
        clearBinding(device, id);
        return true;
    }
    const QString existing = commandFor(device, value);
    if (!existing.isEmpty() && existing != id) {
        if (conflict) *conflict = existing;
        return false;
    }
    map(device).insert(id, value);
    return true;
}

void InputProfile::clearBinding(InputDevice device, const QString& commandId)
{
    map(device).remove(commandId.trimmed());
}

QStringList InputProfile::conflicts(InputDevice device) const
{
    QHash<QString, QString> owners;
    QStringList result;
    for (auto it = map(device).constBegin(); it != map(device).constEnd(); ++it) {
        const QString gesture = it.value().trimmed().toLower();
        if (gesture.isEmpty()) continue;
        if (owners.contains(gesture))
            result << owners.value(gesture) + QStringLiteral(" <-> ") + it.key();
        else
            owners.insert(gesture, it.key());
    }
    return result;
}

InputBindingStore::InputBindingStore(QString rootPath)
    : _rootPath(rootPath.isEmpty()
                    ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                    : std::move(rootPath))
{
    if (_rootPath.isEmpty()) _rootPath = QDir::homePath() + QStringLiteral("/.cgplay");
}

QString InputBindingStore::deviceName(Device device)
{
    switch (device) {
    case Device::Keyboard: return QStringLiteral("keyboard");
    case Device::Mouse: return QStringLiteral("mouse");
    case Device::Gamepad: return QStringLiteral("gamepad");
    }
    return QStringLiteral("keyboard");
}

QString InputBindingStore::filePath(Device device) const
{
    const QString fileName = device == Device::Keyboard
        ? QStringLiteral("shortcuts.json")
        : deviceName(device) + QStringLiteral("_bindings.json");
    return QDir(_rootPath).filePath(fileName);
}

bool InputBindingStore::load(QString* error)
{
    QJsonObject profileObject{{QStringLiteral("version"), InputProfile::SchemaVersion}};
    for (const Device device : {Device::Keyboard, Device::Mouse, Device::Gamepad}) {
        const QString primaryPath = filePath(device);
        QFile file(primaryPath);
        if (!file.exists()) {
            profileObject.insert(deviceKey(device), QJsonObject{});
            continue;
        }
        QJsonParseError parseError;
        QJsonDocument doc;
        if (file.open(QIODevice::ReadOnly)) doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (!doc.isObject()) {
            QFile backup(primaryPath + QStringLiteral(".bak"));
            QJsonParseError backupError;
            QJsonDocument backupDocument;
            if (backup.open(QIODevice::ReadOnly)) {
                backupDocument = QJsonDocument::fromJson(backup.readAll(), &backupError);
            }
            if (!backupDocument.isObject()) {
                if (error) *error = parseError.error != QJsonParseError::NoError
                    ? parseError.errorString() : file.errorString();
                return false;
            }
            doc = backupDocument;
            QSaveFile recovered(primaryPath);
            if (recovered.open(QIODevice::WriteOnly)) {
                recovered.write(doc.toJson(QJsonDocument::Indented));
                recovered.commit();
            }
        }
        profileObject.insert(deviceKey(device), doc.object());
    }
    bool valid = false;
    InputProfile loaded = InputProfile::fromJson(profileObject, &valid, error);
    if (!valid) return false;
    _profile = std::move(loaded);
    return true;
}

bool InputBindingStore::save(QString* error) const
{
    if (!QDir().mkpath(_rootPath)) {
        if (error) *error = QStringLiteral("无法创建输入配置目录");
        return false;
    }
    if (!_profile.validate(error)) return false;
    const QJsonObject profileObject = _profile.toJson();
    for (const Device device : {Device::Keyboard, Device::Mouse, Device::Gamepad}) {
        const QJsonObject object = profileObject.value(deviceKey(device)).toObject();
        QSaveFile file(filePath(device));
        const QString backupPath = filePath(device) + QStringLiteral(".bak");
        if (QFileInfo::exists(filePath(device))) {
            QFile::remove(backupPath);
            QFile::copy(filePath(device), backupPath);
        }
        if (!file.open(QIODevice::WriteOnly)) {
            if (error) *error = file.errorString();
            return false;
        }
        file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
        if (!file.commit()) {
            if (error) *error = file.errorString();
            return false;
        }
    }
    return true;
}

QString InputBindingStore::binding(Device device, const QString& commandId) const
{
    return _profile.binding(device, commandId);
}

QString InputBindingStore::commandFor(Device device, const QString& gesture) const
{
    return _profile.commandFor(device, gesture);
}

bool InputBindingStore::setBinding(Device device, const QString& commandId,
                                   const QString& gesture, QString* conflict)
{
    return _profile.setBinding(device, commandId, gesture, conflict);
}

void InputBindingStore::clearBinding(Device device, const QString& commandId)
{
    _profile.clearBinding(device, commandId);
}

QStringList InputBindingStore::conflicts(Device device) const
{
    return _profile.conflicts(device);
}

} // namespace cgplay
