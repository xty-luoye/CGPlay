// CGPlay OtioExporter.cpp

#include "OtioExporter.h"
#include "playlist/PlaylistModel.h"

#include <opentimelineio/timeline.h>
#include <opentimelineio/track.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/clip.h>
#include <opentimelineio/composition.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/imageSequenceReference.h>
#include <opentimelineio/serialization.h>
#include <opentime/rationalTime.h>
#include <opentime/timeRange.h>

#include <QFileInfo>
#include <QDebug>

namespace cgplay {

namespace OTIO = opentimelineio::OPENTIMELINEIO_VERSION;
namespace opentime = opentime::OPENTIME_VERSION;

bool OtioExporter::exportToFile(const QString& otioPath, PlaylistModel* model,
                                 const QString& timelineName)
{
    _errorString.clear();
    if (!model || model->rowCount() == 0) {
        _errorString = QStringLiteral("播放列表为空，无法导出 OTIO。");
        return false;
    }

    std::vector<ShotItem> shots;
    for (int i = 0; i < model->rowCount(); ++i)
        shots.push_back(model->shotAt(i));

    return exportShotsToFile(otioPath, shots, timelineName);
}

bool OtioExporter::exportShotsToFile(const QString& otioPath,
                                      const std::vector<ShotItem>& shots,
                                      const QString& timelineName)
{
    _errorString.clear();
    if (shots.empty()) {
        _errorString = QStringLiteral("播放列表为空，无法导出 OTIO。");
        return false;
    }
    // Reject incomplete rows before constructing or writing any output. A
    // freshly imported row only has placeholder frame/FPS values until probed.
    for (const auto& shot : shots) {
        if (!shot.isMetadataReady()) {
            _errorString = QStringLiteral("媒体信息尚未读取完成或读取失败，无法导出 OTIO：%1。请等待读取完成，或移除无法读取的条目后重试。")
                .arg(shot.name.isEmpty() ? shot.path : shot.name);
            return false;
        }
    }

    // Determine global FPS from first valid shot
    double globalFps = 24.0;
    for (const auto& s : shots) {
        if (s.fps > 0.0) { globalFps = s.fps; break; }
    }

    opentime::RationalTime globalStart(0.0, globalFps);

    // OTIO owns child objects; retain the root for this export and release the
    // entire graph afterwards, including when serialization fails.
    QString tName = timelineName.isEmpty() ? "CGPlay Timeline" : timelineName;
    OTIO::SerializableObject::Retainer<OTIO::Timeline> timeline(
        new OTIO::Timeline(tName.toStdString(), std::make_optional(globalStart)));

    // Stack
    auto* stack = new OTIO::Stack("Stack");

    // Video Track
    auto* videoTrack = new OTIO::Track(
        "Video", std::nullopt,
        std::string(OTIO::Track::Kind::video),
        OTIO::AnyDictionary());

    for (const auto& shot : shots) {
        QFileInfo fi(shot.path);
        double shotRate = shot.fps > 0.0 ? shot.fps : globalFps;
        bool isSequence = (shot.lastFrame > shot.firstFrame);

        QString ext = fi.suffix().toLower();
        bool isImageSeq = (ext == "exr" || ext == "dpx" || ext == "tif" ||
                           ext == "tiff" || ext == "png" || ext == "jpg" ||
                           ext == "jpeg");

        OTIO::MediaReference* mediaRef = nullptr;

        if (isImageSeq && isSequence) {
            std::string dirPath = fi.path().toStdString();
            std::string prefix = fi.completeBaseName().toStdString();
            auto dotPos = prefix.rfind('.');
            if (dotPos != std::string::npos)
                prefix = prefix.substr(0, dotPos + 1);
            std::string suffix = "." + ext.toStdString();

            int padding = 4;
            int maxFrame = std::max(std::abs(shot.firstFrame),
                                    std::abs(shot.lastFrame));
            if (maxFrame >= 10000) padding = 5;
            else if (maxFrame >= 1000) padding = 4;

            mediaRef = new OTIO::ImageSequenceReference(
                dirPath, prefix, suffix,
                shot.firstFrame, 1, shotRate, padding,
                OTIO::ImageSequenceReference::MissingFramePolicy::error);
        } else {
            mediaRef = new OTIO::ExternalReference(shot.path.toStdString());
        }

        int duration = shot.frameCount() > 0 ? shot.frameCount() : 1;
        opentime::TimeRange sourceRange(
            opentime::RationalTime(0.0, shotRate),
            opentime::RationalTime(static_cast<double>(duration), shotRate));

        auto* clip = new OTIO::Clip(
            shot.name.toStdString(),
            mediaRef,
            std::make_optional(sourceRange));

        videoTrack->append_child(clip);
    }

    stack->append_child(videoTrack);
    timeline->set_tracks(stack);

    OTIO::ErrorStatus error;
    const bool ok = timeline->to_json_file(otioPath.toStdString(), &error, nullptr, 4);

    if (!ok) {
        _errorString = QString::fromStdString(error.full_description);
        qWarning() << "[OtioExporter] Failed to serialize:"
                    << QString::fromStdString(error.full_description);
    }

    return ok;
}

} // namespace cgplay
