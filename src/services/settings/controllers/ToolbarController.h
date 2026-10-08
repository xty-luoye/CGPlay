#pragma once

#include "common/settings/profiles/ToolbarProfile.h"

#include <QSet>
#include <QString>

namespace cgplay {

class SettingsProfileService;

// Validates every custom command chain against the command registry snapshot
// supplied by the application before any toolbar profile is accepted.
class ToolbarController final
{
public:
    explicit ToolbarController(const SettingsProfileService& service,
                               QSet<QString> knownCommands = {});

    void setKnownCommands(QSet<QString> ids);
    const ToolbarProfile& currentProfile() const { return _current; }
    bool hasCurrentProfile() const { return _hasCurrent; }

    bool load(bool* recovered = nullptr, QString* error = nullptr);
    bool validate(const ToolbarProfile& profile, QString* error = nullptr) const;
    bool save(const ToolbarProfile& profile, QString* error = nullptr);
    bool reset(QString* error = nullptr);

private:
    const SettingsProfileService& _service;
    QSet<QString> _knownCommands;
    ToolbarProfile _current = ToolbarProfile::defaults();
    bool _hasCurrent = false;
};

} // namespace cgplay
