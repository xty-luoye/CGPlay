#include "WindowsDpapiCredentialStore.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QMutex>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#endif

namespace cgplay {

namespace {

QMutex credentialMutex;

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
    return storeSecrets({{secretId, secret}}, error);
}

bool WindowsDpapiCredentialStore::storeSecrets(const QMap<QString, QByteArray>& secrets, QString* error)
{
    QMutexLocker guard(&credentialMutex);
    QMap<QString, QByteArray> encrypted;
    for (auto it = secrets.cbegin(); it != secrets.cend(); ++it) {
        const QString id = normalizedSecretId(it.key());
        if (id.isEmpty() || encrypted.contains(id)) {
            if (error) *error = QStringLiteral("Secret id is empty or duplicated");
            return false;
        }
        const QByteArray bytes = it.value().isEmpty() ? QByteArray() : _protect(it.value(), error);
        if (!it.value().isEmpty() && bytes.isEmpty()) return false;
        encrypted.insert(id, bytes);
    }
    const QString path = _settingsPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) *error = QStringLiteral("Cannot create credential directory");
        return false;
    }
    // Serialize an independent INI candidate. Never mutate QSettings' shared
    // cache for the active file before the atomic replacement succeeds.
    QTemporaryDir staging(path + QStringLiteral(".stage-XXXXXX"));
    if (!staging.isValid()) { if (error) *error = staging.errorString(); return false; }
    const QString stagingPath = QDir(staging.path()).filePath(QStringLiteral("candidate.ini"));
    {
        QSettings current(path, QSettings::IniFormat);
        current.sync();
        if (current.status() != QSettings::NoError) {
            if (error) *error = QStringLiteral("Cannot read existing credentials");
            return false;
        }
        QSettings candidate(stagingPath, QSettings::IniFormat);
        for (const QString& key : current.allKeys()) candidate.setValue(key, current.value(key));
        for (auto it = encrypted.cbegin(); it != encrypted.cend(); ++it) {
            if (it.value().isEmpty()) candidate.remove(_settingsKey(it.key()));
            else candidate.setValue(_settingsKey(it.key()), it.value().toBase64());
        }
        candidate.sync();
        if (candidate.status() != QSettings::NoError) {
            if (error) *error = QStringLiteral("Cannot stage credentials");
            return false;
        }
    }
    QFile input(stagingPath);
    if (!input.open(QIODevice::ReadOnly)) { if (error) *error = input.errorString(); return false; }
    const QByteArray bytes = input.readAll();
    if (input.error() != QFileDevice::NoError) { if (error) *error = input.errorString(); return false; }
    input.close();
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
        if (error) *error = output.errorString();
        return false;
    }
    QSettings refresh(path, QSettings::IniFormat);
    refresh.sync();
    return true;
}

QByteArray WindowsDpapiCredentialStore::loadSecret(
    const QString& secretId,
    QString* error) const
{
    QMutexLocker guard(&credentialMutex);
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
    return storeSecrets({{secretId, {}}}, error);
}

bool WindowsDpapiCredentialStore::hasSecret(const QString& secretId) const
{
    QMutexLocker guard(&credentialMutex);
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
