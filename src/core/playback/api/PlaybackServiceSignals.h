#pragma once

#include "PlaybackBackend.h"

#include <QObject>
#include <QString>

// tl::Player is a non-QObject native object. Its shared ownership handle can
// cross queued connections without exposing the SDK to Qt-only consumers.
Q_DECLARE_OPAQUE_POINTER(tl::Player*)
Q_DECLARE_METATYPE(std::shared_ptr<tl::Player>)

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
    void fileOpenFailed(const QString& path, const QString& error);
    void fileClosed();
    void currentFrameChanged(int frame, int total);
    void fpsChanged(double fps);
    void cacheUsageChanged(float videoPercent, float audioPercent);
    void playbackStateChanged(int state);
    void playbackOptionsChanged(double speedMultiplier, int loopMode);
    void inOutPointsChanged(int inFrame, int outFrame);
    void volumeChanged(float volume);
    void muteChanged(bool muted);
    void channelMuteChanged(int channel, bool muted);
    void audioDeviceChanged(const QString& device);
    void audioOffsetChanged(double seconds);

    void playerReady(const std::shared_ptr<tl::Player>& player);
};

} // namespace cgplay
