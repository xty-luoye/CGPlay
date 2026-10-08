#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace cgplay {

enum class InputDevice { Keyboard, Mouse, Gamepad };

// Typed, versioned source of truth for keyboard, mouse and gamepad bindings.
// Persistence adapters may keep legacy files, but validation and lookup live
// here so every input surface follows the same conflict rules.
class InputProfile
{
public:
    static constexpr int SchemaVersion = 1;

    static InputProfile defaults();
    static QJsonObject migrate(const QJsonObject& object, bool* valid = nullptr,
                               QString* error = nullptr);
    static InputProfile fromJson(const QJsonObject& object, bool* valid = nullptr,
                                 QString* error = nullptr);

    QJsonObject toJson() const;
    bool validate(QString* error = nullptr) const;

    QString binding(InputDevice device, const QString& commandId) const;
    bool setBinding(InputDevice device, const QString& commandId,
                    const QString& gesture, QString* conflict = nullptr);
    void clearBinding(InputDevice device, const QString& commandId);
    QString commandFor(InputDevice device, const QString& gesture) const;
    QStringList conflicts(InputDevice device) const;

private:
    friend class InputBindingStore;
    QHash<QString, QString>& map(InputDevice device);
    const QHash<QString, QString>& map(InputDevice device) const;

    QHash<QString, QString> _keyboard;
    QHash<QString, QString> _mouse;
    QHash<QString, QString> _gamepad;
};

// Persistent input bindings shared by keyboard, mouse and gamepad editors.
// A binding is scoped to a device, so the same gesture may intentionally be
// used on different devices, while duplicates on one device are rejected.
class InputBindingStore
{
public:
    using Device = InputDevice;

    explicit InputBindingStore(QString rootPath = {});

    bool load(QString* error = nullptr);
    bool save(QString* error = nullptr) const;

    QString binding(Device device, const QString& commandId) const;
    bool setBinding(Device device, const QString& commandId,
                    const QString& gesture, QString* conflict = nullptr);
    void clearBinding(Device device, const QString& commandId);
    QString commandFor(Device device, const QString& gesture) const;
    QStringList conflicts(Device device) const;

    QString filePath(Device device) const;
    static QString deviceName(Device device);

private:
    QString _rootPath;
    InputProfile _profile;
};

} // namespace cgplay
