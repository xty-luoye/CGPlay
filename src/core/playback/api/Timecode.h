#pragma once

#include <QString>
#include <algorithm>
#include <cmath>

namespace cgplay {

// UI timecode is zero-based, non-drop-frame HH:MM:SS:FF. Fractional media
// rates retain their exact value for playback/seek; only the display uses the
// nominal integer rate (23.976 -> 24, 29.97 -> 30).
inline QString formatFrameTimecode(qint64 relativeFrame, double mediaFps)
{
    const int nominalFps = std::isfinite(mediaFps) && mediaFps > 0.0 && mediaFps <= 1000000.0
        ? std::max(1, static_cast<int>(std::llround(mediaFps))) : 24;
    const qint64 frame = std::max<qint64>(0, relativeFrame);
    const qint64 seconds = frame / nominalFps;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(seconds / 3600, 2, 10, QChar('0'))
        .arg(seconds / 60 % 60, 2, 10, QChar('0'))
        .arg(seconds % 60, 2, 10, QChar('0'))
        .arg(frame % nominalFps, 2, 10, QChar('0'));
}

} // namespace cgplay
