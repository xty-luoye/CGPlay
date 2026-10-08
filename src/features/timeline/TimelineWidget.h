#pragma once
// CGPlay TimelineWidget — 三轨Timeline: Marker + Thumbnail + Frame Scale
#include <QWidget>
#include "ocio/OcioManager.h"
#include <memory>

namespace cgplay {

class IPlaybackService;

class TimelineWidget : public QWidget
{
    Q_OBJECT
public:
    explicit TimelineWidget(std::shared_ptr<IPlaybackService>, QWidget* parent=nullptr);
    ~TimelineWidget() override;
    void setInOutPoints(int inFrame, int outFrame);
    void setMediaPath(const QString& path);
    void setPreviewTransformSettings(const OcioManager::PreviewTransformSettings& settings);

Q_SIGNALS:
    void droppedFile(const QString& path);

protected:
    void resizeEvent(QResizeEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    void _onFrameChanged(int frame, int total);
    void _buildUI();
    void _startThumbnailBuild();
    bool _thumbnailRequestMatchesCurrent(const QString& path,
                                         int total,
                                         double fps,
                                         const OcioManager::PreviewTransformSettings& settings) const;

    struct Private;
    std::unique_ptr<Private> _p;
};

} // namespace cgplay
