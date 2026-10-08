#pragma once

#include "PlaybackBackend.h"

#include <QObject>
#include <QString>

namespace cgplay {

class PlaybackServiceSignals : public QObject
{
    Q_OBJECT
public:
    explicit PlaybackServiceSignals(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

Q_SIGNALS:
    void fileOpened(const QString& path);
    void fileClosed();
    void currentFrameChanged(int frame, int total);
    void fpsChanged(double fps);
    void cacheUsageChanged(float videoPercent, float audioPercent);
    void playbackStateChanged(int state);
    void inOutPointsChanged(int inFrame, int outFrame);
    void volumeChanged(float volume);
    void muteChanged(bool muted);
    void channelMuteChanged(int channel, bool muted);
    void audioDeviceChanged(const QString& device);
    void audioOffsetChanged(double seconds);

#if CGPLAY_HAS_TLRENDER
    void playerReady(const std::shared_ptr<tl::Player>& player);
#endif
};

} // namespace cgplay
