#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

class QWidget;

namespace cgplay {

class DownloadService
{
public:
    static DownloadService& instance();

    bool downloadBytesFromUrls(
        const QStringList& urls,
        QByteArray* out,
        QWidget* parentWidget = nullptr,
        QString* error = nullptr);

    bool downloadFileFromUrls(
        const QStringList& urls,
        const QString& destinationPath,
        QWidget* parentWidget = nullptr,
        QString* error = nullptr);

private:
    DownloadService() = default;
};

} // namespace cgplay
