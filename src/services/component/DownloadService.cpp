#include "DownloadService.h"
#include "common/jobs/JobSystem.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QHostAddress>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressDialog>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QUrl>

#include <memory>

namespace cgplay {

namespace {

constexpr int kDownloadStallTimeoutMs = 60 * 1000;
constexpr int kDownloadTotalTimeoutMs = 30 * 60 * 1000;

QString processFailureMessage(const ProcessOutcome& outcome, const QString& fallback)
{
    const QString detail = QString::fromUtf8(outcome.standardError).trimmed();
    if (!detail.isEmpty()) {
        return detail;
    }
    if (outcome.state == JobState::Canceled) {
        return QStringLiteral("User canceled");
    }
    if (outcome.state == JobState::TimedOut) {
        return QStringLiteral("Download timed out");
    }
    return fallback;
}

ProcessOutcome waitForDownloadProcess(QProcess& process, JobContext& context)
{
    if (process.state() == QProcess::Starting && !process.waitForStarted(15 * 1000)) {
        ProcessOutcome outcome;
        outcome.state = JobState::Failed;
        outcome.standardError = process.errorString().toUtf8();
        return outcome;
    }
    ProcessOutcome outcome = context.waitForProcess(process, 25, kDownloadTotalTimeoutMs);
    if (process.error() == QProcess::FailedToStart) {
        outcome.state = JobState::Failed;
        outcome.exitCode = -1;
        outcome.standardError = process.errorString().toUtf8();
    }
    return outcome;
}

QString preferredArchiveName(const QString& url, const QString& fallback)
{
    const QString clean = QUrl(url).fileName();
    if (!clean.isEmpty()) {
        return clean;
    }
    return fallback;
}

QString quotePowerShellSingle(const QString& value)
{
    QString escaped = value;
    escaped.replace('\'', QStringLiteral("''"));
    return QStringLiteral("'") + escaped + QStringLiteral("'");
}

bool parseProxySpec(const QString& text, QNetworkProxy* proxy)
{
    if (!proxy) {
        return false;
    }

    QString spec = text.trimmed();
    if (spec.isEmpty()) {
        return false;
    }

    if (!spec.contains("://")) {
        spec.prepend(QStringLiteral("http://"));
    }

    const QUrl url(spec);
    if (!url.isValid() || url.host().isEmpty() || url.port() <= 0) {
        return false;
    }

    const QString scheme = url.scheme().trimmed().toLower();
    QNetworkProxy::ProxyType type = QNetworkProxy::HttpProxy;
    if (scheme == QStringLiteral("socks5") || scheme == QStringLiteral("socks")) {
        type = QNetworkProxy::Socks5Proxy;
    }

    *proxy = QNetworkProxy(
        type,
        url.host(),
        static_cast<quint16>(url.port()),
        url.userName(),
        url.password());
    return true;
}

QString registryProxyForScheme(const QString& scheme)
{
#ifdef Q_OS_WIN
    QSettings settings(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings"),
        QSettings::NativeFormat);
    if (!settings.value(QStringLiteral("ProxyEnable")).toBool()) {
        return {};
    }

    const QString proxyServer = settings.value(QStringLiteral("ProxyServer")).toString().trimmed();
    if (proxyServer.isEmpty()) {
        return {};
    }

    const QString lowerScheme = scheme.toLower();
    const QStringList entries = proxyServer.split(';', Qt::SkipEmptyParts);
    QString fallback;
    for (const QString& entry : entries) {
        const QString part = entry.trimmed();
        if (part.isEmpty()) {
            continue;
        }

        const int eq = part.indexOf('=');
        if (eq > 0) {
            const QString key = part.left(eq).trimmed().toLower();
            const QString value = part.mid(eq + 1).trimmed();
            if (value.isEmpty()) {
                continue;
            }
            if (key == lowerScheme) {
                return value;
            }
            if (fallback.isEmpty() && (key == QStringLiteral("http") || key == QStringLiteral("https"))) {
                fallback = value;
            }
        } else if (fallback.isEmpty()) {
            fallback = part;
        }
    }
    return fallback;
#else
    Q_UNUSED(scheme);
    return {};
#endif
}

QNetworkProxy proxyForUrl(const QString& url)
{
    const QUrl parsed(url);
    const QString host = parsed.host().trimmed();
    QHostAddress hostAddress;
    if (host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0 ||
        (hostAddress.setAddress(host) && hostAddress.isLoopback())) {
        return QNetworkProxy::NoProxy;
    }
    const QString scheme = parsed.scheme().isEmpty() ? QStringLiteral("http") : parsed.scheme();

    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QStringList candidates = {
        QStringLiteral("CGPLAY_PROXY_URL"),
        scheme.compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("HTTPS_PROXY")
            : QString(),
        scheme.compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("https_proxy")
            : QString(),
        QStringLiteral("HTTP_PROXY"),
        QStringLiteral("http_proxy"),
        QStringLiteral("ALL_PROXY"),
        QStringLiteral("all_proxy")
    };

    for (const QString& key : candidates) {
        if (key.isEmpty()) {
            continue;
        }
        const QString value = env.value(key).trimmed();
        QNetworkProxy proxy;
        if (parseProxySpec(value, &proxy)) {
            return proxy;
        }
    }

    QString registrySpec = registryProxyForScheme(scheme);
    QNetworkProxy proxy;
    if (parseProxySpec(registrySpec, &proxy)) {
        return proxy;
    }

    return QNetworkProxy::NoProxy;
}

void writeDownloadLog(const QString& message)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    QFile file(QDir(dir).filePath(QStringLiteral("component-download.log")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    const QString line = QStringLiteral("[%1] %2\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), message);
    file.write(line.toUtf8());
}

bool downloadViaPowerShell(
    const QString& url,
    const QNetworkProxy& proxy,
    QByteArray* out,
    QString* error,
    JobContext& context)
{
    if (!out) {
        if (error) {
            *error = QStringLiteral("Download buffer is null");
        }
        return false;
    }

    const QString tempDir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("cgplay_download_fallback"));
    QDir().mkpath(tempDir);
    const QString tempFilePath = QDir(tempDir).filePath(
        QStringLiteral("%1_%2")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))
            .arg(preferredArchiveName(url, QStringLiteral("cgplay_download.bin"))));

    QString proxyArg;
    if (proxy.type() != QNetworkProxy::NoProxy && !proxy.hostName().trimmed().isEmpty() && proxy.port() > 0) {
        const QString scheme = proxy.type() == QNetworkProxy::Socks5Proxy
            ? QStringLiteral("socks5")
            : QStringLiteral("http");
        const QString proxyUrl = QStringLiteral("%1://%2:%3")
            .arg(scheme, proxy.hostName())
            .arg(proxy.port());
        proxyArg = QStringLiteral(" -Proxy %1").arg(quotePowerShellSingle(proxyUrl));
    }

    const QString command = QStringLiteral(
        "$ProgressPreference='SilentlyContinue'; "
        "try { [Net.ServicePointManager]::SecurityProtocol = "
        "[Net.SecurityProtocolType]::Tls12 -bor 12288 } catch {} "
        "Invoke-WebRequest -Uri %1 -OutFile %2 -MaximumRedirection 5 -UseBasicParsing%3")
        .arg(
            quotePowerShellSingle(url),
            quotePowerShellSingle(QDir::toNativeSeparators(tempFilePath)),
            proxyArg);

    QProcess process;
    process.start(
        QStringLiteral("powershell"),
        {
            QStringLiteral("-NoProfile"),
            QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
            QStringLiteral("-Command"), command
        },
        QIODevice::ReadOnly);

    const ProcessOutcome outcome = waitForDownloadProcess(process, context);
    if (!outcome.succeeded()) {
        QFile::remove(tempFilePath);
        if (error) {
            *error = processFailureMessage(outcome, QStringLiteral("PowerShell download failed"));
        }
        return false;
    }

    QFile downloaded(tempFilePath);
    if (!downloaded.open(QIODevice::ReadOnly)) {
        QFile::remove(tempFilePath);
        if (error) {
            *error = QStringLiteral("Downloaded file could not be opened: %1").arg(tempFilePath);
        }
        return false;
    }

    *out = downloaded.readAll();
    downloaded.close();
    QFile::remove(tempFilePath);
    return !out->isEmpty();
}

bool downloadFileViaPowerShell(
    const QString& url,
    const QNetworkProxy& proxy,
    const QString& destinationPath,
    QString* error,
    JobContext& context)
{
    if (destinationPath.trimmed().isEmpty()) {
        if (error) {
            *error = QStringLiteral("Destination path is empty");
        }
        return false;
    }

    QDir().mkpath(QFileInfo(destinationPath).absolutePath());
    QFile::remove(destinationPath);

    QString proxyArg;
    if (proxy.type() != QNetworkProxy::NoProxy && !proxy.hostName().trimmed().isEmpty() && proxy.port() > 0) {
        const QString scheme = proxy.type() == QNetworkProxy::Socks5Proxy
            ? QStringLiteral("socks5")
            : QStringLiteral("http");
        const QString proxyUrl = QStringLiteral("%1://%2:%3")
            .arg(scheme, proxy.hostName())
            .arg(proxy.port());
        proxyArg = QStringLiteral(" -Proxy %1").arg(quotePowerShellSingle(proxyUrl));
    }

    const QString command = QStringLiteral(
        "$ProgressPreference='SilentlyContinue'; "
        "try { [Net.ServicePointManager]::SecurityProtocol = "
        "[Net.SecurityProtocolType]::Tls12 -bor 12288 } catch {} "
        "Invoke-WebRequest -Uri %1 -OutFile %2 -MaximumRedirection 5 -UseBasicParsing%3")
        .arg(
            quotePowerShellSingle(url),
            quotePowerShellSingle(QDir::toNativeSeparators(destinationPath)),
            proxyArg);

    QProcess process;
    process.start(
        QStringLiteral("powershell"),
        {
            QStringLiteral("-NoProfile"),
            QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
            QStringLiteral("-Command"), command
        },
        QIODevice::ReadOnly);

    const ProcessOutcome outcome = waitForDownloadProcess(process, context);
    if (!outcome.succeeded()) {
        QFile::remove(destinationPath);
        if (error) {
            *error = processFailureMessage(outcome, QStringLiteral("PowerShell file download failed"));
        }
        return false;
    }

    if (!QFileInfo::exists(destinationPath) || QFileInfo(destinationPath).size() <= 0) {
        QFile::remove(destinationPath);
        if (error) {
            *error = QStringLiteral("Downloaded file is missing or empty: %1").arg(destinationPath);
        }
        return false;
    }

    return true;
}

bool downloadFileViaCurl(
    const QString& url,
    const QNetworkProxy& proxy,
    const QString& destinationPath,
    QString* error,
    JobContext& context)
{
    if (destinationPath.trimmed().isEmpty()) {
        if (error) {
            *error = QStringLiteral("Destination path is empty");
        }
        return false;
    }

    const QString curlPath = QStringLiteral("C:/Windows/System32/curl.exe");
    if (!QFileInfo::exists(curlPath)) {
        if (error) {
            *error = QStringLiteral("curl.exe is not available");
        }
        return false;
    }

    QDir().mkpath(QFileInfo(destinationPath).absolutePath());
    QFile::remove(destinationPath);

    QStringList args = {
        QStringLiteral("-L"),
        QStringLiteral("--fail"),
        QStringLiteral("--silent"),
        QStringLiteral("--show-error"),
        QStringLiteral("--connect-timeout"), QStringLiteral("30"),
        QStringLiteral("--retry"), QStringLiteral("5"),
        QStringLiteral("--retry-delay"), QStringLiteral("2"),
        QStringLiteral("--retry-all-errors"),
        QStringLiteral("--retry-connrefused"),
        QStringLiteral("--speed-time"), QStringLiteral("180"),
        QStringLiteral("--speed-limit"), QStringLiteral("1024"),
        QStringLiteral("-A"), QStringLiteral("CGPlay/2.0"),
        QStringLiteral("-o"), QDir::toNativeSeparators(destinationPath)
    };

    if (proxy.type() != QNetworkProxy::NoProxy && !proxy.hostName().trimmed().isEmpty() && proxy.port() > 0) {
        const QString scheme = proxy.type() == QNetworkProxy::Socks5Proxy
            ? QStringLiteral("socks5h")
            : QStringLiteral("http");
        args << QStringLiteral("--proxy")
             << QStringLiteral("%1://%2:%3")
                    .arg(scheme, proxy.hostName())
                    .arg(proxy.port());
    }

    args << url;

    QProcess process;
    process.start(curlPath, args, QIODevice::ReadOnly);
    const ProcessOutcome outcome = waitForDownloadProcess(process, context);
    if (!outcome.succeeded()) {
        QFile::remove(destinationPath);
        if (error) {
            *error = processFailureMessage(outcome, QStringLiteral("curl.exe download failed"));
        }
        return false;
    }

    if (!QFileInfo::exists(destinationPath) || QFileInfo(destinationPath).size() <= 0) {
        QFile::remove(destinationPath);
        if (error) {
            *error = QStringLiteral("Downloaded file is missing or empty: %1").arg(destinationPath);
        }
        return false;
    }

    return true;
}

bool downloadFileInternal(
    const QString& url,
    const QString& destinationPath,
    QString* error,
    JobContext& context)
{
    if (destinationPath.trimmed().isEmpty()) {
        if (error) {
            *error = QStringLiteral("Destination path is empty");
        }
        return false;
    }

    const QString tempPath = destinationPath + QStringLiteral(".part");
    QFile::remove(tempPath);
    QFile::remove(destinationPath);
    QDir().mkpath(QFileInfo(tempPath).absolutePath());

    const QNetworkProxy proxy = proxyForUrl(url);
    QString curlError;
    writeDownloadLog(QStringLiteral("file curl get: %1 -> %2").arg(url, tempPath));
    if (downloadFileViaCurl(url, proxy, tempPath, &curlError, context)) {
        QFile::remove(destinationPath);
        if (!QFile::rename(tempPath, destinationPath)) {
            QFile::remove(tempPath);
            if (error) {
                *error = QStringLiteral("Unable to finalize download: %1").arg(destinationPath);
            }
            return false;
        }
        writeDownloadLog(QStringLiteral("file curl ok: %1 bytes -> %2").arg(QFileInfo(destinationPath).size()).arg(destinationPath));
        return true;
    }
    writeDownloadLog(QStringLiteral("file curl failed: %1").arg(curlError));
    if (context.shouldStop()) {
        if (error) {
            *error = context.isCancellationRequested()
                ? QStringLiteral("User canceled")
                : QStringLiteral("Download timed out");
        }
        return false;
    }

    QString powershellError;
    writeDownloadLog(QStringLiteral("file powershell get: %1 -> %2").arg(url, tempPath));
    if (downloadFileViaPowerShell(url, proxy, tempPath, &powershellError, context)) {
        QFile::remove(destinationPath);
        if (!QFile::rename(tempPath, destinationPath)) {
            QFile::remove(tempPath);
            if (error) {
                *error = QStringLiteral("Unable to finalize download: %1").arg(destinationPath);
            }
            return false;
        }
        writeDownloadLog(QStringLiteral("file powershell ok: %1 bytes -> %2").arg(QFileInfo(destinationPath).size()).arg(destinationPath));
        return true;
    }
    writeDownloadLog(QStringLiteral("file powershell failed: %1").arg(powershellError));
    if (context.shouldStop()) {
        if (error) {
            *error = context.isCancellationRequested()
                ? QStringLiteral("User canceled")
                : QStringLiteral("Download timed out");
        }
        return false;
    }

    QFile file(tempPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = QStringLiteral("Unable to write download to %1").arg(tempPath);
        }
        return false;
    }

    QNetworkAccessManager manager;
    manager.setProxy(proxy);
    if (proxy.type() == QNetworkProxy::NoProxy) {
        writeDownloadLog(QStringLiteral("file network proxy: direct"));
    } else {
        writeDownloadLog(QStringLiteral("file network proxy: %1:%2").arg(proxy.hostName()).arg(proxy.port()));
    }

    QNetworkRequest request{QUrl(url)};
    request.setRawHeader("User-Agent", "CGPlay/2.0");
    request.setMaximumRedirectsAllowed(5);
    request.setTransferTimeout(kDownloadStallTimeoutMs);

    bool abortedByStallTimeout = false;
    bool abortedByTotalTimeout = false;
    bool abortedByUserCancel = false;

    QNetworkReply* reply = manager.get(request);
    writeDownloadLog(QStringLiteral("file network get: %1 -> %2").arg(url, tempPath));

    QTimer stallTimer;
    stallTimer.setSingleShot(true);
    stallTimer.setInterval(kDownloadStallTimeoutMs);
    QObject::connect(&stallTimer, &QTimer::timeout, reply, [&abortedByStallTimeout, reply]() {
        abortedByStallTimeout = true;
        writeDownloadLog(QStringLiteral("file network stall timeout, aborting transfer"));
        reply->abort();
    });
    stallTimer.start();

    QTimer totalTimer;
    totalTimer.setSingleShot(true);
    totalTimer.setInterval(kDownloadTotalTimeoutMs);
    QObject::connect(&totalTimer, &QTimer::timeout, reply, [&abortedByTotalTimeout, reply]() {
        abortedByTotalTimeout = true;
        writeDownloadLog(QStringLiteral("file network total timeout, aborting transfer"));
        reply->abort();
    });
    totalTimer.start();

    QObject::connect(reply, &QIODevice::readyRead, [&file, reply, &stallTimer]() {
        stallTimer.start();
        const QByteArray chunk = reply->readAll();
        if (!chunk.isEmpty()) {
            file.write(chunk);
        }
    });

    QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [&stallTimer, &context](qint64 received, qint64 total) {
        stallTimer.start();
        if (total > 0) {
            context.reportProgress(
                static_cast<int>((received * 100) / total),
                QStringLiteral("download.file"));
        }
    });

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer cancelPoll;
    cancelPoll.setInterval(100);
    QObject::connect(&cancelPoll, &QTimer::timeout, reply, [&]() {
        if (!context.shouldStop() || !reply->isRunning()) {
            return;
        }
        abortedByUserCancel = context.isCancellationRequested();
        abortedByTotalTimeout = context.hasTimedOut();
        reply->abort();
    });
    cancelPoll.start();
    loop.exec();
    cancelPoll.stop();
    stallTimer.stop();
    totalTimer.stop();

    const QByteArray tail = reply->readAll();
    if (!tail.isEmpty()) {
        file.write(tail);
    }
    file.flush();
    file.close();

    if (reply->error() != QNetworkReply::NoError) {
        const bool shouldTryFallback =
            !abortedByUserCancel &&
            (abortedByStallTimeout ||
             abortedByTotalTimeout ||
             reply->error() == QNetworkReply::OperationCanceledError);

        reply->deleteLater();
        QFile::remove(tempPath);

        if (shouldTryFallback) {
            QString fallbackError;
            writeDownloadLog(QStringLiteral("file network fallback via powershell: %1").arg(url));
            if (downloadFileViaPowerShell(url, proxy, tempPath, &fallbackError, context)) {
                QFile::remove(destinationPath);
                if (!QFile::rename(tempPath, destinationPath)) {
                    QFile::remove(tempPath);
                    if (error) {
                        *error = QStringLiteral("Unable to finalize download: %1").arg(destinationPath);
                    }
                    return false;
                }
                writeDownloadLog(QStringLiteral("file network fallback ok: %1").arg(destinationPath));
                return true;
            }
            writeDownloadLog(QStringLiteral("file network fallback failed: %1").arg(fallbackError));
            if (error) {
                *error = fallbackError;
            }
            return false;
        }

        if (error) {
            QString reason = reply->errorString();
            if (abortedByUserCancel) {
                reason = QStringLiteral("User canceled");
            } else if (abortedByStallTimeout) {
                reason = QStringLiteral("Download stalled");
            } else if (abortedByTotalTimeout) {
                reason = QStringLiteral("Download timed out");
            }
            *error = QStringLiteral("Download failed: %1\nURL: %2").arg(reason, url);
        }
        writeDownloadLog(QStringLiteral("file network error: %1").arg(reply->errorString()));
        return false;
    }

    reply->deleteLater();

    QFile::remove(destinationPath);
    if (!QFile::rename(tempPath, destinationPath)) {
        QFile::remove(tempPath);
        if (error) {
            *error = QStringLiteral("Unable to finalize download: %1").arg(destinationPath);
        }
        return false;
    }

    writeDownloadLog(QStringLiteral("file network ok: %1 bytes -> %2").arg(QFileInfo(destinationPath).size()).arg(destinationPath));
    return true;
}

bool downloadBytesInternal(
    const QString& url,
    QByteArray* out,
    QString* error,
    JobContext& context)
{
    if (!out) {
        if (error) {
            *error = QStringLiteral("Download buffer is null");
        }
        return false;
    }

    QNetworkAccessManager manager;
    const QNetworkProxy proxy = proxyForUrl(url);
    manager.setProxy(proxy);
    if (proxy.type() == QNetworkProxy::NoProxy) {
        writeDownloadLog(QStringLiteral("network proxy: direct"));
    } else {
        writeDownloadLog(QStringLiteral("network proxy: %1:%2").arg(proxy.hostName()).arg(proxy.port()));
    }

    QNetworkRequest request = QNetworkRequest(QUrl(url));
    request.setRawHeader("User-Agent", "CGPlay/2.0");
    request.setMaximumRedirectsAllowed(5);
    request.setTransferTimeout(kDownloadStallTimeoutMs);

    bool abortedByStallTimeout = false;
    bool abortedByTotalTimeout = false;
    bool abortedByUserCancel = false;

    QNetworkReply* reply = manager.get(request);
    writeDownloadLog(QStringLiteral("network get: %1").arg(url));

    QTimer stallTimer;
    stallTimer.setSingleShot(true);
    stallTimer.setInterval(kDownloadStallTimeoutMs);
    QObject::connect(&stallTimer, &QTimer::timeout, reply, [&abortedByStallTimeout, reply]() {
        abortedByStallTimeout = true;
        writeDownloadLog(QStringLiteral("network stall timeout, aborting transfer"));
        reply->abort();
    });
    stallTimer.start();

    QTimer totalTimer;
    totalTimer.setSingleShot(true);
    totalTimer.setInterval(kDownloadTotalTimeoutMs);
    QObject::connect(&totalTimer, &QTimer::timeout, reply, [&abortedByTotalTimeout, reply]() {
        abortedByTotalTimeout = true;
        writeDownloadLog(QStringLiteral("network total timeout, aborting transfer"));
        reply->abort();
    });
    totalTimer.start();

    QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [&stallTimer, &context](qint64 received, qint64 total) {
        stallTimer.start();
        if (total > 0) {
            context.reportProgress(
                static_cast<int>((received * 100) / total),
                QStringLiteral("download.bytes"));
        }
    });

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer cancelPoll;
    cancelPoll.setInterval(100);
    QObject::connect(&cancelPoll, &QTimer::timeout, reply, [&]() {
        if (!context.shouldStop() || !reply->isRunning()) {
            return;
        }
        abortedByUserCancel = context.isCancellationRequested();
        abortedByTotalTimeout = context.hasTimedOut();
        reply->abort();
    });
    cancelPoll.start();
    loop.exec();
    cancelPoll.stop();
    stallTimer.stop();
    totalTimer.stop();

    if (reply->error() != QNetworkReply::NoError) {
        const bool shouldTryFallback =
            !abortedByUserCancel &&
            (abortedByStallTimeout ||
             abortedByTotalTimeout ||
             reply->error() == QNetworkReply::OperationCanceledError);

        if (shouldTryFallback) {
            writeDownloadLog(QStringLiteral("network fallback via powershell: %1").arg(url));
            QByteArray fallbackData;
            QString fallbackError;
            if (downloadViaPowerShell(url, proxy, &fallbackData, &fallbackError, context)) {
                *out = fallbackData;
                writeDownloadLog(QStringLiteral("network fallback ok: %1 bytes from %2").arg(out->size()).arg(url));
                reply->deleteLater();
                return true;
            }
            writeDownloadLog(QStringLiteral("network fallback failed: %1").arg(fallbackError));
        }

        if (error) {
            QString reason = reply->errorString();
            if (abortedByUserCancel) {
                reason = QStringLiteral("User canceled");
            } else if (abortedByStallTimeout) {
                reason = QStringLiteral("Download stalled");
            } else if (abortedByTotalTimeout) {
                reason = QStringLiteral("Download timed out");
            }
            *error = QStringLiteral("Download failed: %1\nURL: %2").arg(reason, url);
        }
        writeDownloadLog(QStringLiteral("network error: %1").arg(reply->errorString()));
        reply->deleteLater();
        return false;
    }

    *out = reply->readAll();
    writeDownloadLog(QStringLiteral("network ok: %1 bytes from %2").arg(out->size()).arg(url));
    reply->deleteLater();
    return true;
}

JobOutcome runBlockingDownloadJob(
    const QString& id,
    const QString& label,
    QWidget* parentWidget,
    JobRunner::Worker worker)
{
    std::unique_ptr<QProgressDialog> dialog;
    if (parentWidget) {
        dialog = std::make_unique<QProgressDialog>(
            label, QObject::tr("取消"), 0, 100, parentWidget);
        dialog->setWindowModality(Qt::WindowModal);
        dialog->setMinimumDuration(0);
        dialog->setAutoClose(false);
        dialog->setAutoReset(false);
    }

    JobOutcome outcome = JobOutcome::failure(
        QStringLiteral("download.incomplete"),
        QStringLiteral("Download did not complete"));
    QEventLoop loop;
    JobHandle* handle = JobRunner::start(id, &loop, kDownloadTotalTimeoutMs, std::move(worker));
    if (dialog) {
        QObject::connect(handle, &JobHandle::progressChanged, dialog.get(),
            [dialogPtr = dialog.get(), label](int percent, const QString&) {
                dialogPtr->setRange(0, 100);
                dialogPtr->setValue(percent);
                dialogPtr->setLabelText(label);
            });
        QObject::connect(dialog.get(), &QProgressDialog::canceled, handle, &JobHandle::cancel);
        dialog->show();
    }
    QObject::connect(handle, &JobHandle::finished, &loop,
        [&outcome, &loop](const JobOutcome& result) {
            outcome = result;
            loop.quit();
        });

    loop.exec();
    if (dialog) {
        dialog->close();
    }
    return outcome;
}

} // namespace

DownloadService& DownloadService::instance()
{
    static DownloadService service;
    return service;
}

bool DownloadService::downloadBytesFromUrls(
    const QStringList& urls,
    QByteArray* out,
    QWidget* parentWidget,
    QString* error)
{
    if (!out) {
        if (error) {
            *error = QStringLiteral("Download buffer is null");
        }
        return false;
    }

    QStringList cleanedUrls;
    for (const QString& url : urls) {
        const QString trimmed = url.trimmed();
        if (!trimmed.isEmpty() && !cleanedUrls.contains(trimmed)) {
            cleanedUrls.push_back(trimmed);
        }
    }

    if (cleanedUrls.isEmpty()) {
        if (error) {
            *error = QObject::tr("没有可用的下载地址。");
        }
        return false;
    }

    QByteArray downloadedData;
    const JobOutcome outcome = runBlockingDownloadJob(
        QStringLiteral("component.download.bytes"),
        QObject::tr("正在下载组件..."),
        parentWidget,
        [cleanedUrls, &downloadedData](JobContext& context) {
            QStringList errors;
            for (const QString& url : cleanedUrls) {
                if (context.shouldStop()) {
                    return context.isCancellationRequested()
                        ? JobOutcome::canceled(QStringLiteral("User canceled"))
                        : JobOutcome::timedOut(QStringLiteral("Download timed out"));
                }
                QByteArray data;
                QString downloadError;
                if (downloadBytesInternal(url, &data, &downloadError, context)) {
                    downloadedData = std::move(data);
                    return JobOutcome::success();
                }
                errors.push_back(downloadError);
            }
            return JobOutcome::failure(
                QStringLiteral("download.failed"),
                errors.join(QStringLiteral("\n\n")));
        });

    if (!outcome.succeeded()) {
        if (error) {
            *error = outcome.errorMessage;
        }
        return false;
    }
    *out = std::move(downloadedData);
    return true;
}

bool DownloadService::downloadFileFromUrls(
    const QStringList& urls,
    const QString& destinationPath,
    QWidget* parentWidget,
    QString* error)
{
    QStringList cleanedUrls;
    for (const QString& url : urls) {
        const QString trimmed = url.trimmed();
        if (!trimmed.isEmpty() && !cleanedUrls.contains(trimmed)) {
            cleanedUrls.push_back(trimmed);
        }
    }

    if (cleanedUrls.isEmpty()) {
        if (error) {
            *error = QString::fromUtf8(u8"没有可用的下载地址。");
        }
        return false;
    }

    const JobOutcome outcome = runBlockingDownloadJob(
        QStringLiteral("component.download.file"),
        QObject::tr("正在下载文件..."),
        parentWidget,
        [cleanedUrls, destinationPath](JobContext& context) {
            QStringList errors;
            for (const QString& url : cleanedUrls) {
                if (context.shouldStop()) {
                    return context.isCancellationRequested()
                        ? JobOutcome::canceled(QStringLiteral("User canceled"))
                        : JobOutcome::timedOut(QStringLiteral("Download timed out"));
                }
                QString downloadError;
                if (downloadFileInternal(url, destinationPath, &downloadError, context)) {
                    return JobOutcome::success();
                }
                errors.push_back(downloadError);
            }
            return JobOutcome::failure(
                QStringLiteral("download.failed"),
                errors.join(QStringLiteral("\n\n")));
        });

    if (!outcome.succeeded()) {
        if (error) {
            *error = outcome.errorMessage;
        }
        return false;
    }
    return true;
}

} // namespace cgplay
