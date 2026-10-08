#pragma once

#include "common/settings/profiles/AppearanceProfile.h"

#include <QSet>
#include <QString>

namespace cgplay {

class SettingsProfileService;

// Coordinates appearance profile persistence without depending on the UI
// layer or duplicating the profile's schema and value-range validation.
class AppearanceController final
{
public:
    explicit AppearanceController(const SettingsProfileService& service,
                                  QSet<QString> knownStyleTargets = {});

    void setKnownStyleTargets(QSet<QString> ids);
    const AppearanceProfile& currentProfile() const { return _current; }
    QString currentName() const { return _currentName; }
    bool hasCurrentProfile() const { return _hasCurrent; }

    bool load(const QString& name, bool* recovered = nullptr, QString* error = nullptr);
    bool validate(const AppearanceProfile& profile, QString* error = nullptr) const;
    bool save(const QString& name, const AppearanceProfile& profile, QString* error = nullptr);
    bool reset(const QString& name, QString* error = nullptr);

private:
    const SettingsProfileService& _service;
    QSet<QString> _knownStyleTargets;
    AppearanceProfile _current = AppearanceProfile::defaults();
    QString _currentName;
    bool _hasCurrent = false;
};

} // namespace cgplay
