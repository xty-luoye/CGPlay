#pragma once
// CGPlay OtioExporter.h
// Exports PlaylistModel to OTIO JSON file

#include <QString>
#include <memory>
#include <vector>

namespace cgplay {

struct ShotItem;
class PlaylistModel;

class OtioExporter
{
public:
    OtioExporter() = default;

    /// Export playlist as OTIO JSON file. Returns true on success.
    bool exportToFile(const QString& otioPath, PlaylistModel* model,
                      const QString& timelineName = QString());

    /// Export a list of ShotItems directly
    bool exportShotsToFile(const QString& otioPath,
                           const std::vector<ShotItem>& shots,
                           const QString& timelineName = QString());
};

} // namespace cgplay
