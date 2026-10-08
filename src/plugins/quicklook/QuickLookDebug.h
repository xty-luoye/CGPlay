#pragma once

#include <QString>

namespace cgplay::quicklook {

QString quickLookLogPath();
void logQuickLook(const QString& message);
bool isQuickLookVerboseLoggingEnabled();

} // namespace cgplay::quicklook
