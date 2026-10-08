#pragma once

#include "common/settings/profiles/WorkspaceProfile.h"

#include <QString>

namespace cgplay {

class SettingsProfileService;

// Owns workspace profile state transitions; applying panel geometry remains a
// UI responsibility so the persisted model is independently testable.
class WorkspaceController final
{
public:
    explicit WorkspaceController(const SettingsProfileService& service);

    const WorkspaceProfile& currentProfile() const { return _current; }
    bool hasCurrentProfile() const { return _hasCurrent; }

    bool load(const QString& name, bool* recovered = nullptr, QString* error = nullptr);
    bool validate(const WorkspaceProfile& profile, QString* error = nullptr) const;
    bool save(const WorkspaceProfile& profile, QString* error = nullptr);
    bool reset(const QString& name, QString* error = nullptr);

private:
    const SettingsProfileService& _service;
    WorkspaceProfile _current = WorkspaceProfile::defaults();
    bool _hasCurrent = false;
};

} // namespace cgplay
