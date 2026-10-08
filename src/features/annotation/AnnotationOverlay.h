#pragma once
// CGPlay AnnotationOverlay.h — 批注绘制叠加层 (透明 QWidget)
// 覆盖在 overlay host 上，用 QPainter 绘制批注。
// 坐标变换通过 IViewerCoordinateMapper 实时读取，无缓存同步问题。

#include "AnnotationItem.h"
#include "viewer/api/IViewerCoordinateMapper.h"

#include <QWidget>
#include <QVector>

namespace cgplay {

class AnnotationManager;

// ─── AnnotationOverlay ──────────────────────────────────────────────────────
class AnnotationOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit AnnotationOverlay(AnnotationManager* mgr, QWidget* parent = nullptr);
    ~AnnotationOverlay() override = default;

    // ── 坐标映射引用（用于实时读取 zoom/viewPos）──────────────────
    void setCoordinateMapper(IViewerCoordinateMapper* mapper) {
        if (_coordinateMapper == mapper) {
            return;
        }
        _coordinateMapper = mapper;
        update();
    }

    // ── 媒体尺寸（仅尺寸变化时需更新）──────────────────────────────
    void setMediaSize(int w, int h) {
        if (_imgW == w && _imgH == h) {
            return;
        }
        _imgW = w;
        _imgH = h;
        update();
    }

    // ── 保留旧接口兼容 ──────────────────────────────────────────────
    void setViewTransform(int imgW, int imgH,
                          double /*offsetX*/, double /*offsetY*/,
                          double /*zoom*/) {
        if (_imgW == imgW && _imgH == imgH) {
            return;
        }
        _imgW = imgW;
        _imgH = imgH;
        update();
    }

    // ── 批注数据 ─────────────────────────────────────────────────────
    void setAnnotations(const QVector<AnnotationItem>& annotations);
    void setSelectedId(const QString& id);
    QString selectedId() const { return _selectedId; }
    void setCurrentFrame(int frame) { _currentFrame = frame; }

    // ── 批注绘制模式 ─────────────────────────────────────────────────
    enum class ToolMode {
        Select,    // 选择/无绘制
        Arrow,
        Rectangle,
        Circle,
        Text,
        FreeDraw,
        Point
    };
    void setToolMode(ToolMode mode);
    ToolMode toolMode() const { return _toolMode; }
    void setToolColor(const QColor& color) {
        if (_toolColor == color) {
            return;
        }
        _toolColor = color;
        update();
    }
    QColor toolColor() const { return _toolColor; }

    // ── 坐标转换辅助 ─────────────────────────────────────────────────
    QPointF imgToWidget(QPointF imgPt) const;
    QPointF widgetToImg(QPointF widgetPt) const;
    QPointF imgToWidget(double ix, double iy) const {
        return imgToWidget(QPointF(ix, iy));
    }

Q_SIGNALS:
    // 批注绘制完成 (由鼠标事件触发)
    void annotationCreated(const AnnotationItem& ann);
    void annotationSelected(const QString& id);
    void annotationDeleteRequested(const QString& id);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void _drawAnnotation(QPainter& painter, const AnnotationItem& ann,
                         bool isSelected);
    void _drawArrow(QPainter& p, const QVector<QPointF>& pts,
                    const QColor& color, double penW, bool selected);
    void _drawRect(QPainter& p, const QVector<QPointF>& pts,
                   const QColor& color, double penW, bool selected);
    void _drawCircle(QPainter& p, const QVector<QPointF>& pts,
                     const QColor& color, double penW, bool selected);
    void _drawText(QPainter& p, const QVector<QPointF>& pts,
                   const QString& text, const QColor& color);
    void _drawFreeDraw(QPainter& p, const QVector<QPointF>& pts,
                       const QColor& color, double penW, bool selected);
    void _drawPoint(QPainter& p, const QVector<QPointF>& pts,
                    const QColor& color, double penW, bool selected);

    // Hit test
    QString _hitTest(const QPointF& imgPos) const;

    // 视频画面在 widget 中的矩形 + 坐标限制
    QRectF _videoRect() const;
    QPointF _clampToVideo(QPointF wtPt) const;

    double _currentZoom() const;
    AnnotationManager* _mgr;
    IViewerCoordinateMapper* _coordinateMapper = nullptr;  // 直接引用，实时读取变换

    // Media dimensions (set once when video loads)
    int    _imgW = 1920, _imgH = 1080;

    // Current frame annotations
    QVector<AnnotationItem> _annotations;
    QString _selectedId;
    int    _currentFrame = 0;

    // Tool state
    ToolMode _toolMode = ToolMode::Select;
    QColor   _toolColor = QColor(255, 0, 0);
    bool     _drawing = false;
    QVector<QPointF> _drawPts;   // image coords being drawn
};

} // namespace cgplay
