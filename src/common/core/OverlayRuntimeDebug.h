#pragma once

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <utility>

namespace cgplay {

inline bool overlayDebugEnabled()
{
    return qApp && qApp->property("cgplay.overlayDebug").toBool();
}

inline void runtimeTraceLog(
    const QString& category,
    const QString& scope,
    const QString& eventName,
    QJsonObject details = {})
{
    if (!overlayDebugEnabled()) {
        return;
    }

    details.insert(QStringLiteral("category"), category);
    details.insert(QStringLiteral("scope"), scope);
    details.insert(QStringLiteral("event"), eventName);
    details.insert(QStringLiteral("eventName"), eventName);
    details.insert(
        QStringLiteral("timestamp"),
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    qInfo().noquote() << "[OverlayDebug]"
                      << QString::fromUtf8(QJsonDocument(details).toJson(QJsonDocument::Compact));
}

inline void runtimeLifecycleLog(const QString& scope, const QString& eventName, QJsonObject details = {})
{
    runtimeTraceLog(QStringLiteral("runtime.lifecycle"), scope, eventName, std::move(details));
}

inline void runtimeCapabilityLog(const QString& scope, const QString& eventName, QJsonObject details = {})
{
    runtimeTraceLog(QStringLiteral("runtime.capability"), scope, eventName, std::move(details));
}

inline void runtimeOverlayLog(const QString& scope, const QString& eventName, QJsonObject details = {})
{
    runtimeTraceLog(QStringLiteral("runtime.overlay"), scope, eventName, std::move(details));
}

inline void runtimeAnnotationLog(const QString& scope, const QString& eventName, QJsonObject details = {})
{
    runtimeTraceLog(QStringLiteral("runtime.annotation"), scope, eventName, std::move(details));
}

inline void overlayDebugLog(const QString& scope, const QString& eventName, QJsonObject details = {})
{
    runtimeOverlayLog(scope, eventName, std::move(details));
}

} // namespace cgplay
