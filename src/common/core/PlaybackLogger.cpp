// CGPlay PlaybackLogger.cpp

#include "PlaybackLogger.h"

#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>
#include <QByteArray>
#include <QFileInfo>

#include <cmath>

namespace cgplay {

namespace {

constexpr int kFrameLogIntervalMs = 250;
constexpr int kFlushIntervalMs = 1000;
constexpr int kFlushLineThreshold = 64;

}

PlaybackLogger& PlaybackLogger::instance()
{
    static PlaybackLogger s;
    return s;
}

bool PlaybackLogger::frameLoggingEnabled()
{
    static const bool enabled = []() {
        const QByteArray value = qgetenv("CGPLAY_FRAME_LOG");
        if (value.isEmpty()) {
            return false;
        }
        const QByteArray normalized = value.trimmed().toLower();
        return normalized == "1" || normalized == "true" || normalized == "on" || normalized == "yes";
    }();
    return enabled;
}

PlaybackLogger::~PlaybackLogger()
{
    endSession();
}

void PlaybackLogger::startSession()
{
    QMutexLocker lock(&_mutex);

    QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                     + "/logs";
    QDir().mkpath(logDir);

    const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    _sessionStart = QDateTime::currentDateTime().toString(Qt::ISODate);
    _lastSeenFrame = -1;
    _bufferedLineCount = 0;
    const auto now = std::chrono::steady_clock::now();
    _lastFlushTime = now;
    _lastFrameLogTime = now - std::chrono::milliseconds(kFrameLogIntervalMs);

    const QString path = logDir + "/cgplay_playback_" + ts + ".log";
    _file = new QFile(path);
    _file->open(QIODevice::WriteOnly | QIODevice::Text);
    _stream.setDevice(_file);

    _writeLine("INFO", "SESSION_START",
               QString("time=%1,pid=%2,version=%3")
                   .arg(_sessionStart)
                   .arg(QCoreApplication::applicationPid())
                   .arg(QCoreApplication::applicationVersion()));
}

void PlaybackLogger::endSession()
{
    QMutexLocker lock(&_mutex);
    if (_file && _file->isOpen()) {
        const QString duration = QString::number(
            QDateTime::currentDateTime().toSecsSinceEpoch() -
            QDateTime::fromString(_sessionStart, Qt::ISODate).toSecsSinceEpoch());
        _writeLine("INFO", "SESSION_END", "duration_s=" + duration);
        _flushIfNeeded(true);
        _file->close();
        delete _file;
        _file = nullptr;
    }
}

void PlaybackLogger::_writeLine(const QString& level, const QString& event,
                                const QString& keyValues)
{
    QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    QString line = ts + " | " + level + " | " + event;
    if (!keyValues.isEmpty()) {
        line += " | " + keyValues;
    }
    line += "\n";

    if (_file && _file->isOpen()) {
        _stream << line;
        ++_bufferedLineCount;
        _flushIfNeeded(false);
    }
}

void PlaybackLogger::_flushIfNeeded(bool force)
{
    if (!_file || !_file->isOpen()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool intervalReached =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - _lastFlushTime).count() >=
        kFlushIntervalMs;
    if (!force && _bufferedLineCount < kFlushLineThreshold && !intervalReached) {
        return;
    }

    _stream.flush();
    _bufferedLineCount = 0;
    _lastFlushTime = now;
}

void PlaybackLogger::fileOpened(const QString& path, int totalFrames, double fps,
                                int width, int height, const QString& format)
{
    QMutexLocker lock(&_mutex);
    QFileInfo fi(path);
    _writeLine("INFO", "FILE_OPENED",
               QString("path=%1,frames=%2,fps=%3,width=%4,height=%5,format=%6")
                   .arg(fi.fileName(), QString::number(totalFrames),
                        QString::number(fps, 'f', 2),
                        QString::number(width), QString::number(height), format));
}

void PlaybackLogger::frameDisplayed(int frame)
{
    if (!frameLoggingEnabled()) {
        return;
    }

    QMutexLocker lock(&_mutex);
    if (frame == _lastSeenFrame) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool jumped = _lastSeenFrame >= 0 && std::abs(frame - _lastSeenFrame) > 1;
    const bool intervalReached =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - _lastFrameLogTime).count() >=
        kFrameLogIntervalMs;
    _lastSeenFrame = frame;

    if (!jumped && !intervalReached) {
        return;
    }

    _lastFrameLogTime = now;
    _writeLine("DEBUG", "FRAME", "frame=" + QString::number(frame));
}

void PlaybackLogger::forward(double fps)
{
    QMutexLocker lock(&_mutex);
    _writeLine("INFO", "PLAY_FWD", "fps=" + QString::number(fps, 'f', 2));
}

void PlaybackLogger::reverse(double fps)
{
    QMutexLocker lock(&_mutex);
    _writeLine("INFO", "PLAY_REV", "fps=" + QString::number(fps, 'f', 2));
}

void PlaybackLogger::stop()
{
    QMutexLocker lock(&_mutex);
    _writeLine("INFO", "STOP", "");
}

void PlaybackLogger::exportStarted(const QString& type, const QString& path,
                                   int firstFrame, int lastFrame)
{
    QMutexLocker lock(&_mutex);
    _writeLine("INFO", "EXPORT_START",
               QString("type=%1,path=%2,first=%3,last=%4")
                   .arg(type, path,
                        QString::number(firstFrame),
                        QString::number(lastFrame)));
}

void PlaybackLogger::exportFinished(bool ok)
{
    QMutexLocker lock(&_mutex);
    _writeLine("INFO", "EXPORT_END", "success=" + QString(ok ? "1" : "0"));
}

void PlaybackLogger::exportCancelled()
{
    QMutexLocker lock(&_mutex);
    _writeLine("INFO", "EXPORT_CANCEL", "");
}

void PlaybackLogger::annotationAdded(const QString& id, const QString& type, int frame)
{
    QMutexLocker lock(&_mutex);
    _writeLine("INFO", "ANNO_ADD",
               "id=" + id + ",type=" + type + ",frame=" + QString::number(frame));
}

void PlaybackLogger::annotationDeleted(const QString& id)
{
    QMutexLocker lock(&_mutex);
    _writeLine("INFO", "ANNO_DEL", "id=" + id);
}

void PlaybackLogger::ocioDisplayChanged(const QString& display)
{
    QMutexLocker lock(&_mutex);
    _writeLine("INFO", "OCIO_DISPLAY", "display=" + display);
}

void PlaybackLogger::compareModeChanged(int mode)
{
    QMutexLocker lock(&_mutex);
    _writeLine("INFO", "COMPARE_MODE", "mode=" + QString::number(mode));
}

void PlaybackLogger::compareFileSet(const QString& path)
{
    QMutexLocker lock(&_mutex);
    QFileInfo fi(path);
    _writeLine("INFO", "COMPARE_FILE", "file=" + fi.fileName());
}

void PlaybackLogger::fullscreenEntered(bool /*entered*/)
{
    QMutexLocker lock(&_mutex);
    _writeLine("INFO", "FULLSCREEN", "state=enter");
}

void PlaybackLogger::fullscreenExited()
{
    QMutexLocker lock(&_mutex);
    _writeLine("INFO", "FULLSCREEN", "state=exit");
}

} // namespace cgplay
