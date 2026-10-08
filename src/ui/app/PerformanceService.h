#pragma once

#include "media/MediaProbe.h"

#include <QObject>
#include <QString>

class QLabel;

namespace cgplay {

class CacheManager;
class OcioManager;
class IPlaybackService;
class SystemPerformanceMonitor;
class TopBar;

class PerformanceService : public QObject
{
    Q_OBJECT
public:
    struct Widgets
    {
        QLabel* codec = nullptr;
        QLabel* resolution = nullptr;
        QLabel* bitrate = nullptr;
        QLabel* fps = nullptr;
        QLabel* dropped = nullptr;
        QLabel* cache = nullptr;
        QLabel* cpu = nullptr;
        QLabel* gpu = nullptr;
        QLabel* memory = nullptr;
        QLabel* ocio = nullptr;
        TopBar* topBar = nullptr;
    };

    explicit PerformanceService(QObject* parent = nullptr);
    ~PerformanceService() override;

    void bind(
        IPlaybackService* playbackCtrl,
        CacheManager* cacheManager,
        OcioManager* ocioManager,
        const Widgets& widgets);

    void applyMediaInfo(const MediaInfo& mediaInfo, IPlaybackService* playbackCtrl);
    void resetMediaInfo();
    void setBitrateDisplayUnit(const QString& unit);
    QString bitrateDisplayUnit() const;

    static QString formatPercentLabel(const QString& prefix, double value);
    static QString formatFpsText(double fps, int decimals = 1);

private:
    void _bindPlayback();
    void _bindCache();
    void _bindOcio();
    void _bindSystemMonitor();
    void _updateOcioBadge();
    void _updateDroppedFrames();
    void _updateCacheUsage(float videoPercent);
    void _updateSystemSample(double cpuPercent, double gpuPercent, double memoryPercent);

    Widgets _widgets;
    IPlaybackService* _playbackCtrl = nullptr;
    CacheManager* _cacheManager = nullptr;
    OcioManager* _ocioManager = nullptr;
    SystemPerformanceMonitor* _perfMonitor = nullptr;
    MediaInfo _lastMediaInfo;
    bool _hasMediaInfo = false;
    QString _bitrateDisplayUnit = QStringLiteral("auto");
};

} // namespace cgplay
