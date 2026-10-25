#pragma once

#include <QJsonObject>
#include <QString>

namespace cgplay {

class CompareToolbar;
class IPlaybackService;
class OcioManager;
class TlViewport;

// Capture once on the GUI thread. The returned object owns only copied values;
// no Player, Timeline, QWidget or mutable property reference enters a worker.
QJsonObject captureCompareExportInputs(
    const IPlaybackService& playback,
    const CompareToolbar& toolbar,
    const OcioManager& ocio,
    const TlViewport& viewport,
    QString* error = nullptr);

// Recompute using the real tlRender box API after the export dialog changes mode.
// This consumes only the captured JSON, so switching the live B cannot alter it.
bool updateCompareExportLayout(QJsonObject* job, const QString& mode, QString* error = nullptr);

} // namespace cgplay
