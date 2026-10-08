#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

namespace cgplay {

class JobContext;

struct SequenceFrame
{
    int frameNumber = 0;
    QString path;
};

struct MediaInfo
{
    QString path;
    QString name;
    QString extension;
    QString formatLabel;
    // Container format reported by ffprobe (for example "mov" or "matroska").
    // Keep this separate from codecName: a MOV may use PNG video.
    QString containerFormat;
    QString codecName;
    QString pixelFormat;
    QString error;

    int width = 0;
    int height = 0;
    int firstFrame = 0;
    int lastFrame = 0;
    int frameCount = 0;
    int audioStreamCount = 0;
    int subtitleStreamCount = 0;
    // Container/stream bitrate in decimal bits per second.  FFprobe may omit
    // this for raw or variable-rate sources; zero is intentionally rendered
    // as an unknown value instead of being guessed from file size and time.
    qint64 bitrateBitsPerSecond = 0;

    double fps = 24.0;
    double durationSeconds = 0.0;

    bool exists = false;
    bool isVideo = false;
    bool isStillImage = false;
    bool isSequence = false;
    bool hasRealDimensions = false;
    bool hasRealFps = false;
    bool hasRealFrameCount = false;
    bool hasAudio = false;
    bool hasEmbeddedSubtitles = false;
    bool hasExternalSubtitles = false;
    QStringList externalSubtitlePaths;

    int effectiveFrameCount() const;
    QString resolutionText() const;
    QString codecDisplayText() const;
    // `unit` accepts "auto", "mbps" or "kbps". Invalid/empty values use
    // the automatic formatter so persisted settings cannot produce a blank
    // status label.
    QString bitrateText(const QString& unit = QStringLiteral("auto")) const;
};

class MediaProbe
{
public:
    static MediaInfo probe(
        const QString& path,
        double fpsOverride = 0.0,
        JobContext* job = nullptr);

    static bool isStillImagePath(const QString& path);
    static bool isVideoPath(const QString& path);
    static const QStringList& videoExtensions();
    static QString mediaFileDialogFilter();
    static QString formatLabelForPath(const QString& path);
    static QString displayNameForPath(const QString& path);

    static QVector<SequenceFrame> collectSequenceFiles(const QString& path);
    static QString locateFfmpeg();
    static QString locateFfprobe();
    static double parseFpsRatio(const QString& value);
};

} // namespace cgplay
