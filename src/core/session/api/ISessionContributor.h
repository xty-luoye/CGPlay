#pragma once

#include <QJsonObject>
#include <QString>
#include <QtPlugin>

namespace cgplay {

class ISessionContributor
{
public:
    virtual ~ISessionContributor() = default;

    virtual QString sessionKey() const = 0;
    virtual void serializeInto(QJsonObject& root) const = 0;
    virtual void deserializeFrom(const QJsonObject& root) = 0;
};

} // namespace cgplay

#define CGPLAY_ISESSIONCONTRIBUTOR_IID "com.cgplay.ISessionContributor"
Q_DECLARE_INTERFACE(cgplay::ISessionContributor, CGPLAY_ISESSIONCONTRIBUTOR_IID)
