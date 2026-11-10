#include "ThumbnailDiskCache.h"
#include "settings/api/ISettingsService.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QElapsedTimer>
#include <QtConcurrent>
#include <atomic>
#include <algorithm>
#include <chrono>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace cgplay {
namespace {
const QString ownerName = QStringLiteral(".cgplay-thumbnail-owner-v1");
const QByteArray ownerValue("CGPlay thumbnail cache\nschema=1\n");
const QRegularExpression filePattern(QStringLiteral("^thumb_[0-9a-f]{40}_f[0-9]{8,10}\\.jpg$"));
bool isReparse(const QString& path)
{
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return QFileInfo(path).isSymLink();
#endif
}
bool ownedRoot(const QString& root)
{
    const QFileInfo info(root);
    if (!info.isDir() || isReparse(root) || info.fileName() != QStringLiteral("cgplay-thumbnails-v1")) return false;
    const QString marker = QDir(root).filePath(ownerName);
    if (isReparse(marker)) return false;
    QFile file(marker);
    return file.open(QIODevice::ReadOnly) && file.size() == ownerValue.size() && file.readAll() == ownerValue;
}
bool managedPath(const QString& path)
{
    const QFileInfo info(path);
    return info.isAbsolute() && filePattern.match(info.fileName()).hasMatch() &&
        !isReparse(path) && ownedRoot(info.absolutePath());
}
struct State {
    QMutex mutex;
    ThumbnailDiskCache::Policy policy;
    QHash<QString, std::weak_ptr<ThumbnailDiskCache::Lease>> leases;
    std::atomic_bool pruning{false};
    QString lastPruneRoot;
    quint64 lastPruneQuota = 0;
    std::chrono::steady_clock::time_point lastPrune;
};
// Workers may outlive a settings window. No worker depends on QWidget/QObject
// lifetime, and the global pool drains before the process destroys Qt.
State& state() { static auto* value = new State; return *value; }
} // namespace

class ThumbnailDiskCache::Lease
{
public:
    explicit Lease(QString path) : lock(path + QStringLiteral(".lock"))
    { lock.setStaleLockTime(0); }
    QLockFile lock;
};

QString ThumbnailDiskCache::defaultBaseDirectory()
{
    // Explicit test paths are accepted only in the existing background mode.
    if (QCoreApplication::instance() && QCoreApplication::instance()->property("cgplay.automationBackground").toBool()) {
        const QString testRoot = qEnvironmentVariable("CGPLAY_THUMBNAIL_CACHE_TEST_ROOT");
        if (!testRoot.isEmpty() && QFileInfo(testRoot).isAbsolute()) return QDir::cleanPath(testRoot);
    }
    QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return root.isEmpty() ? QDir::tempPath() + QStringLiteral("/CGPlay") : root;
}

void ThumbnailDiskCache::initialize(const ISettingsService& settings)
{
    Policy value;
    value.baseDirectory = settings.value(QStringLiteral("cache/thumbnails/baseDirectory"), defaultBaseDirectory()).toString();
    value.quotaBytes = std::clamp<quint64>(settings.value(QStringLiteral("cache/thumbnails/quotaBytes"),
        QVariant::fromValue(value.quotaBytes)).toULongLong(), 1024ULL * 1024 * 1024, 64ULL * 1024 * 1024 * 1024);
    configure(value);
    schedulePrune(value);
}

ThumbnailDiskCache::Policy ThumbnailDiskCache::policy()
{
    QMutexLocker locker(&state().mutex);
    Policy result = state().policy;
    if (result.baseDirectory.isEmpty()) {
        result.baseDirectory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (result.baseDirectory.isEmpty()) result.baseDirectory = QDir::tempPath() + QStringLiteral("/CGPlay");
    }
    return result;
}

void ThumbnailDiskCache::configure(const Policy& value)
{
    QMutexLocker locker(&state().mutex);
    state().policy = value;
}

QString ThumbnailDiskCache::namespaceDirectory(const Policy& policy)
{ return QDir(policy.baseDirectory).absoluteFilePath(QStringLiteral("cgplay-thumbnails-v1")); }

bool ThumbnailDiskCache::prepare(Policy& value, QString* error)
{
    auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (!QFileInfo(value.baseDirectory).isAbsolute()) return fail(QStringLiteral("缓存目录必须是绝对路径"));
    if (!QDir().mkpath(value.baseDirectory)) return fail(QStringLiteral("无法创建缓存位置"));
    value.baseDirectory = QFileInfo(value.baseDirectory).canonicalFilePath();
    if (value.baseDirectory.isEmpty()) return fail(QStringLiteral("无法确认缓存位置"));
    QLockFile initialization(QDir(value.baseDirectory).filePath(QStringLiteral(".cgplay-thumbnail-initialize.lock")));
    initialization.setStaleLockTime(0);
    if (!initialization.tryLock(1000)) return fail(QStringLiteral("另一个进程正在准备缓存位置"));
    const QString root = namespaceDirectory(value);
    if (QFileInfo::exists(root)) {
        if (!ownedRoot(root)) return fail(QStringLiteral("同名目录不属于 CGPlay，未接管或清理"));
        return true;
    }
    if (!QDir().mkdir(root)) return fail(QStringLiteral("无法创建缩略图缓存目录"));
    QSaveFile marker(QDir(root).filePath(ownerName));
    if (!marker.open(QIODevice::WriteOnly) || marker.write(ownerValue) != ownerValue.size() || !marker.commit())
        return fail(QStringLiteral("无法写入缓存所属标记；此目录不会被清理"));
    return true;
}

QString ThumbnailDiskCache::path(const Policy& policy, const QByteArray& key, int frame)
{
    static const QRegularExpression keyPattern(QStringLiteral("^[0-9a-f]{40}$"));
    if (!keyPattern.match(QString::fromLatin1(key)).hasMatch() || frame < 0) return {};
    Policy prepared = policy;
    if (!prepare(prepared)) return {};
    return QDir(namespaceDirectory(prepared)).filePath(QStringLiteral("thumb_%1_f%2.jpg")
        .arg(QString::fromLatin1(key)).arg(frame, 8, 10, QLatin1Char('0')));
}

std::shared_ptr<ThumbnailDiskCache::Lease> ThumbnailDiskCache::acquire(
    const QString& path, const std::function<bool()>& canceled)
{
    if (path.isEmpty() || !managedPath(path) || isReparse(path + QStringLiteral(".lock"))) return {};
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 9000 && !(canceled && canceled())) {
        {
            QMutexLocker locker(&state().mutex);
            for (auto it = state().leases.begin(); it != state().leases.end();)
                if (it.value().expired()) it = state().leases.erase(it); else ++it;
            if (auto lease = state().leases.value(path).lock()) return lease;
        }
        auto candidate = std::make_shared<Lease>(path);
        if (candidate->lock.tryLock(0)) {
            QMutexLocker locker(&state().mutex);
            state().leases.insert(path, candidate);
            return candidate;
        }
        QThread::msleep(10);
    }
    return {};
}

void ThumbnailDiskCache::touch(const QString& path)
{
    if (!managedPath(path)) return;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
}

ThumbnailDiskCache::Statistics ThumbnailDiskCache::maintain(
    const Policy& policy, bool clear, const std::function<bool()>& canceled)
{
    Statistics result;
    Policy prepared = policy;
    if (!prepare(prepared, &result.error)) return result;
    const QString root = namespaceDirectory(prepared);
    QLockFile maintenance(QDir(root).filePath(QStringLiteral(".maintenance.lock")));
    maintenance.setStaleLockTime(0);
    if (!maintenance.tryLock(0)) { result.error = QStringLiteral("缓存清理正在进行，请稍后刷新"); return result; }
    struct Item { QString path; quint64 bytes; QDateTime modified; };
    QList<Item> items;
    const auto files = QDir(root).entryInfoList({QStringLiteral("thumb_*.jpg")}, QDir::Files | QDir::NoSymLinks, QDir::NoSort);
    for (const auto& info : files) {
        if (canceled && canceled()) { result.canceled = true; return result; }
        if (!filePattern.match(info.fileName()).hasMatch() || isReparse(info.absoluteFilePath())) continue;
        items.push_back({info.absoluteFilePath(), static_cast<quint64>(std::max<qint64>(0, info.size())), info.lastModified()});
        result.bytes += items.back().bytes;
        ++result.files;
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.modified < b.modified; });
    const quint64 target = clear ? 0 : policy.quotaBytes;
    for (const auto& item : items) {
        if (result.bytes <= target) break;
        if (canceled && canceled()) { result.canceled = true; return result; }
        if (!managedPath(item.path) || isReparse(item.path + QStringLiteral(".lock"))) continue;
        QLockFile lease(item.path + QStringLiteral(".lock"));
        lease.setStaleLockTime(0);
        if (!lease.tryLock(0)) { ++result.activeFiles; continue; }
        // A hit may have occurred after enumeration. Skip it until the next
        // sweep rather than evicting an entry that just became most-recent.
        const QFileInfo current(item.path);
        if (!current.isFile() || isReparse(item.path)) {
            result.bytes -= item.bytes;
            --result.files;
            continue;
        }
        const quint64 currentBytes = static_cast<quint64>(std::max<qint64>(0, current.size()));
        result.bytes = result.bytes - item.bytes + currentBytes;
        if (!clear && current.lastModified() > item.modified) continue;
        if (isReparse(item.path) || !QFile::remove(item.path)) continue;
        result.bytes -= currentBytes;
        result.removedBytes += currentBytes;
        --result.files;
        ++result.removedFiles;
    }
    return result;
}

void ThumbnailDiskCache::schedulePrune(const Policy& policy)
{
    {
        QMutexLocker locker(&state().mutex);
        const auto now = std::chrono::steady_clock::now();
        // A large cache must not be enumerated once per decoded thumbnail.
        if (state().pruning.load() || (state().lastPruneRoot == policy.baseDirectory &&
            state().lastPruneQuota == policy.quotaBytes && now - state().lastPrune < std::chrono::seconds(30))) return;
        state().pruning.store(true);
        state().lastPruneRoot = policy.baseDirectory;
        state().lastPruneQuota = policy.quotaBytes;
        state().lastPrune = now;
    }
    (void)QtConcurrent::run([policy] {
        struct Reset { ~Reset() { state().pruning.store(false); } } reset;
        maintain(policy);
    });
}
} // namespace cgplay
