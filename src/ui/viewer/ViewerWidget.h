#pragma once

#include "viewer/api/IOverlayHost.h"
#include "viewer/api/IViewerCoordinateMapper.h"

#include <QColor>
#include <QLabel>
#include <QString>
#include <QWidget>

#include <memory>

namespace cgplay {

class IPlaybackService;
class OcioManager;
class TlViewport;

class ViewerWidget : public QWidget, public IOverlayHost, public IViewerCoordinateMapper
{
    Q_OBJECT
    Q_INTERFACES(cgplay::IOverlayHost cgplay::IViewerCoordinateMapper)
public:
    explicit ViewerWidget(
        std::shared_ptr<IPlaybackService> playback,
        std::shared_ptr<OcioManager> ocio,
        QWidget* parent = nullptr);
    ~ViewerWidget() override;

    QString viewId() const;
    IPlaybackService* playbackService() const;
    TlViewport* viewport() const;
    void invalidateView();

    int mediaW() const { return _mediaW; }
    int mediaH() const { return _mediaH; }

    QWidget* overlayParentWidget() const override;
    QRect overlayGeometry() const override;
    void attachOverlay(QWidget* overlay) override;
    void detachOverlay(QWidget* overlay) override;
    void requestOverlayRefresh() override;
    QPointF imageToWidget(const QPointF& point, int imageWidth, int imageHeight) const override;
    QPointF widgetToImage(const QPointF& point, int imageWidth, int imageHeight) const override;
    QRectF visibleVideoRect() const override;
    double zoom() const override;
    QPointF viewPos() const override;

public Q_SLOTS:
    void clearMedia();
    void fitToWindow();
    void zoom1to1();
    void zoomIn();
    void zoomOut();
    void setZoom(double zoom);
    void setChromeVisible(bool visible);
    void setTimecodeVisible(bool visible);
    void setAlphaChannelVisible(bool visible);
    void toggleAlphaChannel();
    bool isTimecodeVisible() const;

Q_SIGNALS:
    void zoomChanged(double zoom);
    void resolutionChanged(int w, int h);
    void droppedFile(const QString& path);
    void viewportClicked();
    void compareRequested();
    void compareTileRequested();
    void fullscreenRequested();
    void alphaChannelChanged(bool visible);
    void viewActivated(const QString& viewId);
    void viewInvalidated(const QString& viewId);
    void coordinateMapperChanged(const QString& viewId);
    void coordinateMapperInvalidated(const QString& viewId);
    void transformChanged(const QString& viewId);
    void viewportResizedForView(const QString& viewId);

protected:
    void changeEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void _setupUI();
    void _applyRuntimeTheme();
    void _updateResponsiveChrome();
    void _setupViewport();
    void _connectPlayback();
    void _applyOcioOptions();
    void _refreshOverlayGeometry();
    void _scheduleOverlayRepaint();
    bool _applyWheelZoom(QWheelEvent* event, const QPoint& focus);

    struct Private;
    std::unique_ptr<Private> _p;
    int _mediaW = 1920;
    int _mediaH = 1080;
    friend class MainWindow;
};

} // namespace cgplay
