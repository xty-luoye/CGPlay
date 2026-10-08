#pragma once
// CGPlay TlViewport.h
// Bridges tl::ui::Viewport (same as DJV) into a Qt QOpenGLWidget.
// Replicates the essential logic from tl::qtwidget::ContainerWidget.

#include <tlRender/Timeline/BackgroundOptions.h>
#include <tlRender/Timeline/ColorOptions.h>
#include <tlRender/Timeline/CompareOptions.h>
#include <tlRender/Timeline/DisplayOptions.h>
#include <tlRender/Timeline/ForegroundOptions.h>
#include <tlRender/Timeline/Player.h>

#include <ftk/UI/IWidget.h>
#include <ftk/UI/IWindow.h>
#include <ftk/GL/Texture.h>

#include <QOpenGLWidget>
#include <QOpenGLFunctions_4_1_Core>
#include <QJsonObject>

#include <memory>

namespace cgplay {

class GPUUploadQueue;

// ─── TlViewport ───────────────────────────────────────────────────────────────
// QOpenGLWidget that renders a tl::ui::Viewport.
// Follows DJV's architecture: tl::ui::Viewport → offscreen buffer → screen.
// v1.1: 集成 GPUUploadQueue 用于异步纹理上传。
// ─────────────────────────────────────────────────────────────────────────────
class TlViewport :
    public QOpenGLWidget,
    protected QOpenGLFunctions_4_1_Core
{
    Q_OBJECT

public:
    TlViewport(
        const std::shared_ptr<ftk::Context>&,
        const std::shared_ptr<ftk::Style>&,
        QWidget* parent = nullptr);
    ~TlViewport() override;

    // ── View API ──────────────────────────────────────────────────────────
    const ftk::V2I& viewPos() const;
    double zoom() const;
    bool hasFrameView() const;

    // ── Coordinate mapping (image ↔ widget) ────────────────────────────
    QPointF imageToWidget(double ix, double iy, int imgW, int imgH) const;
    QPointF widgetToImage(double wx, double wy, int imgW, int imgH) const;

    // ── v1.1 GPUUploadQueue ───────────────────────────────────────────────
    GPUUploadQueue* uploadQueue() const;

    // ── Benchmark counters (read-only instrumentation) ───────────────────
    void resetRenderStats();
    qint64 renderFrameCount() const;
    double renderStatsSeconds() const;
    double renderFps() const;
    QJsonObject benchmarkDiagnostics() const;

public Q_SLOTS:
    void setPlayer(const std::shared_ptr<tl::Player>&);
    void clearPlayer();
    void setOCIOOptions(const tl::OCIOOptions&);
    void setLUTOptions(const tl::LUTOptions&);
    void setImageOptions(const std::vector<ftk::ImageOptions>&);
    void setDisplayOptions(const std::vector<tl::DisplayOptions>&);
    void setCompareOptions(const tl::CompareOptions&);
    void setBackgroundOptions(const tl::BackgroundOptions&);
    void setForegroundOptions(const tl::ForegroundOptions&);

    void setViewPosAndZoom(const ftk::V2I&, double);
    void setZoom(double, const ftk::V2I& focus = ftk::V2I());
    void setFrameView(bool);
    void resetZoom();
    void zoomIn();
    void zoomOut();

Q_SIGNALS:
    void viewPosAndZoomChanged(const ftk::V2I&, double);
    void frameViewChanged(bool);
    void viewportResized();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void _tickEvent();
    bool _hasSizeUpdate(const std::shared_ptr<ftk::IWidget>&) const;
    void _sizeHintEvent();
    void _setGeometry();
    void _emitViewPosAndZoomChangedIfNeeded(const ftk::V2I&, double);
    void _markInteractionActive();
    void _refreshTimerInterval(bool pendingSizeUpdate = false);
    int _desiredPlaybackTimerIntervalMs() const;
    void _forceRender(); // 轻微改变 geometry 强制触发 Viewport::doRender
    void _requestUpdate(const char* source);
    bool _hasDrawUpdate(const std::shared_ptr<ftk::IWidget>&) const;
    void _drawEvent(
        const std::shared_ptr<ftk::IWidget>&,
        const ftk::Box2I&,
        const ftk::DrawEvent&);

    struct Private;
    std::unique_ptr<Private> _p;
};

} // namespace cgplay
