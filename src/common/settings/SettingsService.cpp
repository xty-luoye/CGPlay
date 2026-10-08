#include "SettingsService.h"

namespace cgplay {

SettingsService::SettingsService(QString organization, QString application)
    : _organization(std::move(organization))
    , _application(std::move(application))
    , _settings(_organization, _application)
{
}

QVariant SettingsService::value(const QString& key, const QVariant& defaultValue) const
{
    return _settings.value(key, defaultValue);
}

void SettingsService::setValue(const QString& key, const QVariant& value)
{
    _settings.setValue(key, value);
}

bool SettingsService::contains(const QString& key) const
{
    return _settings.contains(key);
}

QStringList SettingsService::allKeys() const
{
    return _settings.allKeys();
}

void SettingsService::remove(const QString& key)
{
    _settings.remove(key);
}

void SettingsService::sync()
{
    _settings.sync();
}

QString SettingsService::organization() const
{
    return _organization;
}

QString SettingsService::application() const
{
    return _application;
}

} // namespace cgplay
