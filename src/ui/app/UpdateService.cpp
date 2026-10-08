#include "UpdateService.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <array>
#include <cmath>
#include <string>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace cgplay {
namespace {
constexpr qint64 kMaximumInstallerSize = 4LL * 1024 * 1024 * 1024;
constexpr qint64 kMaximumMetadataSize = 1024 * 1024;

bool versionParts(const QString& version, std::array<quint64, 4>& parts)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9]+(?:\\.[0-9]+){2,3}$"));
    if (!pattern.match(version).hasMatch()) return false;
    parts.fill(0);
    const auto fields = version.split(QLatin1Char('.'));
    for (qsizetype i = 0; i < fields.size(); ++i) {
        bool ok = false;
        parts[size_t(i)] = fields[i].toULongLong(&ok);
        if (!ok) return false;
    }
    return true;
}

bool validHash(const QString& hash)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
    return pattern.match(hash).hasMatch();
}

QString installerName(const QString& version)
{
    return QStringLiteral("CGPlay_Setup_%1_full.exe").arg(version);
}

QString installerUrl(const QString& version)
{
    return QStringLiteral("https://github.com/xty-luoye/CGPlay/releases/download/v%1/%2")
        .arg(version, installerName(version));
}

bool validInstaller(const UpdateCheckResult& release)
{
    std::array<quint64, 4> remote{}, current{};
    return release.refreshed && release.updateAvailable &&
        versionParts(release.remoteVersion, remote) && versionParts(release.currentVersion, current) &&
        remote > current && release.installerUrl == installerUrl(release.remoteVersion) &&
        validHash(release.sha256) && release.installerSize > 0 &&
        release.installerSize <= kMaximumInstallerSize;
}

struct TransferResult {
    QByteArray body;
    QString error;
    qint64 received = 0;
};

TransferResult transfer(const QUrl& url, const std::shared_ptr<UpdateTransferState>& state,
    int totalTimeoutMs, qint64 maximumBytes, QFile* output = nullptr, QCryptographicHash* hash = nullptr)
{
    TransferResult result;
    if (!url.isValid() || (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http")) ||
        !url.userInfo().isEmpty()) {
        result.error = QStringLiteral("更新地址无效。");
        return result;
    }
    if (state) {
        state->received.store(0);
        if (state->cancelled.load()) {
            result.error = QStringLiteral("已取消。");
            return result;
        }
    }
    // Construct the network stack in this worker, never on the GUI thread.
    QNetworkAccessManager network;
    const QString host = url.host().toLower();
    if (host == QStringLiteral("127.0.0.1") || host == QStringLiteral("localhost") || host == QStringLiteral("::1")) {
        network.setProxy(QNetworkProxy::NoProxy);
    } else {
        const auto proxies = QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(url));
        if (!proxies.isEmpty()) network.setProxy(proxies.first());
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("CGPlay-Updater"));
    request.setRawHeader("Accept", output ? "application/octet-stream" : "application/vnd.github+json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(5);
    request.setTransferTimeout(30000);
    QEventLoop loop;
    QElapsedTimer totalClock, progressClock;
    totalClock.start();
    progressClock.start();
    auto* reply = network.get(request);
    reply->setReadBufferSize(256 * 1024);
    const auto fail = [&](const QString& error) {
        if (result.error.isEmpty()) result.error = error;
        reply->abort();
        loop.quit();
    };
    const auto drain = [&] {
        if (!result.error.isEmpty()) return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 300 && status < 400) {
            // Redirect response bodies are not part of the installer.
            while (reply->bytesAvailable()) reply->read(64 * 1024);
            return;
        }
        if (status >= 400) {
            fail(QStringLiteral("更新服务器返回 HTTP %1。").arg(status));
            return;
        }
        while (reply->bytesAvailable() > 0) {
            const QByteArray chunk = reply->read(64 * 1024);
            if (chunk.isEmpty()) break;
            if (result.received > maximumBytes - chunk.size()) {
                fail(QStringLiteral("下载数据超过声明的大小限制。"));
                return;
            }
            if (output && output->write(chunk) != chunk.size()) {
                fail(QStringLiteral("无法完整写入安装包：%1").arg(output->errorString()));
                return;
            }
            if (hash) hash->addData(chunk);
            if (!output) result.body.append(chunk);
            result.received += chunk.size();
            progressClock.restart();
            if (state) state->received.store(result.received);
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, drain);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer cancellation;
    cancellation.setInterval(50);
    QObject::connect(&cancellation, &QTimer::timeout, &loop, [&] {
        if (state && state->cancelled.load()) fail(QStringLiteral("已取消。"));
        else if (totalClock.elapsed() >= totalTimeoutMs) fail(QStringLiteral("更新请求超时。"));
        else if (progressClock.elapsed() >= 30000) fail(QStringLiteral("下载已超过 30 秒没有进展。"));
    });
    cancellation.start();
    if (!reply->isFinished()) loop.exec();
    cancellation.stop();
    drain();
    if (result.error.isEmpty() && state && state->cancelled.load()) result.error = QStringLiteral("已取消。");
    if (result.error.isEmpty() && reply->error() != QNetworkReply::NoError)
        result.error = QStringLiteral("更新请求失败：%1").arg(reply->errorString());
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (result.error.isEmpty() && (status < 200 || status >= 300))
        result.error = QStringLiteral("更新服务器返回 HTTP %1。").arg(status);
    // The reply and manager are destroyed here, on their creating thread.
    delete reply;
    return result;
}

QString psQuoted(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}
} // namespace

QJsonObject UpdateCheckResult::toJson() const
{
    return {{"refreshed", refreshed}, {"updateAvailable", updateAvailable},
        {"currentVersion", currentVersion}, {"remoteVersion", remoteVersion},
        {"installerUrl", installerUrl}, {"sha256", sha256}, {"installerSize", double(installerSize)},
        {"releaseNotes", releaseNotes}, {"error", error}};
}

UpdateCheckResult UpdateCheckResult::fromJson(const QJsonObject& json)
{
    UpdateCheckResult result;
    result.refreshed = json.value("refreshed").toBool();
    result.updateAvailable = json.value("updateAvailable").toBool();
    result.currentVersion = json.value("currentVersion").toString();
    result.remoteVersion = json.value("remoteVersion").toString();
    result.installerUrl = json.value("installerUrl").toString();
    result.sha256 = json.value("sha256").toString();
    const double size = json.value("installerSize").toDouble();
    if (std::isfinite(size) && size > 0 && size <= kMaximumInstallerSize && std::floor(size) == size)
        result.installerSize = qint64(size);
    result.releaseNotes = json.value("releaseNotes").toString();
    result.error = json.value("error").toString();
    return result;
}

UpdateService& UpdateService::instance()
{
    static UpdateService service;
    return service;
}

QUrl UpdateService::latestReleaseUrl()
{
    return QUrl(QStringLiteral("https://api.github.com/repos/xty-luoye/CGPlay/releases/latest"));
}

UpdateCheckResult UpdateService::parseRelease(const QByteArray& json, const QString& currentVersion)
{
    UpdateCheckResult result;
    result.currentVersion = currentVersion;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = QStringLiteral("更新服务器返回了无效的发布信息。");
        return result;
    }
    const auto release = document.object();
    if (!release.value("draft").isBool() || release.value("draft").toBool() ||
        !release.value("prerelease").isBool() || release.value("prerelease").toBool()) {
        result.error = QStringLiteral("更新信息不是正式发布版本。");
        return result;
    }
    const QString tag = release.value("tag_name").toString();
    result.remoteVersion = tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : QString();
    std::array<quint64, 4> remote{}, current{};
    if (!versionParts(currentVersion, current) || !versionParts(result.remoteVersion, remote)) {
        result.error = QStringLiteral("更新版本号无效。");
        return result;
    }
    result.releaseNotes = release.value("body").toString();
    if (remote <= current) {
        result.refreshed = true;
        return result;
    }
    QJsonObject installer;
    int matchingAssets = 0;
    for (const auto& value : release.value("assets").toArray()) {
        const auto asset = value.toObject();
        if (asset.value("name").toString() == installerName(result.remoteVersion)) {
            installer = asset;
            ++matchingAssets;
        }
    }
    const QString digest = installer.value("digest").toString();
    const double size = installer.value("size").toDouble();
    if (matchingAssets != 1 || installer.value("state").toString() != QStringLiteral("uploaded") ||
        !std::isfinite(size) || size <= 0 || size > kMaximumInstallerSize || std::floor(size) != size ||
        !digest.startsWith(QStringLiteral("sha256:")) || !validHash(digest.mid(7)) ||
        installer.value("browser_download_url").toString() != installerUrl(result.remoteVersion)) {
        result.error = QStringLiteral("新版尚无有效的完整安装包或 SHA-256 校验信息，请稍后重试。");
        return result;
    }
    result.installerUrl = installer.value("browser_download_url").toString();
    result.installerSize = qint64(size);
    result.sha256 = digest.mid(7).toLower();
    result.refreshed = true;
    result.updateAvailable = true;
    return result;
}

UpdateCheckResult UpdateService::checkForUpdates(const QString& currentVersion,
    const std::shared_ptr<UpdateTransferState>& state, const QUrl& endpoint)
{
    const auto response = transfer(endpoint, state, 20000, kMaximumMetadataSize);
    if (response.error.isEmpty()) return parseRelease(response.body, currentVersion);
    UpdateCheckResult result;
    result.currentVersion = currentVersion;
    result.error = response.error;
    return result;
}

UpdateInstallResult UpdateService::downloadInstaller(const UpdateCheckResult& release,
    const std::shared_ptr<UpdateTransferState>& state, const QUrl& transportOverride, const QString& temporaryRoot)
{
    UpdateInstallResult result;
    if (!validInstaller(release)) {
        result.error = QStringLiteral("安装包信息无效，请重新检查更新。");
        return result;
    }
    const QString root = temporaryRoot.isEmpty() ? QDir::tempPath() : temporaryRoot;
    QTemporaryDir temporary(QDir(root).filePath(QStringLiteral("CGPlay-update-XXXXXX")));
    if (!temporary.isValid()) {
        result.error = QStringLiteral("无法创建更新临时目录。");
        return result;
    }
    const QString path = QDir(temporary.path()).filePath(installerName(release.remoteVersion));
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("无法创建安装包文件：%1").arg(output.errorString());
        return result;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto response = transfer(transportOverride.isEmpty() ? QUrl(release.installerUrl) : transportOverride,
        state, 30 * 60 * 1000, release.installerSize, &output, &hash);
    result.error = response.error;
    if (result.error.isEmpty() && !output.flush()) result.error = QStringLiteral("安装包写入磁盘失败。");
    output.close();
    if (result.error.isEmpty() && response.received != release.installerSize)
        result.error = QStringLiteral("安装包大小不一致，下载未完成。");
    const QString actualHash = QString::fromLatin1(hash.result().toHex());
    if (result.error.isEmpty() && actualHash.compare(release.sha256, Qt::CaseInsensitive) != 0)
        result.error = QStringLiteral("安装包 SHA-256 校验失败，请重新下载。");
    if (result.error.isEmpty() && state && state->cancelled.load()) result.error = QStringLiteral("已取消。");
    if (!result.error.isEmpty()) return result;
    temporary.setAutoRemove(false);
    result.downloaded = true;
    result.installerPath = path;
    result.sha256 = actualHash;
    return result;
}

QString UpdateService::installerLaunchScript(const UpdateInstallResult& installer, qint64 processId)
{
    if (!installer.downloaded || installer.installerPath.isEmpty() || !validHash(installer.sha256) || processId <= 0)
        return {};
    const QString path = psQuoted(QDir::toNativeSeparators(QFileInfo(installer.installerPath).absoluteFilePath()));
    return QStringLiteral(
        "$ErrorActionPreference = 'Stop'\n"
        "$player = Get-Process -Id %1 -ErrorAction SilentlyContinue\n"
        "if ($null -ne $player) {\n"
        "  try { $player | Wait-Process -Timeout 120 -ErrorAction Stop } catch { exit 1 }\n"
        "}\n"
        "try {\n"
        "  $actual = (Get-FileHash -LiteralPath %2 -Algorithm SHA256 -ErrorAction Stop).Hash\n"
        "  if ($actual -cne '%3') { exit 1 }\n"
        "  Start-Process -FilePath %2 -ErrorAction Stop\n"
        "} catch { exit 1 }\n")
        .arg(processId).arg(path, installer.sha256.toUpper());
}

bool UpdateService::launchInstaller(const UpdateInstallResult& installer, QString* error)
{
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread())
        return fail(QStringLiteral("必须从主界面确认并启动更新。"));
    if (!installer.downloaded || !QFileInfo(installer.installerPath).isFile())
        return fail(QStringLiteral("已验证的安装包不存在。"));
    const QString script = installerLaunchScript(installer, QCoreApplication::applicationPid());
    if (script.isEmpty()) return fail(QStringLiteral("已验证的安装包信息无效。"));
#ifdef Q_OS_WIN
    // PowerShell -EncodedCommand is UTF-16LE. No file path is interpolated
    // into cmd.exe or into the native command line.
    QByteArray utf16;
    utf16.reserve(script.size() * 2);
    for (const QChar ch : script) {
        const ushort value = ch.unicode();
        utf16.append(char(value & 0xff));
        utf16.append(char(value >> 8));
    }
    wchar_t systemDirectory[MAX_PATH]{};
    const UINT systemLength = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (!systemLength || systemLength >= MAX_PATH) return fail(QStringLiteral("无法定位系统更新启动程序。"));
    const QString powershell = QString::fromWCharArray(systemDirectory) +
        QStringLiteral("\\WindowsPowerShell\\v1.0\\powershell.exe");
    QString command = QStringLiteral("\"%1\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand %2")
        .arg(powershell, QString::fromLatin1(utf16.toBase64()));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command.toStdWString();
    if (!CreateProcessW(reinterpret_cast<LPCWSTR>(powershell.utf16()), mutableCommand.data(),
            nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return fail(QStringLiteral("无法启动更新程序（系统错误 %1）。").arg(GetLastError()));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    return fail(QStringLiteral("当前平台不支持 Windows 自动安装。"));
#endif
}
} // namespace cgplay
