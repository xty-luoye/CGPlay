// CGPlay OtioImporter.cpp

#include "OtioImporter.h"
#include "playlist/PlaylistModel.h"

#include <opentimelineio/timeline.h>
#include <opentimelineio/track.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/clip.h>
#include <opentimelineio/composition.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/imageSequenceReference.h>
#include <opentimelineio/deserialization.h>
#include <opentime/rationalTime.h>
#include <opentime/timeRange.h>

#include <QFileInfo>
#include <QDir>
#include <QDebug>

namespace cgplay {

namespace OTIO = opentimelineio::OPENTIMELINEIO_VERSION;
namespace opentime = opentime::OPENTIME_VERSION;

ShotItem OtioImporter::_clipToShot(const std::string& clipName,
                                    const std::string& mediaPath,
                                    int firstFrame, int lastFrame,
                                    double fps,
                                    const std::string& trackKind)
{
    ShotItem item;
    QString qPath = QString::fromStdString(mediaPath);

    item.path       = qPath;
    item.name       = clipName.empty()
                        ? QString::fromStdString(trackKind) + " - " + QFileInfo(qPath).completeBaseName()
                        : QString::fromStdString(clipName);
    item.firstFrame = firstFrame;
    item.lastFrame  = lastFrame;
    item.format     = PlaylistModel::_detectFormatPublic(qPath);
    item.fps        = fps;
    item.trackKind  = QString::fromStdString(trackKind);

    return item;
}

std::vector<ShotItem> OtioImporter::importFile(const QString& otioPath)
{
    std::vector<ShotItem> shots;

    std::any result;
    OTIO::ErrorStatus error;
    bool ok = OTIO::deserialize_json_from_file(otioPath.toStdString(), &result, &error);

    if (!ok || !result.has_value()) {
        qWarning() << "[OtioImporter] Failed to parse OTIO:"
                    << QString::fromStdString(error.full_description);
        return shots;
    }

    // Try to cast to Timeline
    auto* timeline = std::any_cast<OTIO::Timeline>(&result);
    if (!timeline) {
        // Try SerializableObject
        auto* ser = std::any_cast<OTIO::SerializableObject>(&result);
        if (ser) {
            auto* tl = dynamic_cast<OTIO::Timeline*>(ser);
            if (!tl) {
                qWarning() << "[OtioImporter] OTIO root is not a Timeline";
                return shots;
            }
            timeline = tl;
        } else {
            qWarning() << "[OtioImporter] Unknown OTIO root type";
            return shots;
        }
    }

    OTIO::Stack* stack = timeline->tracks();
    if (!stack) {
        qWarning() << "[OtioImporter] Timeline has no tracks (Stack)";
        return shots;
    }

    double globalRate = 24.0;
    std::optional<opentime::RationalTime> gst = timeline->global_start_time();
    if (gst.has_value()) globalRate = gst->rate();

    // Iterate tracks
    auto& children = stack->children();
    for (size_t ti = 0; ti < children.size(); ++ti) {
        auto* otioTrack = dynamic_cast<OTIO::Track*>(children[ti].value);
        if (!otioTrack) continue;

        std::string trackKind = otioTrack->kind();

        auto& trackChildren = otioTrack->children();
        for (size_t ci = 0; ci < trackChildren.size(); ++ci) {
            auto* clip = dynamic_cast<OTIO::Clip*>(trackChildren[ci].value);
            if (!clip) continue;

            std::string clipName = clip->name();
            double clipRate = globalRate;
            int firstFrame = 0, lastFrame = 0;

            // Source range
            auto srcRange = clip->source_range();
            if (srcRange.has_value()) {
                firstFrame = static_cast<int>(srcRange->start_time().value());
                int dur  = static_cast<int>(srcRange->duration().value());
                lastFrame = firstFrame + dur - 1;
                if (lastFrame < firstFrame) lastFrame = firstFrame;
                clipRate = srcRange->start_time().rate();
            }

            // Media reference
            std::string mediaPath;
            auto* extRef = dynamic_cast<OTIO::ExternalReference*>(clip->media_reference());
            auto* seqRef = dynamic_cast<OTIO::ImageSequenceReference*>(clip->media_reference());

            if (extRef) {
                mediaPath = extRef->target_url();
            } else if (seqRef) {
                mediaPath = seqRef->target_url_base();
                if (srcRange.has_value()) {
                    int seqFirst = seqRef->start_frame();
                    firstFrame = static_cast<int>(srcRange->start_time().value()) + seqFirst;
                    lastFrame  = firstFrame + static_cast<int>(srcRange->duration().value()) - 1;
                }
            } else {
                continue; // No usable media reference
            }

            shots.push_back(_clipToShot(clipName, mediaPath,
                                         firstFrame, lastFrame, clipRate, trackKind));
        }
    }

    return shots;
}

bool OtioImporter::importToModel(const QString& otioPath, PlaylistModel* model)
{
    if (!model) return false;

    auto shots = importFile(otioPath);
    if (shots.empty()) return false;

    model->clear();
    for (auto& shot : shots)
        model->addShot(std::move(shot));

    return true;
}

} // namespace cgplay
