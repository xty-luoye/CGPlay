#pragma once

#include "settings/api/ISettingsService.h"

#include <QSettings>

namespace cgplay {

class SettingsService : public ISettingsService
{
public:
    explicit SettingsService(QString organization, QString application);
    ~SettingsService() override = default;

    QVariant value(const QString& key, const QVariant& defaultValue = {}) const override;
    void setValue(const QString& key, const QVariant& value) override;
    bool contains(const QString& key) const override;
    QStringList allKeys() const override;
    void remove(const QString& key) override;
    void sync() override;
    QString organization() const override;
    QString application() const override;

private:
    QString _organization;
    QString _application;
    mutable QSettings _settings;
};

} // namespace cgplay
