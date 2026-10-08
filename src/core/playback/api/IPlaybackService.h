#pragma once

#include "PlaybackBackend.h"
#include "PlaybackServiceSignals.h"

#include <QString>
#include <QStringList>

namespace cgplay {

class PlaybackStats;

class IPlaybackService
{
public:
    virtual ~IPlaybackService() = default;

    virtual PlaybackServiceSignals* signalProxy() const = 0;

    virtual void openFile(const QString& path) = 0;
    virtual void openFile(const QString& path, double sequenceFpsOverride) = 0;
    virtual void closeFile() = 0;
    virtual QString currentPath() const = 0;

    virtual bool isValid() const = 0;
    virtual int currentFrame() const = 0;
    virtual int totalFrames() const = 0;
    virtual double fps() const = 0;
    virtual int playbackState() const = 0;

#if CGPLAY_HAS_TLRENDER
    virtual std::shared_ptr<tl::Player> player() const = 0;
#endif

#if CGPLAY_HAS_FTK
    virtual std::shared_ptr<ftk::Context> context() const = 0;
    virtual std::shared_ptr<ftk::Style> style() const = 0;
#endif

    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void forward() = 0;
    virtual void reverse() = 0;
    virtual void togglePlay() = 0;

    virtual void nextFrame() = 0;
    virtual void prevFrame() = 0;
    virtual void seekRelative(int deltaFrames) = 0;
    virtual void seekToFrame(int frame) = 0;

    virtual void gotoStart() = 0;
    virtual void gotoEnd() = 0;

    virtual void setSpeed(double fps) = 0;
    virtual void setLoop(int mode) = 0;

    virtual void setInPoint(int frame) = 0;
    virtual void setOutPoint(int frame) = 0;
    virtual void clearInPoint() = 0;
    virtual void clearOutPoint() = 0;
    virtual void clearInOutPoints() = 0;
    virtual bool hasInPoint() const = 0;
    virtual bool hasOutPoint() const = 0;
    virtual int inPoint() const = 0;
    virtual int outPoint() const = 0;
    virtual bool hasInOutRange() const = 0;

    virtual void setCompareFile(const QString& path) = 0;
    virtual void clearCompare() = 0;
    virtual bool hasCompare() const = 0;
    virtual void setCompareTime(int mode) = 0;

    virtual float getVolume() const = 0;
    virtual bool isMuted() const = 0;
    virtual void setVolume(float value) = 0;
    virtual void setMute(bool muted) = 0;
    virtual void toggleMute() = 0;

    virtual void setReadAheadEnabled(bool enabled) = 0;
    virtual bool isReadAheadEnabled() const = 0;

    virtual bool isChannelMuted(int channel) const = 0;
    virtual void setChannelMute(int channel, bool mute) = 0;
    virtual void toggleChannelMute(int channel) = 0;

    virtual QString getAudioDeviceName() const = 0;
    virtual void setAudioDevice(const QString& name) = 0;
    virtual QStringList availableAudioDevices() const = 0;

    virtual double getAudioOffset() const = 0;
    virtual void setAudioOffset(double seconds) = 0;

    virtual PlaybackStats* playbackStats() const = 0;
};

} // namespace cgplay
