#pragma once
// CGPlay PlaybackStats.h
// Lightweight playback statistics for UI and diagnostics.

#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <deque>

namespace cgplay {

struct FrameStat
{
    int frame = 0;
    double decodeMs = 0.0;
    double renderMs = 0.0;
    double totalMs = 0.0;
    bool dropped = false;
    bool hwDecoded = false;

    double fps() const { return totalMs > 0 ? 1000.0 / totalMs : 0.0; }
};

struct PlaybackSummary
{
    double avgFps = 0.0;
    double minFps = 0.0;
    double maxFps = 0.0;
    double avgDecodeMs = 0.0;
    double avgRenderMs = 0.0;
    int totalFrames = 0;
    int droppedFrames = 0;
    double dropRate = 0.0;
    int hwDecodedFrames = 0;
    double hwDecodeRatio = 0.0;
};

class PlaybackStats : public QObject
{
    Q_OBJECT
public:
    explicit PlaybackStats(QObject* parent = nullptr);
    ~PlaybackStats() override;

    void recordFrame(const FrameStat& stat);
    void recordDrop();
    void reset();

    int totalFrames() const { return _totalFrames; }
    int droppedFrames() const { return _droppedFrames; }
    double currentFps() const;
    double averageFps() const;
    double avgDecodeMs() const;
    double avgRenderMs() const;
    double dropRate() const;
    int hwDecodedFrames() const { return _hwFrames; }
    double hwDecodeRatio() const;

    PlaybackSummary summary() const;
    QString summaryText() const;
    QString fullReport() const;

    void exportCsv(const QString& path) const;

    void setFpsWindowSize(int frames);
    int fpsWindowSize() const { return static_cast<int>(_fpsWindow.size()); }

Q_SIGNALS:
    void statsUpdated();

private:
    void _publishUpdates(bool force = false);
    void _pruneHistory();

    int _totalFrames = 0;
    int _droppedFrames = 0;
    int _hwFrames = 0;
    double _totalDecodeMs = 0.0;
    double _totalRenderMs = 0.0;

    std::deque<double> _fpsWindow;
    size_t _fpsWindowMax = 120;

    std::chrono::steady_clock::time_point _lastFrameTime;
    std::chrono::steady_clock::time_point _lastPublishTime;
    int _lastFrameCount = 0;
    QTimer* _reportTimer = nullptr;
};

} // namespace cgplay
