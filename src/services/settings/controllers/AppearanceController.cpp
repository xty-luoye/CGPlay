#include "AppearanceController.h"

#include "services/settings/SettingsProfileService.h"

#include <utility>

namespace cgplay {
namespace {

void setError(QString* error, const QString& value)
{
    if (error) *error = value;
}

} // namespace

AppearanceController::AppearanceController(const SettingsProfileService& service,
                                           QSet<QString> knownStyleTargets)
    : _service(service),
      _knownStyleTargets(std::move(knownStyleTargets))
{}

void AppearanceController::setKnownStyleTargets(QSet<QString> ids)
{
    _knownStyleTargets = std::move(ids);
}

bool AppearanceController::load(const QString& name, bool* recovered, QString* error)
{
    if (error) error->clear();
    const QString normalizedName = name.trimmed();
    if (normalizedName.isEmpty()) {
        if (recovered) *recovered = false;
        setError(error, QStringLiteral("Appearance profile name is empty"));
        return false;
    }
    bool wasRecovered = false;
    QString loadMessage;
    const AppearanceProfile candidate = _service.loadAppearance(normalizedName, &wasRecovered, &loadMessage);
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
    _currentName = normalizedName;
    _hasCurrent = true;
    setError(error, loadMessage);
    return true;
}

bool AppearanceController::validate(const AppearanceProfile& profile, QString* error) const
{
    if (error) error->clear();
    if (!profile.validate(error)) return false;
    if (profile.buttonStyles.isEmpty()) return true;
    if (_knownStyleTargets.isEmpty()) {
        setError(error, QStringLiteral("Cannot validate button styles without known style targets"));
        return false;
    }
    for (auto it = profile.buttonStyles.constBegin(); it != profile.buttonStyles.constEnd(); ++it) {
        if (!_knownStyleTargets.contains(it.key().trimmed())) {
            setError(error, QStringLiteral("Unknown button style target: %1").arg(it.key()));
            return false;
        }
    }
    return true;
}

bool AppearanceController::save(const QString& name, const AppearanceProfile& profile,
                                QString* error)
{
    if (error) error->clear();
    const QString normalizedName = name.trimmed();
    if (normalizedName.isEmpty()) {
        setError(error, QStringLiteral("Appearance profile name is empty"));
        return false;
    }
    if (!validate(profile, error) ||
        !_service.saveAppearance(normalizedName, profile, error)) {
        return false;
    }
    _current = profile;
    _currentName = normalizedName;
    _hasCurrent = true;
    return true;
}

bool AppearanceController::reset(const QString& name, QString* error)
{
    const QString normalizedName = name.trimmed();
    if (normalizedName.isEmpty()) {
        setError(error, QStringLiteral("Appearance profile name is empty"));
        return false;
    }
    AppearanceProfile profile = AppearanceProfile::defaults();
    profile.themeName = normalizedName;
    return save(normalizedName, profile, error);
}

} // namespace cgplay
