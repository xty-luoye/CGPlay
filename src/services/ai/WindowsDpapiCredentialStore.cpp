#include "WindowsDpapiCredentialStore.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#endif

namespace cgplay {

namespace {

QString normalizedSecretId(const QString& secretId)
{
    return secretId.trimmed().toLower();
}

} // namespace

bool WindowsDpapiCredentialStore::storeSecret(
    const QString& secretId,
    const QByteArray& secret,
    QString* error)
{
    const QString normalized = normalizedSecretId(secretId);
    if (normalized.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Secret id is empty");
        }
        return false;
    }

    if (secret.isEmpty()) {
        return removeSecret(normalized, error);
    }

    const QByteArray encrypted = _protect(secret, error);
    if (encrypted.isEmpty()) {
        return false;
    }

    QDir().mkpath(QFileInfo(_settingsPath()).absolutePath());
    QSettings settings(_settingsPath(), QSettings::IniFormat);
    settings.setValue(_settingsKey(normalized), encrypted.toBase64());
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (error) {
            *error = QStringLiteral("Failed to persist AI credential");
        }
        return false;
    }
    return true;
}

QByteArray WindowsDpapiCredentialStore::loadSecret(
    const QString& secretId,
    QString* error) const
{
    const QString normalized = normalizedSecretId(secretId);
    if (normalized.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Secret id is empty");
        }
        return {};
    }

    QSettings settings(_settingsPath(), QSettings::IniFormat);
    const QByteArray encoded = settings.value(_settingsKey(normalized)).toByteArray();
    if (encoded.isEmpty()) {
        return {};
    }

    return _unprotect(QByteArray::fromBase64(encoded), error);
}

bool WindowsDpapiCredentialStore::removeSecret(
    const QString& secretId,
    QString* error)
{
    const QString normalized = normalizedSecretId(secretId);
    if (normalized.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Secret id is empty");
        }
        return false;
    }

    QSettings settings(_settingsPath(), QSettings::IniFormat);
    settings.remove(_settingsKey(normalized));
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (error) {
            *error = QStringLiteral("Failed to remove AI credential");
        }
        return false;
    }
    return true;
}

bool WindowsDpapiCredentialStore::hasSecret(const QString& secretId) const
{
    const QString normalized = normalizedSecretId(secretId);
    if (normalized.isEmpty()) {
        return false;
    }
    QSettings settings(_settingsPath(), QSettings::IniFormat);
    return settings.contains(_settingsKey(normalized));
}

QString WindowsDpapiCredentialStore::_settingsPath() const
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(root).filePath(QStringLiteral("ai_credentials.ini"));
}

QString WindowsDpapiCredentialStore::_settingsKey(const QString& secretId) const
{
    return QStringLiteral("credentials/%1").arg(secretId);
}

QByteArray WindowsDpapiCredentialStore::_protect(const QByteArray& plainText, QString* error) const
{
#ifdef Q_OS_WIN
    if (plainText.isEmpty()) {
        return {};
    }

    QByteArray entropyBytes("CGPlay.AI.Credential.v1");
    DATA_BLOB input{
        static_cast<DWORD>(plainText.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(plainText.constData()))
    };
    DATA_BLOB entropy{
        static_cast<DWORD>(entropyBytes.size()),
        reinterpret_cast<BYTE*>(entropyBytes.data())
    };
    DATA_BLOB output{};
    if (!CryptProtectData(
            &input,
            L"CGPlay AI Credential",
            &entropy,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &output)) {
        if (error) {
            *error = QStringLiteral("CryptProtectData failed (%1)").arg(static_cast<unsigned long>(GetLastError()));
        }
        return {};
    }

    QByteArray encrypted(reinterpret_cast<const char*>(output.pbData), static_cast<int>(output.cbData));
    LocalFree(output.pbData);
    return encrypted;
#else
    if (error) {
        *error = QStringLiteral("DPAPI credential store is only available on Windows");
    }
    Q_UNUSED(plainText);
    return {};
#endif
}

QByteArray WindowsDpapiCredentialStore::_unprotect(const QByteArray& cipherText, QString* error) const
{
#ifdef Q_OS_WIN
    if (cipherText.isEmpty()) {
        return {};
    }

    QByteArray entropyBytes("CGPlay.AI.Credential.v1");
    DATA_BLOB input{
        static_cast<DWORD>(cipherText.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(cipherText.constData()))
    };
    DATA_BLOB entropy{
        static_cast<DWORD>(entropyBytes.size()),
        reinterpret_cast<BYTE*>(entropyBytes.data())
    };
    DATA_BLOB output{};
    if (!CryptUnprotectData(
            &input,
            nullptr,
            &entropy,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &output)) {
        if (error) {
            *error = QStringLiteral("CryptUnprotectData failed (%1)").arg(static_cast<unsigned long>(GetLastError()));
        }
        return {};
    }

    QByteArray plainText(reinterpret_cast<const char*>(output.pbData), static_cast<int>(output.cbData));
    LocalFree(output.pbData);
    return plainText;
#else
    if (error) {
        *error = QStringLiteral("DPAPI credential store is only available on Windows");
    }
    Q_UNUSED(cipherText);
    return {};
#endif
}

} // namespace cgplay
