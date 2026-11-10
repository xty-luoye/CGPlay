#pragma once

#include "ai/api/IAICredentialStore.h"

namespace cgplay {

class WindowsDpapiCredentialStore : public IAICredentialStore, public IAtomicAICredentialStore
{
public:
    bool storeSecrets(const QMap<QString, QByteArray>& secrets, QString* error = nullptr) override;
    bool storeSecret(
        const QString& secretId,
        const QByteArray& secret,
        QString* error = nullptr) override;
    QByteArray loadSecret(
        const QString& secretId,
        QString* error = nullptr) const override;
    bool removeSecret(
        const QString& secretId,
        QString* error = nullptr) override;
    bool hasSecret(const QString& secretId) const override;

private:
    QString _settingsPath() const;
    QString _settingsKey(const QString& secretId) const;
    QByteArray _protect(const QByteArray& plainText, QString* error) const;
    QByteArray _unprotect(const QByteArray& cipherText, QString* error) const;
};

} // namespace cgplay
