#pragma once

#include <QString>
#include <QtGlobal>
#include <functional>
#include <memory>

namespace cgplay {
class ISettingsService;

// Only JPEGs in our marked namespace are managed. Transcodes and user media
// never participate. All filesystem methods below run on a worker.
class ThumbnailDiskCache
{
public:
    struct Policy {
        QString baseDirectory;
        quint64 quotaBytes = 2ULL * 1024 * 1024 * 1024;
    };
    struct Statistics {
        quint64 bytes = 0;
        quint64 removedBytes = 0;
        int files = 0;
        int removedFiles = 0;
        int activeFiles = 0;
        bool canceled = false;
        QString error;
    };
    class Lease;

    static void initialize(const ISettingsService& settings);
    static Policy policy();
    static void configure(const Policy& policy);
    static QString defaultBaseDirectory();
    static QString namespaceDirectory(const Policy& policy);
    static bool prepare(Policy& policy, QString* error = nullptr);
    static QString path(const Policy& policy, const QByteArray& key, int frame);
    static std::shared_ptr<Lease> acquire(
        const QString& path, const std::function<bool()>& canceled = {});
    static void touch(const QString& path);
    static Statistics maintain(const Policy& policy, bool clear = false,
        const std::function<bool()>& canceled = {});
    static void schedulePrune(const Policy& policy);
};
} // namespace cgplay
