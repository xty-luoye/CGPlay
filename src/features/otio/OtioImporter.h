#pragma once
// CGPlay OtioImporter.h
// Imports OTIO JSON files into PlaylistModel

#include <QString>
#include <memory>
#include <vector>

namespace cgplay {

struct ShotItem;
class PlaylistModel;

class OtioImporter
{
public:
    OtioImporter() = default;

    /// Load an OTIO JSON file and return list of ShotItems (per Track/Clip)
    std::vector<ShotItem> importFile(const QString& otioPath);

    /// Import into an existing PlaylistModel (clears existing)
    bool importToModel(const QString& otioPath, PlaylistModel* model);

private:
    ShotItem _clipToShot(const std::string& clipName,
                         const std::string& mediaPath,
                         int firstFrame, int lastFrame,
                         double fps,
                         const std::string& trackKind);
};

} // namespace cgplay
