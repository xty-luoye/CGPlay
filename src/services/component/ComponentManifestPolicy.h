#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace cgplay::component_manifest_policy {

// Update locations belong to the shipped application or an explicit host override.
// An old cached payload must not redirect the next update check to its old host.
inline QStringList sourceUrls(const QJsonObject& bundled, const QString& overrideUrl)
{
    const QString explicitUrl = overrideUrl.trimmed();
    if (!explicitUrl.isEmpty()) return {explicitUrl};
    QStringList urls;
    const auto append = [&urls](const QString& value) {
        const QString url = value.trimmed();
        if (!url.isEmpty() && !urls.contains(url)) urls.push_back(url);
    };
    append(bundled.value(QStringLiteral("manifestUrl")).toString());
    for (const auto& value : bundled.value(QStringLiteral("manifestUrls")).toArray())
        append(value.toString());
    return urls;
}

inline QJsonObject mergePayload(const QJsonObject& incoming, const QJsonObject& bundled)
{
    QJsonObject result = incoming;
    QJsonObject components = incoming.value(QStringLiteral("components")).toObject();
    const QJsonObject defaults = bundled.value(QStringLiteral("components")).toObject();
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        if (!components.contains(it.key())) components.insert(it.key(), it.value());
    }
    result.insert(QStringLiteral("components"), components);
    return result;
}

} // namespace cgplay::component_manifest_policy
