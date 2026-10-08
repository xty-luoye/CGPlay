#include "MediaProbe.h"
#include "annotation/ReviewExport.h"
#include "component/ComponentManager.h"
#include "core/AsyncImageLoader.h"
#include "common/jobs/JobSystem.h"

#include <QCoreApplication>
#include <QCache>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

namespace cgplay {

namespace {

// Video metadata is small and reused by opening, playlist and thumbnail paths.
// Never cache image sequences (their directory contents can change independently).
QMutex videoProbeCacheMutex;
QCache<QString, MediaInfo> videoProbeCache(128);

QString videoProbeCacheKey(const QFileInfo& file)
{
    if (!file.isFile()) return {};
    const QString canonical = file.canonicalFilePath();
    return QStringLiteral("%1|%2|%3|%4")
        .arg(canonical.isEmpty() ? file.absoluteFilePath() : canonical)
        .arg(file.size())
        .arg(file.lastModified().toMSecsSinceEpoch())
        .arg(file.birthTime().toMSecsSinceEpoch());
}

bool fileHasSubtitleSuffix(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().trimmed().toLower();
    return suffix == QStringLiteral("srt") ||
        suffix == QStringLiteral("vtt") ||
        suffix == QStringLiteral("ass") ||
        suffix == QStringLiteral("ssa") ||
        suffix == QStringLiteral("sub");
}

bool isCgplayGeneratedSubtitleSidecar(const QFileInfo& mediaFile, const QString& path)
{
    const QFileInfo subtitleFile(path);
    const QString mediaBase = mediaFile.completeBaseName();
    const QString subtitleBase = subtitleFile.completeBaseName();
    if (!subtitleBase.startsWith(mediaBase + QLatin1Char('.'), Qt::CaseInsensitive)) {
        return false;
    }

    const QString tag = subtitleBase.mid(mediaBase.size() + 1).toLower();
    return tag == QStringLiteral("source") ||
        tag == QStringLiteral("zh") ||
        tag == QStringLiteral("zh-hans");
}

QStringList collectExternalSubtitleCandidates(const QFileInfo& mediaFile)
{
    QStringList candidates;
    const QDir dir = mediaFile.dir();
    const QString baseName = mediaFile.completeBaseName();
    const QStringList suffixes{
        QStringLiteral(".srt"),
        QStringLiteral(".vtt"),
        QStringLiteral(".ass"),
        QStringLiteral(".ssa")
    };
    for (const QString& suffix : suffixes) {
        const QString candidate = dir.filePath(baseName + suffix);
        if (QFileInfo::exists(candidate)) {
            candidates.push_back(QFileInfo(candidate).absoluteFilePath());
        }
    }

    const QStringList entries = dir.entryList(
        {baseName + QStringLiteral("*.srt"),
         baseName + QStringLiteral("*.vtt"),
         baseName + QStringLiteral("*.ass"),
         baseName + QStringLiteral("*.ssa")},
        QDir::Files,
        QDir::Name);
    for (const QString& entry : entries) {
        const QString candidate = dir.filePath(entry);
        if (fileHasSubtitleSuffix(candidate) &&
            !isCgplayGeneratedSubtitleSidecar(mediaFile, candidate)) {
            candidates.push_back(QFileInfo(candidate).absoluteFilePath());
        }
    }

    candidates.removeDuplicates();
    return candidates;
}

}

int MediaInfo::effectiveFrameCount() const
{
    if (frameCount > 0) {
        return frameCount;
    }
    if (lastFrame >= firstFrame) {
        return lastFrame - firstFrame + 1;
    }
    return 1;
}

QString MediaInfo::resolutionText() const
{
    return width > 0 && height > 0
        ? QStringLiteral("%1x%2").arg(width).arg(height)
        : QStringLiteral("--");
}

QString MediaInfo::codecDisplayText() const
{
    if (codecName.trimmed().isEmpty()) {
        return formatLabel.isEmpty() ? QStringLiteral("--") : formatLabel;
    }

    QString container = containerFormat.trimmed();
    if (container.isEmpty()) {
        container = extension.trimmed();
    }
    QString text = codecName.trimmed().toUpper();
    if (!container.isEmpty()) {
        text = container.toUpper() + QStringLiteral(" / ") + text;
    }
    if (!pixelFormat.trimmed().isEmpty()) {
        text += QStringLiteral(" ") + pixelFormat.trimmed();
    }
    return text;
}

QString MediaInfo::bitrateText(const QString& unit) const
{
    if (bitrateBitsPerSecond <= 0) {
        return QStringLiteral("--");
    }

    const QString normalizedUnit = unit.trimmed().toLower();
    const double megabits = static_cast<double>(bitrateBitsPerSecond) / 1000000.0;
    if (normalizedUnit == QStringLiteral("mbps") ||
        (normalizedUnit != QStringLiteral("kbps") && megabits >= 1.0)) {
        const int decimals = megabits >= 100.0 ? 0 : (megabits >= 10.0 ? 1 : 2);
        return QStringLiteral("%1 Mbps").arg(megabits, 0, 'f', decimals);
    }

    const double kilobits = static_cast<double>(bitrateBitsPerSecond) / 1000.0;
    const int decimals = kilobits >= 100.0 ? 0 : 1;
    return QStringLiteral("%1 kbps").arg(kilobits, 0, 'f', decimals);
}

bool MediaProbe::isStillImagePath(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == "exr" || ext == "dpx" || ext == "png" ||
           ext == "jpg" || ext == "jpeg" || ext == "tif" ||
           ext == "tiff" || ext == "bmp" || ext == "psd" ||
           ext == "tga";
}

bool MediaProbe::isVideoPath(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return videoExtensions().contains(ext);
}

const QStringList& MediaProbe::videoExtensions()
{
    static const QStringList extensions {
        QStringLiteral("3g2"), QStringLiteral("3gp"), QStringLiteral("asf"),
        QStringLiteral("avi"), QStringLiteral("divx"), QStringLiteral("dv"),
        QStringLiteral("f4v"), QStringLiteral("flv"), QStringLiteral("ivf"),
        QStringLiteral("m1v"), QStringLiteral("m2ts"), QStringLiteral("m2v"),
        QStringLiteral("m4v"), QStringLiteral("mj2"), QStringLiteral("mkv"),
        QStringLiteral("mov"), QStringLiteral("mp4"), QStringLiteral("mpeg"),
        QStringLiteral("mpg"), QStringLiteral("mts"), QStringLiteral("mxf"),
        QStringLiteral("ogv"), QStringLiteral("prores"), QStringLiteral("rm"),
        QStringLiteral("rmvb"), QStringLiteral("ts"), QStringLiteral("vob"),
        QStringLiteral("webm"), QStringLiteral("wmv"), QStringLiteral("wtv"),
        QStringLiteral("y4m")
    };
    return extensions;
}

QString MediaProbe::mediaFileDialogFilter()
{
    QStringList patterns;
    patterns.reserve(videoExtensions().size() + 10);
    for (const QString& extension : videoExtensions()) {
        patterns.push_back(QStringLiteral("*.%1").arg(extension));
    }
    patterns.append({
        QStringLiteral("*.exr"), QStringLiteral("*.dpx"), QStringLiteral("*.png"),
        QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.tif"),
        QStringLiteral("*.tiff"), QStringLiteral("*.bmp"), QStringLiteral("*.psd"),
        QStringLiteral("*.tga")
    });
    return QStringLiteral("媒体文件 (%1);;所有文件 (*)").arg(patterns.join(QLatin1Char(' ')));
}

QString MediaProbe::formatLabelForPath(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "exr") return QStringLiteral("EXR");
    if (ext == "dpx") return QStringLiteral("DPX");
    if (isVideoPath(path)) return QString::fromUtf8("视频");
    if (isStillImagePath(path)) return QString::fromUtf8("图片");
    return QString::fromUtf8("未知");
}

QString MediaProbe::displayNameForPath(const QString& path)
{
    const QFileInfo info(path);
    static const QRegularExpression seqRe(R"((.+?)\.\d+\.[a-zA-Z0-9]+$)");
    const auto match = seqRe.match(info.fileName());
    if (match.hasMatch()) {
        return match.captured(1);
    }
    return info.completeBaseName();
}

QVector<SequenceFrame> MediaProbe::collectSequenceFiles(const QString& path)
{
    const QFileInfo info(path);
    static const QRegularExpression seqRe(R"(^(.*?)(\d+)(\.[^.]+)$)");
    const auto match = seqRe.match(info.fileName());
    if (!match.hasMatch()) {
        return {};
    }

    const QString prefix = match.captured(1);
    const QString digits = match.captured(2);
    const QString suffix = match.captured(3);
    const QRegularExpression exactRe(
        QStringLiteral("^%1(\\d{%2})%3$")
            .arg(QRegularExpression::escape(prefix))
            .arg(digits.size())
            .arg(QRegularExpression::escape(suffix)));

    const QDir dir = info.dir();
    const QStringList names = dir.entryList({prefix + "*" + suffix}, QDir::Files, QDir::Name);
    QVector<SequenceFrame> frames;
    frames.reserve(names.size());
    for (const QString& name : names) {
        const auto fileMatch = exactRe.match(name);
        if (!fileMatch.hasMatch()) {
            continue;
        }
        bool ok = false;
        const int frameNumber = fileMatch.captured(1).toInt(&ok);
        if (ok) {
            frames.push_back({frameNumber, dir.filePath(name)});
        }
    }

    std::sort(frames.begin(), frames.end(), [](const SequenceFrame& a, const SequenceFrame& b) {
        return a.frameNumber < b.frameNumber;
    });

    if (frames.size() <= 1) {
        frames.clear();
    }
    return frames;
}

QString MediaProbe::locateFfmpeg()
{
    return ReviewExport::locateFfmpeg();
}

QString MediaProbe::locateFfprobe()
{
    const QString componentPath =
        ComponentManager::instance().componentExecutablePath(QStringLiteral("ffmpeg"), QStringLiteral("ffprobe.exe"));
    if (!componentPath.isEmpty()) {
        return componentPath;
    }

    const QString ffmpeg = locateFfmpeg();
    if (!ffmpeg.isEmpty()) {
        const QString sibling = QFileInfo(ffmpeg).absolutePath() + QStringLiteral("/ffprobe.exe");
        if (QFileInfo::exists(sibling)) {
            return sibling;
        }
    }

    const QString appSibling = QCoreApplication::applicationDirPath() + QStringLiteral("/ffprobe.exe");
    if (QFileInfo::exists(appSibling)) {
        return appSibling;
    }

    return QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
}

double MediaProbe::parseFpsRatio(const QString& value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty() || trimmed == QStringLiteral("0/0")) {
        return 0.0;
    }

    const auto parts = trimmed.split('/');
    bool okA = false;
    bool okB = false;
    if (parts.size() == 2) {
        const double numerator = parts[0].toDouble(&okA);
        const double denominator = parts[1].toDouble(&okB);
        if (okA && okB && numerator > 0.0 && denominator > 0.0) {
            return numerator / denominator;
        }
    }

    const double fps = trimmed.toDouble(&okA);
    return okA && fps > 0.0 ? fps : 0.0;
}

static void probeVideo(const QString& path, MediaInfo& info, JobContext* job)
{
    const QString ffprobe = MediaProbe::locateFfprobe();
    if (ffprobe.isEmpty()) {
        info.error = QStringLiteral("ffprobe not found");
        return;
    }

    QProcess proc;
    proc.start(ffprobe, {
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-show_entries"),
        QStringLiteral("stream=index,codec_type,width,height,codec_name,pix_fmt,avg_frame_rate,r_frame_rate,nb_frames,duration,bit_rate"),
        QStringLiteral("-show_entries"),
        QStringLiteral("format=format_name,format_long_name,bit_rate"),
        QStringLiteral("-of"), QStringLiteral("json"),
        path
    }, QIODevice::ReadOnly);
    JobContext localJob(8000);
    JobContext& context = job ? *job : localJob;
    const ProcessOutcome process = context.waitForProcess(proc, 25, 8000);
    if (!process.succeeded()) {
        if (process.state == JobState::Canceled) {
            info.error = QStringLiteral("Media probe canceled");
        } else if (process.state == JobState::TimedOut) {
            info.error = QStringLiteral("Media probe timed out");
        } else {
            info.error = QString::fromUtf8(process.standardError).trimmed();
        }
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(process.standardOutput);
    const QJsonObject format = doc.object().value(QStringLiteral("format")).toObject();
    const QString formatNames = format.value(QStringLiteral("format_name")).toString().trimmed();
    if (!formatNames.isEmpty()) {
        info.containerFormat = QFileInfo(path).suffix().trimmed().toLower();
        if (info.containerFormat.isEmpty()) {
            info.containerFormat = formatNames.split(QLatin1Char(',')).value(0).trimmed();
        }
    }

    const auto parseBitrate = [](const QJsonValue& value) {
        if (value.isString()) {
            bool ok = false;
            const qint64 bitrate = value.toString().trimmed().toLongLong(&ok);
            return ok && bitrate > 0 ? bitrate : qint64(0);
        }
        if (value.isDouble()) {
            const double numeric = value.toDouble();
            return std::isfinite(numeric) && numeric > 0.0
                ? static_cast<qint64>(numeric)
                : qint64(0);
        }
        return qint64(0);
    };
    info.bitrateBitsPerSecond = parseBitrate(format.value(QStringLiteral("bit_rate")));
    const QJsonArray streams = doc.object().value(QStringLiteral("streams")).toArray();
    if (streams.isEmpty()) {
        info.error = QStringLiteral("No video stream");
        return;
    }

    for (const QJsonValue& streamValue : streams) {
        const QJsonObject stream = streamValue.toObject();
        const QString codecType = stream.value(QStringLiteral("codec_type")).toString().trimmed().toLower();
        if (codecType == QStringLiteral("video") && info.width <= 0 && info.height <= 0) {
            info.width = stream.value(QStringLiteral("width")).toInt(0);
            info.height = stream.value(QStringLiteral("height")).toInt(0);
            info.hasRealDimensions = info.width > 0 && info.height > 0;
            info.codecName = stream.value(QStringLiteral("codec_name")).toString();
            info.pixelFormat = stream.value(QStringLiteral("pix_fmt")).toString();
            if (info.bitrateBitsPerSecond <= 0) {
                info.bitrateBitsPerSecond = parseBitrate(stream.value(QStringLiteral("bit_rate")));
            }

            const double avgFps = MediaProbe::parseFpsRatio(stream.value(QStringLiteral("avg_frame_rate")).toString());
            const double streamFps = MediaProbe::parseFpsRatio(stream.value(QStringLiteral("r_frame_rate")).toString());
            if (avgFps > 0.0) {
                info.fps = avgFps;
                info.hasRealFps = true;
            } else if (streamFps > 0.0) {
                info.fps = streamFps;
                info.hasRealFps = true;
            }

            bool okFrames = false;
            info.frameCount = stream.value(QStringLiteral("nb_frames")).toString().toInt(&okFrames);
            if (okFrames && info.frameCount > 0) {
                info.firstFrame = 0;
                info.lastFrame = info.frameCount - 1;
                info.hasRealFrameCount = true;
            }

            bool okDuration = false;
            info.durationSeconds = stream.value(QStringLiteral("duration")).toString().toDouble(&okDuration);
            if (!info.hasRealFrameCount && okDuration && info.durationSeconds > 0.0 && info.fps > 0.0) {
                info.frameCount = std::max(1, static_cast<int>(std::round(info.durationSeconds * info.fps)));
                info.firstFrame = 0;
                info.lastFrame = info.frameCount - 1;
                info.hasRealFrameCount = true;
            }
        } else if (codecType == QStringLiteral("audio")) {
            ++info.audioStreamCount;
        } else if (codecType == QStringLiteral("subtitle")) {
            ++info.subtitleStreamCount;
        }
    }

    info.hasAudio = info.audioStreamCount > 0;
    info.hasEmbeddedSubtitles = info.subtitleStreamCount > 0;
}

static void probeStill(const QString& path, MediaInfo& info)
{
    int exrW = 0;
    int exrH = 0;
    if (QFileInfo(path).suffix().compare(QStringLiteral("exr"), Qt::CaseInsensitive) == 0 &&
        AsyncImageLoader::probeEXR(path, exrW, exrH)) {
        info.width = exrW;
        info.height = exrH;
        info.hasRealDimensions = true;
    } else {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        reader.setDecideFormatFromContent(true);
        const QSize size = reader.size();
        if (size.isValid()) {
            info.width = size.width();
            info.height = size.height();
            info.hasRealDimensions = true;
        }
    }

    const QVector<SequenceFrame> sequence = MediaProbe::collectSequenceFiles(path);
    if (!sequence.isEmpty()) {
        info.isSequence = true;
        info.firstFrame = sequence.front().frameNumber;
        info.lastFrame = sequence.back().frameNumber;
        info.frameCount = sequence.size();
        info.hasRealFrameCount = true;
    } else {
        info.firstFrame = 0;
        info.lastFrame = 0;
        info.frameCount = 1;
        info.hasRealFrameCount = true;
    }
}

MediaInfo MediaProbe::probe(const QString& path, double fpsOverride, JobContext* job)
{
    MediaInfo info;
    info.path = path;
    const QFileInfo fileInfo(path);
    info.exists = fileInfo.exists();
    info.name = displayNameForPath(path);
    info.extension = fileInfo.suffix().toLower();
    info.formatLabel = formatLabelForPath(path);
    info.isStillImage = isStillImagePath(path);
    info.isVideo = isVideoPath(path);

    if (!info.exists) {
        info.error = QStringLiteral("File not found");
        return info;
    }

    if (info.isVideo) {
        if (job && job->shouldStop()) {
            info.error = QStringLiteral("Media probe canceled or timed out");
            return info;
        }
        const QString key = videoProbeCacheKey(fileInfo);
        bool cached = false;
        if (!key.isEmpty()) {
            QMutexLocker lock(&videoProbeCacheMutex);
            if (const auto* entry = videoProbeCache.object(key)) {
                info = *entry;
                // Preserve the caller's path/alias and display identity.
                info.path = path;
                info.name = displayNameForPath(path);
                info.extension = fileInfo.suffix().toLower();
                info.containerFormat = info.extension;
                cached = true;
            }
        }
        if (!cached) {
            probeVideo(path, info, job);
            // A failed, canceled or concurrently replaced file must be retried.
            if (!key.isEmpty() && info.error.isEmpty() && info.hasRealDimensions &&
                (!job || !job->shouldStop()) && key == videoProbeCacheKey(QFileInfo(path))) {
                QMutexLocker lock(&videoProbeCacheMutex);
                videoProbeCache.insert(key, new MediaInfo(info));
            }
        }
        // Subtitle sidecars can appear/disappear while video bytes stay unchanged.
        // Refresh them on every request, before applying the per-call FPS override.
        if (info.error.isEmpty()) {
            info.externalSubtitlePaths = collectExternalSubtitleCandidates(fileInfo);
            info.hasExternalSubtitles = !info.externalSubtitlePaths.isEmpty();
        }
    } else if (info.isStillImage) {
        probeStill(path, info);
    }

    if (fpsOverride > 0.0) {
        info.fps = fpsOverride;
        info.hasRealFps = true;
    } else if (info.fps <= 0.0) {
        info.fps = 24.0;
    }

    if (info.frameCount <= 0) {
        info.frameCount = info.effectiveFrameCount();
    }

    return info;
}

} // namespace cgplay
