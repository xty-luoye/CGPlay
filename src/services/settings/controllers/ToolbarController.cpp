#include "ToolbarController.h"

#include "services/settings/SettingsProfileService.h"

#include <utility>

namespace cgplay {
namespace {

void setError(QString* error, const QString& value)
{
    if (error) *error = value;
}

bool hasCommandReferences(const ToolbarProfile& profile)
{
    for (const CustomToolbarButtonProfile& button : profile.customButtons) {
        if (!button.commands.isEmpty()) return true;
    }
    return false;
}

} // namespace

ToolbarController::ToolbarController(const SettingsProfileService& service,
                                     QSet<QString> knownCommands)
    : _service(service),
      _knownCommands(std::move(knownCommands))
{}

void ToolbarController::setKnownCommands(QSet<QString> ids)
{
    _knownCommands = std::move(ids);
}

bool ToolbarController::load(bool* recovered, QString* error)
{
    if (error) error->clear();
    bool wasRecovered = false;
    QString loadMessage;
    const ToolbarProfile candidate = _service.loadToolbar(&wasRecovered, &loadMessage);
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

bool ToolbarController::validate(const ToolbarProfile& profile, QString* error) const
{
    if (error) error->clear();
    if (_knownCommands.isEmpty() && hasCommandReferences(profile)) {
        setError(error, QStringLiteral("Cannot validate toolbar commands without a command catalog"));
        return false;
    }
    return profile.validate(_knownCommands, error);
}

bool ToolbarController::save(const ToolbarProfile& profile, QString* error)
{
    if (error) error->clear();
    if (!validate(profile, error) || !_service.saveToolbar(profile, error)) return false;
    _current = profile;
    _hasCurrent = true;
    return true;
}

bool ToolbarController::reset(QString* error)
{
    return save(ToolbarProfile::defaults(), error);
}

} // namespace cgplay
