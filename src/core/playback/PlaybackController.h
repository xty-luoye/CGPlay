#pragma once
// CGPlay PlaybackController.h
// Wraps tl::timeline::Player — follows DJV's pattern directly.

#include "playback/api/IPlaybackService.h"

#include <QObject>
#include <QString>
#include <memory>

namespace cgplay {

class PlaybackStats;

class CacheManager;
class OcioManager;

// ─── PlaybackController ───────────────────────────────────────────────────────
class PlaybackController : public QObject, public IPlaybackService
{
    Q_OBJECT
public:
    explicit PlaybackController(
        std::shared_ptr<CacheManager> cache,
        std::shared_ptr<OcioManager>  ocio,
        QObject* parent = nullptr);
    ~PlaybackController() override;

    PlaybackServiceSignals* signalProxy() const override;

    // ── File I/O ──────────────────────────────────────────────────────────────
    void openFile(const QString& path) override;
    void openFile(const QString& path, double sequenceFpsOverride) override;
    void openFile(const QString& path, double sequenceFpsOverride, const QString& codecHint);
    void closeFile() override;
    QString currentPath() const override;

    // ── Accessors ─────────────────────────────────────────────────────────────
    bool isValid() const override;
    int  currentFrame() const override;
    int  totalFrames()  const override;
    double fps()        const override;
    int  playbackState() const override; // 0=stop, 1=forward, 2=reverse

#if CGPLAY_HAS_TLRENDER
    std::shared_ptr<tl::Player> player() const override;
#endif

#if CGPLAY_HAS_FTK
    std::shared_ptr<ftk::Context> context() const override;
    std::shared_ptr<ftk::Style>   style()   const override;
#endif

public Q_SLOTS:
    void play() override;
    void pause() override;
    void stop() override;
    void forward() override;
    void reverse() override;
    void togglePlay() override;

    void nextFrame() override;
    void prevFrame() override;
    void seekRelative(int deltaFrames) override;
    void seekToFrame(int frame) override;

    void gotoStart() override;
    void gotoEnd() override;

    void setSpeed(double fps) override;
    void setLoop(int mode) override; // 0=Loop, 1=Once, 2=PingPong

    // ── In/Out points (A/B loop) ──────────────────────────────────────────
    void setInPoint(int frame) override;
    void setOutPoint(int frame) override;
    void clearInPoint() override;
    void clearOutPoint() override;
    void clearInOutPoints() override;
    bool hasInPoint() const override;
    bool hasOutPoint() const override;
    int  inPoint() const override;
    int  outPoint() const override;
    bool hasInOutRange() const override; // both A and B set, A < B

    // ── Compare ────────────────────────────────────────────────────────────
    void setCompareFile(const QString& path) override;   // Set compare (B) file
    void clearCompare() override;                        // Clear compare
    bool hasCompare() const override;
    void setCompareTime(int mode) override;              // 0=Relative, 1=Absolute

    // ── Audio ──────────────────────────────────────────────────────────────
    float getVolume() const override;
    bool  isMuted()  const override;

    // ── v1.1 性能层 ───────────────────────────────────────────────────────
    void setReadAheadEnabled(bool enabled) override;
    bool isReadAheadEnabled() const override;

    // Channel mute (L/R independent) — getChannelMute returns std::vector<bool>
    bool  isChannelMuted(int channel) const override; // 0=L, 1=R, ...
    void  setChannelMute(int channel, bool mute) override;
    void  toggleChannelMute(int channel) override;

    // Audio device
    QString  getAudioDeviceName() const override;
    void     setAudioDevice(const QString& name) override;
    QStringList availableAudioDevices() const override;

    // Audio sync offset (seconds)
    double getAudioOffset() const override;
    void   setAudioOffset(double seconds) override;

    // v1.5 Playback stats
    PlaybackStats* playbackStats() const override;

public Q_SLOTS:
    void setVolume(float v) override;   // 0.0 – 1.0
    void setMute(bool m) override;
    void toggleMute() override;

Q_SIGNALS:
    void fileOpened(const QString& path);
    void fileClosed();
    void currentFrameChanged(int frame, int total);
    void fpsChanged(double fps);
    void cacheUsageChanged(float videoPercent, float audioPercent);
    void playbackStateChanged(int state); // 0=stop, 1=fwd, 2=rev
    void inOutPointsChanged(int inFrame, int outFrame);
    void volumeChanged(float volume);
    void muteChanged(bool muted);
    void channelMuteChanged(int channel, bool muted);
    void audioDeviceChanged(const QString& device);
    void audioOffsetChanged(double seconds);

#if CGPLAY_HAS_TLRENDER
    void playerReady(const std::shared_ptr<tl::Player>& player);
#endif

protected:
    void timerEvent(QTimerEvent* event) override;

private:
    void _restartSystemTickTimer(int intervalMs);
    void _updateTimerCadence(bool force = false);
    int _desiredSystemTickIntervalMs() const;
    int _desiredUiTickIntervalMs() const;
    void _createPlayer(
        const QString& path,
        double sequenceFpsOverride = 0.0,
        const QString& logicalPath = {},
        const QString& codecHint = {});
    void _destroyPlayer();
    void _connectPlayer();
    void _tickTimer();
    void _enforceLoopBounds();

    struct Private;
    std::unique_ptr<Private> _p;
};

} // namespace cgplay
