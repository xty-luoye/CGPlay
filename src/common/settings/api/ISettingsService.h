#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>

namespace cgplay {

class ISettingsService
{
public:
    virtual ~ISettingsService() = default;

    virtual QVariant value(const QString& key, const QVariant& defaultValue = {}) const = 0;
    virtual void setValue(const QString& key, const QVariant& value) = 0;
    virtual bool contains(const QString& key) const = 0;
    virtual QStringList allKeys() const = 0;
    virtual void remove(const QString& key) = 0;
    virtual void sync() = 0;
    virtual QString organization() const = 0;
    virtual QString application() const = 0;
};

} // namespace cgplay
