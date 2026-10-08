// CGPlay PlaybackStats.cpp

#include "PlaybackStats.h"

#include "common/core/ServiceLocator.h"
#include "common/events/api/EventTypes.h"
#include "common/events/api/IEventBus.h"

#include <QDateTime>
#include <QFile>
#include <QTextStream>

#include <algorithm>
#include <numeric>

namespace cgplay {

namespace {

constexpr auto kPlaybackStatsPublishInterval = std::chrono::milliseconds(125);

}

PlaybackStats::PlaybackStats(QObject* parent)
    : QObject(parent)
{
    const auto now = std::chrono::steady_clock::now();
    _lastFrameTime = now;
    _lastPublishTime = now - kPlaybackStatsPublishInterval;
}

PlaybackStats::~PlaybackStats() = default;

void PlaybackStats::recordFrame(const FrameStat& stat)
{
    _totalFrames++;
    _totalDecodeMs += stat.decodeMs;
    _totalRenderMs += stat.renderMs;

    if (stat.dropped) {
        _droppedFrames++;
    }
    if (stat.hwDecoded) {
        _hwFrames++;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<double, std::milli>(now - _lastFrameTime).count();

    if (elapsed > 0.1) {
        const double fps = 1000.0 / elapsed;
        _fpsWindow.push_back(fps);
        if (_fpsWindow.size() > _fpsWindowMax) {
            _fpsWindow.pop_front();
        }
    }

    _lastFrameTime = now;
    _lastFrameCount++;
    _publishUpdates(false);
}

void PlaybackStats::recordDrop()
{
    _droppedFrames++;
    _publishUpdates(true);
}

void PlaybackStats::reset()
{
    _totalFrames = 0;
    _droppedFrames = 0;
    _hwFrames = 0;
    _totalDecodeMs = 0.0;
    _totalRenderMs = 0.0;
    _fpsWindow.clear();
    _lastFrameCount = 0;

    const auto now = std::chrono::steady_clock::now();
    _lastFrameTime = now;
    _lastPublishTime = now - kPlaybackStatsPublishInterval;
    _publishUpdates(true);
}

double PlaybackStats::currentFps() const
{
    if (_fpsWindow.empty()) {
        return 0.0;
    }

    const size_t count = std::min(_fpsWindow.size(), size_t(10));
    double sum = 0.0;
    auto it = _fpsWindow.rbegin();
    for (size_t i = 0; i < count; ++i, ++it) {
        sum += *it;
    }
    return sum / count;
}

double PlaybackStats::averageFps() const
{
    if (_fpsWindow.empty()) {
        return 0.0;
    }
    const double sum = std::accumulate(_fpsWindow.begin(), _fpsWindow.end(), 0.0);
    return sum / _fpsWindow.size();
}

double PlaybackStats::avgDecodeMs() const
{
    return _totalFrames > 0 ? _totalDecodeMs / _totalFrames : 0.0;
}

double PlaybackStats::avgRenderMs() const
{
    return _totalFrames > 0 ? _totalRenderMs / _totalFrames : 0.0;
}

double PlaybackStats::dropRate() const
{
    return _totalFrames > 0 ? 100.0 * _droppedFrames / _totalFrames : 0.0;
}

double PlaybackStats::hwDecodeRatio() const
{
    return _totalFrames > 0 ? 100.0 * _hwFrames / _totalFrames : 0.0;
}

PlaybackSummary PlaybackStats::summary() const
{
    PlaybackSummary s;
    s.avgFps = averageFps();
    s.minFps = _fpsWindow.empty() ? 0.0 : *std::min_element(_fpsWindow.begin(), _fpsWindow.end());
    s.maxFps = _fpsWindow.empty() ? 0.0 : *std::max_element(_fpsWindow.begin(), _fpsWindow.end());
    s.avgDecodeMs = avgDecodeMs();
    s.avgRenderMs = avgRenderMs();
    s.totalFrames = _totalFrames;
    s.droppedFrames = _droppedFrames;
    s.dropRate = dropRate();
    s.hwDecodedFrames = _hwFrames;
    s.hwDecodeRatio = hwDecodeRatio();
    return s;
}

QString PlaybackStats::summaryText() const
{
    const auto s = summary();
    return QString("FPS: %1/%2 | Dec: %3ms | Drop: %4%")
        .arg(s.avgFps, 0, 'f', 1)
        .arg(currentFps(), 0, 'f', 1)
        .arg(s.avgDecodeMs, 0, 'f', 1)
        .arg(s.dropRate, 0, 'f', 1);
}

QString PlaybackStats::fullReport() const
{
    const auto s = summary();
    QString hwInfo;
    if (_hwFrames > 0) {
        hwInfo = QString("\n  硬解帧: %1 (%2%)")
            .arg(s.hwDecodedFrames)
            .arg(s.hwDecodeRatio, 0, 'f', 0);
    }

    return QString(
        "播放统计报告\n"
        "──────────────────────────\n"
        "  总帧数:     %1\n"
        "  平均 FPS:   %2\n"
        "  当前 FPS:   %3\n"
        "  最小 FPS:   %4\n"
        "  最大 FPS:   %5\n"
        "  平均解码:   %6 ms\n"
        "  平均渲染:   %7 ms\n"
        "  丢帧:       %8 (%9%)\n"
        "%10\n"
        "──────────────────────────")
        .arg(s.totalFrames)
        .arg(s.avgFps, 0, 'f', 1)
        .arg(currentFps(), 0, 'f', 1)
        .arg(s.minFps, 0, 'f', 1)
        .arg(s.maxFps, 0, 'f', 1)
        .arg(s.avgDecodeMs, 0, 'f', 1)
        .arg(s.avgRenderMs, 0, 'f', 1)
        .arg(s.droppedFrames)
        .arg(s.dropRate, 0, 'f', 1)
        .arg(hwInfo);
}

void PlaybackStats::exportCsv(const QString& path) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }

    QTextStream out(&file);
    const auto s = summary();

    out << "metric,value\n";
    out << "total_frames," << s.totalFrames << "\n";
    out << "avg_fps," << s.avgFps << "\n";
    out << "min_fps," << s.minFps << "\n";
    out << "max_fps," << s.maxFps << "\n";
    out << "avg_decode_ms," << s.avgDecodeMs << "\n";
    out << "avg_render_ms," << s.avgRenderMs << "\n";
    out << "dropped_frames," << s.droppedFrames << "\n";
    out << "drop_rate_pct," << s.dropRate << "\n";
    out << "hw_decoded_frames," << s.hwDecodedFrames << "\n";
    out << "hw_decode_ratio_pct," << s.hwDecodeRatio << "\n";
    out << "timestamp," << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
}

void PlaybackStats::setFpsWindowSize(int frames)
{
    if (frames <= 0) {
        return;
    }
    _fpsWindowMax = static_cast<size_t>(frames);
    _pruneHistory();
}

void PlaybackStats::_pruneHistory()
{
    while (_fpsWindow.size() > _fpsWindowMax) {
        _fpsWindow.pop_front();
    }
}

void PlaybackStats::_publishUpdates(bool force)
{
    const auto now = std::chrono::steady_clock::now();
    const bool shouldPublish =
        force ||
        _lastFrameCount <= 1 ||
        (now - _lastPublishTime) >= kPlaybackStatsPublishInterval;
    if (!shouldPublish) {
        return;
    }

    _lastPublishTime = now;
    const double current = currentFps();
    const double average = averageFps();

    Q_EMIT statsUpdated();
    if (auto* eventBus = ServiceLocator::getService<IEventBus>()) {
        eventBus->publish(PlaybackStatsUpdatedEvent{ _droppedFrames, current, average });
    }
}

} // namespace cgplay
