#include "SubtitleCredentialMigration.h"

#include "ai/api/IAICredentialStore.h"
#include "settings/api/ISettingsService.h"

#include <QByteArray>

namespace cgplay {

namespace {

struct CredentialBinding
{
    const char* settingsKey;
    const char* credentialId;
};

constexpr CredentialBinding kLegacyBindings[] = {
    { "ai/subtitles/asr/apiKey", "subtitles/asrApiKey" },
    { "ai/subtitles/mimo/apiKey", "mimo/apiKey" },
    { "ai/subtitles/qwen/apiKey", "qwen/apiKey" },
    { "ai/subtitles/translation/apiKey", "subtitles/translationApiKey" },
    { "ai/subtitles/online/apiKey", "subtitles/onlineApiKey" }
};

} // namespace

SubtitleCredentialMigrationResult migrateLegacySubtitleCredentials(
    ISettingsService* settings,
    IAICredentialStore* credentialStore)
{
    SubtitleCredentialMigrationResult result;
    if (!settings || !credentialStore) {
        result.errorMessage = QStringLiteral("Subtitle credential migration services are unavailable");
        return result;
    }

    for (const auto& binding : kLegacyBindings) {
        const QString settingsKey = QString::fromLatin1(binding.settingsKey);
        if (!settings->contains(settingsKey)) {
            continue;
        }

        QByteArray plaintext = settings->value(settingsKey).toString().trimmed().toUtf8();
        const QString credentialId = QString::fromLatin1(binding.credentialId);
        bool hasUsableSecureValue = false;
        if (credentialStore->hasSecret(credentialId)) {
            QString loadError;
            QByteArray existing = credentialStore->loadSecret(credentialId, &loadError).trimmed();
            hasUsableSecureValue = !existing.isEmpty();
            existing.fill('\0');
        }
        if (!plaintext.isEmpty() && !hasUsableSecureValue) {
            QString storeError;
            if (!credentialStore->storeSecret(credentialId, plaintext, &storeError)) {
                plaintext.fill('\0');
                result.errorMessage = storeError.isEmpty()
                    ? QStringLiteral("Failed to migrate legacy subtitle credential: %1").arg(settingsKey)
                    : storeError;
                return result;
            }
            ++result.migratedCount;
        }

        plaintext.fill('\0');
        settings->remove(settingsKey);
        ++result.removedPlaintextCount;
    }

    settings->sync();
    result.success = true;
    return result;
}

} // namespace cgplay
