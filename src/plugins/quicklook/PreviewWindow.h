#pragma once

#include <QWidget>
#include <memory>

class QLabel;
class QPushButton;
class QSlider;

namespace cgplay {
class CacheManager;
class OcioManager;
class PlaybackController;
class ViewerWidget;
}

namespace cgplay::quicklook {

class PreviewWindow : public QWidget
{
    Q_OBJECT
public:
    explicit PreviewWindow(
        std::shared_ptr<CacheManager> cache,
        std::shared_ptr<OcioManager> ocio,
        QWidget* parent = nullptr);

    void openFile(const QString& path, bool autoPlay = false);
    void togglePlayback();
    void closePreview();
    void setStatusText(const QString& text);
    bool isShowingFile(const QString& path) const;
    bool hasFile() const;
    QString currentPath() const;

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QString _formatTimecode(int frame) const;
    void _updateTransport(int frame, int total);
    void _updatePlayState(int state);

    std::shared_ptr<CacheManager> _cache;
    std::shared_ptr<OcioManager> _ocio;
    std::shared_ptr<PlaybackController> _playback;
    ViewerWidget* _viewer = nullptr;
    QLabel* _title = nullptr;
    QLabel* _hint = nullptr;
    QPushButton* _playButton = nullptr;
    QPushButton* _muteButton = nullptr;
    QSlider* _progressSlider = nullptr;
    QSlider* _volumeSlider = nullptr;
    QLabel* _timeLabel = nullptr;
    bool _updatingProgress = false;
    bool _autoPlayPending = false;
    quint64 _openGeneration = 0;
};

} // namespace cgplay::quicklook
