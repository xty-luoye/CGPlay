#pragma once

#include <QJsonArray>
#include <QJsonObject>

namespace cgplay {

// Mirror tlRender FFmpegReadVideo: prefer a stream whose only disposition
// is AV_DISPOSITION_DEFAULT, otherwise play the first video in file order.
inline int playbackVideoStreamPosition(const QJsonArray& streams)
{
    int firstVideo = -1;
    for (qsizetype i = 0; i < streams.size(); ++i) {
        const QJsonObject stream = streams[i].toObject();
        if (stream.value(QStringLiteral("codec_type")).toString() != QStringLiteral("video")) continue;
        if (firstVideo < 0) firstVideo = static_cast<int>(i);
        const auto flags = stream.value(QStringLiteral("disposition")).toObject();
        if (flags.value(QStringLiteral("default")).toInt() != 1) continue;
        bool defaultOnly = true;
        for (auto it = flags.begin(); it != flags.end(); ++it) {
            if (it.key() != QStringLiteral("default") && it.value().toInt()) {
                defaultOnly = false;
                break;
            }
        }
        if (defaultOnly) return static_cast<int>(i);
    }
    return firstVideo;
}

} // namespace cgplay
