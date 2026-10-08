#pragma once
// CGPlay SecondaryWindow.h
// Independent secondary viewer window with its own playback module & viewport

#include <QMainWindow>
#include <memory>

namespace cgplay {

class ViewerWidget;
class TimelineWidget;
class IPlaybackService;
class OcioManager;
class CacheManager;

class SecondaryWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit SecondaryWindow(
        std::shared_ptr<OcioManager>        ocio,
        std::shared_ptr<CacheManager>       cache,
        int windowIndex = 0,
        QWidget* parent = nullptr);
    ~SecondaryWindow() override;

    void openFile(const QString& path);
    void setCompareFile(const QString& path);

    IPlaybackService* playbackCtrl() const { return _playbackCtrl.get(); }
    ViewerWidget* viewerWidget() const { return _viewer; }

Q_SIGNALS:
    void closed(int windowIndex);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void _setupUI();
    void _connectSignals();
    void _toggleFullScreen();
    void _showFullScreenChromeTemporarily();
    void _hideFullScreenChrome();
    void _setFullScreenChromeVisible(bool visible, bool forceApply = false);
    void _setFullScreenCursorHidden(bool hidden);

    std::shared_ptr<OcioManager>         _ocio;
    std::shared_ptr<CacheManager>        _cache;
    std::shared_ptr<IPlaybackService>     _playbackCtrl;

    ViewerWidget*   _viewer   = nullptr;
    TimelineWidget* _timeline = nullptr;
    QTimer* _fullscreenChromeTimer = nullptr;
    bool _fullscreenTimeline = true;
    bool _fullscreenViewerChrome = true;
    bool _fullscreenChromeVisible = false;
    bool _fullscreenCursorHidden = false;

public:
    int _index = 0;  // Public for re-indexing
};

} // namespace cgplay
