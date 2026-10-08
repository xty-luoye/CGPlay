#pragma once

#include <QString>

namespace cgplay {

class IAICredentialStore;
class ISettingsService;

struct SubtitleCredentialMigrationResult
{
    bool success = false;
    int migratedCount = 0;
    int removedPlaintextCount = 0;
    QString errorMessage;
};

SubtitleCredentialMigrationResult migrateLegacySubtitleCredentials(
    ISettingsService* settings,
    IAICredentialStore* credentialStore);

} // namespace cgplay
