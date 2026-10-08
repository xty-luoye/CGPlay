#include "QuickLookDebug.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QProcessEnvironment>

namespace cgplay::quicklook {

QString quickLookLogPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/quicklook.log");
}

bool isQuickLookVerboseLoggingEnabled()
{
    const QString value = QProcessEnvironment::systemEnvironment()
        .value(QStringLiteral("CGPLAY_QUICKLOOK_VERBOSE"))
        .trimmed()
        .toLower();
    return value == QStringLiteral("1") ||
           value == QStringLiteral("true") ||
           value == QStringLiteral("yes") ||
           value == QStringLiteral("on");
}

void logQuickLook(const QString& message)
{
    QFile file(quickLookLogPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
           << " | "
           << message
           << '\n';
}

} // namespace cgplay::quicklook
