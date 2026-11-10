#pragma once

#include <QtPlugin>

#include <QByteArray>
#include <QString>
#include <QMap>

namespace cgplay {

class IAICredentialStore
{
public:
    virtual ~IAICredentialStore() = default;

    virtual bool storeSecret(
        const QString& secretId,
        const QByteArray& secret,
        QString* error = nullptr) = 0;
    virtual QByteArray loadSecret(
        const QString& secretId,
        QString* error = nullptr) const = 0;
    virtual bool removeSecret(
        const QString& secretId,
        QString* error = nullptr) = 0;
    virtual bool hasSecret(const QString& secretId) const = 0;
};

// Optional separate interface preserves the existing credential-store vtable.
class IAtomicAICredentialStore
{
public:
    virtual ~IAtomicAICredentialStore() = default;
    // On false, existing secrets remain unchanged.
    virtual bool storeSecrets(const QMap<QString, QByteArray>& secrets, QString* error = nullptr) = 0;
};

} // namespace cgplay

#define CGPLAY_IAICREDENTIALSTORE_IID "com.cgplay.IAICredentialStore"
Q_DECLARE_INTERFACE(cgplay::IAICredentialStore, CGPLAY_IAICREDENTIALSTORE_IID)
