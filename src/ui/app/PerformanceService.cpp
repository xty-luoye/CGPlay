#include "PerformanceService.h"

#include "TopBar.h"
#include "common/core/ServiceLocator.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"
#include "cache/CacheManager.h"
#include "core/SystemPerformanceMonitor.h"
#include "ocio/OcioManager.h"
#include "playback/api/IPlaybackService.h"
#include "playback/api/PlaybackServiceSignals.h"
#include "playback/PlaybackStats.h"

#include <QLabel>
#include <QtGlobal>

namespace cgplay {

namespace {

void setLabelTextIfChanged(QLabel* label, const QString& text)
{
    if (label && label->text() != text) {
        label->setText(text);
    }
}

void setLabelStyleSheetIfChanged(QLabel* label, const QString& style)
{
    if (label && label->styleSheet() != style) {
        label->setStyleSheet(style);
    }
}

} // namespace

PerformanceService::PerformanceService(QObject* parent)
    : QObject(parent)
    , _perfMonitor(new SystemPerformanceMonitor(this))
{
}

PerformanceService::~PerformanceService() = default;

void PerformanceService::bind(
    IPlaybackService* playbackCtrl,
    CacheManager* cacheManager,
    OcioManager* ocioManager,
    const Widgets& widgets)
{
    _playbackCtrl = playbackCtrl;
    _cacheManager = cacheManager;
    _ocioManager = ocioManager;
    _widgets = widgets;

    _bindPlayback();
    _bindCache();
    _bindOcio();
    _bindSystemMonitor();

    if (_widgets.topBar) {
        if (auto* codecLabel = _widgets.topBar->codecLabel()) {
            setLabelTextIfChanged(codecLabel, _widgets.codec ? _widgets.codec->text() : QStringLiteral("--"));
        }
        if (auto* decoderLabel = _widgets.topBar->decoderLabel()) {
            setLabelTextIfChanged(decoderLabel, QStringLiteral("--"));
        }
        if (auto* fpsLabel = _widgets.topBar->fpsLabel()) {
            setLabelTextIfChanged(fpsLabel, _widgets.fps ? _widgets.fps->text() : QStringLiteral("-- FPS"));
        }
        if (auto* resolutionLabel = _widgets.topBar->resolutionLabel()) {
            setLabelTextIfChanged(resolutionLabel, _widgets.resolution ? _widgets.resolution->text() : QStringLiteral("--"));
        }
    }
}

void PerformanceService::applyMediaInfo(const MediaInfo& mediaInfo, IPlaybackService* playbackCtrl)
{
    _lastMediaInfo = mediaInfo;
    _hasMediaInfo = true;
    const double fps = playbackCtrl && playbackCtrl->fps() > 0.0
        ? playbackCtrl->fps()
        : mediaInfo.fps;
    const QString codecText = mediaInfo.codecDisplayText();
    const QString resText = mediaInfo.resolutionText();
    const QString bitrateText = QStringLiteral("码率: %1").arg(mediaInfo.bitrateText(_bitrateDisplayUnit));
    const QString fpsText = formatFpsText(fps);

    setLabelTextIfChanged(_widgets.codec, codecText);
    setLabelTextIfChanged(_widgets.resolution, resText);
    setLabelTextIfChanged(_widgets.bitrate, bitrateText);
    setLabelTextIfChanged(_widgets.fps, fpsText);
    if (_widgets.topBar) {
        if (auto* codecLabel = _widgets.topBar->codecLabel()) {
            setLabelTextIfChanged(codecLabel, codecText);
        }
        if (auto* resolutionLabel = _widgets.topBar->resolutionLabel()) {
            setLabelTextIfChanged(resolutionLabel, resText);
        }
        if (auto* fpsLabel = _widgets.topBar->fpsLabel()) {
            setLabelTextIfChanged(fpsLabel, formatFpsText(fps, 0));
        }
    }
}

void PerformanceService::resetMediaInfo()
{
    _hasMediaInfo = false;
    _lastMediaInfo = MediaInfo{};
    setLabelTextIfChanged(_widgets.codec, QStringLiteral("--"));
    setLabelTextIfChanged(_widgets.resolution, QStringLiteral("--"));
    setLabelTextIfChanged(_widgets.bitrate, QStringLiteral("码率: --"));
    setLabelTextIfChanged(_widgets.fps, QStringLiteral("-- FPS"));
    setLabelTextIfChanged(_widgets.dropped, QStringLiteral("Dropped:0"));
    setLabelTextIfChanged(_widgets.cache, QStringLiteral("Cache:0.0%"));
    if (_widgets.topBar) {
        if (auto* codecLabel = _widgets.topBar->codecLabel()) {
            setLabelTextIfChanged(codecLabel, QStringLiteral("--"));
        }
        if (auto* resolutionLabel = _widgets.topBar->resolutionLabel()) {
            setLabelTextIfChanged(resolutionLabel, QStringLiteral("--"));
        }
        if (auto* fpsLabel = _widgets.topBar->fpsLabel()) {
            setLabelTextIfChanged(fpsLabel, QStringLiteral("-- FPS"));
        }
    }
}

void PerformanceService::setBitrateDisplayUnit(const QString& unit)
{
    const QString normalized = unit.trimmed().toLower();
    const QString next = normalized == QStringLiteral("mbps")
        ? QStringLiteral("mbps")
        : normalized == QStringLiteral("kbps")
            ? QStringLiteral("kbps")
            : QStringLiteral("auto");
    if (_bitrateDisplayUnit == next) {
        return;
    }
    _bitrateDisplayUnit = next;
    if (_hasMediaInfo) {
        setLabelTextIfChanged(
            _widgets.bitrate,
            QStringLiteral("码率: %1").arg(_lastMediaInfo.bitrateText(_bitrateDisplayUnit)));
    }
}

QString PerformanceService::bitrateDisplayUnit() const
{
    return _bitrateDisplayUnit;
}

QString PerformanceService::formatPercentLabel(const QString& prefix, double value)
{
    const double clamped = qBound(0.0, value, 100.0);
    const int decimals = clamped < 10.0 ? 1 : 0;
    return QStringLiteral("%1:%2%")
        .arg(prefix)
        .arg(clamped, 0, 'f', decimals);
}

QString PerformanceService::formatFpsText(double fps, int decimals)
{
    return fps > 0.0
        ? QStringLiteral("%1 FPS").arg(fps, 0, 'f', decimals)
        : QStringLiteral("-- FPS");
}

void PerformanceService::_bindPlayback()
{
    if (!_playbackCtrl) {
        return;
    }

    if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
        eventBus->subscribe<PlaybackStatsUpdatedEvent>([this](const PlaybackStatsUpdatedEvent& event) {
            setLabelTextIfChanged(_widgets.dropped, QStringLiteral("Dropped:%1").arg(event.droppedFrames));
            if (event.currentFps > 0.0) {
                setLabelTextIfChanged(_widgets.fps, formatFpsText(event.currentFps));
            }
            if (_widgets.topBar && event.currentFps > 0.0) {
                setLabelTextIfChanged(_widgets.topBar->fpsLabel(), formatFpsText(event.currentFps, 0));
            }
        });
    }

    if (auto* playbackSignals = _playbackCtrl->signalProxy()) {
        connect(playbackSignals, &PlaybackServiceSignals::fpsChanged, this, [this](double fps) {
            setLabelTextIfChanged(_widgets.fps, formatFpsText(fps));
            if (_widgets.topBar) {
                setLabelTextIfChanged(_widgets.topBar->fpsLabel(), formatFpsText(fps, 0));
            }
        });
        connect(playbackSignals, &PlaybackServiceSignals::playbackStateChanged, this, [this](int state) {
            if (_perfMonitor) {
                _perfMonitor->start(state == 0 ? 2000 : 1000);
            }
        });
    }
}

void PerformanceService::_bindCache()
{
    if (_cacheManager) {
        connect(_cacheManager, &CacheManager::cacheUpdated, this, [this](float videoPercent, float) {
            _updateCacheUsage(videoPercent);
        });
    }
}

void PerformanceService::_bindOcio()
{
    if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
        eventBus->subscribe<OcioDisplayStateChangedEvent>([this](const OcioDisplayStateChangedEvent& event) {
            if (!_widgets.ocio) {
                return;
            }
            const QString text = event.enabled
                ? (event.display.trimmed().isEmpty() ? QStringLiteral("OCIO") : event.display)
                : QStringLiteral("OCIO: Off");
            const QString color = event.enabled ? QStringLiteral("#FF8A3D") : QStringLiteral("#9AA4B2");
            setLabelTextIfChanged(_widgets.ocio, text);
            setLabelStyleSheetIfChanged(_widgets.ocio, QStringLiteral(
                "QLabel{color:%1;font-size:11px;padding:4px 8px;background:#111418;"
                "border:1px solid rgba(255,255,255,0.055);border-radius:6px;}")
                .arg(color));
        });
    }

    if (_ocioManager) {
        connect(_ocioManager, &OcioManager::displayChanged, this, [this](const QString&) {
            _updateOcioBadge();
        });
        connect(_ocioManager, &OcioManager::enabledChanged, this, [this](bool) {
            _updateOcioBadge();
        });
        connect(_ocioManager, &OcioManager::configLoaded, this, [this](const QString&) {
            _updateOcioBadge();
        });
    }
    _updateOcioBadge();
}

void PerformanceService::_bindSystemMonitor()
{
    if (!_perfMonitor) {
        return;
    }

    connect(_perfMonitor, &SystemPerformanceMonitor::sampleReady, this,
            [this](double cpuPercent, double gpuPercent, double memoryPercent) {
                _updateSystemSample(cpuPercent, gpuPercent, memoryPercent);
            });
    _perfMonitor->start(2000);
}

void PerformanceService::_updateOcioBadge()
{
    if (!_widgets.ocio || !_ocioManager) {
        return;
    }

    const bool enabled = _ocioManager->isEnabled();
    const QString display = _ocioManager->currentDisplay().trimmed();
    const QString text = enabled
        ? (display.isEmpty() ? QStringLiteral("OCIO") : display)
        : QStringLiteral("OCIO: Off");
    const QString color = enabled ? QStringLiteral("#FF8A3D") : QStringLiteral("#9AA4B2");

    setLabelTextIfChanged(_widgets.ocio, text);
    setLabelStyleSheetIfChanged(_widgets.ocio, QStringLiteral(
        "QLabel{color:%1;font-size:11px;padding:4px 8px;background:#111418;"
        "border:1px solid rgba(255,255,255,0.055);border-radius:6px;}")
        .arg(color));
}

void PerformanceService::_updateDroppedFrames()
{
    if (!_widgets.dropped || !_playbackCtrl) {
        return;
    }

    if (auto* stats = _playbackCtrl->playbackStats()) {
        setLabelTextIfChanged(_widgets.dropped, QStringLiteral("Dropped:%1").arg(stats->droppedFrames()));
    }
}

void PerformanceService::_updateCacheUsage(float videoPercent)
{
    setLabelTextIfChanged(_widgets.cache, formatPercentLabel(QStringLiteral("Cache"), videoPercent));
}

void PerformanceService::_updateSystemSample(double cpuPercent, double gpuPercent, double memoryPercent)
{
    setLabelTextIfChanged(_widgets.cpu, formatPercentLabel(QStringLiteral("CPU"), cpuPercent));
    setLabelTextIfChanged(_widgets.gpu, formatPercentLabel(QStringLiteral("GPU"), gpuPercent));
    setLabelTextIfChanged(_widgets.memory, formatPercentLabel(QStringLiteral("Memory"), memoryPercent));
}

} // namespace cgplay
