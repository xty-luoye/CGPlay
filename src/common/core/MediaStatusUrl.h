#pragma once

#include <QString>
#include <QUrl>

namespace cgplay {

inline QString normalizeMediaStatusTemplate(QString value)
{
    // Earlier versions saved the braces through QUrl::FullyEncoded.
    return value.replace(QStringLiteral("%7Bid%7D"), QStringLiteral("{id}"), Qt::CaseInsensitive);
}

inline QUrl mediaStatusUrl(QString value, const QString& taskId)
{
    value = normalizeMediaStatusTemplate(value);
    if (!value.contains(QStringLiteral("{id}")) || taskId.isEmpty()) return {};
    value.replace(QStringLiteral("{id}"), QString::fromLatin1(QUrl::toPercentEncoding(taskId)));
    return QUrl::fromEncoded(value.toUtf8(), QUrl::StrictMode);
}

} // namespace cgplay
