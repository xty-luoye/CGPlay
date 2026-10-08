#include "WorkspaceController.h"

#include "services/settings/SettingsProfileService.h"

namespace cgplay {
namespace {

void setError(QString* error, const QString& value)
{
    if (error) *error = value;
}

} // namespace

WorkspaceController::WorkspaceController(const SettingsProfileService& service)
    : _service(service)
{}

bool WorkspaceController::load(const QString& name, bool* recovered, QString* error)
{
    if (error) error->clear();
    const QString normalizedName = name.trimmed();
    if (normalizedName.isEmpty()) {
        if (recovered) *recovered = false;
        setError(error, QStringLiteral("Workspace profile name is empty"));
        return false;
    }
    bool wasRecovered = false;
    QString loadMessage;
    const WorkspaceProfile candidate = _service.loadWorkspace(normalizedName, &wasRecovered, &loadMessage);
    if (recovered) *recovered = wasRecovered;
    if (!loadMessage.isEmpty() && !wasRecovered) {
        setError(error, loadMessage);
        return false;
    }
    QString validationError;
    if (!validate(candidate, &validationError)) {
        setError(error, validationError);
        return false;
    }
    _current = candidate;
    _hasCurrent = true;
    setError(error, loadMessage);
    return true;
}

bool WorkspaceController::validate(const WorkspaceProfile& profile, QString* error) const
{
    if (error) error->clear();
    return profile.validate(error);
}

bool WorkspaceController::save(const WorkspaceProfile& profile, QString* error)
{
    if (error) error->clear();
    if (!validate(profile, error) || !_service.saveWorkspace(profile, error)) return false;
    _current = profile;
    _hasCurrent = true;
    return true;
}

bool WorkspaceController::reset(const QString& name, QString* error)
{
    if (error) error->clear();
    const QString normalizedName = name.trimmed();
    if (normalizedName.isEmpty()) {
        setError(error, QStringLiteral("Workspace profile name is empty"));
        return false;
    }
    WorkspaceProfile profile = WorkspaceProfile::defaults();
    profile.name = normalizedName;
    return save(profile, error);
}

} // namespace cgplay
