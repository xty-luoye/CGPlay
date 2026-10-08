#pragma once
// CGPlay PlaybackLogger.h
// Session-based playback logger for lightweight runtime tracing.

#include <QString>
#include <QFile>
#include <QTextStream>
#include <QMutex>
#include <QDateTime>

#include <chrono>

#define CGPLAY_LOG() (cgplay::PlaybackLogger::instance())

namespace cgplay {

class PlaybackLogger
{
public:
    static PlaybackLogger& instance();
    static bool frameLoggingEnabled();

    void startSession();
    void endSession();

    void fileOpened(const QString& path, int totalFrames, double fps,
                    int width, int height, const QString& format);
    void frameDisplayed(int frame);
    void forward(double fps);
    void reverse(double fps);
    void stop();
    void exportStarted(const QString& type, const QString& path,
                       int firstFrame, int lastFrame);
    void exportFinished(bool ok);
    void exportCancelled();
    void annotationAdded(const QString& id, const QString& type, int frame);
    void annotationDeleted(const QString& id);
    void ocioDisplayChanged(const QString& display);
    void compareModeChanged(int mode);
    void compareFileSet(const QString& path);
    void fullscreenEntered(bool entered);
    void fullscreenExited();

private:
    PlaybackLogger() = default;
    ~PlaybackLogger();
    PlaybackLogger(const PlaybackLogger&) = delete;
    PlaybackLogger& operator=(const PlaybackLogger&) = delete;

    void _writeLine(const QString& level, const QString& event,
                    const QString& keyValues);
    void _flushIfNeeded(bool force = false);

    QFile* _file = nullptr;
    QTextStream _stream;
    QMutex _mutex;
    QString _sessionStart;
    int _lastSeenFrame = -1;
    int _bufferedLineCount = 0;
    std::chrono::steady_clock::time_point _lastFlushTime;
    std::chrono::steady_clock::time_point _lastFrameLogTime;
};

} // namespace cgplay
